/*
 *  Digital Video Recorder
 *  Copyright (C) 2008 Andreas Öman
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

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <string.h>

#include "settings.h"

#include "tvheadend.h"
#include "dvr.h"
#include "htsp_server.h"
#include "streaming.h"
#include "intlconv.h"
#include "dbus.h"
#include "imagecache.h"
#include "access.h"

int dvr_iov_max;

struct dvr_config_list dvrconfigs;
static dvr_config_t *dvrdefaultconfig = NULL;

static void dvr_config_destroy(dvr_config_t *cfg, int delconf);

/**
 * find a dvr config by name, return NULL if not found
 */
dvr_config_t *
dvr_config_find_by_name(const char *name)
{
  dvr_config_t *cfg;

  if (name == NULL)
    name = "";

  LIST_FOREACH(cfg, &dvrconfigs, config_link)
    if (!strcmp(name, cfg->dvr_config_name))
      return cfg;

  return NULL;
}

/**
 * find a dvr config by name, return the default config if not found
 */
dvr_config_t *
dvr_config_find_by_name_default(const char *name)
{
  dvr_config_t *cfg;

  if (dvrdefaultconfig == NULL)
    dvrdefaultconfig = dvr_config_find_by_name(NULL);

  if (LIST_FIRST(&dvrconfigs) == NULL) {
    cfg = dvr_config_create("", NULL, NULL);
    assert(cfg);
    dvr_config_save(cfg);
    dvrdefaultconfig = cfg;
  }

  if (name == NULL || *name == '\0')
    return dvrdefaultconfig;

  cfg = dvr_config_find_by_name(name);

  if (cfg == NULL) {
    if (name && *name)
      tvhlog(LOG_WARNING, "dvr", "Configuration '%s' not found, using default", name);
    cfg = dvrdefaultconfig;
  } else if (!cfg->dvr_enabled) {
    tvhlog(LOG_WARNING, "dvr", "Configuration '%s' not enabled, using default", name);
    cfg = dvrdefaultconfig;
  }

  return cfg;
}

/**
 *
 */
static int
dvr_charset_update(dvr_config_t *cfg, const char *charset)
{
  const char *s, *id;
  int change = strcmp(cfg->dvr_charset ?: "", charset ?: "");

  free(cfg->dvr_charset);
  free(cfg->dvr_charset_id);
  s = charset ? charset : intlconv_filesystem_charset();
  id = intlconv_charset_id(s, 1, 1);
  cfg->dvr_charset = s ? strdup(s) : NULL;
  cfg->dvr_charset_id = id ? strdup(id) : NULL;
  return change;
}

/**
 * create a new named dvr config; the caller is responsible
 * to avoid duplicates
 */
dvr_config_t *
dvr_config_create(const char *name, const char *uuid, htsmsg_t *conf)
{
  dvr_config_t *cfg;

  if (name == NULL)
    name = "";

  cfg = calloc(1, sizeof(dvr_config_t));
  LIST_INIT(&cfg->dvr_entries);
  LIST_INIT(&cfg->dvr_autorec_entries);
  LIST_INIT(&cfg->dvr_timerec_entries);
  LIST_INIT(&cfg->dvr_accesses);

  if (idnode_insert(&cfg->dvr_id, uuid, &dvr_config_class, 0)) {
    if (uuid)
      tvherror("dvr", "invalid config uuid '%s'", uuid);
    free(cfg);
    return NULL;
  }

  cfg->dvr_enabled = 1;
  if (name)
    cfg->dvr_config_name = strdup(name);
  else
    cfg->dvr_config_name = strdup("");
  cfg->dvr_retention_days = 31;
  cfg->dvr_mc = MC_MATROSKA;
  cfg->dvr_tag_files = 1;
  cfg->dvr_skip_commercials = 1;
  dvr_charset_update(cfg, intlconv_filesystem_charset());

  /* series link support */
  cfg->dvr_sl_brand_lock   = 1; // use brand linking
  cfg->dvr_sl_season_lock  = 0; // ignore season (except if no brand)
  cfg->dvr_sl_channel_lock = 1; // channel locked
  cfg->dvr_sl_time_lock    = 0; // time slot (approx) locked
  cfg->dvr_sl_more_recent  = 1; // Only record more reason episodes
  cfg->dvr_sl_quality_lock = 1; // Don't attempt to ajust quality

  /* Muxer config */
  cfg->dvr_muxcnf.m_cache  = MC_CACHE_DONTKEEP;
  cfg->dvr_muxcnf.m_rewrite_pat = 1;

  /* dup detect */
  cfg->dvr_dup_detect_episode = 1; // detect dup episodes

  /* Default recording file and directory permissions */

  cfg->dvr_muxcnf.m_file_permissions      = 0664;
  cfg->dvr_muxcnf.m_directory_permissions = 0775;

  if (conf) {
    idnode_load(&cfg->dvr_id, conf);
    cfg->dvr_valid = 1;
  }

  tvhinfo("dvr", "Creating new configuration '%s'", cfg->dvr_config_name);

  if (dvr_config_is_default(cfg) && dvr_config_find_by_name(NULL)) {
    tvherror("dvr", "Unable to create second default config, removing");
    LIST_INSERT_HEAD(&dvrconfigs, cfg, config_link);
    dvr_config_destroy(cfg, 0);
    cfg = NULL;
  }

  if (cfg) {
    LIST_INSERT_HEAD(&dvrconfigs, cfg, config_link);
    if (conf && dvr_config_is_default(cfg))
      cfg->dvr_enabled = 1;
  }

  return cfg;
}

