/*
 *  Tvheadend
 *  Copyright (C) 2013 Andreas Öman
 *  Copyright (C) 2014,2015,2016,2017 Jaroslav Kysela
 *
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
#include "settings.h"
#include "descrambler.h"
#include "caid.h"
#include "caclient.h"
#include "ffdecsa/FFdecsa.h"
#include "input.h"
#include "input/mpegts/tsdemux.h"
#include "dvbcam.h"
#include "streaming.h"

typedef struct th_descrambler_data {
  TAILQ_ENTRY(th_descrambler_data) dd_link;
  int64_t dd_timestamp;
  sbuf_t dd_sbuf;
} th_descrambler_data_t;

typedef struct th_descrambler_hint {
  TAILQ_ENTRY(th_descrambler_hint) dh_link;
  uint16_t dh_caid;
  uint16_t dh_mask;
  uint32_t dh_interval;
  uint32_t dh_constcw: 1;
  uint32_t dh_quickecm: 1;
  uint32_t dh_multipid: 1;
} th_descrambler_hint_t;

TAILQ_HEAD(th_descrambler_queue, th_descrambler_data);
static TAILQ_HEAD( , th_descrambler_hint) ca_hints;

static int ca_hints_quickecm;

/*
 *
 */
static void
descrambler_data_destroy(th_descrambler_runtime_t *dr, th_descrambler_data_t *dd, int skip)
{
  if (dd) {
    if (skip && dr->dr_skip)
      ts_skip_packet2((mpegts_service_t *)dr->dr_service,
                      dd->dd_sbuf.sb_data, dd->dd_sbuf.sb_ptr);
    dr->dr_queue_total -= dd->dd_sbuf.sb_ptr;
    TAILQ_REMOVE(&dr->dr_queue, dd, dd_link);
    sbuf_free(&dd->dd_sbuf);
    free(dd);
#if ENABLE_TRACE
    if (TAILQ_EMPTY(&dr->dr_queue))
      assert(dr->dr_queue_total == 0);
#endif
  }
}

static void
descrambler_data_time_flush(th_descrambler_runtime_t *dr, int64_t oldest)
{
  th_descrambler_data_t *dd;

  while ((dd = TAILQ_FIRST(&dr->dr_queue)) != NULL) {
    if (dd->dd_timestamp >= oldest) break;
    descrambler_data_destroy(dr, dd, 1);
  }
}

static void
descrambler_data_append(th_descrambler_runtime_t *dr, const uint8_t *tsb, int len)
{
  th_descrambler_data_t *dd;

  if (len == 0)
    return;
  dd = TAILQ_LAST(&dr->dr_queue, th_descrambler_queue);
  if (dd && monocmpfastsec(dd->dd_timestamp, mclk()) &&
      (dd->dd_sbuf.sb_data[3] & 0x40) == (tsb[3] & 0x40)) { /* key match */
    sbuf_append(&dd->dd_sbuf, tsb, len);
    dr->dr_queue_total += len;
    return;
  }
  dd = malloc(sizeof(*dd));
  dd->dd_timestamp = mclk();
  sbuf_init(&dd->dd_sbuf);
  sbuf_append(&dd->dd_sbuf, tsb, len);
  TAILQ_INSERT_TAIL(&dr->dr_queue, dd, dd_link);
  dr->dr_queue_total += len;
}

static void
descrambler_data_cut(th_descrambler_runtime_t *dr, int len)
{
  th_descrambler_data_t *dd;

  while (len > 0 && (dd = TAILQ_FIRST(&dr->dr_queue)) != NULL) {
    if (dr->dr_skip)
      ts_skip_packet2((mpegts_service_t *)dr->dr_service,
                      dd->dd_sbuf.sb_data, MIN(len, dd->dd_sbuf.sb_ptr));
    if (len < dd->dd_sbuf.sb_ptr) {
      sbuf_cut(&dd->dd_sbuf, len);
      dr->dr_queue_total -= len;
      break;
    }
    len -= dd->dd_sbuf.sb_ptr;
    descrambler_data_destroy(dr, dd, 1);
  }
}

static int
descrambler_data_key_check(th_descrambler_runtime_t *dr, uint8_t key, int len)
{
  th_descrambler_data_t *dd = TAILQ_FIRST(&dr->dr_queue);
  int off = 0;

  if (dd == NULL)
    return 0;
  while (off < len) {
    if (dd->dd_sbuf.sb_ptr <= off) {
      dd = TAILQ_NEXT(dd, dd_link);
      if (dd == NULL)
        return 0;
      len -= off;
      off = 0;
    }
    if ((dd->dd_sbuf.sb_data[off + 3] & 0xc0) != key)
      return 0;
    off += 188;
  }
  return 1;
}

/*
 *
 */
static void
descrambler_load_hints(htsmsg_t *m)
{
  th_descrambler_hint_t hint, *dhint;
  htsmsg_t *e;
  htsmsg_field_t *f;
  const char *s;

  HTSMSG_FOREACH(f, m) {
    if (!(e = htsmsg_field_get_map(f))) continue;
    if ((s = htsmsg_get_str(e, "caid")) == NULL) continue;
    memset(&hint, 0, sizeof(hint));
    hint.dh_caid = strtol(s, NULL, 16);
    hint.dh_mask = 0xffff;
    if ((s = htsmsg_get_str(e, "mask")) != NULL)
      hint.dh_mask = strtol(s, NULL, 16);
    hint.dh_constcw = htsmsg_get_bool_or_default(e, "constcw", 0);
    hint.dh_quickecm = htsmsg_get_bool_or_default(e, "quickecm", 0);
    hint.dh_multipid = htsmsg_get_bool_or_default(e, "multipid", 0);
    hint.dh_interval = htsmsg_get_s32_or_default(e, "interval", 10000);
    tvhinfo(LS_DESCRAMBLER, "adding CAID %04X/%04X as%s%s%s interval %ums (%s)",
                            hint.dh_caid, hint.dh_mask,
                            hint.dh_constcw ? " ConstCW" : "",
                            hint.dh_quickecm ? " QuickECM" : "",
                            hint.dh_multipid ? " MultiPID" : "",
                            hint.dh_interval,
                            htsmsg_get_str(e, "name") ?: "unknown");
    dhint = malloc(sizeof(*dhint));
    *dhint = hint;
    TAILQ_INSERT_TAIL(&ca_hints, dhint, dh_link);
    if (hint.dh_quickecm) ca_hints_quickecm++;
  }
}

/*
 *
 */
