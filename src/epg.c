/*
 *  Electronic Program Guide - Common functions
 *  Copyright (C) 2007 Andreas �man
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

/*
 * TODO: list:
 *
 * - sanity checks on _destroy calls could just do what we're told and
 * unreference all links?
 *
 * - does the broadcast <-> channel referencing need to be 2 way?
 *   i.e. do we need to hold onto the CHANNEL info for DVR held broadcasts?
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include <assert.h>

#include "tvheadend.h"
#include "queue.h"
#include "channels.h"
#include "settings.h"
#include "epg.h"
#include "dvr/dvr.h"
#include "htsp.h"
#include "htsmsg_binary.h"
#include "epggrab.h"

/* Element lists */
struct epg_object_tree epg_brands;
struct epg_object_tree epg_seasons;
struct epg_object_tree epg_episodes;
struct epg_object_tree epg_channels;
struct epg_object_tree epg_broadcasts;

/* Unmapped */
LIST_HEAD(epg_channel_unmapped_list, epg_channel);
LIST_HEAD(channel_unmapped_list, channel);
struct epg_channel_unmapped_list epg_channel_unmapped;
struct channel_unmapped_list     channel_unmapped;

/* Unreferenced */
LIST_HEAD(epg_object_unref_list, epg_object);
struct epg_object_unref_list epg_object_unref;

/* Global counters */
static uint64_t _epg_object_idx    = 0;

/* **************************************************************************
 * Comparators
 * *************************************************************************/

static int _uri_cmp ( const void *a, const void *b )
{
  return strcmp(((epg_object_t*)a)->uri, ((epg_object_t*)b)->uri);
}

static int _id_cmp ( const void *a, const void *b )
{
  return ((epg_object_t*)a)->id - ((epg_object_t*)b)->id;
}

static int _ebc_win_cmp ( const void *a, const void *b )
{
  if ( ((epg_broadcast_t*)a)->start < ((epg_broadcast_t*)b)->start ) return -1;
  if ( ((epg_broadcast_t*)a)->start >= ((epg_broadcast_t*)b)->stop  ) return 1;
  return 0;
}

static int _epg_channel_cmp ( epg_channel_t *ec, channel_t *ch )
{
  int ret = 0;
  if ( ec->name && !strcmp(ec->name, ch->ch_name) ) ret = 1;
  return ret;
}

/* **************************************************************************
 * Setup / Update
 * *************************************************************************/

static int _epg_write ( int fd, htsmsg_t *m )
{
  int ret = 1;
  size_t msglen;
  void *msgdata;
  if (m) {
    int r = htsmsg_binary_serialize(m, &msgdata, &msglen, 0x10000);
    htsmsg_destroy(m);
    if (!r) {
      ssize_t w = write(fd, msgdata, msglen);
      free(msgdata);
      if(w == msglen) ret = 0;
    }
  }
  if(ret) {
    tvhlog(LOG_DEBUG, "epg", "failed to store epg to disk");
    close(fd);
    hts_settings_remove("epgdb");
  }
  return ret;
}

static int _epg_write_sect ( int fd, const char *sect )
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "__section__", sect);
  return _epg_write(fd, m);
}

void epg_save ( void )
{
  int fd;
  epg_object_t  *eo, *ec;
  epggrab_stats_t stats;
  
  fd = hts_settings_open_file(1, "epgdb");

  /* Channels */
  memset(&stats, 0, sizeof(stats));
  if ( _epg_write_sect(fd, "channels") ) return;
  RB_FOREACH(eo,  &epg_channels, glink) {
    if (_epg_write(fd, epg_channel_serialize((epg_channel_t*)eo))) return;
    stats.channels.total++;
  }
  if ( _epg_write_sect(fd, "brands") ) return;
  RB_FOREACH(eo,  &epg_brands, glink) {
    if (_epg_write(fd, epg_brand_serialize((epg_brand_t*)eo))) return;
    stats.brands.total++;
  }
  if ( _epg_write_sect(fd, "seasons") ) return;
  RB_FOREACH(eo,  &epg_seasons, glink) {
    if (_epg_write(fd, epg_season_serialize((epg_season_t*)eo))) return;
    stats.seasons.total++;
  }
  if ( _epg_write_sect(fd, "episodes") ) return;
  RB_FOREACH(eo,  &epg_episodes, glink) {
    if (_epg_write(fd, epg_episode_serialize((epg_episode_t*)eo))) return;
    stats.episodes.total++;
  }
  if ( _epg_write_sect(fd, "broadcasts") ) return;
  RB_FOREACH(ec, &epg_channels, glink) {
    RB_FOREACH(eo, &((epg_channel_t*)ec)->schedule, glink) {
      if (_epg_write(fd, epg_broadcast_serialize((epg_broadcast_t*)eo))) return;
      stats.broadcasts.total++;
    }
  }

  /* Stats */
  tvhlog(LOG_DEBUG, "epg", "database saved");
  tvhlog(LOG_DEBUG, "epg", "  channels   %d", stats.channels.total);
  tvhlog(LOG_DEBUG, "epg", "  brands     %d", stats.brands.total);
  tvhlog(LOG_DEBUG, "epg", "  seasons    %d", stats.seasons.total);
  tvhlog(LOG_DEBUG, "epg", "  episodes   %d", stats.episodes.total);
  tvhlog(LOG_DEBUG, "epg", "  broadcasts %d", stats.broadcasts.total);
}

