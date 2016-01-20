/*
 *  tvheadend, Wizard
 *  Copyright (C) 2015,2016 Jaroslav Kysela
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
#include "config.h"
#include "access.h"
#include "settings.h"
#include "input.h"
#include "input/mpegts/iptv/iptv_private.h"
#include "wizard.h"

/*
 *
 */

static const void *empty_get(void *o)
{
  prop_sbuf[0] = '\0';
  return &prop_sbuf_ptr;
}

static const void *icon_get(void *o)
{
  strcpy(prop_sbuf, "docresources/tvheadendlogo.png");
  return &prop_sbuf_ptr;
}

#define SPECIAL_PROP(idval, getfcn) { \
  .type = PT_STR, \
  .id   = idval, \
  .name = "", \
  .get  = getfcn, \
  .opts = PO_RDONLY | PO_NOUI \
}

#define PREV_BUTTON(page) SPECIAL_PROP("page_prev_" STRINGIFY(page), empty_get)
#define NEXT_BUTTON(page) SPECIAL_PROP("page_next_" STRINGIFY(page), empty_get)
#define LAST_BUTTON()     SPECIAL_PROP("page_last", empty_get)
#define ICON()            SPECIAL_PROP("icon", icon_get)
#define DESCRIPTION(page) SPECIAL_PROP("description", wizard_description_##page)
#define PROGRESS(fcn)     SPECIAL_PROP("progress", fcn)

#define DESCRIPTION_FCN(page, desc) \
static const void *wizard_description_##page(void *o) \
{ \
  static const char *t = desc; \
  return &t; \
}

#define BASIC_STR_OPS(stru, field) \
static const void *wizard_get_value_##field(void *o) \
{ \
  wizard_page_t *p = o; \
  stru *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->field); \
  return &prop_sbuf_ptr; \
} \
static int wizard_set_value_##field(void *o, const void *v) \
{ \
  wizard_page_t *p = o; \
  stru *w = p->aux; \
  snprintf(w->field, sizeof(w->field), "%s", (const char *)v); \
  return 1; \
}

/*
 *
 */

static void page_free(wizard_page_t *page)
{
  free(page->aux);
  free((char *)page->idnode.in_class);
  free(page);
}

static wizard_page_t *page_init
  (const char *name, const char *class_name, const char *caption)
{
  wizard_page_t *page = calloc(1, sizeof(*page));
  idclass_t *ic = calloc(1, sizeof(*ic));
  page->name = name;
  page->idnode.in_class = ic;
  ic->ic_caption = caption;
  ic->ic_class = ic->ic_event = class_name;
  ic->ic_perm_def = ACCESS_ADMIN;
  page->free = page_free;
  return page;
}

/*
 *
 */

static const void *hello_get_network(void *o)
{
  strcpy(prop_sbuf, "Test123");
  return &prop_sbuf_ptr;
}

static int hello_set_network(void *o, const void *v)
{
  return 0;
}

/*
 * Hello
 */

typedef struct wizard_hello {
  char ui_lang[32];
  char epg_lang1[32];
  char epg_lang2[32];
  char epg_lang3[32];
} wizard_hello_t;


static void hello_save(idnode_t *in)
{
  wizard_page_t *p = (wizard_page_t *)in;
  wizard_hello_t *w = p->aux;
  char buf[32];
  size_t l = 0;
  int save = 0;

  if (w->ui_lang[0] && strcmp(config.language_ui ?: "", w->ui_lang)) {
    free(config.language_ui);
    config.language_ui = strdup(w->ui_lang);
    save = 1;
  }
  buf[0] = '\0';
  if (w->epg_lang1[0])
    tvh_strlcatf(buf, sizeof(buf), l, "%s", w->epg_lang1);
  if (w->epg_lang2[0])
    tvh_strlcatf(buf, sizeof(buf), l, "%s%s", l > 0 ? "," : "", w->epg_lang2);
  if (w->epg_lang3[0])
    tvh_strlcatf(buf, sizeof(buf), l, "%s%s", l > 0 ? "," : "", w->epg_lang3);
  if (buf[0] && strcmp(buf, config.language ?: "")) {
    free(config.language);
    config.language = strdup(buf);
    save = 1;
  }
  if (save)
    config_save();
}