void
descrambler_init ( void )
{
  htsmsg_t *c, *m;

  TAILQ_INIT(&ca_hints);
  ca_hints_quickecm = 0;

#if (ENABLE_CWC || ENABLE_CAPMT || ENABLE_CCCAM) && !ENABLE_DVBCSA
  ffdecsa_init();
#endif
  caclient_init();
#if ENABLE_LINUXDVB_CA
  dvbcam_init();
#endif

  if ((c = hts_settings_load("descrambler")) != NULL) {
    m = htsmsg_get_list(c, "caid");
    if (m)
      descrambler_load_hints(m);
    htsmsg_destroy(c);
  }
}

void
descrambler_done ( void )
{
  th_descrambler_hint_t *hint;

  caclient_done();
  while ((hint = TAILQ_FIRST(&ca_hints)) != NULL) {
    TAILQ_REMOVE(&ca_hints, hint, dh_link);
    free(hint);
  }
}

/*
 * Decide, if we should work in "quick ECM" mode
 */
static int
descrambler_quick_ecm ( mpegts_service_t *t, int pid )
{
  elementary_stream_t *st;
  th_descrambler_hint_t *hint;
  caid_t *ca;

  if (!ca_hints_quickecm)
    return 0;
  TAILQ_FOREACH(st, &t->s_filt_components, es_filt_link) {
    if (st->es_pid != pid) continue;
    TAILQ_FOREACH(hint, &ca_hints, dh_link) {
      if (!hint->dh_quickecm) continue;
      LIST_FOREACH(ca, &st->es_caids, link) {
        if (ca->use == 0) continue;
        if (hint->dh_caid == (ca->caid & hint->dh_mask))
          return 1;
      }
    }
  }
  return 0;
}

/*
 * This routine is called from two places
 * a) start a new service
 * b) restart a running service with possible caid changes
 */
void
descrambler_service_start ( service_t *t )
{
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  th_descrambler_hint_t *hint;
  elementary_stream_t *st;
  caid_t *ca;
  int i, count, constcw = 0, multipid = 0, interval = 10000;

  if (t->s_scrambled_pass)
    return;

  if (!t->s_dvb_forcecaid) {

    count = 0;
    TAILQ_FOREACH(st, &t->s_filt_components, es_filt_link)
      LIST_FOREACH(ca, &st->es_caids, link) {
        if (ca->use == 0) continue;
        TAILQ_FOREACH(hint, &ca_hints, dh_link) {
          if (hint->dh_caid == (ca->caid & hint->dh_mask)) {
            if (hint->dh_constcw) constcw = 1;
            if (hint->dh_multipid) multipid = 1;
            if (hint->dh_interval) interval = hint->dh_interval;
          }
        }
        count++;
      }

    /* Do not run descrambler on FTA channels */
    if (count == 0)
      return;

  } else {

    TAILQ_FOREACH(hint, &ca_hints, dh_link) {
      if (hint->dh_caid == (t->s_dvb_forcecaid & hint->dh_mask)) {
        if (hint->dh_constcw) constcw = 1;
        if (hint->dh_multipid) multipid = 1;
        if (hint->dh_interval) interval = hint->dh_interval;
      }
    }

  }

  ((mpegts_service_t *)t)->s_dvb_mux->mm_descrambler_flush = 0;
  if (t->s_descramble == NULL) {
    t->s_descramble = dr = calloc(1, sizeof(th_descrambler_runtime_t));
    dr->dr_service = t;
    TAILQ_INIT(&dr->dr_queue);
    for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
      tk = &dr->dr_keys[i];
      tk->key_index = 0xff;
      tk->key_interval = ms2mono(interval);
      tvhcsa_init(&tk->key_csa);
      if (!multipid) break;
    }
    dr->dr_ecm_key_margin = interval / 5;
    dr->dr_key_const = constcw;
    dr->dr_key_multipid = multipid;
    if (constcw)
      tvhtrace(LS_DESCRAMBLER, "using constcw for \"%s\"", t->s_nicename);
    if (multipid)
      tvhtrace(LS_DESCRAMBLER, "using multipid for \"%s\"", t->s_nicename);
    dr->dr_skip = 0;
    dr->dr_force_skip = 0;
  }

  if (t->s_dvb_forcecaid != 0xffff)
    caclient_start(t);

#if ENABLE_LINUXDVB_CA
  dvbcam_service_start(t);
#endif

  if (t->s_dvb_forcecaid == 0xffff) {
    pthread_mutex_lock(&t->s_stream_mutex);
    descrambler_external(t, 1);
    pthread_mutex_unlock(&t->s_stream_mutex);
  }
}

void
descrambler_service_stop ( service_t *t )
{
  th_descrambler_t *td;
  th_descrambler_runtime_t *dr = t->s_descramble;
  th_descrambler_key_t *tk;
  th_descrambler_data_t *dd;
  void *p;
  int i;

#if ENABLE_LINUXDVB_CA
  dvbcam_service_stop(t);
#endif

  while ((td = LIST_FIRST(&t->s_descramblers)) != NULL)
    td->td_stop(td);
  t->s_descramble = NULL;
  t->s_descrambler = NULL;
  p = t->s_descramble_info;
  t->s_descramble_info = NULL;
  free(p);
  if (dr) {
    for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
      tk = &dr->dr_keys[i];
      tvhcsa_destroy(&tk->key_csa);
      if (!dr->dr_key_multipid) break;
    }
    while ((dd = TAILQ_FIRST(&dr->dr_queue)) != NULL)
      descrambler_data_destroy(dr, dd, 0);
    free(dr);
  }
}

void
descrambler_caid_changed ( service_t *t )
{
  th_descrambler_t *td;

  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    if (td->td_caid_change)
      td->td_caid_change(td);
  }
}

static void
descrambler_notify_deliver( mpegts_service_t *t, descramble_info_t *di )
{
  streaming_message_t *sm;
  int r;

  lock_assert(&t->s_stream_mutex);
  if (!t->s_descramble_info)
    t->s_descramble_info = calloc(1, sizeof(*di));
  r = memcmp(t->s_descramble_info, di, sizeof(*di));
  if (r == 0) { /* identical */
    free(di);
    return;
  }
  memcpy(t->s_descramble_info, di, sizeof(*di));

  sm = streaming_msg_create(SMT_DESCRAMBLE_INFO);
  sm->sm_data = di;

  streaming_pad_deliver(&t->s_streaming_pad, sm);
}

static void
descrambler_notify_nokey( th_descrambler_runtime_t *dr )
{
  mpegts_service_t *t = (mpegts_service_t *)dr->dr_service;
  descramble_info_t *di;

  tvhdebug(LS_DESCRAMBLER, "no key for service='%s'", t->s_dvb_svcname);

  di = calloc(1, sizeof(*di));
  di->pid = t->s_pmt_pid;

  descrambler_notify_deliver(t, di);
}

