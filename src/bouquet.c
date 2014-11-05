/*
 *  tvheadend, bouquets
 *  Copyright (C) 2014 Jaroslav Kysela
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "settings.h"
#include "access.h"
#include "bouquet.h"
#include "service.h"
#include "channels.h"
#include "service_mapper.h"

bouquet_tree_t bouquets;

/**
 *
 */
static int
_bq_cmp(const void *a, const void *b)
{
  return strcmp(((bouquet_t *)a)->bq_src ?: "", ((bouquet_t *)b)->bq_src ?: "");
}

/**
 *
 */
bouquet_t *
bouquet_create(const char *uuid, htsmsg_t *conf,
               const char *name, const char *src)
{
  bouquet_t *bq, *bq2;
  int i;

  lock_assert(&global_lock);

  bq = calloc(1, sizeof(bouquet_t));
  bq->bq_services = idnode_set_create();
  bq->bq_active_services = idnode_set_create();

  if (idnode_insert(&bq->bq_id, uuid, &bouquet_class, 0)) {
    if (uuid)
      tvherror("bouquet", "invalid uuid '%s'", uuid);
    free(bq);
    return NULL;
  }

  if (conf) {
    bq->bq_in_load = 1;
    idnode_load(&bq->bq_id, conf);
    bq->bq_in_load = 0;
    if (!htsmsg_get_bool(conf, "shield", &i) && i)
      bq->bq_shield = 1;
  }

  if (name) {
    free(bq->bq_name);
    bq->bq_name = strdup(name);
  }

  if (src) {
    free(bq->bq_src);
    bq->bq_src = strdup(src);
  }

  bq2 = RB_INSERT_SORTED(&bouquets, bq, bq_link, _bq_cmp);
  assert(bq2 == NULL);

  bq->bq_saveflag = 1;

  return bq;
}

/**
 *
 */
static void
bouquet_destroy(bouquet_t *bq)
{
  if (!bq)
    return;

  RB_REMOVE(&bouquets, bq, bq_link);
  idnode_unlink(&bq->bq_id);

  idnode_set_free(bq->bq_active_services);
  idnode_set_free(bq->bq_services);
  assert(bq->bq_services_waiting == NULL);
  free((char *)bq->bq_chtag_waiting);
  free(bq->bq_name);
  free(bq->bq_src);
  free(bq);
}

/**
 *
 */
void
bouquet_destroy_by_service(service_t *t)
{
  bouquet_t *bq;
  service_lcn_t *sl;

  lock_assert(&global_lock);

  RB_FOREACH(bq, &bouquets, bq_link)
    if (idnode_set_exists(bq->bq_services, &t->s_id))
      idnode_set_remove(bq->bq_services, &t->s_id);
  while ((sl = LIST_FIRST(&t->s_lcns)) != NULL) {
    LIST_REMOVE(sl, sl_link);
    free(sl);
  }
}

/**
 *
 */
void
bouquet_destroy_by_channel_tag(channel_tag_t *ct)
{
  bouquet_t *bq;

  lock_assert(&global_lock);

  RB_FOREACH(bq, &bouquets, bq_link)
    if (bq->bq_chtag_ptr == ct)
      bq->bq_chtag_ptr = NULL;
}

/*
 *
 */
bouquet_t *
bouquet_find_by_source(const char *name, const char *src, int create)
{
  bouquet_t *bq;
  bouquet_t bqs;

  assert(src);

  lock_assert(&global_lock);

  bqs.bq_src = (char *)src;
  bq = RB_FIND(&bouquets, &bqs, bq_link, _bq_cmp);
  if (bq) {
    if (name && *name && strcmp(name, bq->bq_name)) {
      tvhwarn("bouquet", "bouquet name '%s' changed to '%s'", bq->bq_name ?: "", name);
      free(bq->bq_name);
      bq->bq_name = strdup(name);
    }
    return bq;
  }
  if (create && name) {
    bq = bouquet_create(NULL, NULL, name, src);
    tvhinfo("bouquet", "new bouquet '%s'", name);
    return bq;
  }
  return NULL;
}