BASIC_STR_OPS(wizard_hello_t, ui_lang)
BASIC_STR_OPS(wizard_hello_t, epg_lang1)
BASIC_STR_OPS(wizard_hello_t, epg_lang2)
BASIC_STR_OPS(wizard_hello_t, epg_lang3)

DESCRIPTION_FCN(hello, N_("\
Enter the languages for the web user interface and \
for EPG texts.\
"))

wizard_page_t *wizard_hello(const char *lang)
{
  static const property_group_t groups[] = {
    {
      .name     = N_("Web interface"),
      .number   = 1,
    },
    {
      .name     = N_("EPG Language (priority order)"),
      .number   = 2,
    },
    {}
  };
  static const property_t props[] = {
    {
      .type     = PT_STR,
      .id       = "ui_lang",
      .name     = N_("Language"),
      .get      = wizard_get_value_ui_lang,
      .set      = wizard_set_value_ui_lang,
      .list     = language_get_ui_list,
      .group    = 1
    },
    {
      .type     = PT_STR,
      .id       = "epg_lang1",
      .name     = N_("Language 1"),
      .get      = wizard_get_value_epg_lang1,
      .set      = wizard_set_value_epg_lang1,
      .list     = language_get_list,
      .group    = 2
    },
    {
      .type     = PT_STR,
      .id       = "epg_lang2",
      .name     = N_("Language 2"),
      .get      = wizard_get_value_epg_lang2,
      .set      = wizard_set_value_epg_lang2,
      .list     = language_get_list,
      .group    = 2
    },
    {
      .type     = PT_STR,
      .id       = "epg_lang3",
      .name     = N_("Language 3"),
      .get      = wizard_get_value_epg_lang3,
      .set      = wizard_set_value_epg_lang3,
      .list     = language_get_list,
      .group    = 2
    },
    ICON(),
    DESCRIPTION(hello),
    NEXT_BUTTON(login),
    {}
  };
  wizard_page_t *page =
    page_init("hello", "wizard_hello",
    N_("Welcome - Tvheadend - your TV streaming server and video recorder"));
  idclass_t *ic = (idclass_t *)page->idnode.in_class;
  wizard_hello_t *w;
  htsmsg_t *m;
  htsmsg_field_t *f;
  const char *s;
  int idx;

  ic->ic_properties = props;
  ic->ic_groups = groups;
  ic->ic_save = hello_save;
  page->aux = w = calloc(1, sizeof(wizard_hello_t));

  if (config.language_ui)
    strncpy(w->ui_lang, config.language_ui, sizeof(w->ui_lang)-1);

  m = htsmsg_csv_2_list(config.language, ',');
  f = m ? HTSMSG_FIRST(m) : NULL;
  for (idx = 0; idx < 3 && f != NULL; idx++) {
    s = htsmsg_field_get_string(f);
    if (s == NULL) break;
    switch (idx) {
    case 0: strncpy(w->epg_lang1, s, sizeof(w->epg_lang1) - 1); break;
    case 1: strncpy(w->epg_lang2, s, sizeof(w->epg_lang2) - 1); break;
    case 2: strncpy(w->epg_lang3, s, sizeof(w->epg_lang3) - 1); break;
    }
    f = HTSMSG_NEXT(f);
  }
  htsmsg_destroy(m);

  return page;
}

/*
 * Login/Network access
 */

typedef struct wizard_login {
  char network[256];
  char admin_username[32];
  char admin_password[32];
  char username[32];
  char password[32];
} wizard_login_t;


