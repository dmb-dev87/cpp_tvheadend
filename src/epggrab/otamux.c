/*
 *  Electronic Program Guide - EPG grabber OTA functions
 *  Copyright (C) 2012 Adam Sutton
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
#include "queue.h"
#include "settings.h"
#include "epg.h"
#include "epggrab.h"
#include "epggrab/private.h"
#include "input.h"
#include "subscriptions.h"
#include "cron.h"

#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define EPGGRAB_OTA_MIN_TIMEOUT     30
#define EPGGRAB_OTA_MAX_TIMEOUT   7200

#define EPGGRAB_OTA_DONE_COMPLETE    0
#define EPGGRAB_OTA_DONE_TIMEOUT     1
#define EPGGRAB_OTA_DONE_STOLEN      2

typedef TAILQ_HEAD(epggrab_ota_head,epggrab_ota_mux) epggrab_ota_head_t;

uint32_t                     epggrab_ota_initial;
char                        *epggrab_ota_cron;
cron_multi_t                *epggrab_ota_cron_multi;
uint32_t                     epggrab_ota_timeout;

RB_HEAD(,epggrab_ota_mux)    epggrab_ota_all;
epggrab_ota_head_t           epggrab_ota_pending;
epggrab_ota_head_t           epggrab_ota_active;

gtimer_t                     epggrab_ota_kick_timer;
gtimer_t                     epggrab_ota_start_timer;

int                          epggrab_ota_pending_flag;

pthread_mutex_t              epggrab_ota_mutex;

SKEL_DECLARE(epggrab_ota_mux_skel, epggrab_ota_mux_t);
SKEL_DECLARE(epggrab_svc_link_skel, epggrab_ota_svc_link_t);

static void epggrab_ota_timeout_cb ( void *p );
static void epggrab_ota_kick_cb ( void *p );

static void epggrab_ota_save ( epggrab_ota_mux_t *ota );

static void epggrab_ota_free ( epggrab_ota_head_t *head, epggrab_ota_mux_t *ota );

/* **************************************************************************
 * Utilities
 * *************************************************************************/

static int
om_id_cmp   ( epggrab_ota_mux_t *a, epggrab_ota_mux_t *b )
{
  return strcmp(a->om_mux_uuid, b->om_mux_uuid);
}

static int
om_svcl_cmp ( epggrab_ota_svc_link_t *a, epggrab_ota_svc_link_t *b )
{
  return strcmp(a->uuid, b->uuid);
}

static int
epggrab_ota_timeout_get ( void )
{
  int timeout = epggrab_ota_timeout;

  if (timeout < EPGGRAB_OTA_MIN_TIMEOUT)
    timeout = EPGGRAB_OTA_MIN_TIMEOUT;
  if (timeout > EPGGRAB_OTA_MAX_TIMEOUT)
    timeout = EPGGRAB_OTA_MAX_TIMEOUT;

  return timeout;
}

static void
epggrab_ota_kick ( int delay )
{
  epggrab_ota_mux_t *om;

  if (TAILQ_EMPTY(&epggrab_ota_pending) &&
      TAILQ_EMPTY(&epggrab_ota_active)) {
    /* next round is pending? queue all ota muxes */
    if (epggrab_ota_pending_flag) {
      epggrab_ota_pending_flag = 0;
      RB_FOREACH(om, &epggrab_ota_all, om_global_link)
        TAILQ_INSERT_TAIL(&epggrab_ota_pending, om, om_q_link);
    } else {
      return;
    }
  }
  gtimer_arm(&epggrab_ota_kick_timer, epggrab_ota_kick_cb, NULL, delay);
}