/*
 *
 */
static channel_tag_t *
bouquet_tag(bouquet_t *bq, int create)
{
  channel_tag_t *ct;
  char buf[128];

  assert(!bq->bq_in_load);
  if (bq->bq_chtag_waiting) {
    bq->bq_chtag_ptr = channel_tag_find_by_uuid(bq->bq_chtag_waiting);
    free((char *)bq->bq_chtag_waiting);
    bq->bq_chtag_waiting = NULL;
  }
  if (bq->bq_chtag_ptr)
    return bq->bq_chtag_ptr;
  snprintf(buf, sizeof(buf), "*** %s", bq->bq_name ?: "???");
  ct = channel_tag_find_by_name(buf, create);
  if (ct) {
    bq->bq_chtag_ptr = ct;
    bouquet_save(bq, 0);
  }
  return ct;
}

/*
 *
 */
static int
noname(const char *s)
{
  if (!s)
    return 1;
  while (*s) {
    if (*s > ' ')
      return 0;
    s++;
  }
  return 1;
}

/*
 *
 */
static void
bouquet_map_channel(bouquet_t *bq, service_t *t)
{
  channel_t *ch = NULL;
  channel_service_mapping_t *csm;

  if (!bq->bq_mapradio && service_is_radio(t))
    return;
  if (!bq->bq_mapnolcn &&
      service_get_channel_number(t) <= 0 &&
      bouquet_get_channel_number(bq, t) <= 0)
    return;
  if (!bq->bq_mapnoname && noname(service_get_channel_name(t)))
    return;
  LIST_FOREACH(csm, &t->s_channels, csm_svc_link)
    if (csm->csm_chn->ch_bouquet == bq)
      break;
  if (!csm)
    ch = service_mapper_process(t, bq);
  else
    ch = csm->csm_chn;
  if (ch && bq->bq_chtag)
    channel_tag_map(ch, bouquet_tag(bq, 1));
}

/*
 *
 */
void
bouquet_add_service(bouquet_t *bq, service_t *s, uint32_t lcn)
{
  service_lcn_t *tl;

  lock_assert(&global_lock);

  if (!idnode_set_exists(bq->bq_services, &s->s_id)) {
    tvhtrace("bouquet", "add service %s to %s", s->s_nicename, bq->bq_name ?: "<unknown>");
    idnode_set_add(bq->bq_services, &s->s_id, NULL);

    LIST_FOREACH(tl, &s->s_lcns, sl_link)
      if (tl->sl_bouquet == bq) {
        tl->sl_lcn = lcn;
        break;
      }

    if (!tl) {
      tl = calloc(1, sizeof(*tl));
      tl->sl_bouquet = bq;
      tl->sl_lcn = lcn;
      LIST_INSERT_HEAD(&s->s_lcns, tl, sl_link);
    }
    tl->sl_seen = 1;

    bq->bq_saveflag = 1;
    if (bq->bq_enabled && bq->bq_maptoch)
      bouquet_map_channel(bq, s);
  }
  if (!bq->bq_in_load &&
      !idnode_set_exists(bq->bq_active_services, &s->s_id))
    idnode_set_add(bq->bq_active_services, &s->s_id, NULL);
}

/*
 *
 */
static void
bouquet_unmap_channel(bouquet_t *bq, service_t *t)
{
  channel_service_mapping_t *csm, *csm_next;

  csm = LIST_FIRST(&t->s_channels);
  while (csm) {
    csm_next = LIST_NEXT(csm, csm_svc_link);
    if (csm->csm_chn->ch_bouquet == bq) {
      tvhinfo("bouquet", "%s / %s: unmapped from %s",
              channel_get_name(csm->csm_chn), t->s_nicename,
              bq->bq_name ?: "<unknown>");
      channel_delete(csm->csm_chn, 1);
    }
    csm = csm_next;
  }
}