void epg_init ( void )
{
  int save, fd;
  struct stat st;
  size_t remain;
  uint8_t *mem, *rp;
  char *sect = NULL;
  const char *s;
  epggrab_stats_t stats;
  
  /* Map file to memory */
  fd = hts_settings_open_file(0, "epgdb");
  if ( fd < 0 ) {
    tvhlog(LOG_DEBUG, "epg", "database does not exist");
    return;
  }
  if ( fstat(fd, &st) != 0 ) {
    tvhlog(LOG_ERR, "epg", "failed to detect database size");
    return;
  }
  if ( !st.st_size ) {
    tvhlog(LOG_DEBUG, "epg", "database is empty");
    return;
  }
  remain   = st.st_size;
  rp = mem = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if ( mem == MAP_FAILED ) {
    tvhlog(LOG_ERR, "epg", "failed to mmap database");
    return;
  }

  /* Process */
  memset(&stats, 0, sizeof(stats));
  while ( remain > 4 ) {

    // TODO: would be nice if htsmsg_binary handled this for us!

    /* Get message length */
    int msglen = (rp[0] << 24) | (rp[1] << 16) | (rp[2] << 8) | rp[3];
    remain    -= 4;
    rp        += 4;

    /* Extract message */
    htsmsg_t *m = htsmsg_binary_deserialize(rp, msglen, NULL);

    /* Process */
    if(m) {

      /* New section */
      s = htsmsg_get_str(m, "__section__");
      if (s) {
        if (sect) free(sect);
        sect = strdup(s);

      /* Channel */
      } else if ( !strcmp(sect, "channels") ) {
        if (epg_channel_deserialize(m, 1, &save)) stats.channels.total++;

      /* Brand */
      } else if ( !strcmp(sect, "brands") ) {
        if (epg_brand_deserialize(m, 1, &save)) stats.brands.total++;
        
      /* Season */
      } else if ( !strcmp(sect, "seasons") ) {
        if (epg_season_deserialize(m, 1, &save)) stats.seasons.total++;

      /* Episode */
      } else if ( !strcmp(sect, "episodes") ) {
        if (epg_episode_deserialize(m, 1, &save)) stats.episodes.total++;
  
      /* Broadcasts */
      } else if ( !strcmp(sect, "broadcasts") ) {
        if (epg_broadcast_deserialize(m, 1, &save)) stats.broadcasts.total++;

      /* Unknown */
      } else {
        tvhlog(LOG_DEBUG, "epg", "malformed database section [%s]", sect);
        //htsmsg_print(m);
      }

      /* Cleanup */
      htsmsg_destroy(m);
    }

    /* Next */
    rp     += msglen;
    remain -= msglen;
  }
  if (sect) free(sect);

  /* Stats */
  tvhlog(LOG_DEBUG, "epg", "database loaded");
  tvhlog(LOG_DEBUG, "epg", "  channels   %d", stats.channels.total);
  tvhlog(LOG_DEBUG, "epg", "  brands     %d", stats.brands.total);
  tvhlog(LOG_DEBUG, "epg", "  seasons    %d", stats.seasons.total);
  tvhlog(LOG_DEBUG, "epg", "  episodes   %d", stats.episodes.total);
  tvhlog(LOG_DEBUG, "epg", "  broadcasts %d", stats.broadcasts.total);
  tvhlog(LOG_DEBUG, "epg", "next object id %lu", _epg_object_idx);

  /* Close file */
  munmap(mem, st.st_size);
  close(fd);
}

void epg_updated ( void )
{
  epg_object_t *eo;
  while ((eo = LIST_FIRST(&epg_object_unref))) {
    tvhlog(LOG_DEBUG, "epg",
           "unref'd object %lu (%s) created during update", eo->id, eo->uri);
    LIST_REMOVE(eo, ulink);
    eo->destroy(eo);
  }
}

/* **************************************************************************
 * Object
 * *************************************************************************/

static void _epg_object_destroy ( epg_object_t *eo, epg_object_tree_t *tree )
{
  if (eo->uri) free(eo->uri);
  if (tree) RB_REMOVE(tree, eo, glink);
}

static void _epg_object_getref ( epg_object_t *eo )
{
  if (eo->refcount == 0) LIST_REMOVE(eo, ulink);
  eo->refcount++;
}

static void _epg_object_putref ( epg_object_t *eo )
{
  assert(eo->refcount>0); // Sanity!
  eo->refcount--;
  if (!eo->refcount) eo->destroy(eo);
}

static epg_object_t *_epg_object_find
  ( int create, int *save, epg_object_tree_t *tree,
    epg_object_t **skel, int (*cmp) (const void*,const void*))
{
  epg_object_t *eo;

  /* Find */
  if ( !create ) {
    eo = RB_FIND(tree, *skel, glink, cmp);
  
  /* Create */
  } else {
    eo = RB_INSERT_SORTED(tree, *skel, glink, cmp);
    if ( eo == NULL ) {
      *save     |= 1;
      eo         = *skel;
      *skel      = NULL;
      if (!eo->getref) eo->getref = _epg_object_getref;
      if (!eo->putref) eo->putref = _epg_object_putref;
      _epg_object_idx++;
      LIST_INSERT_HEAD(&epg_object_unref, eo, ulink);
    }
  }

  return eo;
}

static epg_object_t *_epg_object_find_by_uri 
  ( const char *uri, int create, int *save,
    epg_object_tree_t *tree, epg_object_t **skel )
{
  int save2 = 0;
  epg_object_t *eo;
  
  assert(skel != NULL);
  lock_assert(&global_lock); // pointless!

  (*skel)->uri = (char*)uri;
  (*skel)->id  = _epg_object_idx;

  eo = _epg_object_find(create, &save2, tree, skel, _uri_cmp);
  if (save2) {
    eo->uri = strdup(uri);
    *save  |= 1;
  }
  return eo;
}