static void login_save(idnode_t *in)
{
  wizard_page_t *p = (wizard_page_t *)in;
  wizard_login_t *w = p->aux;
  access_entry_t *ae, *ae_next;
  passwd_entry_t *pw, *pw_next;
  htsmsg_t *conf;
  const char *s;

  for (ae = TAILQ_FIRST(&access_entries); ae; ae = ae_next) {
    ae_next = TAILQ_NEXT(ae, ae_link);
    if (ae->ae_wizard)
      access_entry_destroy(ae, 1);
  }

  for (pw = TAILQ_FIRST(&passwd_entries); pw; pw = pw_next) {
    pw_next = TAILQ_NEXT(pw, pw_link);
    if (pw->pw_wizard)
      passwd_entry_destroy(pw, 1);
  }

  s = w->admin_username[0] ? w->admin_username : "*";
  conf = htsmsg_create_map();
  htsmsg_add_bool(conf, "enabled", 1);
  htsmsg_add_str(conf, "prefix", w->network);
  htsmsg_add_str(conf, "username", s);
  htsmsg_add_str(conf, "password", w->admin_password);
  htsmsg_add_bool(conf, "streaming", 1);
  htsmsg_add_bool(conf, "adv_streaming", 1);
  htsmsg_add_bool(conf, "htsp_streaming", 1);
  htsmsg_add_bool(conf, "dvr", 1);
  htsmsg_add_bool(conf, "htsp_dvr", 1);
  htsmsg_add_bool(conf, "webui", 1);
  htsmsg_add_bool(conf, "admin", 1);
  ae = access_entry_create(NULL, conf);
  if (ae) {
    ae->ae_wizard = 1;
    access_entry_save(ae);
  }
  htsmsg_destroy(conf);

  if (s && s[0] != '*' && w->admin_password[0]) {
    conf = htsmsg_create_map();
    htsmsg_add_bool(conf, "enabled", 1);
    htsmsg_add_str(conf, "username", s);
    htsmsg_add_str(conf, "password", w->admin_password);
    pw = passwd_entry_create(NULL, conf);
    if (pw) {
      pw->pw_wizard = 1;
      passwd_entry_save(pw);
    }
    htsmsg_destroy(conf);
  }

  if (w->username[0]) {
    s = w->username[0] ? w->username : "*";
    conf = htsmsg_create_map();
    htsmsg_add_str(conf, "prefix", w->network);
    htsmsg_add_str(conf, "username", s);
    htsmsg_add_str(conf, "password", w->password);
    ae = access_entry_create(NULL, conf);
    if (ae) {
      ae->ae_wizard = 1;
      access_entry_save(ae);
    }
    htsmsg_destroy(conf);

    if (s[0] != '*' && w->password[0]) {
      conf = htsmsg_create_map();
      htsmsg_add_bool(conf, "enabled", 1);
      htsmsg_add_str(conf, "username", s);
      htsmsg_add_str(conf, "password", w->password);
      pw = passwd_entry_create(NULL, conf);
      if (pw) {
        pw->pw_wizard = 1;
        passwd_entry_save(pw);
      }
      htsmsg_destroy(conf);
    }
  }
}

BASIC_STR_OPS(wizard_login_t, network)
BASIC_STR_OPS(wizard_login_t, admin_username)
BASIC_STR_OPS(wizard_login_t, admin_password)
BASIC_STR_OPS(wizard_login_t, username)
BASIC_STR_OPS(wizard_login_t, password)

DESCRIPTION_FCN(login, N_("\
Enter the access control details to secure your system. \
The first part of this covers the IPv4 network details \
for address-based access to the system; for example, \
192.168.1.0/24 to allow local access only to 192.168.1.x clients, \
or 0.0.0.0/0 or empty value for access from any system.\n\
This works alongside the second part, which is a familiar \
username/password combination, so provide these for both \
an administrator and regular (day-to-day) user. \
You can leave the username and password blank if you don't want \
this part, and would prefer anonymous access to anyone.\n\
This wizard should be run only on the initial setup. Please, cancel \
it, if you are not willing to touch the current configuration.\
"))