/**
 * destroy a dvr config
 */
static void
dvr_config_destroy(dvr_config_t *cfg, int delconf)
{
  if (delconf) {
    tvhinfo("dvr", "Deleting configuration '%s'", cfg->dvr_config_name);
    hts_settings_remove("dvr/config/%s", idnode_uuid_as_str(&cfg->dvr_id));
  }
  LIST_REMOVE(cfg, config_link);
  idnode_unlink(&cfg->dvr_id);

  dvr_entry_destroy_by_config(cfg, delconf);
  access_destroy_by_dvr_config(cfg, delconf);
  autorec_destroy_by_config(cfg, delconf);
  timerec_destroy_by_config(cfg, delconf);

  free(cfg->dvr_charset_id);
  free(cfg->dvr_charset);
  free(cfg->dvr_storage);
  free(cfg->dvr_config_name);
  free(cfg);
}

/**
 *
 */
void
dvr_config_delete(const char *name)
{
  dvr_config_t *cfg;

  cfg = dvr_config_find_by_name(name);
  if (!dvr_config_is_default(cfg))
    dvr_config_destroy(cfg, 1);
  else
    tvhwarn("dvr", "Attempt to delete default config ignored");
}

/*
 *
 */
void
dvr_config_save(dvr_config_t *cfg)
{
  htsmsg_t *m = htsmsg_create_map();

  lock_assert(&global_lock);

  idnode_save(&cfg->dvr_id, m);
  hts_settings_save(m, "dvr/config/%s", idnode_uuid_as_str(&cfg->dvr_id));
  htsmsg_destroy(m);
}

/* **************************************************************************
 * DVR Config Class definition
 * **************************************************************************/

static void
dvr_config_class_save(idnode_t *self)
{
  dvr_config_t *cfg = (dvr_config_t *)self;
  if (dvr_config_is_default(cfg))
    cfg->dvr_enabled = 1;
  cfg->dvr_valid = 1;
  dvr_config_save(cfg);
}

static void
dvr_config_class_delete(idnode_t *self)
{
  dvr_config_t *cfg = (dvr_config_t *)self;
  if (!dvr_config_is_default(cfg))
      dvr_config_destroy(cfg, 1);
}

static int
dvr_config_class_perm(idnode_t *self, access_t *a, htsmsg_t *msg_to_write)
{
  dvr_config_t *cfg = (dvr_config_t *)self;
  htsmsg_field_t *f;
  const char *uuid, *my_uuid;

  if (access_verify2(a, ACCESS_RECORDER))
    return -1;
  if (!access_verify2(a, ACCESS_ADMIN))
    return 0;
  if (a->aa_dvrcfgs) {
    my_uuid = idnode_uuid_as_str(&cfg->dvr_id);
    HTSMSG_FOREACH(f, a->aa_dvrcfgs) {
      uuid = htsmsg_field_get_str(f) ?: "";
      if (!strcmp(uuid, my_uuid))
        goto fine;
    }
    return -1;
  }
fine:
  return 0;
}