static epg_object_t *_epg_object_find_by_id 
  ( uint64_t id, epg_object_tree_t *tree )
{
  epg_object_t *eo;
  if (!tree) return NULL;
  RB_FOREACH(eo, tree, glink) {
    if ( eo->id == id ) return eo;
  }
  return NULL;
}

/* **************************************************************************
 * Brand
 * *************************************************************************/

static void _epg_brand_destroy ( epg_object_t *eo )
{
  epg_brand_t *eb = (epg_brand_t*)eo;
  if (RB_FIRST(&eb->seasons)) {
    tvhlog(LOG_CRIT, "epg", "attempt to destroy brand with seasons");
    assert(0);
  }
  if (RB_FIRST(&eb->episodes)) {
    tvhlog(LOG_CRIT, "epg", "attempt to destroy brand with episodes");
    assert(0);
  }
  _epg_object_destroy(eo, &epg_brands);
  if (eb->title)   free(eb->title);
  if (eb->summary) free(eb->summary);
  free(eb);
}

epg_brand_t* epg_brand_find_by_uri 
  ( const char *uri, int create, int *save )
{
  static epg_object_t *skel = NULL;
  if ( !skel ) {
    skel          = calloc(1, sizeof(epg_brand_t));
    skel->destroy = _epg_brand_destroy;
  }
  return (epg_brand_t*)
    _epg_object_find_by_uri(uri, create, save,
                            &epg_brands, &skel);
}

epg_brand_t *epg_brand_find_by_id ( uint64_t id )
{
  return (epg_brand_t*)_epg_object_find_by_id(id, &epg_brands);
}

int epg_brand_set_title ( epg_brand_t *brand, const char *title )
{
  int save = 0;
  if ( !brand || !title ) return 0;
  if ( !brand->title || strcmp(brand->title, title) ) {
    if ( brand->title ) free(brand->title);
    brand->title = strdup(title);
    save = 1;
  }
  return save;
}

int epg_brand_set_summary ( epg_brand_t *brand, const char *summary )
{
  int save = 0;
  if ( !brand || !summary ) return 0;
  if ( !brand->summary || strcmp(brand->summary, summary) ) {
    if ( brand->summary ) free(brand->summary);
    brand->summary = strdup(summary);
    save = 1;
  }
  return save;
}

int epg_brand_set_season_count ( epg_brand_t *brand, uint16_t count )
{
  int save = 0;
  if ( !brand || !count ) return 0;
  if ( brand->season_count != count ) {
    brand->season_count = count;
    save = 1;
  }
  return save;
}

static void _epg_brand_add_season 
  ( epg_brand_t *brand, epg_season_t *season )
{
  RB_INSERT_SORTED(&brand->seasons, season, blink, _uri_cmp);
}

static void _epg_brand_rem_season
  ( epg_brand_t *brand, epg_season_t *season )
{
  RB_REMOVE(&brand->seasons, season, blink);
}

static void _epg_brand_add_episode
  ( epg_brand_t *brand, epg_episode_t *episode )
{
  RB_INSERT_SORTED(&brand->episodes, episode, blink, _uri_cmp);
}

static void _epg_brand_rem_episode
  ( epg_brand_t *brand, epg_episode_t *episode )
{
  RB_REMOVE(&brand->episodes, episode, blink);
}

htsmsg_t *epg_brand_serialize ( epg_brand_t *brand )
{
  htsmsg_t *m;
  if ( !brand || !brand->_.uri ) return NULL;
  m = htsmsg_create_map();
  // TODO: generic serialize?
  htsmsg_add_str(m, "uri", brand->_.uri);
  if (brand->title)
    htsmsg_add_str(m, "title",   brand->title);
  if (brand->summary)
    htsmsg_add_str(m, "summary", brand->summary);
  if (brand->season_count)
    htsmsg_add_u32(m, "season-count", brand->season_count);
  return m;
}

epg_brand_t *epg_brand_deserialize ( htsmsg_t *m, int create, int *save )
{
  epg_brand_t *eb;
  uint32_t u32;
  const char *str;
  
  // TODO: generic deserialize?
  if ( !(str = htsmsg_get_str(m, "uri"))                ) return NULL;
  if ( !(eb = epg_brand_find_by_uri(str, create, save)) ) return NULL;
  
  if ( (str = htsmsg_get_str(m, "title")) )
    *save |= epg_brand_set_title(eb, str);
  if ( (str = htsmsg_get_str(m, "summary")) )
    *save |= epg_brand_set_summary(eb, str);
  if ( !htsmsg_get_u32(m, "season-count", &u32) )
    *save |= epg_brand_set_season_count(eb, u32);

  return eb;
}

/* **************************************************************************
 * Season
 * *************************************************************************/

static void _epg_season_destroy ( epg_object_t *eo )
{
  epg_season_t *es = (epg_season_t*)eo;
  if (RB_FIRST(&es->episodes)) {
    tvhlog(LOG_CRIT, "epg", "attempt to destory season with episodes");
    assert(0);
  }
  _epg_object_destroy(eo, &epg_seasons);
  if (es->brand) {
    _epg_brand_rem_season(es->brand, es);
    es->brand->_.putref((epg_object_t*)es->brand);
  }
  if (es->summary) free(es->summary);
  free(es);
}

epg_season_t* epg_season_find_by_uri 
  ( const char *uri, int create, int *save )
{
  static epg_object_t *skel = NULL;
  if ( !skel ) {
    skel          = calloc(1, sizeof(epg_season_t));
    skel->destroy = _epg_season_destroy;
  }
  return (epg_season_t*)
    _epg_object_find_by_uri(uri, create, save,
                            &epg_seasons, &skel);
}

epg_season_t *epg_season_find_by_id ( uint64_t id )
{
  return (epg_season_t*)_epg_object_find_by_id(id, &epg_seasons);
}