wizard_page_t *wizard_login(const char *lang)
{
  static const property_group_t groups[] = {
    {
      .name     = N_("Network access"),
      .number   = 1,
    },
    {
      .name     = N_("Administrator login"),
      .number   = 2,
    },
    {
      .name     = N_("User login"),
      .number   = 3,
    },
    {}
  };
  static const property_t props[] = {
    {
      .type     = PT_STR,
      .id       = "network",
      .name     = N_("Allowed network"),
      .get      = wizard_get_value_network,
      .set      = wizard_set_value_network,
      .group    = 1
    },
    {
      .type     = PT_STR,
      .id       = "admin_username",
      .name     = N_("Admin username"),
      .get      = wizard_get_value_admin_username,
      .set      = wizard_set_value_admin_username,
      .group    = 2
    },
    {
      .type     = PT_STR,
      .id       = "admin_password",
      .name     = N_("Admin password"),
      .get      = wizard_get_value_admin_password,
      .set      = wizard_set_value_admin_password,
      .group    = 2
    },
    {
      .type     = PT_STR,
      .id       = "username",
      .name     = N_("Username"),
      .get      = wizard_get_value_username,
      .set      = wizard_set_value_username,
      .group    = 3
    },
    {
      .type     = PT_STR,
      .id       = "password",
      .name     = N_("Password"),
      .get      = wizard_get_value_password,
      .set      = wizard_set_value_password,
      .group    = 3
    },
    ICON(),
    DESCRIPTION(login),
    PREV_BUTTON(hello),
    NEXT_BUTTON(network),
    {}
  };
  wizard_page_t *page =
    page_init("login", "wizard_login",
    N_("Welcome - Tvheadend - your TV streaming server and video recorder"));
  idclass_t *ic = (idclass_t *)page->idnode.in_class;
  wizard_login_t *w;
  access_entry_t *ae;
  passwd_entry_t *pw;

  ic->ic_properties = props;
  ic->ic_groups = groups;
  ic->ic_save = login_save;
  page->aux = w = calloc(1, sizeof(wizard_login_t));

  TAILQ_FOREACH(ae, &access_entries, ae_link) {
    if (!ae->ae_wizard)
      continue;
    if (ae->ae_admin) {
      htsmsg_t *c = htsmsg_create_map();
      idnode_save(&ae->ae_id, c);
      snprintf(w->admin_username, sizeof(w->admin_username), "%s", ae->ae_username);
      snprintf(w->network, sizeof(w->network), "%s", htsmsg_get_str(c, "prefix") ?: "");
      htsmsg_destroy(c);
    } else {
      snprintf(w->username, sizeof(w->username), "%s", ae->ae_username);
    }
  }

  TAILQ_FOREACH(pw, &passwd_entries, pw_link) {
    if (!pw->pw_wizard || !pw->pw_username)
      continue;
    if (w->admin_username[0] &&
        strcmp(w->admin_username, pw->pw_username) == 0) {
      snprintf(w->admin_password, sizeof(w->admin_password), "%s", pw->pw_password);
    } else if (w->username[0] && strcmp(w->username, pw->pw_username) == 0) {
      snprintf(w->password, sizeof(w->password), "%s", pw->pw_password);
    }
  }

  return page;
}

/*
 * Network settings
 */
#define WIZARD_NETWORKS 6

typedef struct wizard_network {
  char lang        [64];
  property_t props [WIZARD_NETWORKS * 3 + 10];
  char tuner       [WIZARD_NETWORKS][64];
  char tunerid     [WIZARD_NETWORKS][UUID_HEX_SIZE];
  char network_type[WIZARD_NETWORKS][64];
  htsmsg_t *network_types[WIZARD_NETWORKS];
} wizard_network_t;

static void network_free(wizard_page_t *page)
{
  wizard_network_t *w = page->aux;
  int idx;

  for (idx = 0; idx < WIZARD_NETWORKS; idx++)
    htsmsg_destroy(w->network_types[idx]);
  page_free(page);
}