static int
dvr_config_class_enabled_set(void *o, const void *v)
{
  dvr_config_t *cfg = (dvr_config_t *)o;
  if (dvr_config_is_default(cfg) && dvr_config_is_valid(cfg))
    return 0;
  if (cfg->dvr_enabled != *(int *)v) {
    cfg->dvr_enabled = *(int *)v;
    return 1;
  }
  return 0;
}

static uint32_t
dvr_config_class_enabled_opts(void *o)
{
  dvr_config_t *cfg = (dvr_config_t *)o;
  if (cfg && dvr_config_is_default(cfg) && dvr_config_is_valid(cfg))
    return PO_RDONLY;
  return 0;
}

static int
dvr_config_class_name_set(void *o, const void *v)
{
  dvr_config_t *cfg = (dvr_config_t *)o;
  if (dvr_config_is_default(cfg) && dvr_config_is_valid(cfg))
    return 0;
  if (strcmp(cfg->dvr_config_name, v ?: "")) {
    if (dvr_config_is_valid(cfg) && (v == NULL || *(char *)v == '\0'))
      return 0;
    free(cfg->dvr_config_name);
    cfg->dvr_config_name = strdup(v);
    return 1;
  }
  return 0;
}

static const char *
dvr_config_class_get_title (idnode_t *self)
{
  dvr_config_t *cfg = (dvr_config_t *)self;
  if (!dvr_config_is_default(cfg))
    return cfg->dvr_config_name;
  return "(Default Profile)";
}

static int
dvr_config_class_charset_set(void *o, const void *v)
{
  dvr_config_t *cfg = (dvr_config_t *)o;
  return dvr_charset_update(cfg, v);
}

static htsmsg_t *
dvr_config_class_charset_list(void *o)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "type",  "api");
  htsmsg_add_str(m, "uri",   "intlconv/charsets");
  return m;
}

static htsmsg_t *
dvr_config_class_cache_list(void *o)
{
  static struct strtab tab[] = {
    { "Unknown",            MC_CACHE_UNKNOWN },
    { "System",             MC_CACHE_SYSTEM },
    { "Do not keep",        MC_CACHE_DONTKEEP },
    { "Sync",               MC_CACHE_SYNC },
    { "Sync + Do not keep", MC_CACHE_SYNCDONTKEEP }
  };
  return strtab2htsmsg(tab);
}

