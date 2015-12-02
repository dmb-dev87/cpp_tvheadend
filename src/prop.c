/*
 *  Tvheadend - property system (part of idnode)
 *
 *  Copyright (C) 2013 Andreas Öman
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

#include <stdio.h>
#include <string.h>

#include "tvheadend.h"
#include "prop.h"
#include "tvh_locale.h"
#include "lang_str.h"

char prop_sbuf[PROP_SBUF_LEN];
char *prop_sbuf_ptr = prop_sbuf;

/* **************************************************************************
 * Utilities
 * *************************************************************************/

/**
 *
 */
const static struct strtab typetab[] = {
  { "bool",    PT_BOOL },
  { "int",     PT_INT },
  { "str",     PT_STR },
  { "u16",     PT_U16 },
  { "u32",     PT_U32 },
  { "s64",     PT_S64 },
  { "dbl",     PT_DBL },
  { "time",    PT_TIME },
  { "langstr", PT_LANGSTR },
  { "perm",    PT_PERM },
};


const property_t *
prop_find(const property_t *p, const char *id)
{
  for(; p->id; p++)
    if(!strcmp(id, p->id))
      return p;
  return NULL;
}

/* **************************************************************************
 * Write
 * *************************************************************************/

/**
 *
 */
int
prop_write_values
  (void *obj, const property_t *pl, htsmsg_t *m, int optmask,
   htsmsg_t *updated)
{
  int save, save2 = 0;
  htsmsg_field_t *f;
  const property_t *p;
  void *cur;
  const void *new;
  double dbl;
  int i;
  int64_t s64;
  uint32_t u32;
  uint16_t u16;
  time_t tm;
#define PROP_UPDATE(v, t)\
  new = &v;\
  if (!p->set && (*((t*)cur) != *((t*)new))) {\
    save = 1;\
    *((t*)cur) = *((t*)new);\
  } (void)0

  if (!pl) return 0;

  for (p = pl; p->id; p++) {

    if (p->type == PT_NONE) continue;

    f = htsmsg_field_find(m, p->id);
    if (!f) continue;

    /* Ignore */
    u32 = p->get_opts ? p->get_opts(obj) : p->opts;
    if(u32 & optmask) continue;

    /* Sanity check */
    assert(p->set || p->off);

    /* Write */
    save = 0;
    cur  = obj + p->off;
    new  = NULL;

    /* List */
    if (p->islist)
      new = (f->hmf_type == HMF_MAP) ?
              htsmsg_field_get_map(f) :
              htsmsg_field_get_list(f);

    /* Singular */
    else {
      switch (p->type) {
      case PT_BOOL: {
        if (htsmsg_field_get_bool(f, &i))
          continue;
        PROP_UPDATE(i, int);
        break;
      }
      case PT_INT: {
        if (htsmsg_field_get_s64(f, &s64))
          continue;
        i = s64;
        PROP_UPDATE(i, int);
        break;
      }
      case PT_U16: {
        if (htsmsg_field_get_u32(f, &u32))
          continue;
        u16 = (uint16_t)u32;
        PROP_UPDATE(u16, uint16_t);
        break;
      }
      case PT_U32: {
        if (p->intsplit) {
          char *s;
          if (!(new = htsmsg_field_get_str(f)))
            continue;
          u32 = atol(new) * p->intsplit;
          if ((s = strchr(new, '.')) != NULL)
            u32 += (atol(s + 1) % p->intsplit);
        } else {
          if (htsmsg_field_get_u32(f, &u32))
            continue;
        }
        PROP_UPDATE(u32, uint32_t);
        break;
      }
      case PT_S64: {
        if (p->intsplit) {
          if (!(new = htsmsg_field_get_str(f)))
            continue;
          s64 = prop_intsplit_from_str(new, p->intsplit);
        } else {
          if (htsmsg_field_get_s64(f, &s64))
            continue;
        }
        PROP_UPDATE(s64, int64_t);
        break;
      }
      case PT_DBL: {
        if (htsmsg_field_get_dbl(f, &dbl))
          continue;
        PROP_UPDATE(dbl, double);
        break;
      }
      case PT_STR: {
        char **str = cur;
        if (!(new = htsmsg_field_get_str(f)))
          continue;
        if (!p->set && strcmp((*str) ?: "", new)) {
          /* make sure that the string is valid all time */
          void *old = *str;
          *str = strdup(new);
          free(old);
          save = 1;
        }
        break;
      }
      case PT_TIME: {
        if (htsmsg_field_get_s64(f, &s64))
          continue;
        tm = s64;
        PROP_UPDATE(tm, time_t);
        break;
      }
      case PT_LANGSTR: {
        lang_str_t **lstr1 = cur;
        lang_str_t  *lstr2;
        new = htsmsg_field_get_map(f);
        if (!new)
          continue;
        if (!p->set) {
          lstr2 = lang_str_deserialize_map((htsmsg_t *)new);
          if (lang_str_compare(*lstr1, lstr2)) {
            lang_str_destroy(*lstr1);
            *lstr1 = lstr2;
            save = 1;
          } else {
            lang_str_destroy(lstr2);
          }
        }
        break;
      }
      case PT_PERM: {
        if (!(new = htsmsg_field_get_str(f)))
          continue;
        u32 = (int)strtol(new,NULL,0);
        PROP_UPDATE(u32, uint32_t);
        break;
      }
      case PT_NONE:
        break;
      }
    }
  
    /* Setter */
    if (p->set && new)
      save = p->set(obj, new);

    /* Updated */
    if (save) {
      save2 = 1;
      if (p->notify)
        p->notify(obj, NULL);
      if (updated)
        htsmsg_set_u32(updated, p->id, 1);
    }
  }
#undef PROP_UPDATE
  return save2;
}

