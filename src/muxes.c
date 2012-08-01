/*
 *  tvheadend, intial mux list
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

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>
#include <libgen.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "tvheadend.h"
#include "dvb/dvb.h"
#include "muxes.h"

region_list_t regions_DVBC;
region_list_t regions_DVBT;
region_list_t regions_DVBS;
region_list_t regions_ATSC;

/* **************************************************************************
 * Country codes
 * *************************************************************************/

static const struct {
  const char *code;
  const char *name;

} tldlist[] = {
  {"auto", "--Generic--"},
  {"ad", "Andorra"},
  {"at", "Austria"},
  {"au", "Australia"},
  {"ax", "Aland Islands"},
  {"be", "Belgium"},
  {"br", "Brazil"},
  {"ca", "Canada"},
  {"ch", "Switzerland"},
  {"cz", "Czech Republic"},
  {"de", "Germany"},
  {"dk", "Denmark"},
  {"es", "Spain"},
  {"fi", "Finland"},
  {"fr", "France"},
  {"gr", "Greece"},
  {"hk", "Hong Kong"},
  {"hr", "Croatia"},
  {"hu", "Hungary"},
  {"il", "Israel"},
  {"ir", "Iran"},
  {"is", "Iceland"},
  {"it", "Italy"},
  {"lt", "Lithuania"},
  {"lu", "Luxembourg"},
  {"lv", "Latvia"},
  {"nl", "Netherlands"},
  {"no", "Norway"},
  {"nz", "New Zealand"},
  {"pl", "Poland"},
  {"ro", "Romania"},
  {"se", "Sweden"},
  {"si", "Slovenia"},
  {"sk", "Slovakia"},
  {"tw", "Taiwan"},
  {"uk", "United Kingdom"},
  {"us", "United States"},
  {"vn", "Vietnam"},
};

static const char *
tldcode2longname(const char *tld)
{
  int i;
  for(i = 0; i < sizeof(tldlist) / sizeof(tldlist[0]); i++)
    if(!strcmp(tld, tldlist[i].code))
      return tldlist[i].name;
  return NULL;
}

/* **************************************************************************
 * Type specific parsers
 * *************************************************************************/

static int _muxes_load_atsc ( mux_t *mux, const char *line )
{
  char qam[20];
  int r;

  r = sscanf(line, "%u %s", &mux->freq, qam);
  if (r != 2) return 1;
  if ((mux->constellation = dvb_mux_str2qam(qam)) == -1) return 1;

  return 0;
}

static int _muxes_load_dvbt ( mux_t *mux, const char *line )
{
  char bw[20], fec[20], fec2[20], qam[20], mode[20], guard[20], hier[20];
  int r;

  r = sscanf(line, "%u %10s %10s %10s %10s %10s %10s %10s",
	           &mux->freq, bw, fec, fec2, qam, mode, guard, hier);
  if(r != 8) return 1;

  if ((mux->bw            = dvb_mux_str2bw(bw))       == -1) return 1;
  if ((mux->constellation = dvb_mux_str2qam(qam))     == -1) return 1;
  if ((mux->fechp         = dvb_mux_str2fec(fec))     == -1) return 1;
  if ((mux->feclp         = dvb_mux_str2fec(fec2))    == -1) return 1;
  if ((mux->tmode         = dvb_mux_str2mode(mode))   == -1) return 1;
  if ((mux->guard         = dvb_mux_str2guard(guard)) == -1) return 1;
  if ((mux->hierarchy     = dvb_mux_str2hier(hier))   == -1) return 1;

  return 0;
}

static int _muxes_load_dvbs ( mux_t *mux, const char *line )
{
  char fec[20], qam[20], hier[20];
  int r, v2 = 0;

  if (*line == '2') {
    v2 = 2;
    line++;
  }

  r = sscanf(line, "%u %c %u %s %s %s",
	           &mux->freq, &mux->polarisation, &mux->symrate,
             fec, hier, qam);
  if (r != (4+v2)) return 1;
  
  if ((mux->fec             = dvb_mux_str2fec(fec)) == -1)   return 1;
  if (v2) {
    if ((mux->hierarchy     = dvb_mux_str2hier(hier)) == -1) return 1;
    if ((mux->constellation = dvb_mux_str2qam(qam))   == -1) return 1;
  }

  return 0;
}

static int _muxes_load_dvbc ( mux_t *mux, const char *line )
{
  char fec[20], qam[20];
  int r;

  r = sscanf(line, "%u %u %s %s",
	           &mux->freq, &mux->symrate, fec, qam);
  if(r != 4) return 1;

  if ((mux->fec           = dvb_mux_str2fec(fec)) == -1) return 1;
  if ((mux->constellation = dvb_mux_str2qam(qam)) == -1) return 1;

  return 0;
}