static void
epggrab_ota_done ( epggrab_ota_mux_t *om, int reason )
{
  mpegts_mux_t *mm;

  gtimer_disarm(&om->om_timer);

  TAILQ_REMOVE(&epggrab_ota_active, om, om_q_link);
  if (reason == EPGGRAB_OTA_DONE_STOLEN)
    TAILQ_INSERT_HEAD(&epggrab_ota_pending, om, om_q_link);
  else if (reason == EPGGRAB_OTA_DONE_TIMEOUT) {
    char name[256];
    mpegts_mux_t *mm = mpegts_mux_find(om->om_mux_uuid);
    mm->mm_display_name(mm, name, sizeof(name));
    tvhlog(LOG_WARNING, "epggrab", "data completion timeout for %s", name);
  }

  /* Remove subscriber */
  if ((mm = mpegts_mux_find(om->om_mux_uuid)))
    mpegts_mux_unsubscribe_by_name(mm, "epggrab");

  /* Kick - try start waiting muxes */
  epggrab_ota_kick(1);
}

static void
epggrab_ota_start ( epggrab_ota_mux_t *om, mpegts_mux_t *mm, int grace,
                    const char *modname )
{
  epggrab_module_t  *m;
  epggrab_ota_map_t *map;

  TAILQ_INSERT_TAIL(&epggrab_ota_active, om, om_q_link);
  gtimer_arm(&om->om_timer, epggrab_ota_timeout_cb, om,
             epggrab_ota_timeout_get() + grace);
  if (modname) {
    LIST_FOREACH(m, &epggrab_modules, link)
      if (!strcmp(m->id, modname)) {
        epggrab_ota_register((epggrab_module_ota_t *)m, om, mm);
        break;
      }
  }
  LIST_FOREACH(map, &om->om_modules, om_link) {
    map->om_first    = 1;
    map->om_forced   = 0;
    if (modname && !strcmp(modname, map->om_module->id))
      map->om_forced = 1;
    map->om_complete = 0;
    tvhdebug(map->om_module->id, "grab started");
  }
}

/* **************************************************************************
 * MPEG-TS listener
 * *************************************************************************/

static void
epggrab_mux_start ( mpegts_mux_t *mm, void *p )
{
  epggrab_module_t  *m;
  epggrab_ota_map_t *map;
  epggrab_ota_mux_t *ota, ota_skel;
  const char *uuid = idnode_uuid_as_str(&mm->mm_id);

  /* Already started */
  TAILQ_FOREACH(ota, &epggrab_ota_active, om_q_link)
    if (!strcmp(ota->om_mux_uuid, uuid))
      return;

  /* Find the configuration */
  ota_skel.om_mux_uuid = (char *)uuid;
  ota = RB_FIND(&epggrab_ota_all, &ota_skel, om_global_link, om_id_cmp);
  if (!ota)
    return;

  /* Register all modules */
  LIST_FOREACH(m, &epggrab_modules, link) {
    if (m->type == EPGGRAB_OTA && m->enabled)
      epggrab_ota_register((epggrab_module_ota_t *)m, ota, mm);
  }

  /* Check if already active */
  LIST_FOREACH(map, &ota->om_modules, om_link)
    map->om_module->start(map, mm);
}

static void
epggrab_mux_stop ( mpegts_mux_t *mm, void *p )
{
  epggrab_ota_mux_t *ota;
  const char *uuid = idnode_uuid_as_str(&mm->mm_id);

  TAILQ_FOREACH(ota, &epggrab_ota_active, om_q_link)
    if (!strcmp(ota->om_mux_uuid, uuid)) {
      epggrab_ota_done(ota, EPGGRAB_OTA_DONE_STOLEN);
      break;
    }
}

/* **************************************************************************
 * Module methods
 * *************************************************************************/