static void network_save(idnode_t *in)
{
  wizard_page_t *p = (wizard_page_t *)in;
  wizard_network_t *w = p->aux;
  mpegts_network_t *mn, *mn_next;
  tvh_input_t *ti;
  htsmsg_t *m;
  int idx;

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link)
    if (mn->mn_wizard)
      mn->mn_wizard_free = 1;
  for (idx = 0; idx < WIZARD_NETWORKS; idx++) {
    if (w->network_type[idx][0] == '\0')
      continue;
    ti = tvh_input_find_by_uuid(w->tunerid[idx]);
    if (ti == NULL || ti->ti_wizard_set == NULL)
      continue;
    m = htsmsg_create_map();
    htsmsg_add_str(m, "mpegts_network_type", w->network_type[idx]);
    ti->ti_wizard_set(ti, m, w->lang[0] ? w->lang : NULL);
    htsmsg_destroy(m);
  }
  for (mn = LIST_FIRST(&mpegts_network_all); mn != NULL; mn = mn_next) {
    mn_next = LIST_NEXT(mn, mn_global_link);
    if (mn->mn_wizard_free)
      mn->mn_delete(mn, 1);
  }
}

#define NETWORK_GROUP(num) { \
  .name     = N_("Network " STRINGIFY(num)), \
  .number   = num, \
}

#define NETWORK(num) { \
  .type = PT_STR, \
  .id   = "tuner" STRINGIFY(num), \
  .name = N_("Tuner"), \
  .get  = network_get_tvalue##num, \
  .opts = PO_RDONLY, \
  .group = num, \
}, { \
  .type = PT_STR, \
  .id   = "tunerid" STRINGIFY(num), \
  .name = "Tuner", \
  .get  = network_get_tidvalue##num, \
  .set  = network_set_tidvalue##num, \
  .opts = PO_PERSIST | PO_NOUI, \
}, { \
  .type = PT_STR, \
  .id   = "network" STRINGIFY(num), \
  .name = N_("Network type"), \
  .get  = network_get_value##num, \
  .set  = network_set_value##num, \
  .list = network_get_list##num, \
  .group = num, \
}

#define NETWORK_FCN(num) \
static const void *network_get_tvalue##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_network_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->tuner[num-1]); \
  return &prop_sbuf_ptr; \
} \
static const void *network_get_tidvalue##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_network_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->tunerid[num-1]); \
  return &prop_sbuf_ptr; \
} \
static int network_set_tidvalue##num(void *o, const void *v) \
{ \
  wizard_page_t *p = o; \
  wizard_network_t *w = p->aux; \
  snprintf(w->tunerid[num-1], sizeof(w->tunerid[num-1]), "%s", (const char *)v); \
  return 1; \
} \
static const void *network_get_value##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_network_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->network_type[num-1]); \
  return &prop_sbuf_ptr; \
} \
static int network_set_value##num(void *o, const void *v) \
{ \
  wizard_page_t *p = o; \
  wizard_network_t *w = p->aux; \
  snprintf(w->network_type[num-1], sizeof(w->network_type[num-1]), "%s", (const char *)v); \
  return 1; \
} \
static htsmsg_t *network_get_list##num(void *o, const char *lang) \
{ \
  if (o == NULL) return NULL; \
  wizard_page_t *p = o; \
  wizard_network_t *w = p->aux; \
  return htsmsg_copy(w->network_types[num-1]); \
}

NETWORK_FCN(1)
NETWORK_FCN(2)
NETWORK_FCN(3)
NETWORK_FCN(4)
NETWORK_FCN(5)
NETWORK_FCN(6)

DESCRIPTION_FCN(network, N_("\
Select network type for detected tuners.\n\
The T means terresterial, C is cable and S is satellite.\
"))