int epg_season_set_summary ( epg_season_t *season, const char *summary )
{
  int save = 0;
  if ( !season || !summary ) return 0;
  if ( !season->summary || strcmp(season->summary, summary) ) {
    if ( season->summary ) free(season->summary);
    season->summary = strdup(summary);
    save = 1;
  }
  return save;
}

int epg_season_set_episode_count ( epg_season_t *season, uint16_t count )
{
  int save = 0;
  if ( !season || !count ) return 0;
  if ( season->episode_count != count ) {
    season->episode_count = count;
    save = 1;
  }
  return save;
}

int epg_season_set_number ( epg_season_t *season, uint16_t number )
{
  int save = 0;
  if ( !season || !number ) return 0;
  if ( season->number != number ) {
    season->number = number;
    save = 1;
  }
  return save;
}

int epg_season_set_brand ( epg_season_t *season, epg_brand_t *brand, int u )
{
  int save = 0;
  if ( !season || !brand ) return 0;
  if ( season->brand != brand ) {
    if ( season->brand ) {
      _epg_brand_rem_season(season->brand, season);
      season->brand->_.putref((epg_object_t*)season->brand);
    }
    season->brand = brand;
    _epg_brand_add_season(brand, season);
    brand->_.getref((epg_object_t*)brand);
    save = 1;
  }
  return save;
}

static void _epg_season_add_episode
  ( epg_season_t *season, epg_episode_t *episode )
{
  RB_INSERT_SORTED(&season->episodes, episode, slink, _uri_cmp);
}

static void _epg_season_rem_episode
  ( epg_season_t *season, epg_episode_t *episode )
{
  RB_REMOVE(&season->episodes, episode, slink);
}

htsmsg_t *epg_season_serialize ( epg_season_t *season )
{
  htsmsg_t *m;
  if (!season || !season->_.uri) return NULL;
  m = htsmsg_create_map();
  // TODO: generic
  htsmsg_add_str(m, "uri", season->_.uri);
  if (season->summary)
    htsmsg_add_str(m, "summary", season->summary);
  if (season->number)
    htsmsg_add_u32(m, "number", season->number);
  if (season->episode_count)
    htsmsg_add_u32(m, "episode-count", season->episode_count);
  if (season->brand)
    htsmsg_add_str(m, "brand", season->brand->_.uri);
  return m;
}

epg_season_t *epg_season_deserialize ( htsmsg_t *m, int create, int *save )
{
  epg_season_t *es;
  epg_brand_t *eb;
  uint32_t u32;
  const char *str;

  // TODO: generic
  if ( !(str = htsmsg_get_str(m, "uri")) )                  return NULL;
  if ( !(es  = epg_season_find_by_uri(str, create, save)) ) return NULL;
  
  if ( (str = htsmsg_get_str(m, "summary")) )
    *save |= epg_season_set_summary(es, str);
  if ( !htsmsg_get_u32(m, "number", &u32) )
    *save |= epg_season_set_number(es, u32);
  if ( !htsmsg_get_u32(m, "episode-count", &u32) )
    *save |= epg_season_set_episode_count(es, u32);
  
  if ( (str = htsmsg_get_str(m, "brand")) )
    if ( (eb = epg_brand_find_by_uri(str, 0, NULL)) )
      *save |= epg_season_set_brand(es, eb, 1);

  return es;
}

/* **************************************************************************
 * Episode
 * *************************************************************************/

static void _epg_episode_destroy ( epg_object_t *eo )
{
  epg_episode_t *ee = (epg_episode_t*)eo;
  if (RB_FIRST(&ee->broadcasts)) {
    tvhlog(LOG_CRIT, "epg", "attempt to destroy episode with broadcasts");
    assert(0);
  }
  _epg_object_destroy(eo, &epg_episodes);
  if (ee->brand) {
    _epg_brand_rem_episode(ee->brand, ee);
    ee->brand->_.putref((epg_object_t*)ee->brand);
  }
  if (ee->season) {
    _epg_season_rem_episode(ee->season, ee);
    ee->season->_.putref((epg_object_t*)ee->season);
  }
  if (ee->title)       free(ee->title);
  if (ee->subtitle)    free(ee->subtitle);
  if (ee->summary)     free(ee->summary);
  if (ee->description) free(ee->description);
  free(ee);
}

epg_episode_t* epg_episode_find_by_uri
  ( const char *uri, int create, int *save )
{
  static epg_object_t *skel = NULL;
  if ( !skel ) {
    skel          = calloc(1, sizeof(epg_episode_t));
    skel->destroy = _epg_episode_destroy;
  }
  return (epg_episode_t*)
    _epg_object_find_by_uri(uri, create, save,
                            &epg_episodes, &skel);
}

epg_episode_t *epg_episode_find_by_id ( uint64_t id )
{
  return (epg_episode_t*)_epg_object_find_by_id(id, &epg_episodes);
}

int epg_episode_set_title ( epg_episode_t *episode, const char *title )
{
  int save = 0;
  if ( !episode || !title ) return 0;
  if ( !episode->title || strcmp(episode->title, title) ) {
    if ( episode->title ) free(episode->title);
    episode->title = strdup(title);
    save = 1;
  }
  return save;
}

int epg_episode_set_subtitle ( epg_episode_t *episode, const char *subtitle )
{
  int save = 0;
  if ( !episode || !subtitle ) return 0;
  if ( !episode->subtitle || strcmp(episode->subtitle, subtitle) ) {
    if ( episode->subtitle ) free(episode->subtitle);
    episode->subtitle = strdup(subtitle);
    save = 1;
  }
  return save;
}