epggrab_ota_mux_t *
epggrab_ota_register
  ( epggrab_module_ota_t *mod, epggrab_ota_mux_t *ota, mpegts_mux_t *mm )
{
  int save = 0;
  epggrab_ota_map_t *map;

  if (ota == NULL) {
    /* Find mux entry */
    const char *uuid = idnode_uuid_as_str(&mm->mm_id);
    SKEL_ALLOC(epggrab_ota_mux_skel);
    epggrab_ota_mux_skel->om_mux_uuid = (char*)uuid;

    ota = RB_INSERT_SORTED(&epggrab_ota_all, epggrab_ota_mux_skel, om_global_link, om_id_cmp);
    if (!ota) {
      char buf[256];
      mm->mm_display_name(mm, buf, sizeof(buf));
      tvhinfo(mod->id, "registering mux %s", buf);
      ota  = epggrab_ota_mux_skel;
      SKEL_USED(epggrab_ota_mux_skel);
      ota->om_mux_uuid = strdup(uuid);
      TAILQ_INSERT_TAIL(&epggrab_ota_pending, ota, om_q_link);
      if (TAILQ_FIRST(&epggrab_ota_pending) == ota)
        epggrab_ota_kick(1);
      save = 1;
    }
  }
  
  /* Find module entry */
  LIST_FOREACH(map, &ota->om_modules, om_link)
    if (map->om_module == mod)
      break;
  if (!map) {
    map = calloc(1, sizeof(epggrab_ota_map_t));
    RB_INIT(&map->om_svcs);
    map->om_module   = mod;
    LIST_INSERT_HEAD(&ota->om_modules, map, om_link);
    save = 1;
  }

  /* Save config */
  if (save) epggrab_ota_save(ota);

  return ota;
}

void
epggrab_ota_complete
  ( epggrab_module_ota_t *mod, epggrab_ota_mux_t *ota )
{
  int done = 1;
  epggrab_ota_mux_t *ota2;
  epggrab_ota_map_t *map;
  lock_assert(&global_lock);
  tvhdebug(mod->id, "grab complete");

  /* Mark */
  if (!ota->om_complete) {
    ota->om_complete = 1;
    epggrab_ota_save(ota);
  }

  /* Test for completion */
  LIST_FOREACH(map, &ota->om_modules, om_link) {
    if (map->om_module == mod) {
      map->om_complete = 1;
    } else if (!map->om_complete) {
      done = 0;
    }
  }
  if (!done) return;

  /* Done */
  TAILQ_FOREACH(ota2, &epggrab_ota_active, om_q_link)
    if (ota == ota2) {
      epggrab_ota_done(ota, EPGGRAB_OTA_DONE_COMPLETE);
      break;
    }
}

/* **************************************************************************
 * Timer callbacks
 * *************************************************************************/

static void
epggrab_ota_timeout_cb ( void *p )
{
  epggrab_ota_mux_t *om = p;

  lock_assert(&global_lock);

  if (!om)
    return;

  /* Re-queue */
  epggrab_ota_done(om, EPGGRAB_OTA_DONE_TIMEOUT);
}