/*
 *
 */
static void
bouquet_remove_service(bouquet_t *bq, service_t *s)
{
  tvhtrace("bouquet", "remove service %s from %s",
           s->s_nicename, bq->bq_name ?: "<unknown>");
  idnode_set_remove(bq->bq_services, &s->s_id);
}

/*
 *
 */
void
bouquet_completed(bouquet_t *bq)
{
  idnode_set_t *remove;
  service_t *s;
  service_lcn_t *lcn, *lcn_next;
  size_t z;

  if (!bq)
    return;

  tvhtrace("bouquet", "%s: completed: active=%zi old=%zi",
            bq->bq_name ?: "", bq->bq_active_services->is_count,
            bq->bq_services->is_count);

  /* Add/Remove services */
  remove = idnode_set_create();
  for (z = 0; z < bq->bq_services->is_count; z++)
    if (!idnode_set_exists(bq->bq_active_services, bq->bq_services->is_array[z]))
      idnode_set_add(remove, bq->bq_services->is_array[z], NULL);
  for (z = 0; z < remove->is_count; z++)
    bouquet_remove_service(bq, (service_t *)remove->is_array[z]);
  idnode_set_free(remove);

  /* Remove no longer used LCNs */
  for (z = 0; z < bq->bq_services->is_count; z++) {
    s = (service_t *)bq->bq_services->is_array[z];
    for (lcn = LIST_FIRST(&s->s_lcns); lcn; lcn = lcn_next) {
      lcn_next = LIST_NEXT(lcn, sl_link);
      if (lcn->sl_bouquet != bq) continue;
      if (!lcn->sl_seen) {
        LIST_REMOVE(lcn, sl_link);
        free(lcn);
      } else {
        lcn->sl_seen = 0;
      }
    }
  }


  idnode_set_free(bq->bq_active_services);
  bq->bq_active_services = idnode_set_create();

  if (bq->bq_saveflag)
    bouquet_save(bq, 1);
}

/*
 *
 */
void
bouquet_map_to_channels(bouquet_t *bq)
{
  service_t *t;
  size_t z;

  for (z = 0; z < bq->bq_services->is_count; z++) {
    t = (service_t *)bq->bq_services->is_array[z];
    if (bq->bq_enabled && bq->bq_maptoch) {
      bouquet_map_channel(bq, t);
    } else {
      bouquet_unmap_channel(bq, t);
    }
  }
}

/*
 *
 */
void
bouquet_notify_channels(bouquet_t *bq)
{
  channel_service_mapping_t *csm;
  service_t *t;
  size_t z;

  for (z = 0; z < bq->bq_services->is_count; z++) {
    t = (service_t *)bq->bq_services->is_array[z];
    LIST_FOREACH(csm, &t->s_channels, csm_svc_link)
      if (csm->csm_chn->ch_bouquet == bq)
        idnode_notify_simple(&csm->csm_chn->ch_id);
  }
}

/*
 *
 */
uint64_t
bouquet_get_channel_number(bouquet_t *bq, service_t *t)
{
  service_lcn_t *tl;

  LIST_FOREACH(tl, &t->s_lcns, sl_link)
    if (tl->sl_bouquet == bq)
      return (int64_t)tl->sl_lcn * CHANNEL_SPLIT;
  return 0;
}

/**
 *
 */
void
bouquet_save(bouquet_t *bq, int notify)
{
  htsmsg_t *c = htsmsg_create_map();
  idnode_save(&bq->bq_id, c);
  hts_settings_save(c, "bouquet/%s", idnode_uuid_as_str(&bq->bq_id));
  if (bq->bq_shield)
    htsmsg_add_bool(c, "shield", 1);
  htsmsg_destroy(c);
  bq->bq_saveflag = 0;
  if (notify)
    idnode_notify_simple(&bq->bq_id);
}