/* **************************************************************************
 * Read
 * *************************************************************************/

/**
 *
 */
static void
prop_read_value
  (void *obj, const property_t *p, htsmsg_t *m, const char *name,
   int optmask, const char *lang)
{
  const char *s;
  const void *val = obj + p->off;
  uint32_t u32;
  char buf[24];

  /* Ignore */
  u32 = p->get_opts ? p->get_opts(obj) : p->opts;
  if (u32 & optmask) return;
  if (p->type == PT_NONE) return;

  /* Sanity check */
  assert(p->get || p->off);

  /* Get method */
  if (!(optmask & PO_USERAW) || !p->off)
    if (p->get)
      val = p->get(obj);

  /* List */
  if (p->islist) {
    assert(p->get); /* requirement */
    htsmsg_add_msg(m, name, (htsmsg_t*)val);
  
  /* Single */
  } else {
    switch(p->type) {
    case PT_BOOL:
      htsmsg_add_bool(m, name, *(int *)val);
      break;
    case PT_INT:
      htsmsg_add_s64(m, name, *(int *)val);
      break;
    case PT_U16:
      htsmsg_add_u32(m, name, *(uint16_t *)val);
      break;
    case PT_U32:
      if (p->intsplit) {
        uint32_t maj = *(int64_t *)val / p->intsplit;
        uint32_t min = *(int64_t *)val % p->intsplit;
        if (min) {
          snprintf(buf, sizeof(buf), "%u.%u", (unsigned int)maj, (unsigned int)min);
          htsmsg_add_str(m, name, buf);
        } else
          htsmsg_add_s64(m, name, maj);
      } else
        htsmsg_add_u32(m, name, *(uint32_t *)val);
      break;
    case PT_S64:
      if (p->intsplit) {
        int64_t maj = *(int64_t *)val / p->intsplit;
        int64_t min = *(int64_t *)val % p->intsplit;
        if (min) {
          snprintf(buf, sizeof(buf), "%lu.%lu", (unsigned long)maj, (unsigned long)min);
          htsmsg_add_str(m, name, buf);
        } else
          htsmsg_add_s64(m, name, maj);
      } else
        htsmsg_add_s64(m, name, *(int64_t *)val);
      break;
    case PT_STR:
      if (optmask & PO_LOCALE) {
        if ((s = *(const char **)val))
          htsmsg_add_str(m, name, lang ? tvh_gettext_lang(lang, s) : s);
      } else {
        if ((s = *(const char **)val))
          htsmsg_add_str(m, name, s);
      }
      break;
    case PT_DBL:
      htsmsg_add_dbl(m, name, *(double*)val);
      break;
    case PT_TIME:
      htsmsg_add_s64(m, name, *(time_t *)val);
      break;
    case PT_LANGSTR:
      lang_str_serialize(*(lang_str_t **)val, m, name);
      break;
    case PT_PERM:
      snprintf(buf, sizeof(buf), "%04o", *(uint32_t *)val);
      htsmsg_add_str(m, name, buf);
      break;
    case PT_NONE:
      break;
    }
  }
}

/**
 *
 */
void
prop_read_values
  (void *obj, const property_t *pl, htsmsg_t *m, htsmsg_t *list,
   int optmask, const char *lang)
{
  if(pl == NULL)
    return;

  if(list == NULL) {
    for (; pl->id; pl++)
      prop_read_value(obj, pl, m, pl->id, optmask, lang);
  } else {
    const property_t *p;
    htsmsg_field_t *f;
    int b, total = 0, count = 0;
    
    HTSMSG_FOREACH(f, list) {
      total++;
      if (!htsmsg_field_get_bool(f, &b)) {
        if (b > 0) {
          p = prop_find(pl, f->hmf_name);
          if (p)
            prop_read_value(obj, p, m, p->id, optmask, lang);
          count++;
        }
      }
    }
    if (total && !count) {
      for (; pl->id; pl++) {
        HTSMSG_FOREACH(f, list)
          if (!strcmp(pl->id, f->hmf_name))
            break;
        if (f == NULL)
          prop_read_value(obj, pl, m, pl->id, optmask, lang);
      }
    }
  }
}

/**
 *
 */