static void
epggrab_ota_kick_cb ( void *p )
{
  extern const idclass_t mpegts_mux_class;
  epggrab_ota_map_t *map;
  epggrab_ota_mux_t *om = TAILQ_FIRST(&epggrab_ota_pending);
  epggrab_ota_mux_t *first = NULL;
  mpegts_mux_t *mm;
  struct {
    mpegts_network_t *net;
    int failed;
  } networks[64], *net;	/* more than 64 networks? - you're a king */
  int i, r, networks_count = 0, epg_flag;
  const char *modname;
  static const char *modnames[] = {
    [MM_EPG_DISABLE]                 = NULL,
    [MM_EPG_ENABLE]                  = NULL,
    [MM_EPG_FORCE]                   = NULL,
    [MM_EPG_FORCE_EIT]               = "eit",
    [MM_EPG_FORCE_UK_FREESAT]        = "uk_freesat",
    [MM_EPG_FORCE_UK_FREEVIEW]       = "uk_freeview",
    [MM_EPG_FORCE_VIASAT_BALTIC]     = "viasat_baltic",
    [MM_EPG_FORCE_OPENTV_SKY_UK]     = "opentv-skyuk",
    [MM_EPG_FORCE_OPENTV_SKY_ITALIA] = "opentv-skyit",
    [MM_EPG_FORCE_OPENTV_SKY_AUSAT]  = "opentv-ausat",
  };

  lock_assert(&global_lock);

  if (!om)
    return;

next_one:
  /* Find the mux */
  mm = mpegts_mux_find(om->om_mux_uuid);
  if (!mm) {
    epggrab_ota_free(&epggrab_ota_pending, om);
    goto done;
  }

  TAILQ_REMOVE(&epggrab_ota_pending, om, om_q_link);

  /* Check if this network failed before */
  for (i = 0, net = NULL; i < networks_count; i++) {
    net = &networks[i];
    if (net->net == mm->mm_network) {
      if (net->failed)
        goto done;
      break;
    }
  }
  if (i >= networks_count) {
    net = &networks[networks_count++];
    net->net = mm->mm_network;
    net->failed = 0;
  }

  epg_flag = mm->mm_is_epg(mm);
  if (ARRAY_SIZE(modnames) >= epg_flag)
    epg_flag = MM_EPG_ENABLE;
  modname  = modnames[epg_flag];

  if (epg_flag < 0 || epg_flag == MM_EPG_DISABLE) {
#if TRACE_ENABLE
    char name[256];
    mm->mm_display_name(mm, name, sizeof(name));
    tvhtrace("epggrab", "epg mux %s is disabled, skipping", name);
#endif
    goto done;
  }

  if (epg_flag != MM_EPG_FORCE) {
    /* Check we have modules attached and enabled */
    LIST_FOREACH(map, &om->om_modules, om_link) {
      if (map->om_module->tune(map, om, mm))
        break;
    }
    if (!map) {
      char name[256];
      mm->mm_display_name(mm, name, sizeof(name));
      tvhdebug("epggrab", "no modules attached to %s, check again next time", name);
      goto done;
    }
  }

  /* Subscribe to the mux */
  if ((r = mpegts_mux_subscribe(mm, "epggrab", SUBSCRIPTION_PRIO_EPG))) {
    TAILQ_INSERT_TAIL(&epggrab_ota_pending, om, om_q_link);
    if (r == SM_CODE_NO_FREE_ADAPTER)
      net->failed = 1;
    if (first == NULL)
      first = om;
  } else {
    mpegts_mux_instance_t *mmi = mm->mm_active;
    epggrab_ota_start(om, mm, mpegts_input_grace(mmi->mmi_input, mm), modname);
  }

done:
  om = TAILQ_FIRST(&epggrab_ota_pending);
  if (networks_count < ARRAY_SIZE(networks) && om && first != om)
    goto next_one;
}

/*
 * Start times management
 */

static void
epggrab_ota_start_cb ( void *p )
{
  time_t next;

  tvhtrace("epggrab", "ota start callback");

  epggrab_ota_pending_flag = 1;

  /* Finish previous job? */
  if (TAILQ_EMPTY(&epggrab_ota_pending) &&
      TAILQ_EMPTY(&epggrab_ota_active)) {
    tvhtrace("epggrab", "ota - idle - kicked");
    epggrab_ota_kick(1);
  }

  pthread_mutex_lock(&epggrab_ota_mutex);
  if (!cron_multi_next(epggrab_ota_cron_multi, dispatch_clock, &next)) {
    tvhtrace("epggrab", "next ota start event in %li seconds", next - time(NULL));
    gtimer_arm_abs(&epggrab_ota_start_timer, epggrab_ota_start_cb, NULL, next);
  }
  pthread_mutex_unlock(&epggrab_ota_mutex);
}

static void
epggrab_ota_arm ( time_t last )
{
  time_t next;

  pthread_mutex_lock(&epggrab_ota_mutex);

  if (!cron_multi_next(epggrab_ota_cron_multi, time(NULL), &next)) {
    /* do not trigger the next EPG scan for 1/2 hour */
    if (last != (time_t)-1 && last + 1800 > next)
      next = last + 1800;
    tvhtrace("epggrab", "next ota start event in %li seconds", next - time(NULL));
    gtimer_arm_abs(&epggrab_ota_start_timer, epggrab_ota_start_cb, NULL, next);
  }
  pthread_mutex_unlock(&epggrab_ota_mutex);
}

/*
 * Service management
 */

void
epggrab_ota_service_add ( epggrab_ota_map_t *map, epggrab_ota_mux_t *ota,
                          const char *uuid, int save )
{
  epggrab_ota_svc_link_t *svcl;

  if (uuid == NULL)
    return;
  SKEL_ALLOC(epggrab_svc_link_skel);
  epggrab_svc_link_skel->uuid = (char *)uuid;
  svcl = RB_INSERT_SORTED(&map->om_svcs, epggrab_svc_link_skel, link, om_svcl_cmp);
  if (svcl == NULL) {
    svcl = epggrab_svc_link_skel;
    SKEL_USED(epggrab_svc_link_skel);
    svcl->uuid = strdup(uuid);
    if (save && ota->om_complete)
      epggrab_ota_save(ota);
  }
}

