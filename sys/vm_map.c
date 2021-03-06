#define KL_LOG KL_VM
#include <klog.h>
#include <stdc.h>
#include <pool.h>
#include <pmap.h>
#include <vm_pager.h>
#include <vm_object.h>
#include <vm_map.h>
#include <errno.h>
#include <proc.h>
#include <sched.h>
#include <pcpu.h>
#include <sysinit.h>

struct vm_segment {
  TAILQ_ENTRY(vm_segment) link;
  vm_object_t *object;
  vm_prot_t prot;
  vaddr_t start;
  vaddr_t end;
};

struct vm_map {
  TAILQ_HEAD(vm_map_list, vm_segment) entries;
  size_t nentries;
  pmap_t *pmap;
  mtx_t mtx; /* Mutex guarding vm_map structure and all its entries. */
};

static POOL_DEFINE(P_VMMAP, "vm_map", sizeof(vm_map_t));
static POOL_DEFINE(P_VMENTRY, "vm_segment", sizeof(vm_segment_t));

static vm_map_t *kspace = &(vm_map_t){};

void vm_map_activate(vm_map_t *map) {
  SCOPED_NO_PREEMPTION();

  PCPU_SET(uspace, map);
  pmap_activate(map ? map->pmap : NULL);
}

vm_map_t *get_user_vm_map(void) {
  return PCPU_GET(uspace);
}

vm_map_t *get_kernel_vm_map(void) {
  return kspace;
}

void vm_segment_range(vm_segment_t *seg, vaddr_t *start_p, vaddr_t *end_p) {
  *start_p = seg->start;
  *end_p = seg->end;
}

void vm_map_range(vm_map_t *map, vaddr_t *start_p, vaddr_t *end_p) {
  *start_p = map->pmap->start;
  *end_p = map->pmap->end;
}

bool vm_map_in_range(vm_map_t *map, vaddr_t addr) {
  return map && map->pmap && map->pmap->start <= addr && addr < map->pmap->end;
}

vm_map_t *get_active_vm_map_by_addr(vaddr_t addr) {
  if (vm_map_in_range(get_user_vm_map(), addr))
    return get_user_vm_map();
  if (vm_map_in_range(get_kernel_vm_map(), addr))
    return get_kernel_vm_map();
  return NULL;
}

static void vm_map_setup(vm_map_t *map) {
  TAILQ_INIT(&map->entries);
  mtx_init(&map->mtx, MTX_DEF);
}

static void vm_map_init(void) {
  vm_map_setup(kspace);
  kspace->pmap = get_kernel_pmap();
}

vm_map_t *vm_map_new(void) {
  vm_map_t *map = pool_alloc(P_VMMAP, PF_ZERO);
  vm_map_setup(map);
  map->pmap = pmap_new();
  return map;
}

vm_segment_t *vm_segment_alloc(vm_object_t *obj, vaddr_t start, vaddr_t end,
                               vm_prot_t prot) {
  assert(is_aligned(start, PAGESIZE));
  assert(is_aligned(end, PAGESIZE));

  vm_segment_t *seg = pool_alloc(P_VMENTRY, PF_ZERO);
  seg->object = obj;
  seg->start = start;
  seg->end = end;
  seg->prot = prot;
  return seg;
}

void vm_segment_free(vm_segment_t *seg) {
  if (seg->object)
    vm_object_free(seg->object);
  pool_free(P_VMENTRY, seg);
}

vm_segment_t *vm_map_find_segment(vm_map_t *map, vaddr_t vaddr) {
  SCOPED_MTX_LOCK(&map->mtx);
  vm_segment_t *it;
  TAILQ_FOREACH (it, &map->entries, link)
    if (it->start <= vaddr && vaddr < it->end)
      return it;
  return NULL;
}

static void vm_map_insert_after(vm_map_t *map, vm_segment_t *after,
                                vm_segment_t *seg) {
  assert(mtx_owned(&map->mtx));
  if (after)
    TAILQ_INSERT_AFTER(&map->entries, after, seg, link);
  else
    TAILQ_INSERT_HEAD(&map->entries, seg, link);
  map->nentries++;
}

static void vm_map_remove_segment(vm_map_t *map, vm_segment_t *seg) {
  assert(mtx_owned(&map->mtx));
  TAILQ_REMOVE(&map->entries, seg, link);
  map->nentries--;
}

void vm_map_delete(vm_map_t *map) {
  WITH_MTX_LOCK (&map->mtx) {
    vm_segment_t *seg;
    while ((seg = TAILQ_FIRST(&map->entries))) {
      vm_map_remove_segment(map, seg);
      vm_segment_free(seg);
    }
  }
  pmap_delete(map->pmap);
  pool_free(P_VMMAP, map);
}

/* TODO: not implemented */
void vm_map_protect(vm_map_t *map, vaddr_t start, vaddr_t end, vm_prot_t prot) {
}