void
descrambler_notify( th_descrambler_t *td,
                    uint16_t caid, uint32_t provid,
                    const char *cardsystem, uint16_t pid, uint32_t ecmtime,
                    uint16_t hops, const char *reader, const char *from,
                    const char *protocol )
{
  mpegts_service_t *t = (mpegts_service_t *)td->td_service;
  descramble_info_t *di;

  tvhdebug(LS_DESCRAMBLER, "info - service='%s' caid=%04X(%s) "
                                   "provid=%06X ecmtime=%d hops=%d "
                                   "reader='%s' from='%s' protocol='%s'%s",
         t->s_dvb_svcname, caid, cardsystem, provid,
         ecmtime, hops, reader, from, protocol,
         t->s_descrambler != td ? " (inactive)" : "");

  if (t->s_descrambler != td)
    return;

  di = calloc(1, sizeof(*di));

  di->pid     = pid;
  di->caid    = caid;
  di->provid  = provid;
  di->ecmtime = ecmtime;
  di->hops    = hops;
  strncpy(di->cardsystem, cardsystem, sizeof(di->cardsystem)-1);
  strncpy(di->reader, reader, sizeof(di->reader)-1);
  strncpy(di->from, from, sizeof(di->protocol)-1);
  strncpy(di->protocol, protocol, sizeof(di->protocol)-1);

  pthread_mutex_lock(&t->s_stream_mutex);
  descrambler_notify_deliver(t, di);
  pthread_mutex_unlock(&t->s_stream_mutex);
}

int
descrambler_resolved( service_t *t, th_descrambler_t *ignore )
{
  th_descrambler_t *td;

  LIST_FOREACH(td, &t->s_descramblers, td_service_link)
    if (td != ignore && td->td_keystate == DS_RESOLVED)
      return 1;
  return 0;
}

void
descrambler_external ( service_t *t, int state )
{
  th_descrambler_runtime_t *dr;

  if (t == NULL)
    return;

  lock_assert(&t->s_stream_mutex);

  if ((dr = t->s_descramble) == NULL)
    return;
  dr->dr_external = state ? 1 : 0;
  service_reset_streaming_status_flags(t, TSS_NO_DESCRAMBLER);
}

int
descrambler_multi_pid ( th_descrambler_t *td )
{
  service_t *t = td->td_service;
  th_descrambler_runtime_t *dr;

  if (t == NULL || (dr = t->s_descramble) == NULL)
    return 0;
  return dr->dr_key_multipid;
}