static void
prop_serialize_value
  (void *obj, const property_t *pl, htsmsg_t *msg, int optmask, const char *lang)
{
  htsmsg_field_t *f;
  char buf[16];
  uint32_t opts;

  /* Remove parent */
  // TODO: this is really horrible and inefficient!
  HTSMSG_FOREACH(f, msg) {
    htsmsg_t *t = htsmsg_field_get_map(f);
    const char *str;
    if (t && (str = htsmsg_get_str(t, "id"))) {
      if (!strcmp(str, pl->id)) {
        htsmsg_field_destroy(msg, f);
        break;
      }
    }
  }

  /* Skip - special blocker */
  if (pl->type == PT_NONE)
    return;

  htsmsg_t *m = htsmsg_create_map();

  /* ID / type */
  htsmsg_add_str(m, "id",       pl->id);
  htsmsg_add_str(m, "type",     val2str(pl->type, typetab) ?: "none");

  /* Metadata */
  htsmsg_add_str(m, "caption",  tvh_gettext_lang(lang, pl->name));
  if (pl->islist) {
    htsmsg_add_u32(m, "list", 1);
    if (pl->def.list)
      htsmsg_add_msg(m, "default", pl->def.list());
  } else {
    /* Default */
    switch (pl->type) {
      case PT_BOOL:
        htsmsg_add_bool(m, "default", pl->def.i);
        break;
      case PT_INT:
        htsmsg_add_s32(m, "default", pl->def.i);
        break;
      case PT_U16:
        htsmsg_add_u32(m, "default", pl->def.u16);
        break;
      case PT_U32:
        htsmsg_add_u32(m, "default", pl->def.u32);
        break;
      case PT_S64:
        htsmsg_add_s64(m, "default", pl->def.s64);
        break;
      case PT_DBL:
        htsmsg_add_dbl(m, "default", pl->def.d);
        break;
      case PT_STR:
        htsmsg_add_str(m, "default", pl->def.s ?: "");
        break;
      case PT_TIME:
        htsmsg_add_s64(m, "default", pl->def.tm);
        break;
      case PT_LANGSTR:
        /* TODO? */
        break;
      case PT_PERM:
        snprintf(buf, sizeof(buf), "%04o", pl->def.u32);
        htsmsg_add_str(m, "default", buf);
        break;
      case PT_NONE:
        break;
    }
  }

  /* Options */
  opts = pl->get_opts ? pl->get_opts(obj) : pl->opts;
  if (opts & PO_RDONLY)
    htsmsg_add_bool(m, "rdonly", 1);
  if (opts & PO_NOSAVE)
    htsmsg_add_bool(m, "nosave", 1);
  if (opts & PO_WRONCE)
    htsmsg_add_bool(m, "wronce", 1);
  if (opts & PO_EXPERT)
    htsmsg_add_bool(m, "expert", 1);
  else if (opts & PO_ADVANCED)
    htsmsg_add_bool(m, "advanced", 1);
  if (opts & PO_NOUI)
    htsmsg_add_bool(m, "noui", 1);
  if (opts & PO_HIDDEN)
    htsmsg_add_bool(m, "hidden", 1);
  if (opts & PO_PASSWORD)
    htsmsg_add_bool(m, "password", 1);
  if (opts & PO_DURATION)
    htsmsg_add_bool(m, "duration", 1);
  if (opts & PO_HEXA)
    htsmsg_add_bool(m, "hexa", 1);
  if (opts & PO_DATE)
    htsmsg_add_bool(m, "date", 1);
  if (opts & PO_LORDER)
    htsmsg_add_bool(m, "lorder", 1);
  if (opts & PO_MULTILINE)
    htsmsg_add_bool(m, "multiline", 1);

  /* Enum list */
  if (pl->list) {
    htsmsg_t *list = pl->list(obj, lang);
    if (list)
      htsmsg_add_msg(m, "enum", list);
  }

  /* Visual group */
  if (pl->group)
    htsmsg_add_u32(m, "group", pl->group);

  /* Split integer value */
  if (pl->intsplit)
    htsmsg_add_u32(m, "intsplit", pl->intsplit);

  /* Data */
  if (obj)
    prop_read_value(obj, pl, m, "value", optmask, lang);

  htsmsg_add_msg(msg, NULL, m);
}

/**
 *
 */
void
prop_serialize
  (void *obj, const property_t *pl, htsmsg_t *msg, htsmsg_t *list,
   int optmask, const char *lang)
{
  if(pl == NULL)
    return;

  if(list == NULL) {
    for (; pl->id; pl++)
      prop_serialize_value(obj, pl, msg, optmask, lang);
  } else {
    const property_t *p;
    htsmsg_field_t *f;
    int b, total = 0, count = 0;
    HTSMSG_FOREACH(f, list) {
      total++;
      if (!htsmsg_field_get_bool(f, &b) && b > 0) {
        p = prop_find(pl, f->hmf_name);
        if (p)
          prop_serialize_value(obj, p, msg, optmask, lang);
        count++;
      }
    }
    if (total && !count) {
      for (; pl->id; pl++) {
        HTSMSG_FOREACH(f, list)
          if (!strcmp(pl->id, f->hmf_name))
            break;
        if (f == NULL)
          prop_serialize_value(obj, pl, msg, optmask, lang);
      }
    }
  }
}

/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
