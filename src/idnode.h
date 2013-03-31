#pragma once

#include "tvheadend.h"
#include "prop.h"

struct htsmsg;
struct idnode;

typedef struct idclass {
  const struct idclass *ic_super;
  const char *ic_class;
  const char *ic_caption;
  struct htsmsg *(*ic_serialize)(struct idnode *self);
  struct idnode **(*ic_get_childs)(struct idnode *self);
  const char *(*ic_get_title)(struct idnode *self);
  void (*ic_save)(struct idnode *self);
  const property_t *ic_properties;
} idclass_t;


typedef struct idnode {
  uint8_t in_uuid[16];
  RB_ENTRY(idnode) in_link;
  const idclass_t *in_class;
} idnode_t;

void idnode_init(void);

int idnode_insert(idnode_t *in, const char *uuid, const idclass_t *class);

const char *idnode_uuid_as_str(const idnode_t *in);

void *idnode_find(const char *uuid, const idclass_t *class);

void idnode_unlink(idnode_t *in);

htsmsg_t *idnode_serialize(struct idnode *self);

void idnode_set_prop(idnode_t *in, const char *key, const char *value);

void idnode_update_all_props(idnode_t *in,
                             const char *(*getvalue)(void *opaque,
                                                     const char *key),
                             void *opaque);