void
descrambler_keys ( th_descrambler_t *td, int type, uint16_t pid,
                   const uint8_t *even, const uint8_t *odd )
{
  static uint8_t empty[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  service_t *t = td->td_service;
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  th_descrambler_t *td2;
  char pidname[16];
  const char *ktype;
  uint16_t pid2;
  int j = 0;

  if (t == NULL || (dr = t->s_descramble) == NULL) {
    td->td_keystate = DS_FORBIDDEN;
    return;
  }

  pthread_mutex_lock(&t->s_stream_mutex);

  if (!dr->dr_key_multipid)
    pid = 0;

  for (j = 0; j < DESCRAMBLER_MAX_KEYS; j++) {
    tk = &dr->dr_keys[j];
    pid2 = tk->key_pid;
    if (pid2 == 0 || pid2 == pid) break;
  }

  if (j >= DESCRAMBLER_MAX_KEYS) {
    tvherror(LS_DESCRAMBLER, "too many keys");
    goto fin;
  }

  if (tvhcsa_set_type(&tk->key_csa, type) < 0)
    goto fin;

  LIST_FOREACH(td2, &t->s_descramblers, td_service_link)
    if (td2 != td && td2->td_keystate == DS_RESOLVED) {
      tvhdebug(LS_DESCRAMBLER,
               "Already has a key[%d] from %s for service \"%s\", "
               "ignoring key from \"%s\"%s",
               tk->key_pid, td2->td_nicename,
               ((mpegts_service_t *)td2->td_service)->s_dvb_svcname,
               td->td_nicename,
               dr->dr_key_const ? " (const)" : "");
      td->td_keystate = DS_IDLE;
      if (td->td_ecm_idle)
        td->td_ecm_idle(td);
      goto fin;
    }

  if (pid == 0)
    pidname[0] = '\0';
  else
    snprintf(pidname, sizeof(pidname), "[%d]", pid);
  switch(type) {
  case DESCRAMBLER_CSA_CBC: ktype = "CSA"; break;
  case DESCRAMBLER_DES_NCB: ktype = "DES"; break;
  case DESCRAMBLER_AES_ECB: ktype = "AES EBC"; break;
  default: abort();
  }

  if (even && memcmp(empty, even, tk->key_csa.csa_keylen)) {
    j++;
    memcpy(tk->key_data[0], even, tk->key_csa.csa_keylen);
    tk->key_pid = pid;
    tk->key_changed |= 1;
    tk->key_valid |= 0x40;
    tk->key_timestamp[0] = mclk();
    if (dr->dr_ecm_start[0] < dr->dr_ecm_start[1]) {
      dr->dr_ecm_start[0] = dr->dr_ecm_start[1];
      tvhdebug(LS_DESCRAMBLER,
               "Both keys received, marking ECM start for even key%s for service \"%s\"",
               pidname, ((mpegts_service_t *)t)->s_dvb_svcname);
    }
  } else {
    even = empty;
  }
  if (odd && memcmp(empty, odd, tk->key_csa.csa_keylen)) {
    j++;
    memcpy(tk->key_data[1], odd, tk->key_csa.csa_keylen);
    tk->key_pid = pid;
    tk->key_changed |= 2;
    tk->key_valid |= 0x80;
    tk->key_timestamp[1] = mclk();
    if (dr->dr_ecm_start[1] < dr->dr_ecm_start[0]) {
      dr->dr_ecm_start[1] = dr->dr_ecm_start[0];
      tvhdebug(LS_DESCRAMBLER,
               "Both keys received, marking ECM start for odd key%s for service \"%s\"",
               pidname, ((mpegts_service_t *)t)->s_dvb_svcname);
    }
  } else {
    odd = empty;
  }

  if (j) {
    if (td->td_keystate != DS_RESOLVED)
      tvhdebug(LS_DESCRAMBLER,
               "Obtained %s keys%s from %s for service \"%s\"%s",
               ktype, pidname, td->td_nicename,
               ((mpegts_service_t *)t)->s_dvb_svcname,
               dr->dr_key_const ? " (const)" : "");
    if (tk->key_csa.csa_keylen == 8) {
      tvhtrace(LS_DESCRAMBLER, "Obtained %s keys%s "
               "%02X%02X%02X%02X%02X%02X%02X%02X:%02X%02X%02X%02X%02X%02X%02X%02X"
               " pid %04X from %s for service \"%s\"",
               ktype, pidname,
               even[0], even[1], even[2], even[3], even[4], even[5], even[6], even[7],
               odd[0], odd[1], odd[2], odd[3], odd[4], odd[5], odd[6], odd[7],
               pid, td->td_nicename,
               ((mpegts_service_t *)t)->s_dvb_svcname);
    } else if (tk->key_csa.csa_keylen == 16) {
      tvhtrace(LS_DESCRAMBLER, "Obtained %s keys%s "
               "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X:"
               "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X"
               " pid %04X from %s for service \"%s\"",
               ktype, pidname,
               even[0], even[1], even[2], even[3], even[4], even[5], even[6], even[7],
               even[8], even[9], even[10], even[11], even[12], even[13], even[14], even[15],
               odd[0], odd[1], odd[2], odd[3], odd[4], odd[5], odd[6], odd[7],
               odd[8], odd[9], odd[10], odd[11], odd[12], odd[13], odd[14], odd[15],
               pid, td->td_nicename,
               ((mpegts_service_t *)t)->s_dvb_svcname);
    } else {
      tvhtrace(LS_DESCRAMBLER, "Unknown keys%s pid %04X from %s for for service \"%s\"",
               pidname, pid, td->td_nicename, ((mpegts_service_t *)t)->s_dvb_svcname);
    }
    dr->dr_ecm_last_key_time = mclk();
    td->td_keystate = DS_RESOLVED;
    td->td_service->s_descrambler = td;
  } else {
    tvhdebug(LS_DESCRAMBLER,
             "Empty %s keys%s received from %s for service \"%s\"%s",
             ktype, pidname, td->td_nicename,
             ((mpegts_service_t *)t)->s_dvb_svcname,
             dr->dr_key_const ? " (const)" : "");
  }

fin:
#if ENABLE_TSDEBUG
  if (j) {
    tsdebug_packet_t *tp = malloc(sizeof(*tp));
    uint16_t keylen = tk->key_csa.csa_keylen;
    mpegts_service_t *ms = (mpegts_service_t *)t;
    uint16_t sid = ms->s_dvb_service_id;
    uint32_t pos = 0, crc;
    mpegts_mux_t *mm = ms->s_dvb_mux;
    mpegts_mux_instance_t *mmi = mm ? mm->mm_active : NULL;
    mpegts_input_t *mi = mmi ? mmi->mmi_input : NULL;
    if (mi == NULL) {
      free(tp);
      return;
    }
    pthread_mutex_unlock(&t->s_stream_mutex);
    pthread_mutex_lock(&mi->mi_output_lock);
    tp->pos = mm->mm_tsdebug_pos;
    memset(tp->pkt, 0xff, sizeof(tp->pkt));
    tp->pkt[pos++] = 0x47; /* sync byte */
    tp->pkt[pos++] = 0x1f; /* PID MSB */
    tp->pkt[pos++] = 0xff; /* PID LSB */
    tp->pkt[pos++] = 0x00; /* CC */
    memcpy(tp->pkt + pos, "TVHeadendDescramblerKeys", 24);
    pos += 24;
    tp->pkt[pos++] = type & 0xff;
    tp->pkt[pos++] = keylen & 0xff;
    tp->pkt[pos++] = (sid >> 8) & 0xff;
    tp->pkt[pos++] = sid & 0xff;
    tp->pkt[pos++] = (pid >> 8) & 0xff;
    tp->pkt[pos++] = pid & 0xff;
    memcpy(tp->pkt + pos, even, keylen);
    memcpy(tp->pkt + pos + keylen, odd, keylen);
    pos += 2 * keylen;
    crc = tvh_crc32(tp->pkt, pos, 0x859aa5ba);
    tp->pkt[pos++] = (crc >> 24) & 0xff;
    tp->pkt[pos++] = (crc >> 16) & 0xff;
    tp->pkt[pos++] = (crc >> 8) & 0xff;
    tp->pkt[pos++] = crc & 0xff;
    TAILQ_INSERT_HEAD(&mm->mm_tsdebug_packets, tp, link);
    pthread_mutex_unlock(&mi->mi_output_lock);
    return;
  }
#endif
  pthread_mutex_unlock(&t->s_stream_mutex);
}

void
descrambler_flush_table_data( service_t *t )
{
  mpegts_service_t *ms = (mpegts_service_t *)t;
  mpegts_mux_t *mux = ms->s_dvb_mux;
  descrambler_table_t *dt;
  descrambler_section_t *ds;
  descrambler_ecmsec_t *des;

  if (mux == NULL)
    return;
  tvhtrace(LS_DESCRAMBLER, "flush table data for service \"%s\"", ms->s_dvb_svcname);
  pthread_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(dt, &mux->mm_descrambler_tables, link) {
    if (dt->table == NULL || dt->table->mt_service != ms)
      continue;
    TAILQ_FOREACH(ds, &dt->sections, link)
      while ((des = LIST_FIRST(&ds->ecmsecs)) != NULL) {
        LIST_REMOVE(des, link);
        free(des->last_data);
        free(des);
      }
  }
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
}

static inline void 
key_update( th_descrambler_key_t *tk, uint8_t key, int64_t timestamp )
{
  /* set the even (0) or odd (0x40) key index */
  tk->key_index = key & 0x40;
  if (tk->key_start) {
    tk->key_interval = tk->key_start + sec2mono(50) < timestamp ?
                         sec2mono(10) : timestamp - tk->key_start;
    tk->key_start = timestamp;
  } else {
    /* We don't know the exact start key switch time */
    tk->key_start = timestamp - sec2mono(60);
  }
}

static inline int
key_changed ( th_descrambler_runtime_t *dr, th_descrambler_key_t *tk, uint8_t ki, int64_t timestamp )
{
  return tk->key_index != (ki & 0x40) &&
         tk->key_start + dr->dr_ecm_key_margin < timestamp;
}

static inline int
key_valid ( th_descrambler_key_t *tk, uint8_t ki )
{
  /* 0x40 (for even) or 0x80 (for odd) */
  uint8_t mask = ((ki & 0x40) + 0x40);
  return tk->key_valid & mask;
}

static inline int
key_late( th_descrambler_runtime_t *dr, th_descrambler_key_t *tk, uint8_t ki, int64_t timestamp )
{
  uint8_t kidx = (ki & 0x40) >> 6;
  /* constcw - do not handle keys */
  if (dr->dr_key_const)
    return 0;
  /* required key is older than previous? */
  if (tk->key_timestamp[kidx] < tk->key_timestamp[kidx^1]) {
    /* but don't take in account the keys modified just now */
    if (tk->key_timestamp[kidx^1] + 2 < timestamp)
      goto late;
  }
  /* ECM was sent, but no new key was received */
  if (dr->dr_ecm_last_key_time + dr->dr_ecm_key_margin < tk->key_start &&
      (!dr->dr_quick_ecm || dr->dr_ecm_start[kidx] + 4 < tk->key_start)) {
late:
    tk->key_valid &= ~((ki & 0x40) + 0x40);
    return 1;
  }
  return 0;
}

static inline int
key_started( th_descrambler_runtime_t *dr, uint8_t ki )
{
  uint8_t kidx = (ki & 0x40) >> 6;
  return mclk() - dr->dr_ecm_start[kidx] < dr->dr_ecm_key_margin * 2;
}

static void
key_flush( th_descrambler_key_t *tk, service_t *t, int force )
{
  if (tk->key_changed || force) {
    tk->key_csa.csa_flush(&tk->key_csa, (mpegts_service_t *)t);
    /* update the keys */
    if (tk->key_changed & 1)
      tvhcsa_set_key_even(&tk->key_csa, tk->key_data[0]);
    if (tk->key_changed & 2)
      tvhcsa_set_key_odd(&tk->key_csa, tk->key_data[1]);
    tk->key_changed = 0;
  }
}

static th_descrambler_key_t *
key_find_struct( th_descrambler_runtime_t *dr,
                 th_descrambler_key_t *tk_old,
                 const uint8_t *tsb,
                 service_t *t )
{
  th_descrambler_key_t *tk;
  int i, pid = (tsb[1] & 0x1f) << 8 | tsb[2];
  for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
    tk = &dr->dr_keys[i];
    if (tk->key_pid == pid) {
      if (tk != tk_old && tk_old)
        key_flush(tk_old, t, 1);
      key_flush(tk, t, 0);
      return tk;
    }
  }
  return NULL;
}