const idclass_t dvr_config_class = {
  .ic_class      = "dvrconfig",
  .ic_caption    = "DVR Configuration Profile",
  .ic_event      = "dvrconfig",
  .ic_save       = dvr_config_class_save,
  .ic_get_title  = dvr_config_class_get_title,
  .ic_delete     = dvr_config_class_delete,
  .ic_perm       = dvr_config_class_perm,
  .ic_groups     = (const property_group_t[]) {
      {
         .name   = "DVR Behaviour",
         .number = 1,
      },
      {
         .name   = "Recording File Options",
         .number = 2,
      },
      {
         .name   = "Subdirectory Options",
         .number = 3,
      },
      {
         .name   = "Filename Options",
         .number = 4,
         .column = 1,
      },
      {
         .name   = "",
         .number = 5,
         .parent = 4,
         .column = 2,
      },
      {}
  },
  .ic_properties = (const property_t[]){
    {
      .type     = PT_BOOL,
      .id       = "enabled",
      .name     = "Enabled",
      .set      = dvr_config_class_enabled_set,
      .off      = offsetof(dvr_config_t, dvr_enabled),
      .def.i    = 1,
      .group    = 1,
      .get_opts = dvr_config_class_enabled_opts,
    },
    {
      .type     = PT_STR,
      .id       = "name",
      .name     = "Config Name",
      .set      = dvr_config_class_name_set,
      .off      = offsetof(dvr_config_t, dvr_config_name),
      .def.s    = "! New config",
      .group    = 1,
      .get_opts = dvr_config_class_enabled_opts,
    },
    {
      .type     = PT_INT,
      .id       = "container",
      .name     = "Container",
      .off      = offsetof(dvr_config_t, dvr_mc),
      .def.i    = MC_MATROSKA,
      .list     = dvr_entry_class_mc_list,
      .group    = 1,
    },
    {
      .type     = PT_INT,
      .id       = "cache",
      .name     = "Cache Scheme",
      .off      = offsetof(dvr_config_t, dvr_muxcnf.m_cache),
      .def.i    = MC_CACHE_DONTKEEP,
      .list     = dvr_config_class_cache_list,
      .group    = 1,
    },
    {
      .type     = PT_U32,
      .id       = "retention-days",
      .name     = "DVR Log Retention Time (days)",
      .off      = offsetof(dvr_config_t, dvr_retention_days),
      .def.u32  = 31,
      .group    = 1,
    },
    {
      .type     = PT_U32,
      .id       = "pre-extra-time",
      .name     = "Extra Time Before Recordings (minutes)",
      .off      = offsetof(dvr_config_t, dvr_extra_time_pre),
      .group    = 1,
    },
    {
      .type     = PT_U32,
      .id       = "post-extra-time",
      .name     = "Extra Time After Recordings (minutes)",
      .off      = offsetof(dvr_config_t, dvr_extra_time_post),
      .group    = 1,
    },
    {
      .type     = PT_BOOL,
      .id       = "episode-duplicate-detection",
      .name     = "Episode Duplicate Detect",
      .off      = offsetof(dvr_config_t, dvr_episode_duplicate),
      .group    = 1,
    },
    {
      .type     = PT_STR,
      .id       = "postproc",
      .name     = "Post-Processor Command",
      .off      = offsetof(dvr_config_t, dvr_postproc),
      .group    = 1,
    },
    {
      .type     = PT_STR,
      .id       = "storage",
      .name     = "Recording System Path",
      .off      = offsetof(dvr_config_t, dvr_storage),
      .group    = 2,
    },
    {
      .type     = PT_PERM,
      .id       = "file-permissions",
      .name     = "File Permissions (octal, e.g. 0664)",
      .off      = offsetof(dvr_config_t, dvr_muxcnf.m_file_permissions),
      .def.u32  = 0664,
      .group    = 2,
    },
    {
      .type     = PT_STR,
      .id       = "charset",
      .name     = "Filename Charset",
      .off      = offsetof(dvr_config_t, dvr_charset),
      .set      = dvr_config_class_charset_set,
      .list     = dvr_config_class_charset_list,
      .def.s    = "UTF-8",
      .group    = 2,
    },
    {
      .type     = PT_BOOL,
      .id       = "rewrite-pat",
      .name     = "Rewrite PAT",
      .off      = offsetof(dvr_config_t, dvr_muxcnf.m_rewrite_pat),
      .def.i    = 1,
      .group    = 2,
    },
    {
      .type     = PT_BOOL,
      .id       = "rewrite-pmt",
      .name     = "Rewrite PMT",
      .off      = offsetof(dvr_config_t, dvr_muxcnf.m_rewrite_pmt),
      .group    = 2,
    },
    {
      .type     = PT_BOOL,
      .id       = "tag-files",
      .name     = "Tag Files With Metadata",
      .off      = offsetof(dvr_config_t, dvr_tag_files),
      .def.i    = 1,
      .group    = 2,
    },
    {
      .type     = PT_BOOL,
      .id       = "skip-commercials",
      .name     = "Skip Commercials",
      .off      = offsetof(dvr_config_t, dvr_skip_commercials),
      .def.i    = 1,
      .group    = 2,
    },
    {
      .type     = PT_PERM,
      .id       = "directory-permissions",
      .name     = "Directory Permissions (octal, e.g. 0775)",
      .off      = offsetof(dvr_config_t, dvr_muxcnf.m_directory_permissions),
      .def.u32  = 0775,
      .group    = 3,
    },
    {
      .type     = PT_BOOL,
      .id       = "day-dir",
      .name     = "Make Subdirectories Per Day",
      .off      = offsetof(dvr_config_t, dvr_dir_per_day),
      .group    = 3,
    },
    {
      .type     = PT_BOOL,
      .id       = "channel-dir",
      .name     = "Make Subdirectories Per Channel",
      .off      = offsetof(dvr_config_t, dvr_channel_dir),
      .group    = 3,
    },
    {
      .type     = PT_BOOL,
      .id       = "title-dir",
      .name     = "Make Subdirectories Per Title",
      .off      = offsetof(dvr_config_t, dvr_title_dir),
      .group    = 3,
    },
    {
      .type     = PT_BOOL,
      .id       = "channel-in-title",
      .name     = "Include Channel Name In Filename",
      .off      = offsetof(dvr_config_t, dvr_channel_in_title),
      .group    = 4,
    },
    {
      .type     = PT_BOOL,
      .id       = "date-in-title",
      .name     = "Include Date In Filename",
      .off      = offsetof(dvr_config_t, dvr_date_in_title),
      .group    = 4,
    },
    {
      .type     = PT_BOOL,
      .id       = "time-in-title",
      .name     = "Include Time In Filename",
      .off      = offsetof(dvr_config_t, dvr_time_in_title),
      .group    = 4,
    },
    {
      .type     = PT_BOOL,
      .id       = "episode-in-title",
      .name     = "Include Episode In Filename",
      .off      = offsetof(dvr_config_t, dvr_episode_in_title),
      .group    = 4,
    },
    {
      .type     = PT_BOOL,
      .id       = "episode-before-date",
      .name     = "Put Episode In Filename Before Date And Time",
      .off      = offsetof(dvr_config_t, dvr_episode_before_date),
      .group    = 4,
    },
    {
      .type     = PT_BOOL,
      .id       = "subtitle-in-title",
      .name     = "Include Subtitle In Filename",
      .off      = offsetof(dvr_config_t, dvr_subtitle_in_title),
      .group    = 5,
    },
    {
      .type     = PT_BOOL,
      .id       = "omit-title",
      .name     = "Do Not Include Title To Filename",
      .off      = offsetof(dvr_config_t, dvr_omit_title),
      .group    = 5,
    },
    {
      .type     = PT_BOOL,
      .id       = "clean-title",
      .name     = "Remove All Unsafe Characters From Filename",
      .off      = offsetof(dvr_config_t, dvr_clean_title),
      .group    = 5,
    },
    {
      .type     = PT_BOOL,
      .id       = "whitespace-in-title",
      .name     = "Replace Whitespace In Title with '-'",
      .off      = offsetof(dvr_config_t, dvr_whitespace_in_title),
      .group    = 5,
    },
    {}
  },
};

