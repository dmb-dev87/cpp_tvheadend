/*
 *  TV Input - Linux DVB interface
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

#include <pthread.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include <libhts/htssettings.h>

#include "tvhead.h"
#include "dvb.h"
#include "channels.h"
#include "transports.h"
#include "teletext.h"
#include "psi.h"
#include "dvb_support.h"
#include "notify.h"

struct th_dvb_mux_instance_tree dvb_muxes;

static struct strtab muxfestatustab[] = {
  { "Unknown",      TDMI_FE_UNKNOWN },
  { "No signal",    TDMI_FE_NO_SIGNAL },
  { "Faint signal", TDMI_FE_FAINT_SIGNAL },
  { "Bad signal",   TDMI_FE_BAD_SIGNAL },
  { "Constant FEC", TDMI_FE_CONSTANT_FEC },
  { "Bursty FEC",   TDMI_FE_BURSTY_FEC },
  { "OK",           TDMI_FE_OK },
};


/**
 *  Return a readable status text for the given mux
 */
const char *
dvb_mux_status(th_dvb_mux_instance_t *tdmi)
{
  return val2str(tdmi->tdmi_fe_status, muxfestatustab) ?: "Invalid";
}




/**
 *
 */
static int
tdmi_cmp(th_dvb_mux_instance_t *a, th_dvb_mux_instance_t *b)
{
  if(a->tdmi_switchport != b->tdmi_switchport)
    return a->tdmi_switchport - b->tdmi_switchport;

  if(a->tdmi_fe_params.frequency != b->tdmi_fe_params.frequency)
    return a->tdmi_fe_params.frequency - b->tdmi_fe_params.frequency;

  return a->tdmi_polarisation - b->tdmi_polarisation;
}

/**
 *
 */
static int
tdmi_global_cmp(th_dvb_mux_instance_t *a, th_dvb_mux_instance_t *b)
{
  return strcmp(a->tdmi_identifier, b->tdmi_identifier);
}


/**
 * Create a new mux on the given adapter, return NULL if it already exists
 */
th_dvb_mux_instance_t *
dvb_mux_create(th_dvb_adapter_t *tda, struct dvb_frontend_parameters *fe_param,
	       int polarisation, int switchport,
	       uint16_t tsid, const char *network, const char *source)
{
  th_dvb_mux_instance_t *tdmi;
  static th_dvb_mux_instance_t *skel;
  char buf[200];
  char qpsktxt[20];
  int entries_before = tda->tda_muxes.entries;

  lock_assert(&global_lock);

  if(skel == NULL)
    skel = calloc(1, sizeof(th_dvb_mux_instance_t));

  skel->tdmi_polarisation = polarisation;
  skel->tdmi_switchport = switchport;
  skel->tdmi_fe_params.frequency = fe_param->frequency;

  tdmi = RB_INSERT_SORTED(&tda->tda_muxes, skel, tdmi_adapter_link, tdmi_cmp);
  if(tdmi != NULL)
    return NULL;

  tdmi = skel;
  skel = NULL;

  tdmi->tdmi_scan_queue = &tda->tda_scan_queues[DVB_MUX_SCAN_INITIAL];
  TAILQ_INSERT_TAIL(tdmi->tdmi_scan_queue, tdmi, tdmi_scan_link);

  tdmi->tdmi_transport_stream_id = tsid;
  tdmi->tdmi_adapter = tda;
  tdmi->tdmi_network = network ? strdup(network) : NULL;
  tdmi->tdmi_quality = 100;

  if(entries_before == 0 && tda->tda_rootpath != NULL) {
    /* First mux on adapter with backing hardware, start scanner */
    gtimer_arm(&tda->tda_mux_scanner_timer, dvb_adapter_mux_scanner, tda, 1);
  }

  memcpy(&tdmi->tdmi_fe_params, fe_param, 
	 sizeof(struct dvb_frontend_parameters));

  if(tda->tda_type == FE_QPSK)
    snprintf(qpsktxt, sizeof(qpsktxt), "_%s_%d",
	     dvb_polarisation_to_str(polarisation), switchport);
  else
    qpsktxt[0] = 0;

  snprintf(buf, sizeof(buf), "%s%d%s", 
	   tda->tda_identifier,fe_param->frequency, qpsktxt);

  tdmi->tdmi_identifier = strdup(buf);

  RB_INSERT_SORTED(&dvb_muxes, tdmi, tdmi_global_link, tdmi_global_cmp);

  if(source != NULL) {
    dvb_mux_nicename(buf, sizeof(buf), tdmi);
    tvhlog(LOG_NOTICE, "dvb", "New mux \"%s\" created by %s", buf, source);

    dvb_mux_save(tdmi);
    dvb_adapter_notify_reload(tda);
  }

  dvb_transport_load(tdmi);

  return tdmi;
}