static int
ecm_reset( service_t *t, th_descrambler_runtime_t *dr )
{
  th_descrambler_t *td;
  th_descrambler_key_t *tk;
  int ret = 0, i;

  /* reset the reader ECM state */
  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    if (!td->td_ecm_reset(td)) {
      for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
        tk = &dr->dr_keys[i];
        tk->key_valid = 0;
        if (tk->key_pid == 0)
          break;
      }
      ret = 1;
    }
  }
  return ret;
}

int
descrambler_descramble ( service_t *t,
                         elementary_stream_t *st,
                         const uint8_t *tsb,
                         int len )
{
  th_descrambler_t *td;
  th_descrambler_runtime_t *dr = t->s_descramble;
  th_descrambler_key_t *tk;
  th_descrambler_data_t *dd, *dd_next;
  int count, failed, resolved, len2, len3, flush_data = 0;
  uint32_t dbuflen;
  const uint8_t *tsb2;
  uint8_t ki;
  sbuf_t *sb;

  lock_assert(&t->s_stream_mutex);

  if (dr == NULL || dr->dr_external) {
    if ((tsb[3] & 0x80) == 0) {
      ts_recv_packet0((mpegts_service_t *)t, st, tsb, len);
      return 1;
    }
    return dr && dr->dr_external ? 1 : -1;
  }

  if (!dr->dr_key_multipid) {
    tk = &dr->dr_keys[0];
  } else {
    tk = key_find_struct(dr, NULL, tsb, t);
  }  
  if ((tk == NULL || tk->key_csa.csa_type == DESCRAMBLER_NONE) && dr->dr_queue_total == 0)
    if ((tsb[3] & 0x80) == 0) {
      ts_recv_packet0((mpegts_service_t *)t, st, tsb, len);
      return 1;
    }

  count = failed = resolved = 0;
  LIST_FOREACH(td, &t->s_descramblers, td_service_link) {
    count++;
    switch (td->td_keystate) {
    case DS_FORBIDDEN: failed++;   break;
    case DS_RESOLVED : resolved++; break;
    default: break;
    }
  }

  if (resolved) {

    if (!dr->dr_key_multipid) {
      tk = &dr->dr_keys[0];
      key_flush(tk, t, 0);
    } else {
      tk = NULL;
    }

    /* process the queued TS packets */
    if (dr->dr_queue_total > 0) {
      if (!dr->dr_key_multipid)
        descrambler_data_time_flush(dr, mclk() - (tk->key_interval - sec2mono(2)));
      for (dd = TAILQ_FIRST(&dr->dr_queue); dd; dd = dd_next) {
        dd_next = TAILQ_NEXT(dd, dd_link);
        sb = &dd->dd_sbuf;
        tsb2 = sb->sb_data;
        len2 = sb->sb_ptr;
        for (; len2 > 0; tsb2 += len3, len2 -= len3) {
          ki = tsb2[3];
          if (dr->dr_key_multipid) {
            tk = key_find_struct(dr, tk, tsb2, t);
            if (tk == NULL) goto next;
          }
          if ((ki & 0x80) != 0x00) {
            if (key_valid(tk, ki) == 0)
              goto next;
            if (key_changed(dr, tk, ki, dd->dd_timestamp)) {
              tvhtrace(LS_DESCRAMBLER, "stream key[%d] changed to %s for service \"%s\"",
                                      tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                      ((mpegts_service_t *)t)->s_dvb_svcname);
              if (key_late(dr, tk, ki, dd->dd_timestamp)) {
                descrambler_notify_nokey(dr);
                if (ecm_reset(t, dr)) {
                  descrambler_data_cut(dr, tsb2 - sb->sb_data);
                  flush_data = 1;
                  goto next;
                }
              }
              key_update(tk, ki, dd->dd_timestamp);
            }
          }
          len3 = mpegts_word_count(tsb2, len2, dr->dr_key_multipid ? 0xFF1FFFC0 : 0xFF0000C0);
          tk->key_csa.csa_descramble(&tk->key_csa, (mpegts_service_t *)t, tsb2, len3);
        }
        if (len2 == 0)
          service_reset_streaming_status_flags(t, TSS_NO_ACCESS);
        descrambler_data_destroy(dr, dd, 0);
      }
    }

    /* check for key change */
    ki = tsb[3];
    if (dr->dr_key_multipid) {
      tk = key_find_struct(dr, tk, tsb, t);
      if (tk == NULL) goto next;
    }
    if ((ki & 0x80) != 0x00) {
      if (key_valid(tk, ki) == 0) {
        if (!key_started(dr, ki) && tvhlog_limit(&dr->dr_loglimit_key, 10))
          tvhwarn(LS_DESCRAMBLER, "%s %s stream key[%d] is not valid",
                   ((mpegts_service_t *)t)->s_dvb_svcname,
                   (ki & 0x40) ? "odd" : "even", tk->key_pid);
        goto next;
      }
      if (key_changed(dr, tk, ki, mclk())) {
        tvhtrace(LS_DESCRAMBLER, "stream key[%d] changed to %s for service \"%s\"",
                                tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                ((mpegts_service_t *)t)->s_dvb_svcname);
        if (key_late(dr, tk, ki, mclk())) {
          tvherror(LS_DESCRAMBLER, "ECM - key[%d] late (%"PRId64" ms) for service \"%s\"",
                                  tk->key_pid,
                                  mono2ms(mclk() - dr->dr_ecm_last_key_time),
                                  ((mpegts_service_t *)t)->s_dvb_svcname);
          descrambler_notify_nokey(dr);
          if (ecm_reset(t, dr)) {
            flush_data = 1;
            goto next;
          }
        }
        key_update(tk, ki, mclk());
      }
    }
    dr->dr_skip = 1;
    tk->key_csa.csa_descramble(&tk->key_csa, (mpegts_service_t *)t, tsb, len);
    service_reset_streaming_status_flags(t, TSS_NO_ACCESS);
    return 1;
  }
next:
  if (!dr->dr_skip) {
    if (!dr->dr_force_skip)
      dr->dr_force_skip = mclk() + sec2mono(30);
    else if (dr->dr_force_skip < mclk())
      dr->dr_skip = 1;
  }
  if (dr->dr_ecm_start[0] || dr->dr_ecm_start[1]) { /* ECM sent */
    ki = tsb[3];
    if ((ki & 0x80) != 0x00) {
      if (dr->dr_key_multipid) {
        tk = key_find_struct(dr, tk, tsb, t);
        if (tk == NULL) goto queue;
      } else {
        tk = &dr->dr_keys[0];
      }
      if (tk->key_start == 0) {
        if (dr->dr_key_multipid)
          goto update;
        /* do not use the first TS packet to decide - it may be wrong */
        while (dr->dr_queue_total > 20 * 188) {
          if (descrambler_data_key_check(dr, ki & 0xc0, 20 * 188)) {
            tvhtrace(LS_DESCRAMBLER, "initial stream key[%d] set to %s for service \"%s\"",
                                    tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                    ((mpegts_service_t *)t)->s_dvb_svcname);
            key_update(tk, ki, mclk());
            break;
          } else {
            descrambler_data_cut(dr, 188);
          }
        }
      } else if (tk->key_index != (ki & 0x40) &&
                 tk->key_start + dr->dr_ecm_key_margin < mclk()) {
update:
        tvhtrace(LS_DESCRAMBLER, "stream key[%d] changed to %s for service \"%s\"",
                                tk->key_pid, (ki & 0x40) ? "odd" : "even",
                                ((mpegts_service_t *)t)->s_dvb_svcname);
        key_update(tk, ki, mclk());
      }
    }
queue:
    if (count != failed) {
      /*
       * Fill a temporary buffer until the keys are known to make
       * streaming faster.
       */
      dbuflen = MAX(300, config.descrambler_buffer);
      if (dr->dr_queue_total >= dbuflen * 188) {
        descrambler_data_cut(dr, MAX((dbuflen / 10) * 188, len));
        if (dr->dr_last_err + sec2mono(10) < mclk()) {
          dr->dr_last_err = mclk();
          tvherror(LS_DESCRAMBLER, "cannot decode packets for service \"%s\"",
                   ((mpegts_service_t *)t)->s_dvb_svcname);
        } else {
          tvhtrace(LS_DESCRAMBLER, "cannot decode packets for service \"%s\"",
                   ((mpegts_service_t *)t)->s_dvb_svcname);
        }
      }
      descrambler_data_append(dr, tsb, len);
      service_set_streaming_status_flags(t, TSS_NO_ACCESS);
    }
  } else {
    if (dr->dr_skip || count == 0)
      ts_skip_packet2((mpegts_service_t *)dr->dr_service, tsb, len);
    service_set_streaming_status_flags(t, TSS_NO_ACCESS);
  }
  if (flush_data)
    descrambler_flush_table_data(t);
  if (count && count == failed)
    return -1;
  return count;
}