/**
 *
 */
void
dvr_config_init(void)
{
  htsmsg_t *m, *l;
  htsmsg_field_t *f;
  char buf[500];
  const char *homedir;
  struct stat st;
  dvr_config_t *cfg;

  dvr_iov_max = sysconf(_SC_IOV_MAX);

  /* Default settings */

  LIST_INIT(&dvrconfigs);

  if ((l = hts_settings_load("dvr/config")) != NULL) {
    HTSMSG_FOREACH(f, l) {
      if ((m = htsmsg_get_map_by_field(f)) == NULL) continue;
      (void)dvr_config_create(NULL, f->hmf_name, m);
    }
    htsmsg_destroy(l);
  }

  /* Create the default entry */

  cfg = dvr_config_find_by_name_default(NULL);
  assert(cfg);

  LIST_FOREACH(cfg, &dvrconfigs, config_link) {
    if(cfg->dvr_storage == NULL || !strlen(cfg->dvr_storage)) {
      /* Try to figure out a good place to put them videos */

      homedir = getenv("HOME");

      if(homedir != NULL) {
        snprintf(buf, sizeof(buf), "%s/Videos", homedir);
        if(stat(buf, &st) == 0 && S_ISDIR(st.st_mode))
          cfg->dvr_storage = strdup(buf);
        
        else if(stat(homedir, &st) == 0 && S_ISDIR(st.st_mode))
          cfg->dvr_storage = strdup(homedir);
        else
          cfg->dvr_storage = strdup(getcwd(buf, sizeof(buf)));
      }

      tvhlog(LOG_WARNING, "dvr",
             "Output directory for video recording is not yet configured "
             "for DVR configuration \"%s\". "
             "Defaulting to to \"%s\". "
             "This can be changed from the web user interface.",
             cfg->dvr_config_name, cfg->dvr_storage);
    }
  }
}

void
dvr_init(void)
{
#if ENABLE_INOTIFY
  dvr_inotify_init();
#endif
  dvr_autorec_init();
  dvr_timerec_init();
  dvr_entry_init();
  dvr_autorec_update();
  dvr_timerec_update();
}

/**
 *
 */
void
dvr_done(void)
{
  dvr_config_t *cfg;

#if ENABLE_INOTIFY
  dvr_inotify_done();
#endif
  pthread_mutex_lock(&global_lock);
  dvr_entry_done();
  while ((cfg = LIST_FIRST(&dvrconfigs)) != NULL)
    dvr_config_destroy(cfg, 0);
  pthread_mutex_unlock(&global_lock);
  dvr_autorec_done();
  dvr_timerec_done();
}