/* **************************************************************************
 * Class definition
 * **************************************************************************/

static void
bouquet_class_save(idnode_t *self)
{
  bouquet_save((bouquet_t *)self, 0);
}

static void
bouquet_class_delete(idnode_t *self)
{
  bouquet_t *bq = (bouquet_t *)self;

  if (!bq->bq_shield) {
    hts_settings_remove("bouquet/%s", idnode_uuid_as_str(&bq->bq_id));
    bouquet_destroy(bq);
  } else {
    idnode_set_free(bq->bq_services);
    bq->bq_services = idnode_set_create();
    bouquet_save(bq, 1);
  }
}

static const char *
bouquet_class_get_title (idnode_t *self)
{
  bouquet_t *bq = (bouquet_t *)self;

  if (bq->bq_comment && bq->bq_comment[0] != '\0')
    return bq->bq_comment;
  return bq->bq_name ?: "";
}

/* exported for others */
htsmsg_t *
bouquet_class_get_list(void *o)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "bouquet/list");
  htsmsg_add_str(m, "event", "bouquet");
  return m;
}

static void
bouquet_class_enabled_notify ( void *obj )
{
  bouquet_map_to_channels((bouquet_t *)obj);
}

static void
bouquet_class_maptoch_notify ( void *obj )
{
  bouquet_map_to_channels((bouquet_t *)obj);
}

static void
bouquet_class_mapnolcn_notify ( void *obj )
{
  bouquet_t *bq = obj;
  service_t *t;
  size_t z;

  if (bq->bq_in_load)
    return;
  if (!bq->bq_mapnolcn && bq->bq_enabled && bq->bq_maptoch) {
    for (z = 0; z < bq->bq_services->is_count; z++) {
      t = (service_t *)bq->bq_services->is_array[z];
      if (service_get_channel_number(t) <= 0 &&
          bouquet_get_channel_number(bq, t) <= 0)
        bouquet_unmap_channel(bq, t);
    }
  } else {
    bouquet_map_to_channels(bq);
  }
}

static void
bouquet_class_mapnoname_notify ( void *obj )
{
  bouquet_t *bq = obj;
  service_t *t;
  size_t z;

  if (bq->bq_in_load)
    return;
  if (!bq->bq_mapnoname && bq->bq_enabled && bq->bq_maptoch) {
    for (z = 0; z < bq->bq_services->is_count; z++) {
      t = (service_t *)bq->bq_services->is_array[z];
      if (noname(service_get_channel_name(t)))
        bouquet_unmap_channel(bq, t);
    }
  } else {
    bouquet_map_to_channels(bq);
  }
}

static void
bouquet_class_mapradio_notify ( void *obj )
{
  bouquet_t *bq = obj;
  service_t *t;
  size_t z;

  if (bq->bq_in_load)
    return;
  if (!bq->bq_mapradio && bq->bq_enabled && bq->bq_maptoch) {
    for (z = 0; z < bq->bq_services->is_count; z++) {
      t = (service_t *)bq->bq_services->is_array[z];
      if (service_is_radio(t))
        bouquet_unmap_channel(bq, t);
    }
  } else {
    bouquet_map_to_channels(bq);
  }
}

static void
bouquet_class_chtag_notify ( void *obj )
{
  bouquet_t *bq = obj;
  service_t *t;
  channel_service_mapping_t *csm;
  channel_tag_t *ct;
  size_t z;

  if (bq->bq_in_load)
    return;
  if (!bq->bq_chtag && bq->bq_enabled && bq->bq_maptoch) {
    ct = bouquet_tag(bq, 0);
    if (!ct)
      return;
    for (z = 0; z < bq->bq_services->is_count; z++) {
      t = (service_t *)bq->bq_services->is_array[z];
      LIST_FOREACH(csm, &t->s_channels, csm_svc_link)
        if (csm->csm_chn->ch_bouquet == bq)
          break;
      if (csm)
        channel_tag_unmap(csm->csm_chn, ct);
    }
  } else {
    bouquet_map_to_channels(bq);
  }
}

