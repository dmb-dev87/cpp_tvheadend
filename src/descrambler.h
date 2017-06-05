/*
 *  Tvheadend
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

#ifndef __TVH_DESCRAMBLER_H__
#define __TVH_DESCRAMBLER_H__

#include <stdint.h>
#include <stdlib.h>
#include "queue.h"
#include "descrambler/tvhcsa.h"

struct service;
struct elementary_stream;
struct tvhcsa;
struct mpegts_table;
struct mpegts_mux;
struct th_descrambler_data;

#define DESCRAMBLER_NONE	0
/* 64-bit keys */
#define DESCRAMBLER_CSA_CBC	1
#define DESCRAMBLER_DES_NCB	2 /* no block cipher mode! */
#define DESCRAMBLER_AES_ECB	3
/* 128-bit keys */
#define DESCRAMBLER_AES128_ECB  16

#define DESCRAMBLER_KEY_SIZE(type) ((type >= DESCRAMBLER_AES128_ECB) ? 16 : 8)

/**
 * Descrambler superclass
 *
 * Created/Destroyed on per-transport basis upon transport start/stop
 */
typedef struct th_descrambler {
  LIST_ENTRY(th_descrambler) td_service_link;

  char *td_nicename;

  enum {
    DS_UNKNOWN,
    DS_RESOLVED,
    DS_FORBIDDEN,
    DS_IDLE
  } td_keystate;

  struct service *td_service;

  void (*td_stop)       (struct th_descrambler *d);
  void (*td_caid_change)(struct th_descrambler *d);
  int  (*td_ecm_reset)  (struct th_descrambler *d);
  void (*td_ecm_idle)   (struct th_descrambler *d);

} th_descrambler_t;

typedef struct th_descrambler_key {
  uint8_t  key_data[2][16]; /* 0 = even, 1 = odd, max 16-byte key */
  tvhcsa_t key_csa;
  uint16_t key_pid;         /* keys are assigned to this pid (when multipid is set) */
  uint64_t key_interval;
  uint64_t key_initial_interval;
  int64_t  key_start;
  int64_t  key_timestamp[2];
  uint8_t  key_index;
  uint8_t  key_valid;
  uint8_t  key_changed;
} th_descrambler_key_t;

typedef struct th_descrambler_runtime {
  struct service *dr_service;
  uint32_t dr_external:1;
  uint32_t dr_skip:1;
  uint32_t dr_quick_ecm:1;
  uint32_t dr_key_const:1;
  uint32_t dr_key_multipid:1;
  int64_t  dr_ecm_start[2];
  int64_t  dr_ecm_last_key_time;
  int64_t  dr_ecm_key_margin;
  int64_t  dr_last_err;
  int64_t  dr_force_skip;
  th_descrambler_key_t dr_keys[DESCRAMBLER_MAX_KEYS];
  TAILQ_HEAD(, th_descrambler_data) dr_queue;
  uint32_t dr_queue_total;
  tvhlog_limit_t dr_loglimit_key;
} th_descrambler_runtime_t;

typedef void (*descrambler_section_callback_t)
  (void *opaque, int pid, const uint8_t *section, int section_len, int emm);

/**
 * Track required PIDs
 */
typedef struct descrambler_ecmsec {
  LIST_ENTRY(descrambler_ecmsec) link;
  uint8_t   number;
  uint8_t  *last_data;
  int       last_data_len;
} descrambler_ecmsec_t;

typedef struct descrambler_section {
  TAILQ_ENTRY(descrambler_section) link;
  descrambler_section_callback_t callback;
  void     *opaque;
  LIST_HEAD(, descrambler_ecmsec) ecmsecs;
  uint8_t   quick_ecm_called;
} descrambler_section_t;

typedef struct descrambler_table {
  TAILQ_ENTRY(descrambler_table) link;
  struct mpegts_table *table;
  TAILQ_HEAD(,descrambler_section) sections;
} descrambler_table_t;

/**
 * List of CA ids
 */
#define CAID_REMOVE_ME ((uint16_t)-1)

typedef struct caid {
  LIST_ENTRY(caid) link;

  uint16_t pid;
  uint16_t caid;
  uint32_t providerid;
  uint8_t  use;
  uint8_t  filter;

} caid_t;

/**
 * List of EMM subscribers
 */
#define EMM_PID_UNKNOWN ((uint16_t)-1)

typedef struct descrambler_emm {
  TAILQ_ENTRY(descrambler_emm) link;

  uint16_t caid;
  uint16_t pid;
  unsigned int to_be_removed:1;

  descrambler_section_callback_t callback;
  void *opaque;
} descrambler_emm_t;

LIST_HEAD(caid_list, caid);

#define DESCRAMBLER_ECM_PID(pid) ((pid) | (MT_FAST << 16))

void descrambler_init          ( void );
void descrambler_done          ( void );
void descrambler_service_start ( struct service *t );
void descrambler_service_stop  ( struct service *t );
void descrambler_caid_changed  ( struct service *t );
int  descrambler_resolved      ( struct service *t, th_descrambler_t *ignore );
void descrambler_external      ( struct service *t, int state );
int  descrambler_multi_pid     ( th_descrambler_t *t );
void descrambler_keys          ( th_descrambler_t *t, int type, uint16_t pid,
                                 const uint8_t *even, const uint8_t *odd );
void descrambler_notify        ( th_descrambler_t *t,
                                 uint16_t caid, uint32_t provid,
                                 const char *cardsystem, uint16_t pid, uint32_t ecmtime,
                                 uint16_t hops, const char *reader, const char *from,
                                 const char *protocol );
int  descrambler_descramble    ( struct service *t,
                                 struct elementary_stream *st,
                                 const uint8_t *tsb, int len );
void descrambler_flush_table_data( struct service *t );
int  descrambler_open_pid      ( struct mpegts_mux *mux, void *opaque, int pid,
                                 descrambler_section_callback_t callback,
                                 struct service *service );
int  descrambler_close_pid     ( struct mpegts_mux *mux, void *opaque, int pid );
void descrambler_flush_tables  ( struct mpegts_mux *mux );
void descrambler_cat_data      ( struct mpegts_mux *mux, const uint8_t *data, int len );
int  descrambler_open_emm      ( struct mpegts_mux *mux, void *opaque, int caid,
                                 descrambler_section_callback_t callback );
int  descrambler_close_emm     ( struct mpegts_mux *mux, void *opaque, int caid );

#endif /* __TVH_DESCRAMBLER_H__ */

/* **************************************************************************
 * Editor
 *
 * vim:sts=2:ts=2:sw=2:et
 * *************************************************************************/