static int
descrambler_table_callback
  (mpegts_table_t *mt, const uint8_t *ptr, int len, int tableid)
{
  descrambler_table_t *dt = mt->mt_opaque;
  descrambler_section_t *ds;
  descrambler_ecmsec_t *des;
  th_descrambler_runtime_t *dr;
  th_descrambler_key_t *tk;
  int emm = (mt->mt_flags & MT_FAST) == 0;
  mpegts_service_t *t;
  int64_t clk;
  uint8_t ki;
  int i, j;

  if (len < 6)
    return 0;
  pthread_mutex_lock(&mt->mt_mux->mm_descrambler_lock);
  TAILQ_FOREACH(ds, &dt->sections, link) {
    if (!emm) {
      LIST_FOREACH(des, &ds->ecmsecs, link)
        if (des->number == ptr[4])
          break;
    } else {
      des = LIST_FIRST(&ds->ecmsecs);
    }
    if (des == NULL) {
      des = calloc(1, sizeof(*des));
      des->number = emm ? 0 : ptr[4];
      LIST_INSERT_HEAD(&ds->ecmsecs, des, link);
    }
    if (des->last_data == NULL || len != des->last_data_len ||
        memcmp(des->last_data, ptr, len)) {
      free(des->last_data);
      des->last_data = malloc(len);
      if (des->last_data) {
        memcpy(des->last_data, ptr, len);
        des->last_data_len = len;
      } else {
        des->last_data_len = 0;
      }
      ds->callback(ds->opaque, mt->mt_pid, ptr, len, emm);
      if (!emm) { /* ECM */
        if ((t = mt->mt_service) != NULL) {
          /* The keys are requested from this moment */
          dr = t->s_descramble;
          if (dr) {
            if (!dr->dr_quick_ecm && !ds->quick_ecm_called) {
              ds->quick_ecm_called = 1;
              dr->dr_quick_ecm = descrambler_quick_ecm(mt->mt_service, mt->mt_pid);
              if (dr->dr_quick_ecm)
                tvhdebug(LS_DESCRAMBLER, "quick ECM enabled for service '%s'",
                         t->s_dvb_svcname);
            }
            if ((ptr[0] & 0xfe) == 0x80) { /* 0x80 = even, 0x81 = odd */
              dr->dr_ecm_start[ptr[0] & 1] = mclk();
              if (dr->dr_quick_ecm) {
                ki = 1 << ((ptr[0] & 1) + 6); /* 0x40 = even, 0x80 = odd */
                for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
                  tk = &dr->dr_keys[i];
                  tk->key_valid &= ~ki;
                  if (tk->key_pid == 0) break;
                }
              }
            }
            tvhtrace(LS_DESCRAMBLER, "ECM message %02x (section %d, len %d, pid %d) for service \"%s\"",
                     ptr[0], des->number, len, mt->mt_pid, t->s_dvb_svcname);
          }
        } else
          tvhtrace(LS_DESCRAMBLER, "Unknown fast table message %02x (section %d, len %d, pid %d)",
                   ptr[0], des->number, len, mt->mt_pid);
      } else if (tvhtrace_enabled()) {
        const char *s;
        if (mt->mt_pid == DVB_PAT_PID)      s = "PAT";
        else if (mt->mt_pid == DVB_CAT_PID) s = "CAT";
        else                                s = "EMM";
        if (len >= 18)
          tvhtrace(LS_DESCRAMBLER_EMM, "%s message %02x:{%02x:%02x}:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x (len %d, pid %d)",
                   s, ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], ptr[6], ptr[7],
                   ptr[8], ptr[9], ptr[10], ptr[11], ptr[12], ptr[13], ptr[14], ptr[15],
                   ptr[16], ptr[17], len, mt->mt_pid);
        else if (len >= 6)
          tvhtrace(LS_DESCRAMBLER_EMM, "%s message %02x:{%02x:%02x}:%02x:%02x:%02x (len %d, pid %d)",
                   s, ptr[0], ptr[1], ptr[2], ptr[3], ptr[4], ptr[5], len, mt->mt_pid);
        else if (len >= 4)
          tvhtrace(LS_DESCRAMBLER_EMM, "%s message %02x:{%02x:%02x}:%02x (len %d, pid %d)",
                   s, ptr[0], ptr[1], ptr[2], ptr[3], len, mt->mt_pid);
      }
    } else if (des->last_data && !emm) {
      if ((t = mt->mt_service) != NULL) {
        if ((dr = t->s_descramble) != NULL) {
          clk = mclk();
          for (i = 0; i < DESCRAMBLER_MAX_KEYS; i++) {
            tk = &dr->dr_keys[i];
            for (j = 0; j < 2; j++) {
              if (tk->key_timestamp[j] > dr->dr_ecm_start[j] &&
                  tk->key_timestamp[j] + ms2mono(200) <= clk) {
                tk->key_timestamp[j] = clk;
                tvhtrace(LS_DESCRAMBLER, "ECM: %s key[%d] for service \"%s\" still valid",
                                         j == 0 ? "Even" : "Odd",
                                         tk->key_pid, t->s_dvb_svcname);
              }
            }
          }
        }
      }
    }
  }
  pthread_mutex_unlock(&mt->mt_mux->mm_descrambler_lock);
  return 0;
}