void
epggrab_ota_service_del ( epggrab_ota_map_t *map, epggrab_ota_mux_t *ota,
                          epggrab_ota_svc_link_t *svcl, int save )
{
  if (svcl == NULL)
    return;
  RB_REMOVE(&map->om_svcs, svcl, link);
  free(svcl->uuid);
  free(svcl);
  if (save)
    epggrab_ota_save(ota);
}

/* **************************************************************************
 * Config
 * *************************************************************************/

static void
epggrab_ota_save ( epggrab_ota_mux_t *ota )
{
  epggrab_ota_map_t *map;
  epggrab_ota_svc_link_t *svcl;
  htsmsg_t *e, *l, *l2, *c = htsmsg_create_map();

  htsmsg_add_u32(c, "complete", ota->om_complete);
  l = htsmsg_create_list();
  LIST_FOREACH(map, &ota->om_modules, om_link) {
    e = htsmsg_create_map();
    htsmsg_add_str(e, "id", map->om_module->id);
    l2 = htsmsg_create_list();
    RB_FOREACH(svcl, &map->om_svcs, link)
      if (svcl->uuid)
        htsmsg_add_str(l2, NULL, svcl->uuid);
    htsmsg_add_msg(e, "services", l2);
    htsmsg_add_msg(l, NULL, e);
  }
  htsmsg_add_msg(c, "modules", l);
  hts_settings_save(c, "epggrab/otamux/%s", ota->om_mux_uuid);
  htsmsg_destroy(c);
}

static void
epggrab_ota_load_one
  ( const char *uuid, htsmsg_t *c )
{
  htsmsg_t *l, *l2, *e;
  htsmsg_field_t *f, *f2;
  mpegts_mux_t *mm;
  epggrab_module_ota_t *mod;
  epggrab_ota_mux_t *ota;
  epggrab_ota_map_t *map;
  const char *id;
  
  mm = mpegts_mux_find(uuid);
  if (!mm) {
    hts_settings_remove("epggrab/otamux/%s", uuid);
    return;
  }

  ota = calloc(1, sizeof(epggrab_ota_mux_t));
  ota->om_mux_uuid = strdup(uuid);
  if (RB_INSERT_SORTED(&epggrab_ota_all, ota, om_global_link, om_id_cmp)) {
    free(ota->om_mux_uuid);
    free(ota);
    return;
  }
  
  if (!(l = htsmsg_get_list(c, "modules"))) return;
  HTSMSG_FOREACH(f, l) {
    if (!(e   = htsmsg_field_get_map(f))) continue;
    if (!(id  = htsmsg_get_str(e, "id"))) continue;
    if (!(mod = (epggrab_module_ota_t*)epggrab_module_find_by_id(id)))
      continue;
    
    map = calloc(1, sizeof(epggrab_ota_map_t));
    RB_INIT(&map->om_svcs);
    map->om_module   = mod;
    if ((l2 = htsmsg_get_list(e, "services")) != NULL) {
      HTSMSG_FOREACH(f2, l2)
        epggrab_ota_service_add(map, ota, htsmsg_field_get_str(f2), 0);
    }
    LIST_INSERT_HEAD(&ota->om_modules, map, om_link);
  }
}

