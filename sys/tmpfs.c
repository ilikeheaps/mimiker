#include <tmpfs.h>
#include <mount.h>
#include <vfs.h>
#include <malloc.h>
#include <vnode.h>
#include <errno.h>
#include <common.h>
#include <stdc.h>

static MALLOC_DEFINE(TMPFS_POOL, "tmpfs", 8, 128);

#define TMPFS_BUF_SIZE 1024
#define TMPFS_MAX_NAMELEN 256

static tmpfs_node_t *tmpfs_new_node(tmpfs_node_type type, const char *name) {
  tmpfs_node_t *res = kmalloc(TMPFS_POOL, sizeof(tmpfs_node_t), 0);
  res->name = kstrndup(TMPFS_POOL, name, TMPFS_MAX_NAMELEN);
  res->type = type;

  if (type == T_DIR) {
    TAILQ_INIT(&res->dirdata.head);
  }

  if (type == T_REG) {
    res->data.buf = kmalloc(TMPFS_POOL, TMPFS_BUF_SIZE, 0);
    res->data.size = TMPFS_BUF_SIZE;
  }

  return res;
}

static void tmpfs_delete_node(tmpfs_node_t *node) {
  kfree(TMPFS_POOL, node->name);
  kfree(TMPFS_POOL, node);
}

static void tmpfs_dirnode_insert(tmpfs_dirnode_data_t *dirdata,
                                 tmpfs_node_t *node) {
  TAILQ_INSERT_TAIL(&dirdata->head, node, direntry);
}

static void tmpfs_dirnode_remove(tmpfs_dirnode_data_t *dirdata,
                                 tmpfs_node_t *node) {
  TAILQ_REMOVE(&dirdata->head, node, direntry);
}

static tmpfs_node_t *tmpfs_dirnode_find(tmpfs_dirnode_data_t *dirdata,
                                        const char *name) {
  tmpfs_node_t *it;
  TAILQ_FOREACH (it, &dirdata->head, direntry) {
    if (strcmp(name, it->name) == 0) {
      return it;
    }
  }
  return NULL;
}

int tmpfs_vnode_lookup(vnode_t *dv, const char *name, vnode_t **vp) {
  tmpfs_node_t *dirnode = (tmpfs_node_t *)dv->v_data;
  assert(dirnode->type == T_DIR);
  tmpfs_dirnode_data_t *dirdata = &dirnode->dirdata;
  tmpfs_node_t *node = tmpfs_dirnode_find(dirdata, name);

  if (!node) {
    vnodetype_t type = V_REG;
    if (node->type == T_DIR)
      type = V_DIR;
    *vp = vnode_new(type, &tmpfs_ops);
    (*vp)->v_data = (void *)node;
    vnode_ref(*vp);
    return 0;
  }

  return -ENOENT;
}

int tmpfs_vnode_readdir(vnode_t *dv, uio_t *uio, void *state) {
  tmpfs_node_t *node = (tmpfs_node_t *)dv->v_data;
  assert(node->type == T_DIR);
  return 0;
}

int tmpfs_vnode_read(vnode_t *v, uio_t *uio) {
  tmpfs_node_t *node = (tmpfs_node_t *)v->v_data;
  assert(node->type == T_REG);

  tmpfs_node_data_t *data = &node->data;
  return uiomove_frombuf(data->buf, data->size, uio);
}

int tmpfs_vnode_write(vnode_t *v, uio_t *uio) {
  tmpfs_node_t *node = (tmpfs_node_t *)v->v_data;
  assert(node->type == T_REG);

  tmpfs_node_data_t *data = &node->data;
  return uiomove_frombuf(data->buf, data->size, uio);
}

int tmpfs_vnode_getattr(vnode_t *v, vattr_t *va) {
  return 0;
}

int tmpfs_vnode_create(vnode_t *dv, const char *name, vnode_t **vp) {
  tmpfs_node_t *dirnode = (tmpfs_node_t *)dv->v_data;
  assert(dirnode->type == T_DIR);
  tmpfs_dirnode_data_t *dirdata = &dirnode->dirdata;

  vnode_t *res = vnode_new(T_REG, &tmpfs_ops);
  *vp = res;

  tmpfs_node_t *node = tmpfs_new_node(T_REG, name);
  tmpfs_dirnode_insert(dirdata, node);

  return 0;
}

int tmpfs_vnode_remove(vnode_t *dv, const char *name) {
  tmpfs_node_t *dirnode = (tmpfs_node_t *)dv->v_data;
  assert(dirnode->type == T_DIR);
  tmpfs_dirnode_data_t *dirdata = &dirnode->dirdata;

  tmpfs_node_t *node = tmpfs_dirnode_find(dirdata, name);
  assert(node->type == T_REG);
  if (!node) {
    tmpfs_dirnode_remove(dirdata, node);
    tmpfs_delete_node(node);
  }

  return 0;
}

int tmpfs_vnode_mkdir(vnode_t *dv, const char *name, vnode_t **vp) {
  tmpfs_node_t *dirnode = (tmpfs_node_t *)dv->v_data;
  assert(dirnode->type == T_DIR);
  tmpfs_dirnode_data_t *dirdata = &dirnode->dirdata;

  vnode_t *res = vnode_new(T_DIR, &tmpfs_ops);
  *vp = res;

  tmpfs_node_t *node = tmpfs_new_node(T_REG, name);
  tmpfs_dirnode_insert(dirdata, node);

  return 0;
}

int tmpfs_vnode_rmdir(vnode_t *dv, const char *name) {
  tmpfs_node_t *dirnode = (tmpfs_node_t *)dv->v_data;
  assert(dirnode->type == T_DIR);
  tmpfs_dirnode_data_t *dirdata = &dirnode->dirdata;

  tmpfs_node_t *node = tmpfs_dirnode_find(dirdata, name);
  assert(node->type == T_DIR);
  if (!node) {
    tmpfs_dirnode_remove(dirdata, node);
    tmpfs_delete_node(node);
  }

  return 0;
}

vnodeops_t tmpfs_ops = {.v_lookup = tmpfs_vnode_lookup,
                        .v_readdir = tmpfs_vnode_readdir,
                        .v_open = vnode_open_generic,
                        .v_read = tmpfs_vnode_read,
                        .v_write = tmpfs_vnode_write,
                        .v_getattr = tmpfs_vnode_getattr,
                        .v_create = tmpfs_vnode_create,
                        .v_remove = tmpfs_vnode_remove,
                        .v_mkdir = tmpfs_vnode_mkdir,
                        .v_rmdir = tmpfs_vnode_rmdir};

static int tmpfs_init(vfsconf_t *vfc) {
  return 0;
}

static int tmpfs_root(mount_t *m, vnode_t **v) {
  *v = (vnode_t *)m->mnt_data;
  return 0;
}

static int tmpfs_mount(mount_t *m) {
  vnode_t *root = vnode_new(V_DIR, &tmpfs_ops);
  root->v_mount = m;

  tmpfs_node_t *node = tmpfs_new_node(T_DIR, "/");
  root->v_data = node;

  return 0;
}
vfsops_t tmpfs_vfsops = {
  .vfs_mount = tmpfs_mount, .vfs_root = tmpfs_root, .vfs_init = tmpfs_init};

vfsconf_t tmpfs_conf = {.vfc_name = "tmpsfs", .vfc_vfsops = &tmpfs_vfsops};

SET_ENTRY(vfsconf, tmpfs_conf);