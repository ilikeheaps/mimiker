/* Heavily inspired by FreeBSD / NetBSD `gt_pci.c` file. */

#define KL_LOG KL_DEV
#include <mips/malta.h>
#include <mips/intr.h>
#include <dev/i8259.h>
#include <dev/piixreg.h>
#include <dev/isareg.h>
#include <dev/gt64120reg.h>
#include <interrupt.h>
#include <pci.h>
#include <spinlock.h>
#include <stdc.h>
#include <klog.h>
#include <bus.h>

#define PCI0_CFG_REG_SHIFT 2
#define PCI0_CFG_FUNCT_SHIFT 8
#define PCI0_CFG_DEV_SHIFT 11
#define PCI0_CFG_BUS_SHIFT 16
#define PCI0_CFG_ENABLE 0x80000000

static unsigned gt_pci_make_addr(pci_addr_t addr, unsigned reg) {
  return (((addr.device) << PCI0_CFG_DEV_SHIFT) |
          ((addr.function) << PCI0_CFG_FUNCT_SHIFT) |
          ((reg) << PCI0_CFG_REG_SHIFT));
}

typedef union {
  uint8_t byte[4];
  uint16_t word[2];
  uint32_t dword;
} pci_reg_t;

#define LO(x) ((x)&0xff)
#define HI(x) (((x) >> 8) & 0xff)
#define ICU_ADDR(x) ((x) + 0)
#define ICU_DATA(x) ((x) + 1)
#define ICU1_ADDR ICU_ADDR(IO_ICU1)
#define ICU1_DATA ICU_DATA(IO_ICU1)
#define ICU2_ADDR ICU_ADDR(IO_ICU2)
#define ICU2_DATA ICU_DATA(IO_ICU2)

typedef struct gt_pci_state {

  /* Resources belonging to this driver. */
  resource_t *corectrl;
  resource_t *isa_io;
  resource_t *pci_io;
  resource_t *pci_mem;

  /* Resource managers which manage resources used by child devices. */
  rman_t rman_pci_iospace;
  rman_t rman_pci_memspace;

  intr_handler_t intr_handler;
  intr_chain_t intr_chain[16];

  uint16_t imask;
  uint16_t elcr;
} gt_pci_state_t;

extern bus_space_t *mips_bus_space_generic;
pci_bus_driver_t gt_pci_bus;

/* Access configuration space through memory mapped GT-64120 registers. Take
 * care of the fact that MIPS processor cannot handle unaligned accesses. */
static uint32_t gt_pci_read_config(device_t *dev, unsigned reg, unsigned size) {
  pci_device_t *pcid = pci_device_of(dev);
  gt_pci_state_t *gtpci = dev->parent->state;
  resource_t *pcicfg = gtpci->corectrl;

  if (pcid->addr.bus > 0)
    return -1;

  bus_space_write_4(pcicfg, GT_PCI0_CFG_ADDR,
                    PCI0_CFG_ENABLE | gt_pci_make_addr(pcid->addr, reg >> 2));
  pci_reg_t data = (pci_reg_t)bus_space_read_4(pcicfg, GT_PCI0_CFG_DATA);

  reg &= 3;
  switch (size) {
    case 1:
      return data.byte[3 - reg];
    case 2:
      return data.word[1 - (reg >> 1)];
    case 4:
      return data.dword;
    default:
      return -1;
  }
}

static void gt_pci_write_config(device_t *dev, unsigned reg, unsigned size,
                                uint32_t value) {
  pci_device_t *pcid = pci_device_of(dev);
  gt_pci_state_t *gtpci = dev->parent->state;
  resource_t *pcicfg = gtpci->corectrl;

  if (pcid->addr.bus > 0)
    return;

  bus_space_write_4(pcicfg, GT_PCI0_CFG_ADDR,
                    PCI0_CFG_ENABLE | gt_pci_make_addr(pcid->addr, reg >> 2));
  pci_reg_t data = (pci_reg_t)bus_space_read_4(pcicfg, GT_PCI0_CFG_DATA);

  reg &= 3;
  switch (size) {
    case 1:
      data.byte[3 - reg] = value;
    case 2:
      data.word[1 - (reg >> 1)] = value;
    case 4:
      data.dword = value;
    default:
      break;
  }

  bus_space_write_4(pcicfg, GT_PCI0_CFG_DATA, data.dword);
}