wizard_page_t *wizard_network(const char *lang)
{
  static const property_group_t groups[] = {
    NETWORK_GROUP(1),
    NETWORK_GROUP(2),
    NETWORK_GROUP(3),
    NETWORK_GROUP(4),
    NETWORK_GROUP(5),
    NETWORK_GROUP(6),
    {}
  };
  static const property_t nprops[] = {
    NETWORK(1),
    NETWORK(2),
    NETWORK(3),
    NETWORK(4),
    NETWORK(5),
    NETWORK(6),
  };
  static const property_t props[] = {
    ICON(),
    DESCRIPTION(network),
    PREV_BUTTON(login),
    NEXT_BUTTON(muxes),
  };
  wizard_page_t *page = page_init("network", "wizard_network", N_("Network settings"));
  idclass_t *ic = (idclass_t *)page->idnode.in_class;
  wizard_network_t *w;
  mpegts_network_t *mn;
  tvh_input_t *ti;
  const char *name;
  htsmsg_t *m;
  int idx, nidx = 0;

  page->aux = w = calloc(1, sizeof(wizard_network_t));
  ic->ic_groups = groups;
  ic->ic_properties = w->props;
  ic->ic_save = network_save;
  page->free = network_free;
  snprintf(w->lang, sizeof(w->lang), "%s", lang ?: "");

  for (idx = 0; idx < ARRAY_SIZE(props); idx++)
    w->props[idx] = props[idx];

  for (ti = LIST_LAST(tvh_input_t, &tvh_inputs, ti_link); ti;
       ti = LIST_PREV(ti, tvh_input_t, &tvh_inputs, ti_link)) {
    if (ti->ti_wizard_get == NULL)
      continue;
    m = ti->ti_wizard_get(ti, lang);
    if (m == NULL)
      continue;
    name = htsmsg_get_str(m, "input_name");
    if (name) {
      snprintf(w->tuner[nidx], sizeof(w->tuner[nidx]), "%s", name);
      idnode_uuid_as_str(&ti->ti_id, w->tunerid[nidx]);
      mn = mpegts_network_find(htsmsg_get_str(m, "mpegts_network"));
      if (mn) {
        snprintf(w->network_type[nidx], sizeof(w->network_type[nidx]), "%s",
                 mn->mn_id.in_class->ic_class);
      }
      w->network_types[nidx] = htsmsg_copy(htsmsg_get_list(m, "mpegts_network_types"));
      w->props[idx++] = nprops[nidx * 3 + 0];
      w->props[idx++] = nprops[nidx * 3 + 1];
      w->props[idx++] = nprops[nidx * 3 + 2];
      nidx++;
    }
    htsmsg_destroy(m);
    if (nidx >= WIZARD_NETWORKS)
      break;
  }

  assert(idx < ARRAY_SIZE(w->props));

  return page;
}

/*
 * Muxes settings
 */

typedef struct wizard_muxes {
  char lang        [64];
  property_t props [WIZARD_NETWORKS * 3 + 10];
  char network     [WIZARD_NETWORKS][64];
  char networkid   [WIZARD_NETWORKS][UUID_HEX_SIZE];
  char muxes       [WIZARD_NETWORKS][64];
  char iptv_url    [WIZARD_NETWORKS][512];
} wizard_muxes_t;

static void muxes_free(wizard_page_t *page)
{
  page_free(page);
}

static void muxes_save(idnode_t *in)
{
  wizard_page_t *p = (wizard_page_t *)in;
  wizard_muxes_t *w = p->aux;
  mpegts_network_t *mn;
  htsmsg_t *m;
  int idx;

  for (idx = 0; idx < WIZARD_NETWORKS; idx++) {
    if (w->networkid[idx][0] == '\0')
      continue;
    mn = mpegts_network_find(w->networkid[idx]);
    if (mn == NULL || !mn->mn_wizard)
      continue;
    if (idnode_is_instance(&mn->mn_id, &dvb_network_class) && w->muxes[idx][0]) {
      dvb_network_scanfile_set((dvb_network_t *)mn, w->muxes[idx]);
    } else if (idnode_is_instance(&mn->mn_id, &iptv_auto_network_class) &&
               w->iptv_url[idx]) {
      m = htsmsg_create_map();
      htsmsg_add_str(m, "url", w->iptv_url[idx]);
      idnode_load(&mn->mn_id, m);
      htsmsg_destroy(m);
    }
  }
}

static const void *muxes_progress_get(void *o)
{
  wizard_page_t *p = o;
  wizard_muxes_t *w = p->aux;
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", tvh_gettext_lang(w->lang, N_("Scan progress")));
  return &prop_sbuf_ptr;
}