static int vm_map_findspace_nolock(vm_map_t *map, vaddr_t /*inout*/ *start_p,
                                   size_t length, vm_segment_t **after_p) {
  vaddr_t start = *start_p;

  assert(is_aligned(start, PAGESIZE));
  assert(is_aligned(length, PAGESIZE));

  /* Bounds check */
  if (start < map->pmap->start)
    start = map->pmap->start;
  if (start + length > map->pmap->end)
    return -ENOMEM;

  if (after_p)
    *after_p = NULL;

  /* Entire space free? */
  if (TAILQ_EMPTY(&map->entries))
    goto found;

  /* Is enought space before the first entry in the map? */
  vm_segment_t *first = TAILQ_FIRST(&map->entries);
  if (start + length <= first->start)
    goto found;

  /* Browse available gaps. */
  vm_segment_t *it;
  TAILQ_FOREACH (it, &map->entries, link) {
    vm_segment_t *next = TAILQ_NEXT(it, link);
    vaddr_t gap_start = it->end;
    vaddr_t gap_end = next ? next->start : map->pmap->end;

    /* Move start address forward if it points inside allocated space. */
    if (start < gap_start)
      start = gap_start;

    /* Will we fit inside this gap? */
    if (start + length <= gap_end) {
      if (after_p)
        *after_p = it;
      goto found;
    }
  }

  /* Failed to find free space. */
  return -ENOMEM;

found:
  *start_p = start;
  return 0;
}

int vm_map_findspace(vm_map_t *map, vaddr_t *start_p, size_t length) {
  SCOPED_MTX_LOCK(&map->mtx);
  return vm_map_findspace_nolock(map, start_p, length, NULL);
}

int vm_map_insert(vm_map_t *map, vm_segment_t *seg, vm_flags_t flags) {
  SCOPED_MTX_LOCK(&map->mtx);
  vm_segment_t *after;
  vaddr_t start = seg->start;
  size_t length = seg->end - seg->start;
  int error = vm_map_findspace_nolock(map, &start, length, &after);
  if (error)
    return error;
  if ((flags & VM_FIXED) && (start != seg->start))
    return -ENOMEM;
  seg->start = start;
  seg->end = start + length;
  vm_map_insert_after(map, after, seg);
  return 0;
}

int vm_map_resize(vm_map_t *map, vm_segment_t *seg, vaddr_t new_end) {
  assert(is_aligned(new_end, PAGESIZE));

  SCOPED_MTX_LOCK(&map->mtx);

  /* TODO: As for now, we are unable to decrease the size of an entry, because
     it would require unmapping physical pages, which in turn should clean
     TLB. This is not implemented yet, and therefore shrinking an entry
     immediately leads to very confusing behavior, as the vm_map and TLB entries
     do not match. */
  assert(new_end >= seg->end);

  if (new_end > seg->end) {
    /* Expanding entry */
    vm_segment_t *next = TAILQ_NEXT(seg, link);
    vaddr_t gap_end = next ? next->start : map->pmap->end;
    if (new_end > gap_end)
      return -ENOMEM;
  } else {
    /* Shrinking entry */
    if (new_end < seg->start)
      return -ENOMEM;
    /* TODO: Invalidate tlb? */
  }
  /* Note that tailq does not require updating. */
  seg->end = new_end;
  return 0;
}

void vm_map_dump(vm_map_t *map) {
  SCOPED_MTX_LOCK(&map->mtx);

  klog("Virtual memory map (%08lx - %08lx):", map->pmap->start, map->pmap->end);

  vm_segment_t *it;
  TAILQ_FOREACH (it, &map->entries, link) {
    klog(" * %08lx - %08lx [%c%c%c]", it->start, it->end,
         (it->prot & VM_PROT_READ) ? 'r' : '-',
         (it->prot & VM_PROT_WRITE) ? 'w' : '-',
         (it->prot & VM_PROT_EXEC) ? 'x' : '-');
    vm_map_object_dump(it->object);
  }
}

vm_map_t *vm_map_clone(vm_map_t *map) {
  thread_t *td = thread_self();
  assert(td->td_proc);

  vm_map_t *new_map = vm_map_new();

  WITH_MTX_LOCK (&map->mtx) {
    vm_segment_t *it;
    TAILQ_FOREACH (it, &map->entries, link) {
      vm_object_t *obj = vm_object_clone(it->object);
      vm_segment_t *seg = vm_segment_alloc(obj, it->start, it->end, it->prot);
      TAILQ_INSERT_TAIL(&new_map->entries, seg, link);
      new_map->nentries++;
    }
  }

  return new_map;
}

int vm_page_fault(vm_map_t *map, vaddr_t fault_addr, vm_prot_t fault_type) {
  vm_segment_t *seg = vm_map_find_segment(map, fault_addr);

  if (!seg) {
    klog("Tried to access unmapped memory region: 0x%08lx!", fault_addr);
    return -EFAULT;
  }

  if (seg->prot == VM_PROT_NONE) {
    klog("Cannot access to address: 0x%08lx", fault_addr);
    return -EACCES;
  }

  if (!(seg->prot & VM_PROT_WRITE) && (fault_type == VM_PROT_WRITE)) {
    klog("Cannot write to address: 0x%08lx", fault_addr);
    return -EACCES;
  }

  if (!(seg->prot & VM_PROT_READ) && (fault_type == VM_PROT_READ)) {
    klog("Cannot read from address: 0x%08lx", fault_addr);
    return -EACCES;
  }

  assert(seg->start <= fault_addr && fault_addr < seg->end);

  vm_object_t *obj = seg->object;

  assert(obj != NULL);

  vaddr_t fault_page = fault_addr & -PAGESIZE;
  vaddr_t offset = fault_page - seg->start;
  vm_page_t *frame = vm_object_find_page(seg->object, offset);

  if (frame == NULL)
    frame = obj->pager->pgr_fault(obj, offset);

  if (frame == NULL)
    return -EFAULT;

  pmap_enter(map->pmap, fault_page, frame, seg->prot);

  return 0;
}

SYSINIT_ADD(vm_map, vm_map_init, NODEPS);