int epg_episode_set_summary ( epg_episode_t *episode, const char *summary )
{
  int save = 0;
  if ( !episode || !summary ) return 0;
  if ( !episode->summary || strcmp(episode->summary, summary) ) {
    if ( episode->summary ) free(episode->summary);
    episode->summary = strdup(summary);
    save = 1;
  }
  return save;
}

int epg_episode_set_description ( epg_episode_t *episode, const char *desc )
{
  int save = 0;
  if ( !episode || !desc ) return 0;
  if ( !episode->description || strcmp(episode->description, desc) ) {
    if ( episode->description ) free(episode->description);
    episode->description = strdup(desc);
    save = 1;
  }
  return save;
}

int epg_episode_set_number ( epg_episode_t *episode, uint16_t number )
{
  int save = 0;
  if ( !episode || !number ) return 0;
  if ( episode->number != number ) {
    episode->number = number;
    save = 1;
  }
  return save;
}

int epg_episode_set_part ( epg_episode_t *episode, uint16_t part, uint16_t count )
{
  int save = 0;
  // TODO: could treat part/count independently
  if ( !episode || !part || !count ) return 0;
  if ( episode->part_number != part ) {
    episode->part_number = part;
    save |= 1;
  }
  if ( episode->part_count != count ) {
    episode->part_count = count;
    save |= 1;
  }
  return save;
}

int epg_episode_set_brand ( epg_episode_t *episode, epg_brand_t *brand )
{
  int save = 0;
  if ( !episode || !brand ) return 0;
  if ( episode->brand != brand ) {
    if ( episode->brand ) {
      _epg_brand_rem_episode(episode->brand, episode);
      episode->brand->_.putref((epg_object_t*)episode->brand);
    }
    episode->brand = brand;
    _epg_brand_add_episode(brand, episode);
    brand->_.getref((epg_object_t*)brand);
    save = 1;
  }
  return save;
}

int epg_episode_set_season ( epg_episode_t *episode, epg_season_t *season )
{
  int save = 0;
  if ( !episode || !season ) return 0;
  if ( episode->season != season ) {
    if ( episode->season ) {
      _epg_season_rem_episode(episode->season, episode);
      episode->season->_.putref((epg_object_t*)episode->season);
    }
    episode->season = season;
    _epg_season_add_episode(season, episode);
    season->_.getref((epg_object_t*)season);
    if ( season->brand ) save |= epg_episode_set_brand(episode, season->brand);
    save = 1;
  }
  return save;
}

static void _epg_episode_add_broadcast 
  ( epg_episode_t *episode, epg_broadcast_t *broadcast )
{
  RB_INSERT_SORTED(&episode->broadcasts, broadcast, elink, _id_cmp);
}

static void _epg_episode_rem_broadcast
  ( epg_episode_t *episode, epg_broadcast_t *broadcast )
{
  RB_REMOVE(&episode->broadcasts, broadcast, elink);
}


size_t epg_episode_number_format 
  ( epg_episode_t *episode, char *buf, size_t len,
    const char *pre,  const char *sfmt,
    const char *sep,  const char *efmt,
    const char *cfmt )
{
  size_t i = 0;
  if ( episode->number ) {
    if (pre) i += snprintf(&buf[i], len-i, "%s", pre);
    if ( sfmt && episode->season && episode->season->number ) {
      i += snprintf(&buf[i], len-i, sfmt, episode->season->number);
      if ( cfmt && episode->brand && episode->brand->season_count )
        i += snprintf(&buf[i], len-i, cfmt,
                      episode->brand->season_count);
      if (sep) i += snprintf(&buf[i], len-i, "%s", sep);
    }
    i += snprintf(&buf[i], len-i, efmt, episode->number);
    if ( cfmt && episode->season && episode->season->episode_count)
      i+= snprintf(&buf[i], len-i, cfmt, episode->season->episode_count);
  }
  return i;
}

htsmsg_t *epg_episode_serialize ( epg_episode_t *episode )
{
  htsmsg_t *m;
  if (!episode || !episode->_.uri) return NULL;
  m = htsmsg_create_map();
  htsmsg_add_str(m, "uri", episode->_.uri);
  if (episode->title)
    htsmsg_add_str(m, "title", episode->title);
  if (episode->subtitle)
    htsmsg_add_str(m, "subtitle", episode->subtitle);
  if (episode->summary)
    htsmsg_add_str(m, "summary", episode->summary);
  if (episode->description)
    htsmsg_add_str(m, "description", episode->description);
  if (episode->number)
    htsmsg_add_u32(m, "number", episode->number);
  if (episode->part_count && episode->part_count) {
    htsmsg_add_u32(m, "part-number", episode->part_number);
    htsmsg_add_u32(m, "part-count", episode->part_count);
  }
  if (episode->brand)
    htsmsg_add_str(m, "brand", episode->brand->_.uri);
  if (episode->season)
    htsmsg_add_str(m, "season", episode->season->_.uri);
  return m;
}