#define MUXES(num) { \
  .type = PT_STR, \
  .id   = "network" STRINGIFY(num), \
  .name = N_("Network"), \
  .get  = muxes_get_nvalue##num, \
  .opts = PO_RDONLY, \
  .group = num, \
}, { \
  .type = PT_STR, \
  .id   = "networkid" STRINGIFY(num), \
  .name = "Network", \
  .get  = muxes_get_idvalue##num, \
  .set  = muxes_set_idvalue##num, \
  .opts = PO_PERSIST | PO_NOUI, \
}, { \
  .type = PT_STR, \
  .id   = "muxes" STRINGIFY(num), \
  .name = N_("Pre-defined muxes"), \
  .get  = muxes_get_value##num, \
  .set  = muxes_set_value##num, \
  .list = muxes_get_list##num, \
  .group = num, \
}

#define MUXES_IPTV(num) { \
  .type = PT_STR, \
  .id   = "network" STRINGIFY(num), \
  .name = N_("Network"), \
  .get  = muxes_get_nvalue##num, \
  .opts = PO_RDONLY, \
  .group = num, \
}, { \
  .type = PT_STR, \
  .id   = "networkid" STRINGIFY(num), \
  .name = "Network", \
  .get  = muxes_get_idvalue##num, \
  .set  = muxes_set_idvalue##num, \
  .opts = PO_PERSIST | PO_NOUI, \
}, { \
  .type = PT_STR, \
  .id   = "muxes" STRINGIFY(num), \
  .name = N_("URL"), \
  .desc = N_("URL of the M3U playlist."), \
  .get  = muxes_get_iptv_value##num, \
  .set  = muxes_set_iptv_value##num, \
  .group = num, \
}

#define MUXES_FCN(num) \
static const void *muxes_get_nvalue##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->network[num-1]); \
  return &prop_sbuf_ptr; \
} \
static const void *muxes_get_idvalue##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->networkid[num-1]); \
  return &prop_sbuf_ptr; \
} \
static int muxes_set_idvalue##num(void *o, const void *v) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(w->networkid[num-1], sizeof(w->networkid[num-1]), "%s", (const char *)v); \
  return 1; \
} \
static const void *muxes_get_value##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->muxes[num-1]); \
  return &prop_sbuf_ptr; \
} \
static int muxes_set_value##num(void *o, const void *v) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(w->muxes[num-1], sizeof(w->muxes[num-1]), "%s", (const char *)v); \
  return 1; \
} \
static htsmsg_t *muxes_get_list##num(void *o, const char *lang) \
{ \
  if (o == NULL) return NULL; \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  mpegts_network_t *mn = mpegts_network_find(w->networkid[num-1]); \
  return mn ? dvb_network_class_scanfile_list(mn, lang) : NULL; \
} \
static const void *muxes_get_iptv_value##num(void *o) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(prop_sbuf, PROP_SBUF_LEN, "%s", w->iptv_url[num-1]); \
  return &prop_sbuf_ptr; \
} \
static int muxes_set_iptv_value##num(void *o, const void *v) \
{ \
  wizard_page_t *p = o; \
  wizard_muxes_t *w = p->aux; \
  snprintf(w->iptv_url[num-1], sizeof(w->iptv_url[num-1]), "%s", (const char *)v); \
  return 1; \
}

MUXES_FCN(1)
MUXES_FCN(2)
MUXES_FCN(3)
MUXES_FCN(4)
MUXES_FCN(5)
MUXES_FCN(6)

DESCRIPTION_FCN(muxes, N_("\
Assign predefined muxes to networks.\
"))