static void gt_pci_set_icus(gt_pci_state_t *gtpci) {
  /* Enable the cascade IRQ (2) if 8-15 is enabled. */
  if ((gtpci->imask & 0xff00) != 0xff00)
    gtpci->imask &= ~(1U << 2);
  else
    gtpci->imask |= (1U << 2);

  resource_t *io = gtpci->isa_io;
  bus_space_write_1(io, ICU1_DATA, LO(gtpci->imask));
  bus_space_write_1(io, ICU2_DATA, HI(gtpci->imask));
  bus_space_write_1(io, PIIX_REG_ELCR + 0, LO(gtpci->elcr));
  bus_space_write_1(io, PIIX_REG_ELCR + 1, HI(gtpci->elcr));
}

static void gt_pci_mask_irq(gt_pci_state_t *gtpci, unsigned irq) {
  gtpci->imask |= (1 << irq);
  gtpci->elcr |= (1 << irq);
  gt_pci_set_icus(gtpci);
}

static void gt_pci_unmask_irq(gt_pci_state_t *gtpci, unsigned irq) {
  gtpci->imask &= ~(1 << irq);
  gtpci->elcr &= ~(1 << irq);
  gt_pci_set_icus(gtpci);
}

static void gt_pci_intr_setup(device_t *pcib, unsigned irq,
                              intr_handler_t *handler) {
  assert(pcib->parent->driver == &gt_pci_bus.driver);

  gt_pci_state_t *gtpci = pcib->parent->state;
  intr_chain_t *chain = &gtpci->intr_chain[irq];
  WITH_SPINLOCK(&chain->ic_lock) {
    intr_chain_add_handler(chain, handler);
    if (chain->ic_count == 1)
      gt_pci_unmask_irq(gtpci, irq);
  }
}

static void gt_pci_intr_teardown(device_t *pcib, intr_handler_t *handler) {
  assert(pcib->parent->driver == &gt_pci_bus.driver);

  gt_pci_state_t *gtpci = pcib->parent->state;
  intr_chain_t *chain = handler->ih_chain;
  WITH_SPINLOCK(&chain->ic_lock) {
    if (chain->ic_count == 1)
      gt_pci_mask_irq(gtpci, chain->ic_irq);
    intr_chain_remove_handler(handler);
  }
}

static void init_8259(resource_t *io, unsigned icu, unsigned imask) {
  /* reset, program device, 4 bytes */
  bus_space_write_1(io, ICU_ADDR(icu), ICW1_RESET | ICW1_IC4);
  bus_space_write_1(io, ICU_DATA(icu), 0);
  bus_space_write_1(io, ICU_DATA(icu), 1 << 2); /* XXX magic value ??? */
  bus_space_write_1(io, ICU_DATA(icu), ICW4_8086);
  /* mask all interrupts */
  bus_space_write_1(io, ICU_DATA(icu), imask);
  /* enable special mask mode */
  bus_space_write_1(io, ICU_ADDR(icu), OCW3_SEL | OCW3_ESMM | OCW3_SMM);
  /* read IRR by default */
  bus_space_write_1(io, ICU_ADDR(icu), OCW3_SEL | OCW3_RR);
}