epg_episode_t *epg_episode_deserialize ( htsmsg_t *m, int create, int *save )
{
  epg_episode_t *ee;
  epg_season_t *es;
  epg_brand_t *eb;
  uint32_t u32, u32a;
  const char *str;
  
  if ( !(str = htsmsg_get_str(m, "uri")) )                   return NULL;
  if ( !(ee  = epg_episode_find_by_uri(str, create, save)) ) return NULL;

  if ( (str = htsmsg_get_str(m, "title")) )
    *save |= epg_episode_set_title(ee, str);
  if ( (str = htsmsg_get_str(m, "subtitle")) )
    *save |= epg_episode_set_subtitle(ee, str);
  if ( (str = htsmsg_get_str(m, "summary")) )
    *save |= epg_episode_set_summary(ee, str);
  if ( (str = htsmsg_get_str(m, "description")) )
    *save |= epg_episode_set_description(ee, str);
  if ( !htsmsg_get_u32(m, "number", &u32) )
    *save |= epg_episode_set_number(ee, u32);
  if ( !htsmsg_get_u32(m, "part-number", &u32) &&
       !htsmsg_get_u32(m, "part-count", &u32a) )
    *save |= epg_episode_set_part(ee, u32, u32a);
  
  if ( (str = htsmsg_get_str(m, "brand")) )
    if ( (eb = epg_brand_find_by_uri(str, 0, NULL)) )
      *save |= epg_episode_set_brand(ee, eb);
  if ( (str = htsmsg_get_str(m, "season")) )
    if ( (es = epg_season_find_by_uri(str, 0, NULL)) )
      *save |= epg_episode_set_season(ee, es);
  
  return ee;
}

/* **************************************************************************
 * Broadcast
 * *************************************************************************/

static void _epg_broadcast_destroy ( epg_object_t *eo )
{
  epg_broadcast_t *ebc = (epg_broadcast_t*)eo;
  _epg_object_destroy(eo, NULL);
  if (ebc->episode) {
    _epg_episode_rem_broadcast(ebc->episode, ebc);
    ebc->episode->_.putref((epg_object_t*)ebc->episode);
  }
  free(ebc);
}

epg_broadcast_t* epg_broadcast_find_by_time 
  ( epg_channel_t *channel, time_t start, time_t stop, int create, int *save )
{
  return epg_channel_get_broadcast(channel, start, stop, create, save);
}

// TODO: allow optional channel parameter?
epg_broadcast_t *epg_broadcast_find_by_id ( uint64_t id, epg_channel_t *ec )
{
  epg_object_t *eo = NULL, *eoc;
  if ( ec ) {
    eo = _epg_object_find_by_id(id, &((epg_channel_t*)ec)->schedule);
  } else {
    RB_FOREACH(eoc, &epg_channels, glink) {
      eo = _epg_object_find_by_id(id, &((epg_channel_t*)eoc)->schedule);
      if (eo) break;
    }
  }
  return (epg_broadcast_t*)eo;
}

int epg_broadcast_set_episode 
  ( epg_broadcast_t *broadcast, epg_episode_t *episode )
{
  int save = 0;
  if ( !broadcast || !episode ) return 0;
  if ( broadcast->episode != episode ) {
    if ( broadcast->episode ) {
      _epg_episode_rem_broadcast(broadcast->episode, broadcast);
      broadcast->episode->_.putref((epg_object_t*)broadcast->episode);
    }
    _epg_episode_add_broadcast(episode, broadcast);
    broadcast->episode = episode;
    episode->_.getref((epg_object_t*)episode);
    save = 1;
  }
  return save;
}

epg_broadcast_t *epg_broadcast_get_next ( epg_broadcast_t *broadcast )
{
  if ( !broadcast ) return NULL;
  return (epg_broadcast_t*)RB_NEXT((epg_object_t*)broadcast, glink);
}

htsmsg_t *epg_broadcast_serialize ( epg_broadcast_t *broadcast )
{
  htsmsg_t *m;
  if (!broadcast) return NULL;
  if (!broadcast->channel || !broadcast->channel->_.uri) return NULL;
  if (!broadcast->episode || !broadcast->episode->_.uri) return NULL;
  m = htsmsg_create_map();

  htsmsg_add_u32(m, "id", broadcast->_.id);
  htsmsg_add_u32(m, "start", broadcast->start);
  htsmsg_add_u32(m, "stop", broadcast->stop);
  if (broadcast->channel)
    htsmsg_add_str(m, "channel", broadcast->channel->_.uri);
  htsmsg_add_str(m, "episode", broadcast->episode->_.uri);
  
  if (broadcast->dvb_id)
    htsmsg_add_u32(m, "dvb-id", broadcast->dvb_id);
  // TODO: add other metadata fields
  return m;
}

epg_broadcast_t *epg_broadcast_deserialize
  ( htsmsg_t *m, int create, int *save )
{
  epg_broadcast_t *ebc;
  epg_channel_t *ec;
  epg_episode_t *ee;
  const char *str;
  uint32_t start, stop;
  uint64_t id;

  // TODO: need to handle broadcasts without a channel
  //       this will happen for DVR maintained broadcasts which
  //       reference a no longer used channel (and also they will be
  //       outside the time limit).
  if ( htsmsg_get_u64(m, "id", &id)                   ) return NULL;
  if ( htsmsg_get_u32(m, "start", &start)             ) return NULL;
  if ( htsmsg_get_u32(m, "stop", &stop)               ) return NULL;
  if ( !(str = htsmsg_get_str(m, "channel"))          ) return NULL;
  if ( !(ec  = epg_channel_find_by_uri(str, 0, NULL)) ) return NULL;
  if ( !(str = htsmsg_get_str(m, "episode"))          ) return NULL;
  if ( !(ee  = epg_episode_find_by_uri(str, 0, NULL)) ) return NULL;

  ebc = epg_broadcast_find_by_time(ec, start, stop, create, save);
  if ( !ebc ) return NULL;

  *save |= epg_broadcast_set_episode(ebc, ee);

  /* Bodge the ID - keep them the same */
  ebc->_.id = id;
  if ( id >= _epg_object_idx ) _epg_object_idx = id + 1;

#if TODO_BROADCAST_METADATA
  if ( !htsmsg_get_u32(m, "dvb-id", &u32) )
    save |= epg_broadcast_set_dvb_id(ebc, u32);
  // TODO: more metadata
#endif

  return ebc;
}