static void
bouquet_class_lcn_offset_notify ( void *obj )
{
  if (((bouquet_t *)obj)->bq_in_load)
    return;
  bouquet_notify_channels((bouquet_t *)obj);
}

static const void *
bouquet_class_chtag_ref_get ( void *obj )
{
  static const char *buf;
  bouquet_t *bq = obj;

  if (bq->bq_chtag_ptr)
    buf = idnode_uuid_as_str(&bq->bq_chtag_ptr->ct_id);
  else
    buf = "";
  return &buf;
}

static char *
bouquet_class_chtag_ref_rend ( void *obj )
{
  bouquet_t *bq = obj;
  if (bq->bq_chtag_ptr)
    return strdup(bq->bq_chtag_ptr->ct_name ?: "");
  else
    return strdup("");
}

static int
bouquet_class_chtag_ref_set ( void *obj, const void *p )
{
  bouquet_t *bq = obj;

  free((char *)bq->bq_chtag_waiting);
  bq->bq_chtag_waiting = NULL;
  if (bq->bq_in_load)
    bq->bq_chtag_waiting = strdup((const char *)p);
  return 0;
}

static const void *
bouquet_class_services_get ( void *obj )
{
  htsmsg_t *m = htsmsg_create_map();
  bouquet_t *bq = obj;
  service_t *t;
  size_t z;

  /* Add all */
  for (z = 0; z < bq->bq_services->is_count; z++) {
    t = (service_t *)bq->bq_services->is_array[z];
    htsmsg_add_u32(m, idnode_uuid_as_str(&t->s_id),
                   bouquet_get_channel_number(bq, t));
  }

  return m;
}

static char *
bouquet_class_services_rend ( void *obj )
{
  bouquet_t *bq = obj;
  char buf[32];
  snprintf(buf, sizeof(buf), "Services Count %zi", bq->bq_services->is_count);
  return strdup(buf);
}

static int
bouquet_class_services_set ( void *obj, const void *p )
{
  bouquet_t *bq = obj;

  if (bq->bq_services_waiting)
    htsmsg_destroy(bq->bq_services_waiting);
  bq->bq_services_waiting = NULL;
  if (bq->bq_in_load)
    bq->bq_services_waiting = htsmsg_copy((htsmsg_t *)p);
  return 0;
}

static const void *
bouquet_class_services_count_get ( void *obj )
{
  static uint32_t u32;
  bouquet_t *bq = obj;

  u32 = bq->bq_services->is_count;
  return &u32;
}