/* **************************************************************************
 * File processing
 * *************************************************************************/

/*
 * Sorting
 */
static int _net_cmp ( void *a, void *b )
{
  return strcmp(((network_t*)a)->name, ((network_t*)b)->name);
}
static int _reg_cmp ( void *a, void *b )
{
  return strcmp(((region_t*)a)->name, ((region_t*)b)->name);
}

/*
 * Create/Find region entry
 */
static region_t *_muxes_region_create 
  ( const char *type, const char *id, const char *desc )
{
  region_t *reg;
  region_list_t *list = NULL;
  if      (!strcmp(type, "dvb-s")) list = &regions_DVBS;
  else if (!strcmp(type, "dvb-t")) list = &regions_DVBT;
  else if (!strcmp(type, "dvb-c")) list = &regions_DVBC;
  else if (!strcmp(type, "atsc"))  list = &regions_ATSC;
  if (!list) return NULL;

  LIST_FOREACH(reg, list, link) {
    if (!strcmp(reg->id, id)) break;
  }

  if (!reg) {
    reg = calloc(1, sizeof(region_t));
    reg->id   = strdup(id);
    reg->name = strdup(desc);
    LIST_INSERT_SORTED(list, reg, link, _reg_cmp);
  }
  
  return reg;
}

/*
 * Process mux entry
 */
static void _muxes_load_one ( network_t *net, const char *line )
{
  int r = 1;
  mux_t *mux = calloc(1, sizeof(mux_t));
  
  switch (line[0]) {
    case 'A':
      r = _muxes_load_atsc(mux, line+1);
      break;
    case 'T':
      r = _muxes_load_dvbt(mux, line+1);
      break;
    case 'S':
      r = _muxes_load_dvbs(mux, line+1);
      break;
    case 'C':
      r = _muxes_load_dvbc(mux, line+1);
      break;
  }

  if (r) {
    free(mux);
  } else {
    LIST_INSERT_HEAD(&net->muxes, mux, link);
  }
}

/*
 * Process a file
 */
static void _muxes_load_file ( const char *type, const char *path )
{
  FILE *fp;
  int i;
  region_t *reg;
  network_t *net;
  char *name, *co;
  char buf[256], buf2[256];

  fp = fopen(path, "r");
  if (!fp) return;

  /* Region */
  strcpy(buf, path);
  if (!(name = basename(buf))) return;
  if (!strcmp(type, "dvb-s")) {
    reg = _muxes_region_create(type, "geo", "Geo-synchronous Orbit");
  } else {
    if (!(co   = strtok(name, "-"))) return;
    reg = _muxes_region_create(type, co, tldcode2longname(co));
  }
  if (!reg) return;

  /* Network */
  i = 0;
  name = basename((char*)path);
  while (*name) {
    buf[i++] = isalnum(*name) ? *name : '_';
    name++;
  }
  buf[i] = '\0';
  snprintf(buf2, sizeof(buf2), "%s_%s", type, buf);
  net = calloc(1, sizeof(network_t));
  net->id   = strdup(buf2);
  net->name = strdup(buf);
  LIST_INSERT_SORTED(&reg->networks, net, link, _net_cmp);

  /* Process file */
  while (!feof(fp)) {

    /* Get line */
    memset(buf, 0, sizeof(buf));
    if (!fgets(buf, sizeof(buf) - 1, fp)) break;
    i = 0;
    while (buf[i]) {
      if (buf[i] == '#') 
        buf[i] = '\0';
      else
        i++;
    }
    while (i > 0 && buf[i-1] < 32) buf[--i] = 0;

    /* Process mux */
    switch (*buf) {
      case 'A':
      case 'C':
      case 'T':
      case 'S':
        _muxes_load_one(net, buf);
      default:
        break;
    }
  }
}

/*
 * Process directory
 *
 * Note: should we follow symlinks?
 */
static void _muxes_load_dir ( const char *path, const char *type )
{
  DIR *dir;
  struct dirent *dent;
  struct stat st;

  if (!(dir = opendir(path))) return;
  
  while ((dent = readdir(dir))) {
    if (*dent->d_name == '.') continue;
    char p[256];
    sprintf(p, "%s/%s", path, dent->d_name);
    if (stat(p, &st)) continue;
    if (S_ISDIR(st.st_mode))
      _muxes_load_dir(p, dent->d_name);
    else
      _muxes_load_file(type, p);
  }

  closedir(dir);
}

/*
 * Initialise the mux list
 */
void muxes_init ( const char *path )
{
  /* TODO: Default */
  if (!path) return;

  /* Process */
  _muxes_load_dir(path, NULL);
} 