static int
descrambler_open_pid_( mpegts_mux_t *mux, void *opaque, int pid,
                       descrambler_section_callback_t callback,
                       service_t *service )
{
  descrambler_table_t *dt;
  descrambler_section_t *ds;
  int flags;

  if (mux == NULL)
    return 0;
  if (mux->mm_descrambler_flush)
    return 0;
  flags  = (pid >> 16) & MT_FAST;
  pid   &= 0x1fff;
  TAILQ_FOREACH(dt, &mux->mm_descrambler_tables, link) {
    if (dt->table->mt_pid != pid || (dt->table->mt_flags & MT_FAST) != flags)
      continue;
    TAILQ_FOREACH(ds, &dt->sections, link) {
      if (ds->opaque == opaque)
        return 0;
    }
    break;
  }
  if (!dt) {
    dt = calloc(1, sizeof(*dt));
    TAILQ_INIT(&dt->sections);
    dt->table = mpegts_table_add(mux, 0, 0, descrambler_table_callback,
                                 dt, (flags & MT_FAST) ? "ecm" : "emm",
                                 LS_TBL_CSA, MT_FULL | MT_DEFER | flags, pid,
                                 MPS_WEIGHT_CA);
    if (dt->table)
      dt->table->mt_service = (mpegts_service_t *)service;
    TAILQ_INSERT_TAIL(&mux->mm_descrambler_tables, dt, link);
  }
  ds = calloc(1, sizeof(*ds));
  ds->callback    = callback;
  ds->opaque      = opaque;
  LIST_INIT(&ds->ecmsecs);
  TAILQ_INSERT_TAIL(&dt->sections, ds, link);
  tvhtrace(LS_DESCRAMBLER, "mux %p open pid %04X (%i) (flags 0x%04x) for %p", mux, pid, pid, flags, opaque);
  return 1;
}

int
descrambler_open_pid( mpegts_mux_t *mux, void *opaque, int pid,
                      descrambler_section_callback_t callback,
                      service_t *service )
{
  int res;

  pthread_mutex_lock(&mux->mm_descrambler_lock);
  res = descrambler_open_pid_(mux, opaque, pid, callback, service);
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
  return res;
}

static int
descrambler_close_pid_( mpegts_mux_t *mux, void *opaque, int pid )
{
  descrambler_table_t *dt;
  descrambler_section_t *ds;
  descrambler_ecmsec_t *des;
  int flags;

  if (mux == NULL)
    return 0;
  flags =  (pid >> 16) & MT_FAST;
  pid   &= 0x1fff;
  TAILQ_FOREACH(dt, &mux->mm_descrambler_tables, link) {
    if (dt->table->mt_pid != pid || (dt->table->mt_flags & MT_FAST) != flags)
      continue;
    TAILQ_FOREACH(ds, &dt->sections, link) {
      if (ds->opaque == opaque) {
        TAILQ_REMOVE(&dt->sections, ds, link);
        ds->callback(ds->opaque, -1, NULL, 0, (flags & MT_FAST) == 0);
        while ((des = LIST_FIRST(&ds->ecmsecs)) != NULL) {
          LIST_REMOVE(des, link);
          free(des->last_data);
          free(des);
        }
        if (TAILQ_FIRST(&dt->sections) == NULL) {
          TAILQ_REMOVE(&mux->mm_descrambler_tables, dt, link);
          mpegts_table_destroy(dt->table);
          free(dt);
        }
        free(ds);
        tvhtrace(LS_DESCRAMBLER, "mux %p close pid %04X (%i) (flags 0x%04x) for %p", mux, pid, pid, flags, opaque);
        return 1;
      }
    }
  }
  return 0;
}

int
descrambler_close_pid( mpegts_mux_t *mux, void *opaque, int pid )
{
  int res;

  pthread_mutex_lock(&mux->mm_descrambler_lock);
  res = descrambler_close_pid_(mux, opaque, pid);
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
  return res;
}