static intr_filter_t gt_pci_intr(void *data) {
  gt_pci_state_t *gtpci = data;
  resource_t *io = gtpci->isa_io;
  unsigned irq;

  assert(data != NULL);

  for (;;) {
    /* Handle master PIC, irq 0..7 */
    bus_space_write_1(io, ICU1_ADDR, OCW3_SEL | OCW3_POLL);
    irq = bus_space_read_1(io, ICU1_DATA);
    if ((irq & OCW3_POLL_PENDING) == 0)
      return IF_FILTERED;
    irq = OCW3_POLL_IRQ(irq);
    /* Handle slave PIC, irq 8..15 */
    if (irq == 2) {
      bus_space_write_1(io, ICU2_ADDR, OCW3_SEL | OCW3_POLL);
      irq = bus_space_read_1(io, ICU2_DATA);
      irq = (irq & OCW3_POLL_PENDING) ? (OCW3_POLL_IRQ(irq) + 8) : 2;
    }

    /* Irq 2 is used for PIC chaining, ignore it. */
    if (irq != 2)
      intr_chain_run_handlers(&gtpci->intr_chain[irq]);

    /* Send a specific EOI to slave PIC... */
    if (irq > 7) {
      bus_space_write_1(io, ICU2_ADDR,
                        OCW2_SEL | OCW2_EOI | OCW2_SL | OCW2_ILS(irq & 7));
      irq = 2;
    }

    /* ... and finally to master PIC. */
    bus_space_write_1(io, ICU1_ADDR,
                      OCW2_SEL | OCW2_EOI | OCW2_SL | OCW2_ILS(irq));
  }

  return IF_FILTERED;
}

static inline void gt_pci_intr_chain_init(gt_pci_state_t *gtpci, unsigned irq,
                                          const char *name) {
  gtpci->intr_chain[irq] = (intr_chain_t){
    .ic_name = (name),
    .ic_irq = (irq),
    .ic_lock = SPINLOCK_INITIALIZER(),
    .ic_handlers = TAILQ_HEAD_INITIALIZER(gtpci->intr_chain[irq].ic_handlers)};
  intr_chain_register(&gtpci->intr_chain[irq]);
}

#define MALTA_CORECTRL_SIZE (MALTA_CORECTRL_END - MALTA_CORECTRL_BASE + 1)
#define MALTA_PCI0_MEMORY_SIZE                                                 \
  (MALTA_PCI0_MEMORY_END - MALTA_PCI0_MEMORY_BASE + 1)

static int gt_pci_attach(device_t *pcib) {
  gt_pci_state_t *gtpci = pcib->state;

  /* PCI I/O memory */
  gtpci->pci_mem =
    bus_resource_alloc(pcib, RT_MEMORY, 0, MALTA_PCI0_MEMORY_BASE,
                       MALTA_PCI0_MEMORY_END, MALTA_PCI0_MEMORY_SIZE, 0);
  /* PCI I/O ports 0x1000-0xffff */
  gtpci->pci_io =
    bus_resource_alloc(pcib, RT_MEMORY, 0, MALTA_PCI0_IO_BASE + 0x1000,
                       MALTA_PCI0_IO_BASE + 0xffff, 0xf000, 0);
  /* GT64120 registers */
  gtpci->corectrl =
    bus_resource_alloc(pcib, RT_MEMORY, 0, MALTA_CORECTRL_BASE,
                       MALTA_CORECTRL_END, MALTA_CORECTRL_SIZE, 0);
  /* ISA I/O ports 0x0000-0x0fff */
  gtpci->isa_io = bus_resource_alloc(pcib, RT_MEMORY, 0, MALTA_PCI0_IO_BASE,
                                     MALTA_PCI0_IO_BASE + 0xfff, 0x1000, 0);

  if (gtpci->corectrl == NULL || gtpci->pci_mem == NULL ||
      gtpci->pci_io == NULL || gtpci->isa_io == NULL) {
    panic("gt64120 resource allocation fail");
  }

  rman_create_from_resource(&gtpci->rman_pci_iospace, gtpci->pci_io);
  rman_create_from_resource(&gtpci->rman_pci_memspace, gtpci->pci_mem);

  pcib->bus = DEV_BUS_PCI;

  /* All interrupts default to "masked off" and edge-triggered. */
  gtpci->imask = 0xffff;
  gtpci->elcr = 0;

  /* Initialize the 8259s. */
  resource_t *io = gtpci->isa_io;
  init_8259(io, IO_ICU1, LO(gtpci->imask));
  init_8259(io, IO_ICU2, HI(gtpci->imask));

  /* Default all interrupts to edge-triggered. */
  bus_space_write_1(io, PIIX_REG_ELCR + 0, LO(gtpci->elcr));
  bus_space_write_1(io, PIIX_REG_ELCR + 1, HI(gtpci->elcr));

  gt_pci_intr_chain_init(gtpci, 0, "timer");
  gt_pci_intr_chain_init(gtpci, 1, "kbd");       /* kbd controller (keyboard) */
  gt_pci_intr_chain_init(gtpci, 2, "pic-slave"); /* PIC cascade */
  gt_pci_intr_chain_init(gtpci, 3, "uart(1)");   /* COM 2 */
  gt_pci_intr_chain_init(gtpci, 4, "uart(0)");   /* COM 1 */
  gt_pci_intr_chain_init(gtpci, 5, "unused(0)");
  gt_pci_intr_chain_init(gtpci, 6, "floppy");   /* floppy */
  gt_pci_intr_chain_init(gtpci, 7, "parallel"); /* centronics */
  gt_pci_intr_chain_init(gtpci, 8, "rtc");      /* RTC */
  gt_pci_intr_chain_init(gtpci, 9, "i2c");      /* I2C */
  gt_pci_intr_chain_init(gtpci, 10, "unused(1)");
  gt_pci_intr_chain_init(gtpci, 11, "unused(2)");
  gt_pci_intr_chain_init(gtpci, 12, "mouse"); /* kbd controller (mouse) */
  gt_pci_intr_chain_init(gtpci, 13, "unused(3)");
  gt_pci_intr_chain_init(gtpci, 14, "ide(0)"); /* IDE primary */
  gt_pci_intr_chain_init(gtpci, 15, "ide(1)"); /* IDE secondary */

  pci_bus_enumerate(pcib);

  gtpci->intr_handler =
    INTR_HANDLER_INIT(gt_pci_intr, NULL, gtpci, "GT64120 interrupt", 0);
  bus_intr_setup(pcib, MIPS_HWINT0, &gtpci->intr_handler);

  return bus_generic_probe(pcib);
}