/**
 * Destroy a DVB mux (it might come back by itself very soon though :)
 */
void
dvb_mux_destroy(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  th_transport_t *t;

  lock_assert(&global_lock);
  
  hts_settings_remove("dvbmuxes/%s/%s",
		      tda->tda_identifier, tdmi->tdmi_identifier);  

  while((t = LIST_FIRST(&tdmi->tdmi_transports)) != NULL)
    transport_destroy(t);

  if(tda->tda_mux_current == tdmi)
    dvb_fe_stop(tda->tda_mux_current);

  RB_REMOVE(&dvb_muxes, tdmi, tdmi_global_link);
  RB_REMOVE(&tda->tda_muxes, tdmi, tdmi_adapter_link);

  if(tdmi->tdmi_scan_queue != NULL)
    TAILQ_REMOVE(tdmi->tdmi_scan_queue, tdmi, tdmi_scan_link);

  hts_settings_remove("dvbmuxes/%s", tdmi->tdmi_identifier);

  free(tdmi->tdmi_network);
  free(tdmi->tdmi_identifier);
  free(tdmi);
}


/**
 *
 */
th_dvb_mux_instance_t *
dvb_mux_find_by_identifier(const char *identifier)
{
  th_dvb_mux_instance_t skel;

  lock_assert(&global_lock);

  skel.tdmi_identifier = (char *)identifier;
  return RB_FIND(&dvb_muxes, &skel, tdmi_global_link, tdmi_global_cmp);
}






static struct strtab fectab[] = {
  { "NONE", FEC_NONE },
  { "1/2",  FEC_1_2 },
  { "2/3",  FEC_2_3 },
  { "3/4",  FEC_3_4 },
  { "4/5",  FEC_4_5 },
  { "5/6",  FEC_5_6 },
  { "6/7",  FEC_6_7 },
  { "7/8",  FEC_7_8 },
  { "8/9",  FEC_8_9 },
  { "AUTO", FEC_AUTO }
};

static struct strtab qamtab[] = {
  { "QPSK",   QPSK },
  { "QAM16",  QAM_16 },
  { "QAM32",  QAM_32 },
  { "QAM64",  QAM_64 },
  { "QAM128", QAM_128 },
  { "QAM256", QAM_256 },
  { "AUTO",   QAM_AUTO },
  { "8VSB",   VSB_8 },
  { "16VSB",  VSB_16 }
};

static struct strtab bwtab[] = {
  { "8MHz", BANDWIDTH_8_MHZ },
  { "7MHz", BANDWIDTH_7_MHZ },
  { "6MHz", BANDWIDTH_6_MHZ },
  { "AUTO", BANDWIDTH_AUTO }
};

static struct strtab modetab[] = {
  { "2k",   TRANSMISSION_MODE_2K },
  { "8k",   TRANSMISSION_MODE_8K },
  { "AUTO", TRANSMISSION_MODE_AUTO }
};

static struct strtab guardtab[] = {
  { "1/32", GUARD_INTERVAL_1_32 },
  { "1/16", GUARD_INTERVAL_1_16 },
  { "1/8",  GUARD_INTERVAL_1_8 },
  { "1/4",  GUARD_INTERVAL_1_4 },
  { "AUTO", GUARD_INTERVAL_AUTO },
};

static struct strtab hiertab[] = {
  { "NONE", HIERARCHY_NONE },
  { "1",    HIERARCHY_1 },
  { "2",    HIERARCHY_2 },
  { "4",    HIERARCHY_4 },
  { "AUTO", HIERARCHY_AUTO }
};

static struct strtab poltab[] = {
  { "Vertical",      POLARISATION_VERTICAL },
  { "Horizontal",    POLARISATION_HORIZONTAL },
};