/* **************************************************************************
 * Channel
 * *************************************************************************/

static void _epg_channel_timer_callback ( void *p )
{
  time_t next = 0;
  epg_object_t *eo;
  epg_broadcast_t *ebc, *cur;
  epg_channel_t *ec = (epg_channel_t*)p;

  /* Clear now/next */
  cur = ec->now;
  ec->now = ec->next = NULL;

  /* Check events */
  while ( (eo = RB_FIRST(&ec->schedule)) ) {
    ebc = (epg_broadcast_t*)eo;

    /* Expire */
    if ( ebc->stop <= dispatch_clock ) {
      RB_REMOVE(&ec->schedule, eo, glink);
      eo->putref(eo);
      tvhlog(LOG_DEBUG, "epg", "expire event %lu from %s",
             eo->id, ec->_.uri);
      continue; // skip to next

    /* No now */
    } else if ( ebc->start > dispatch_clock ) {
      ec->next = ebc;
      next     = ebc->start;

    /* Now/Next */
    } else {
      ec->now  = ebc;
      ec->next = (epg_broadcast_t*)RB_NEXT(eo, glink);
      next     = ebc->stop;
    }
    break;
  }
  tvhlog(LOG_DEBUG, "epg", "now/next %lu/%lu set on %s",
         ec->now ? ec->now->_.id : 0,
         ec->next ? ec->next->_.id : 0,
         ec->_.uri);

  /* re-arm */
  if ( next ) {
    tvhlog(LOG_DEBUG, "epg", "arm channel timer @ %lu for %s",
           next, ec->_.uri);
    gtimer_arm_abs(&ec->expire, _epg_channel_timer_callback, ec, next);
  }

  /* Update HTSP */
  if ( (cur != ec->now) && ec->channel ) {
    tvhlog(LOG_DEBUG, "epg", "inform HTSP of now event change on %s",
           ec->_.uri);
    htsp_channel_update_current(ec->channel);
  }
}

static void _epg_channel_destroy ( epg_object_t *eo )
{
  epg_object_t *ebc;
  epg_channel_t *ec = (epg_channel_t*)eo;
  if (ec->channel) {
    tvhlog(LOG_CRIT, "epg", "attempt to destroy mapped channel");
    assert(0);
  }
#if TODO_WHAT_SHOULD_BE_DONE
  if (RB_FIRST(&ec->schedule)) {
    tvhlog(LOG_CRIT, "epg", "attempt to destroy channel with schedule");
    assert(0);
  }
#endif
  _epg_object_destroy(eo, &epg_channels);
  // TODO: should we be doing this?
  while ((ebc = RB_FIRST(&ec->schedule))) {
    RB_REMOVE(&ec->schedule, ebc, glink);
    ebc->putref(ebc);
  }
  gtimer_disarm(&ec->expire);
  if (ec->name)  free(ec->name);
#if TODO_NOT_IMPLEMENTED
  if (ec->sname) free(ec->sname);
  if (ec->sid)   free(ec->sid);
#endif
  free(ec);
}

epg_channel_t* epg_channel_find_by_uri
  ( const char *uri, int create, int *save )
{
  int save2 = 0;
  static epg_object_t *skel = NULL;
  if ( !skel ) {
    skel          = calloc(1, sizeof(epg_channel_t));
    skel->destroy = _epg_channel_destroy;
  }
  epg_channel_t *ec = (epg_channel_t*)
    _epg_object_find_by_uri(uri, create, &save2,
                            &epg_channels, &skel);
  if (save2) {
    LIST_INSERT_HEAD(&epg_channel_unmapped, ec, umlink);
    *save |= 1;
  }
  return ec;
}

epg_channel_t *epg_channel_find_by_id ( uint64_t id )
{
  return (epg_channel_t*)_epg_object_find_by_id(id, &epg_channels);
}

int epg_channel_set_name ( epg_channel_t *channel, const char *name )
{
  int save = 0;
  channel_t *ch;
  if ( !channel || !name ) return 0;
  if ( !channel->name || strcmp(channel->name, name) ) {
    if (channel->name) free(channel->name);
    channel->name = strdup(name);
    // NOTE: does not remap
    if ( !channel->channel ) {
      LIST_FOREACH(ch, &channel_unmapped, ch_eulink) {
        if ( _epg_channel_cmp(channel, ch) ) {
          epg_channel_set_channel(channel, ch);
          break;
        }
      }
    }
    save |= 1;
  }
  return save;
}

int epg_channel_set_channel ( epg_channel_t *ec, channel_t *ch )
{
  int save = 0;
  if ( !ec ) return 0;
  if ( ec->channel != ch ) {
    if (ec->channel) {
      tvhlog(LOG_DEBUG, "epg", "unlink channels %-30s -> %s",
             ec->_.uri, ec->channel->ch_name);
      channel_set_epg_source(ec->channel, NULL);
      LIST_INSERT_HEAD(&channel_unmapped, ec->channel, ch_eulink);
    } else {
      LIST_REMOVE(ec, umlink);
    }
    ec->channel = ch;
    if (!ch) {
      LIST_INSERT_HEAD(&epg_channel_unmapped, ec, umlink);
      ec->_.putref((epg_object_t*)ec);
    } else {
      tvhlog(LOG_DEBUG, "epg", "link channels %-30s -> %s",
             ec->_.uri, ch->ch_name);
      channel_set_epg_source(ch, ec);
      ec->_.getref((epg_object_t*)ec);
    }
    save |= 1;
  }
  return save;
}