void
epggrab_ota_init ( void )
{
  htsmsg_t *c, *m;
  htsmsg_field_t *f;
  char path[1024];
  struct stat st;

  epggrab_ota_initial      = 1;
  epggrab_ota_timeout      = 600;
  epggrab_ota_cron         = strdup("# Default config (02:04 and 14:04 everyday)\n4 2 * * *\n4 14 * * *");;
  epggrab_ota_cron_multi   = NULL;
  epggrab_ota_pending_flag = 0;

  RB_INIT(&epggrab_ota_all);
  TAILQ_INIT(&epggrab_ota_pending);
  TAILQ_INIT(&epggrab_ota_active);

  pthread_mutex_init(&epggrab_ota_mutex, NULL);

  /* Add listener */
  static mpegts_listener_t ml = {
    .ml_mux_start = epggrab_mux_start,
    .ml_mux_stop  = epggrab_mux_stop,
  };
  mpegts_add_listener(&ml);

  /* Delete old config */
  hts_settings_buildpath(path, sizeof(path), "epggrab/otamux");
  if (!lstat(path, &st))
    if (!S_ISDIR(st.st_mode))
      hts_settings_remove("epggrab/otamux");
  
  /* Load config */
  if ((c = hts_settings_load_r(1, "epggrab/otamux"))) {
    HTSMSG_FOREACH(f, c) {
      if (!(m  = htsmsg_field_get_map(f))) continue;
      epggrab_ota_load_one(f->hmf_name, m); 
    }
    htsmsg_destroy(c);
  }
}

void
epggrab_ota_post ( void )
{
  time_t t = (time_t)-1;

  /* Init timer (call after full init - wait for network tuners) */
  if (epggrab_ota_initial) {
    epggrab_ota_pending_flag = 1;
    epggrab_ota_kick(15);
    t = time(NULL);
  }

  /* arm the first scheduled time */
  epggrab_ota_arm(t);
}

static void
epggrab_ota_free ( epggrab_ota_head_t *head, epggrab_ota_mux_t *ota  )
{
  epggrab_ota_map_t *map;
  epggrab_ota_svc_link_t *svcl;

  TAILQ_REMOVE(head, ota, om_q_link);
  while ((map = LIST_FIRST(&ota->om_modules)) != NULL) {
    LIST_REMOVE(map, om_link);
    while ((svcl = RB_FIRST(&map->om_svcs)) != NULL)
      epggrab_ota_service_del(map, ota, svcl, 0);
    free(map);
  }
  free(ota->om_mux_uuid);
  free(ota);
}

void
epggrab_ota_shutdown ( void )
{
  epggrab_ota_mux_t *ota;

  pthread_mutex_lock(&global_lock);
  while ((ota = TAILQ_FIRST(&epggrab_ota_active)) != NULL)
    epggrab_ota_free(&epggrab_ota_active, ota);
  while ((ota = TAILQ_FIRST(&epggrab_ota_pending)) != NULL)
    epggrab_ota_free(&epggrab_ota_pending, ota);
  pthread_mutex_unlock(&global_lock);
  SKEL_FREE(epggrab_ota_mux_skel);
  SKEL_FREE(epggrab_svc_link_skel);
  free(epggrab_ota_cron);
  epggrab_ota_cron = NULL;
  free(epggrab_ota_cron_multi);
  epggrab_ota_cron_multi = NULL;
}

/*
 *  Global configuration handlers
 */

int
epggrab_ota_set_cron ( const char *cron, int lock )
{
  int save = 0;
  if ( epggrab_ota_cron == NULL || strcmp(epggrab_ota_cron, cron) ) {
    save = 1;
    pthread_mutex_lock(&epggrab_ota_mutex);
    free(epggrab_ota_cron);
    epggrab_ota_cron       = strdup(cron);
    free(epggrab_ota_cron_multi);
    epggrab_ota_cron_multi = cron_multi_set(cron);
    pthread_mutex_unlock(&epggrab_ota_mutex);
    if (lock) {
      pthread_mutex_lock(&global_lock);
      epggrab_ota_arm((time_t)-1);
      pthread_mutex_unlock(&global_lock);
    } else {
      epggrab_ota_arm((time_t)-1);
    }
  }
  return save;
}

int
epggrab_ota_set_timeout( uint32_t e )
{
  int save = 0;
  if (epggrab_ota_timeout != e) {
    epggrab_ota_timeout = e;
    save = 1;
  }
  return save;
}

int
epggrab_ota_set_initial( uint32_t e )
{
  int save = 0;
  if (epggrab_ota_initial != e) {
    epggrab_ota_initial = e;
    save = 1;
  }
  return save;
}

/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