static resource_t *gt_pci_resource_alloc(device_t *pcib, device_t *dev,
                                         resource_type_t type, int rid,
                                         rman_addr_t start, rman_addr_t end,
                                         size_t size, unsigned flags) {

  gt_pci_state_t *gtpci = pcib->state;

  /* Hack to directly return ISA resource. Need to implement PCI-ISA bridge. */
  if (type == RT_ISA)
    return gtpci->isa_io;

  /* Now handle only PCI devices. */

  /* Currently all devices are logicaly attached to PCI bus, because we don't
     have PCI-ISA bridge implemented. ISA devices are required to specify
     RT_ISA_F flag, and have their dev->bus set to DEV_BUS_NONE. */
  assert(dev->bus == DEV_BUS_PCI && dev->parent->bus == DEV_BUS_PCI);

  /* Find identified bar by rid. */
  pci_device_t *pcid = pci_device_of(dev);
  pci_bar_t *bar = &pcid->bar[rid];

  if (bar->size == 0)
    return NULL;

  resource_t *r = NULL;

  if (type == RT_MEMORY) {
    r = rman_allocate_resource(&gtpci->rman_pci_memspace, start, end, bar->size,
                               bar->size, flags);
  } else if (type == RT_IOPORTS) {
    r = rman_allocate_resource(&gtpci->rman_pci_iospace, start, end, bar->size,
                               bar->size, flags);
  } else {
    panic("Unknown PCI device type: %d", type);
  }

  if (!r)
    return NULL;

  device_add_resource(dev, r, rid, mips_bus_space_generic);

  /* Write BAR address to PCI device register. */
  if (!(flags & RF_ACTIVATED)) {
    pci_write_config(dev, PCIR_BAR(rid), 4, r->r_start);
    r->r_flags |= RF_ACTIVATED;
  }

  return r;
}

pci_bus_driver_t gt_pci_bus = {
  .driver = {.desc = "GT-64120 PCI bus driver",
             .size = sizeof(gt_pci_state_t),
             .attach = gt_pci_attach},
  .bus = {.intr_setup = gt_pci_intr_setup,
          .intr_teardown = gt_pci_intr_teardown,
          .resource_alloc = gt_pci_resource_alloc},
  .pci_bus =
    {
      .read_config = gt_pci_read_config, .write_config = gt_pci_write_config,
    }};