/**
 *
 */
void
dvb_mux_save(th_dvb_mux_instance_t *tdmi)
{
  struct dvb_frontend_parameters *f = &tdmi->tdmi_fe_params;

  htsmsg_t *m = htsmsg_create_map();

  htsmsg_add_u32(m, "quality", tdmi->tdmi_quality);
  htsmsg_add_str(m, "status", dvb_mux_status(tdmi));

  htsmsg_add_u32(m, "transportstreamid", tdmi->tdmi_transport_stream_id);
  if(tdmi->tdmi_network != NULL)
    htsmsg_add_str(m, "network", tdmi->tdmi_network);

  htsmsg_add_u32(m, "frequency", f->frequency);

  switch(tdmi->tdmi_adapter->tda_type) {
  case FE_OFDM:
    htsmsg_add_str(m, "bandwidth",
		   val2str(f->u.ofdm.bandwidth, bwtab));

    htsmsg_add_str(m, "constellation", 
	    val2str(f->u.ofdm.constellation, qamtab));

    htsmsg_add_str(m, "transmission_mode", 
	    val2str(f->u.ofdm.transmission_mode, modetab));

    htsmsg_add_str(m, "guard_interval", 
	    val2str(f->u.ofdm.guard_interval, guardtab));

    htsmsg_add_str(m, "hierarchy", 
	    val2str(f->u.ofdm.hierarchy_information, hiertab));

    htsmsg_add_str(m, "fec_hi", 
	    val2str(f->u.ofdm.code_rate_HP, fectab));

    htsmsg_add_str(m, "fec_lo", 
	    val2str(f->u.ofdm.code_rate_LP, fectab));
    break;

  case FE_QPSK:
    htsmsg_add_u32(m, "symbol_rate", f->u.qpsk.symbol_rate);

    htsmsg_add_str(m, "fec", 
	    val2str(f->u.qpsk.fec_inner, fectab));

    htsmsg_add_str(m, "polarisation", 
	    val2str(tdmi->tdmi_polarisation, poltab));
 
    htsmsg_add_u32(m, "switchport", tdmi->tdmi_switchport);
    break;

  case FE_QAM:
    htsmsg_add_u32(m, "symbol_rate", f->u.qam.symbol_rate);

    htsmsg_add_str(m, "fec", 
	    val2str(f->u.qam.fec_inner, fectab));

    htsmsg_add_str(m, "constellation", 
	    val2str(f->u.qam.modulation, qamtab));
    break;

  case FE_ATSC:
    break;
  }

  hts_settings_save(m, "dvbmuxes/%s/%s", 
		    tdmi->tdmi_adapter->tda_identifier, tdmi->tdmi_identifier);
  htsmsg_destroy(m);
}


/**
 *
 */