const idclass_t bouquet_class = {
  .ic_class      = "bouquet",
  .ic_caption    = "Bouquet",
  .ic_event      = "bouquet",
  .ic_perm_def   = ACCESS_ADMIN,
  .ic_save       = bouquet_class_save,
  .ic_get_title  = bouquet_class_get_title,
  .ic_delete     = bouquet_class_delete,
  .ic_properties = (const property_t[]){
    {
      .type     = PT_BOOL,
      .id       = "enabled",
      .name     = "Enabled",
      .off      = offsetof(bouquet_t, bq_enabled),
      .notify   = bouquet_class_enabled_notify,
    },
    {
      .type     = PT_BOOL,
      .id       = "maptoch",
      .name     = "Auto-Map to Channels",
      .off      = offsetof(bouquet_t, bq_maptoch),
      .notify   = bouquet_class_maptoch_notify,
    },
    {
      .type     = PT_BOOL,
      .id       = "mapnolcn",
      .name     = "Map Zero Numbers",
      .off      = offsetof(bouquet_t, bq_mapnolcn),
      .notify   = bouquet_class_mapnolcn_notify,
    },
    {
      .type     = PT_BOOL,
      .id       = "mapnoname",
      .name     = "Map No Name",
      .off      = offsetof(bouquet_t, bq_mapnoname),
      .notify   = bouquet_class_mapnoname_notify,
    },
    {
      .type     = PT_BOOL,
      .id       = "mapradio",
      .name     = "Map Radio",
      .off      = offsetof(bouquet_t, bq_mapradio),
      .notify   = bouquet_class_mapradio_notify,
    },
    {
      .type     = PT_BOOL,
      .id       = "chtag",
      .name     = "Create Tag",
      .off      = offsetof(bouquet_t, bq_chtag),
      .notify   = bouquet_class_chtag_notify,
    },
    {
      .type     = PT_STR,
      .id       = "chtag_ref",
      .name     = "Channel Tag Reference",
      .get      = bouquet_class_chtag_ref_get,
      .set      = bouquet_class_chtag_ref_set,
      .rend     = bouquet_class_chtag_ref_rend,
      .opts     = PO_RDONLY | PO_HIDDEN,
    },
    {
      .type     = PT_STR,
      .id       = "name",
      .name     = "Name",
      .off      = offsetof(bouquet_t, bq_name),
    },
    {
      .type     = PT_STR,
      .id       = "source",
      .name     = "Source",
      .off      = offsetof(bouquet_t, bq_src),
      .opts     = PO_RDONLY,
    },
    {
      .type     = PT_STR,
      .islist   = 1,
      .id       = "services",
      .name     = "Services",
      .get      = bouquet_class_services_get,
      .set      = bouquet_class_services_set,
      .rend     = bouquet_class_services_rend,
      .opts     = PO_RDONLY | PO_HIDDEN,
    },
    {
      .type     = PT_U32,
      .id       = "services_count",
      .name     = "# Services",
      .get      = bouquet_class_services_count_get,
      .opts     = PO_RDONLY | PO_NOSAVE,
    },
    {
      .type     = PT_STR,
      .id       = "comment",
      .name     = "Comment",
      .off      = offsetof(bouquet_t, bq_comment),
    },
    {
      .type     = PT_U32,
      .id       = "lcn_off",
      .name     = "Channel Number Offset",
      .off      = offsetof(bouquet_t, bq_lcn_offset),
      .notify   = bouquet_class_lcn_offset_notify,
    },
    {}
  }
};

/**
 *
 */
void
bouquet_init(void)
{
  htsmsg_t *c, *m;
  htsmsg_field_t *f;
  bouquet_t *bq;

  RB_INIT(&bouquets);

  /* Load */
  if ((c = hts_settings_load("bouquet")) != NULL) {
    HTSMSG_FOREACH(f, c) {
      if (!(m = htsmsg_field_get_map(f))) continue;
      bq = bouquet_create(f->hmf_name, m, NULL, NULL);
      bq->bq_saveflag = 0;
    }
    htsmsg_destroy(c);
  }
}

void
bouquet_service_resolve(void)
{
  bouquet_t *bq;
  htsmsg_field_t *f;
  service_t *s;
  uint32_t lcn;
  int saveflag;

  lock_assert(&global_lock);

  RB_FOREACH(bq, &bouquets, bq_link)  {
    if (!bq->bq_services_waiting)
      continue;
    saveflag = bq->bq_saveflag;
    HTSMSG_FOREACH(f, bq->bq_services_waiting) {
      if (htsmsg_field_get_u32(f, &lcn)) continue;
      s = service_find_by_identifier(f->hmf_name);
      if (s)
        bouquet_add_service(bq, s, lcn);
    }
    htsmsg_destroy(bq->bq_services_waiting);
    bq->bq_services_waiting = NULL;
    bq->bq_saveflag = saveflag;
  }
}

void
bouquet_done(void)
{
  bouquet_t *bq;

  pthread_mutex_lock(&global_lock);
  while ((bq = RB_FIRST(&bouquets)) != NULL)
    bouquet_destroy(bq);
  pthread_mutex_unlock(&global_lock);
}