void
descrambler_flush_tables( mpegts_mux_t *mux )
{
  descrambler_table_t *dt;
  descrambler_section_t *ds;
  descrambler_ecmsec_t *des;
  descrambler_emm_t *emm;

  if (mux == NULL)
    return;
  tvhtrace(LS_DESCRAMBLER, "mux %p - flush tables", mux);
  caclient_caid_update(mux, 0, 0, -1);
  pthread_mutex_lock(&mux->mm_descrambler_lock);
  mux->mm_descrambler_flush = 1;
  while ((dt = TAILQ_FIRST(&mux->mm_descrambler_tables)) != NULL) {
    while ((ds = TAILQ_FIRST(&dt->sections)) != NULL) {
      TAILQ_REMOVE(&dt->sections, ds, link);
      ds->callback(ds->opaque, -1, NULL, 0, (dt->table->mt_flags & MT_FAST) ? 0 : 1);
      while ((des = LIST_FIRST(&ds->ecmsecs)) != NULL) {
        LIST_REMOVE(des, link);
        free(des->last_data);
        free(des);
      }
      free(ds);
    }
    TAILQ_REMOVE(&mux->mm_descrambler_tables, dt, link);
    mpegts_table_destroy(dt->table);
    free(dt);
  }
  while ((emm = TAILQ_FIRST(&mux->mm_descrambler_emms)) != NULL) {
    TAILQ_REMOVE(&mux->mm_descrambler_emms, emm, link);
    free(emm);
  }
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
}

void
descrambler_cat_data( mpegts_mux_t *mux, const uint8_t *data, int len )
{
  descrambler_emm_t *emm;
  uint8_t dtag, dlen;
  uint16_t caid = 0, pid = 0;
  TAILQ_HEAD(,descrambler_emm) removing;

  tvhtrace(LS_DESCRAMBLER, "CAT data (len %d)", len);
  tvhlog_hexdump(LS_DESCRAMBLER, data, len);
  pthread_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    emm->to_be_removed = 1;
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
  while (len > 2) {
    if (len > 2) {
      dtag = *data++;
      dlen = *data++;
      len -= 2;
      if (dtag != DVB_DESC_CA || len < 4 || dlen < 4)
        goto next;
      caid =  (data[0] << 8) | data[1];
      pid  = ((data[2] << 8) | data[3]) & 0x1fff;
      if (pid == 0)
        goto next;
      caclient_caid_update(mux, caid, pid, 1);
      pthread_mutex_lock(&mux->mm_descrambler_lock);
      TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
        if (emm->caid == caid) {
          emm->to_be_removed = 0;
          if (emm->pid == EMM_PID_UNKNOWN) {
            tvhtrace(LS_DESCRAMBLER, "attach emm caid %04X (%i) pid %04X (%i)", caid, caid, pid, pid);
            emm->pid = pid;
            descrambler_open_pid_(mux, emm->opaque, pid, emm->callback, NULL);
          }
        }
      pthread_mutex_unlock(&mux->mm_descrambler_lock);
next:
      data += dlen;
      len  -= dlen;
    }
  }
  TAILQ_INIT(&removing);
  pthread_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    if (emm->to_be_removed) {
      if (emm->pid != EMM_PID_UNKNOWN) {
        caid = emm->caid;
        pid  = emm->pid;
        tvhtrace(LS_DESCRAMBLER, "close emm caid %04X (%i) pid %04X (%i)", caid, caid, pid, pid);
        descrambler_close_pid_(mux, emm->opaque, pid);
      }
      TAILQ_REMOVE(&mux->mm_descrambler_emms, emm, link);
      TAILQ_INSERT_TAIL(&removing, emm, link);
    }
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
  while ((emm = TAILQ_FIRST(&removing)) != NULL) {
    if (emm->pid != EMM_PID_UNKNOWN)
      caclient_caid_update(mux, emm->caid, emm->pid, 0);
    TAILQ_REMOVE(&removing, emm, link);
    free(emm);
  }
}

int
descrambler_open_emm( mpegts_mux_t *mux, void *opaque, int caid,
                      descrambler_section_callback_t callback )
{
  descrambler_emm_t *emm;
  caid_t *c;
  int pid = EMM_PID_UNKNOWN;

  if (mux == NULL)
    return 0;
  pthread_mutex_lock(&mux->mm_descrambler_lock);
  if (mux->mm_descrambler_flush)
    goto unlock;
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link) {
    if (emm->caid == caid && emm->opaque == opaque) {
unlock:
      pthread_mutex_unlock(&mux->mm_descrambler_lock);
      return 0;
    }
  }
  emm = calloc(1, sizeof(*emm));
  emm->caid     = caid;
  emm->pid      = EMM_PID_UNKNOWN;
  emm->opaque   = opaque;
  emm->callback = callback;
  LIST_FOREACH(c, &mux->mm_descrambler_caids, link) {
    if (c->caid == caid) {
      emm->pid = pid = c->pid;
      break;
    }
  }
  TAILQ_INSERT_TAIL(&mux->mm_descrambler_emms, emm, link);
  if (pid != EMM_PID_UNKNOWN) {
    tvhtrace(LS_DESCRAMBLER,
             "attach emm caid %04X (%i) pid %04X (%i) - direct",
             caid, caid, pid, pid);
    descrambler_open_pid_(mux, opaque, pid, callback, NULL);
  }
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
  return 1;
}

int
descrambler_close_emm( mpegts_mux_t *mux, void *opaque, int caid )
{
  descrambler_emm_t *emm;
  int pid;

  if (mux == NULL)
    return 0;
  pthread_mutex_lock(&mux->mm_descrambler_lock);
  TAILQ_FOREACH(emm, &mux->mm_descrambler_emms, link)
    if (emm->caid == caid && emm->opaque == opaque)
      break;
  if (!emm) {
    pthread_mutex_unlock(&mux->mm_descrambler_lock);
    return 0;
  }
  TAILQ_REMOVE(&mux->mm_descrambler_emms, emm, link);
  pid  = emm->pid;
  if (pid != EMM_PID_UNKNOWN) {
    caid = emm->caid;
    tvhtrace(LS_DESCRAMBLER, "close emm caid %04X (%i) pid %04X (%i) - direct", caid, caid, pid, pid);
    descrambler_close_pid_(mux, opaque, pid);
  }
  pthread_mutex_unlock(&mux->mm_descrambler_lock);
  free(emm);
  return 1;
}

/* **************************************************************************
 * Editor
 *
 * vim:sts=2:ts=2:sw=2:et
 * *************************************************************************/