wizard_page_t *wizard_muxes(const char *lang)
{
  static const property_group_t groups[] = {
    NETWORK_GROUP(1),
    NETWORK_GROUP(2),
    NETWORK_GROUP(3),
    NETWORK_GROUP(4),
    NETWORK_GROUP(5),
    NETWORK_GROUP(6),
    {}
  };
  static const property_t nprops[] = {
    MUXES(1),
    MUXES(2),
    MUXES(3),
    MUXES(4),
    MUXES(5),
    MUXES(6),
  };
  static const property_t iptvprops[] = {
    MUXES_IPTV(1),
    MUXES_IPTV(2),
    MUXES_IPTV(3),
    MUXES_IPTV(4),
    MUXES_IPTV(5),
    MUXES_IPTV(6),
  };
  static const property_t props[] = {
    ICON(),
    DESCRIPTION(muxes),
    PREV_BUTTON(network),
    NEXT_BUTTON(status),
  };
  wizard_page_t *page = page_init("muxes", "wizard_muxes", N_("Assign predefined muxes to networks"));
  idclass_t *ic = (idclass_t *)page->idnode.in_class;
  wizard_muxes_t *w;
  mpegts_network_t *mn;
  int idx, midx = 0;

  page->aux = w = calloc(1, sizeof(wizard_muxes_t));
  ic->ic_groups = groups;
  ic->ic_properties = w->props;
  ic->ic_save = muxes_save;
  page->free = muxes_free;
  snprintf(w->lang, sizeof(w->lang), "%s", lang ?: "");

  for (idx = 0; idx < ARRAY_SIZE(props); idx++)
    w->props[idx] = props[idx];

  LIST_FOREACH(mn, &mpegts_network_all, mn_global_link)
    if (mn->mn_wizard) {
      mn->mn_display_name(mn, w->network[midx], sizeof(w->network[midx]));
      idnode_uuid_as_str(&mn->mn_id, w->networkid[midx]);
      if (idnode_is_instance(&mn->mn_id, &dvb_network_class)) {
        w->props[idx++] = nprops[midx * 3 + 0];
        w->props[idx++] = nprops[midx * 3 + 1];
        w->props[idx++] = nprops[midx * 3 + 2];
        midx++;
      } else if (idnode_is_instance(&mn->mn_id, &iptv_auto_network_class)) {
        snprintf(w->iptv_url[midx], sizeof(w->iptv_url[midx]), "%s", ((iptv_network_t *)mn)->in_url ?: "");
        w->props[idx++] = iptvprops[midx * 3 + 0];
        w->props[idx++] = iptvprops[midx * 3 + 1];
        w->props[idx++] = iptvprops[midx * 3 + 2];
        midx++;
      }
    }

  assert(idx < ARRAY_SIZE(w->props));

  return page;
}

/*
 * Status
 */

DESCRIPTION_FCN(status, N_("\
Show the scan status.\n\
Please, wait until the scan finishes.\
"))


wizard_page_t *wizard_status(const char *lang)
{
  static const property_group_t groups[] = {
    {
      .name     = "",
      .number   = 1,
    },
    {}
  };
  static const property_t props[] = {
    {
      .type     = PT_STR,
      .id       = "muxes",
      .name     = N_("Found muxes"),
      .desc     = N_("Number of muxes found."),
      .get      = empty_get,
      .opts     = PO_RDONLY,
      .group    = 1,
    },
    {
      .type     = PT_STR,
      .id       = "services",
      .name     = N_("Found services"),
      .desc     = N_("Total number of services found."),
      .get      = empty_get,
      .opts     = PO_RDONLY,
      .group    = 1,
    },
    PROGRESS(muxes_progress_get),
    ICON(),
    DESCRIPTION(status),
    PREV_BUTTON(muxes),
    NEXT_BUTTON(mapping),
    {}
  };
  wizard_page_t *page = page_init("status", "wizard_status", N_("Scan status"));
  idclass_t *ic = (idclass_t *)page->idnode.in_class;
  ic->ic_properties = props;
  ic->ic_groups = groups;
  return page;
}

/*
 * Service Mapping
 */

DESCRIPTION_FCN(mapping, N_("\
Do the service mapping to channels.\
"))


wizard_page_t *wizard_mapping(const char *lang)
{
  static const property_t props[] = {
    {
      .type     = PT_STR,
      .id       = "pnetwork",
      .name     = N_("Select network"),
      .desc     = N_("Select a Network."),
      .get      = hello_get_network,
      .set      = hello_set_network,
    },
    ICON(),
    DESCRIPTION(mapping),
    PREV_BUTTON(status),
    LAST_BUTTON(),
    {}
  };
  wizard_page_t *page = page_init("mapping", "wizard_service_map", N_("Service mapping"));
  idclass_t *ic = (idclass_t *)page->idnode.in_class;
  ic->ic_properties = props;
  return page;
}