static const char *
tdmi_create_by_msg(th_dvb_adapter_t *tda, htsmsg_t *m)
{
  th_dvb_mux_instance_t *tdmi;
  struct dvb_frontend_parameters f;
  const char *s;
  int r;
  int polarisation = 0;
  unsigned int switchport = 0;
  unsigned int tsid, u32;

  memset(&f, 0, sizeof(f));
  
  f.inversion = INVERSION_AUTO;
  htsmsg_get_u32(m, "frequency", &f.frequency);


  switch(tda->tda_type) {
  case FE_OFDM:
    s = htsmsg_get_str(m, "bandwidth");
    if(s == NULL || (r = str2val(s, bwtab)) < 0)
      return "Invalid bandwidth";
    f.u.ofdm.bandwidth = r;

    s = htsmsg_get_str(m, "constellation");
    if(s == NULL || (r = str2val(s, qamtab)) < 0)
      return "Invalid QAM constellation";
    f.u.ofdm.constellation = r;

    s = htsmsg_get_str(m, "transmission_mode");
    if(s == NULL || (r = str2val(s, modetab)) < 0)
      return "Invalid transmission mode";
    f.u.ofdm.transmission_mode = r;

    s = htsmsg_get_str(m, "guard_interval");
    if(s == NULL || (r = str2val(s, guardtab)) < 0)
      return "Invalid guard interval";
    f.u.ofdm.guard_interval = r;

    s = htsmsg_get_str(m, "hierarchy");
    if(s == NULL || (r = str2val(s, hiertab)) < 0)
      return "Invalid heirarchy information";
    f.u.ofdm.hierarchy_information = r;

    s = htsmsg_get_str(m, "fec_hi");
    if(s == NULL || (r = str2val(s, fectab)) < 0)
      return "Invalid hi-FEC";
    f.u.ofdm.code_rate_HP = r;

    s = htsmsg_get_str(m, "fec_lo");
    if(s == NULL || (r = str2val(s, fectab)) < 0)
      return "Invalid lo-FEC";
    f.u.ofdm.code_rate_LP = r;
    break;

  case FE_QPSK:
    htsmsg_get_u32(m, "symbol_rate", &f.u.qpsk.symbol_rate);
    if(f.u.qpsk.symbol_rate == 0)
      return "Invalid symbol rate";
    
    s = htsmsg_get_str(m, "fec");
    if(s == NULL || (r = str2val(s, fectab)) < 0)
      return "Invalid FEC";
    f.u.qpsk.fec_inner = r;

    s = htsmsg_get_str(m, "polarisation");
    if(s == NULL || (r = str2val(s, poltab)) < 0)
      return "Invalid polarisation";
    polarisation = r;
    
    htsmsg_get_u32(m, "switchport", &switchport);
    break;

  case FE_QAM:
    htsmsg_get_u32(m, "symbol_rate", &f.u.qam.symbol_rate);
    if(f.u.qam.symbol_rate == 0)
      return "Invalid symbol rate";
    
    s = htsmsg_get_str(m, "constellation");
    if(s == NULL || (r = str2val(s, qamtab)) < 0)
      return "Invalid QAM constellation";
    f.u.qam.modulation = r;

    s = htsmsg_get_str(m, "fec");
    if(s == NULL || (r = str2val(s, fectab)) < 0)
      return "Invalid FEC";
    f.u.qam.fec_inner = r;
    break;

  case FE_ATSC:
    break;
  }

  if(htsmsg_get_u32(m, "transportstreamid", &tsid))
    tsid = 0xffff;

  tdmi = dvb_mux_create(tda, &f, polarisation, switchport,
			tsid, htsmsg_get_str(m, "network"), NULL);
  if(tdmi != NULL) {

    if((s = htsmsg_get_str(m, "status")) != NULL)
      tdmi->tdmi_fe_status = str2val(s, muxfestatustab);

    if(!htsmsg_get_u32(m, "quality", &u32)) {
      tdmi->tdmi_quality = u32;

      if(tdmi->tdmi_scan_queue != NULL)
	TAILQ_REMOVE(tdmi->tdmi_scan_queue, tdmi, tdmi_scan_link);

      if(tdmi->tdmi_quality == 100) {
	tdmi->tdmi_scan_queue = &tda->tda_scan_queues[DVB_MUX_SCAN_OK];
      } else {
	tdmi->tdmi_scan_queue = &tda->tda_scan_queues[DVB_MUX_SCAN_BAD];
      }
      TAILQ_INSERT_TAIL(tdmi->tdmi_scan_queue, tdmi, tdmi_scan_link);
    }

  }
  return NULL;
}



/**
 *
 */
void
dvb_mux_load(th_dvb_adapter_t *tda)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;

  if((l = hts_settings_load("dvbmuxes/%s", tda->tda_identifier)) == NULL)
    return;
 
  HTSMSG_FOREACH(f, l) {
    if((c = htsmsg_get_map_by_field(f)) == NULL)
      continue;
    
    tdmi_create_by_msg(tda, c);
  }
  htsmsg_destroy(l);
}

/**
 *
 */
void
dvb_mux_set_networkname(th_dvb_mux_instance_t *tdmi, const char *networkname)
{
  htsmsg_t *m = htsmsg_create_map();
  char buf[100];

  htsmsg_add_str(m, "id", tdmi->tdmi_identifier);

  free((void *)tdmi->tdmi_network);
  tdmi->tdmi_network = strdup(networkname);
  dvb_mux_save(tdmi);

  dvb_mux_nicename(buf, sizeof(buf), tdmi);
  htsmsg_add_str(m, "name", buf);
  notify_by_msg("dvbmux", m);
}