epg_broadcast_t *epg_channel_get_broadcast 
  ( epg_channel_t *channel, time_t start, time_t stop, int create, int *save )
{
  int save2 = 0;
  epg_broadcast_t *ebc;
  static epg_broadcast_t *skel;
  if ( !channel || !start || !stop ) return NULL;
  if ( stop <= start ) return NULL;
  if ( stop < dispatch_clock ) return NULL;

  if ( skel == NULL ) skel = calloc(1, sizeof(epg_broadcast_t));
  skel->channel   = channel;
  skel->start     = start;
  skel->stop      = stop;
  skel->_.id      = _epg_object_idx;
  skel->_.destroy = _epg_broadcast_destroy;

  ebc = (epg_broadcast_t*)
    _epg_object_find(create, &save2, &channel->schedule,
                     (epg_object_t**)&skel, _ebc_win_cmp);
  if (save2) {
    ebc->_.getref((epg_object_t*)ebc);
    
    /* New current/next */
    if ( (RB_FIRST(&channel->schedule) == (epg_object_t*)ebc) ||
         (channel->now &&
          RB_NEXT((epg_object_t*)channel->now, glink) == (epg_object_t*)ebc) ) {
      _epg_channel_timer_callback(channel);
    }
    *save |= 1;
  }
  return ebc;
}

htsmsg_t *epg_channel_serialize ( epg_channel_t *channel )
{
  htsmsg_t *m;
  if (!channel || !channel->_.uri) return NULL;
  m = htsmsg_create_map();
  htsmsg_add_str(m, "uri", channel->_.uri);
  if (channel->name)
    htsmsg_add_str(m, "name", channel->name);
  if (channel->channel)
    htsmsg_add_u32(m, "channel", channel->channel->ch_id);
  // TODO: other data
  return m;
}

epg_channel_t *epg_channel_deserialize ( htsmsg_t *m, int create, int *save )
{
  epg_channel_t *ec;
  channel_t *ch;
  uint32_t u32;
  const char *str;
  
  if ( !(str = htsmsg_get_str(m, "uri"))                   ) return NULL;
  if ( !(ec  = epg_channel_find_by_uri(str, create, save)) ) return NULL;

  if ( (str = htsmsg_get_str(m, "name")) )
    *save |= epg_channel_set_name(ec, str);

  if ( !htsmsg_get_u32(m, "channel", &u32) )
    if ( (ch = channel_find_by_identifier(u32)) )
      epg_channel_set_channel(ec, ch);
  // TODO: this call needs updating
      
  return ec;
}

/* **************************************************************************
 * Channel mapping
 * *************************************************************************/

void epg_channel_map_add ( channel_t *ch )
{
  epg_channel_t *ec;
  LIST_FOREACH(ec, &epg_channel_unmapped, umlink) {
    if ( _epg_channel_cmp(ec, ch) ) {
      epg_channel_set_channel(ec, ch);
      return;
    }
  }
  LIST_INSERT_HEAD(&channel_unmapped, ch, ch_eulink);
}

void epg_channel_map_rem ( channel_t *ch )
{
  if (ch->ch_epg_channel) {
    epg_channel_set_channel(ch->ch_epg_channel, NULL);
  } else {
    LIST_REMOVE(ch, ch_eulink);
  }
}

void epg_channel_map_mod ( channel_t *ch )
{
  // If already mapped, ignore
  if (!ch->ch_epg_channel) epg_channel_map_add(ch);
}


/* **************************************************************************
 * Querying
 * *************************************************************************/

static void _eqr_add ( epg_query_result_t *eqr, epg_broadcast_t *e )
{
  /* More space */
  if ( eqr->eqr_entries == eqr->eqr_alloced ) {
    eqr->eqr_alloced = MAX(100, eqr->eqr_alloced * 2);
    eqr->eqr_array   = realloc(eqr->eqr_array, 
                               eqr->eqr_alloced * sizeof(epg_broadcast_t));
  }
  
  /* Store */
  eqr->eqr_array[eqr->eqr_entries++] = e;
  // TODO: ref counting
}

static void _eqr_add_channel 
  ( epg_query_result_t *eqr, epg_channel_t *ec )
{
  epg_object_t *eo;
  epg_broadcast_t *ebc;
  RB_FOREACH(eo, &ec->schedule, glink) {
    ebc = (epg_broadcast_t*)eo;
    if ( ebc->episode && ebc->channel ) _eqr_add(eqr, ebc);
  }
}

void epg_query0
  ( epg_query_result_t *eqr, channel_t *channel, channel_tag_t *tag,
    uint8_t contentgroup, const char *title )
{
  epg_object_t *ec;

  /* Clear (just incase) */
  memset(eqr, 0, sizeof(epg_query_result_t));

  /* All channels */
  if (!channel) {
    RB_FOREACH(ec, &epg_channels, glink) {
      _eqr_add_channel(eqr, (epg_channel_t*)ec);
    }

  /* Single channel */
  } else if ( channel->ch_epg_channel ) {
    _eqr_add_channel(eqr, channel->ch_epg_channel);
  }

  return;
}

void epg_query(epg_query_result_t *eqr, const char *channel, const char *tag,
	       const char *contentgroup, const char *title)
{
  channel_t *ch = NULL;
  if (channel) ch = channel_find_by_name(channel, 0, 0);
  epg_query0(eqr, ch, NULL, 0, title);
}

void epg_query_free(epg_query_result_t *eqr)
{
  free(eqr->eqr_array);
  // TODO: reference counting
}

static int _epg_sort_start_ascending ( const void *a, const void *b )
{
  return (*(epg_broadcast_t**)a)->start - (*(epg_broadcast_t**)b)->start;
}

void epg_query_sort(epg_query_result_t *eqr)
{
  qsort(eqr->eqr_array, eqr->eqr_entries, sizeof(epg_broadcast_t*),
        _epg_sort_start_ascending);
}
