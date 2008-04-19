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

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include <libhts/htscfg.h>

#include "tvhead.h"
#include "dispatch.h"
#include "dvb.h"
#include "channels.h"
#include "transports.h"
#include "subscriptions.h"
#include "teletext.h"
#include "epg.h"
#include "psi.h"
#include "dvb_support.h"
#include "dvb_dvr.h"
#include "dvb_muxconfig.h"
#include "notify.h"

struct th_dvb_adapter_list dvb_adapters;
struct th_dvb_mux_instance_list dvb_muxes;
static void dvb_mux_scanner(void *aux, int64_t now);
static void dvb_fec_monitor(void *aux, int64_t now);

static void dvb_tda_load(th_dvb_adapter_t *tda);
static void dvb_tdmi_load(th_dvb_mux_instance_t *tdmi);
static void dvb_transport_config_change(th_transport_t *t);
static const char *dvb_source_name(th_transport_t *t);

static void
dvb_add_adapter(const char *path)
{
  char fname[256];
  int fe, i, r;
  th_dvb_adapter_t *tda;
  char buf[400];

  snprintf(fname, sizeof(fname), "%s/frontend0", path);
  
  fe = open(fname, O_RDWR | O_NONBLOCK);
  if(fe == -1) {
    if(errno != ENOENT)
      syslog(LOG_ALERT, "Unable to open %s -- %s\n", fname, strerror(errno));
    return;
  }
  tda = calloc(1, sizeof(th_dvb_adapter_t));

  tda->tda_rootpath = strdup(path);
  tda->tda_demux_path = malloc(256);
  snprintf(tda->tda_demux_path, 256, "%s/demux0", path);
  tda->tda_dvr_path = malloc(256);
  snprintf(tda->tda_dvr_path, 256, "%s/dvr0", path);


  tda->tda_fe_fd = fe;

  tda->tda_fe_info = malloc(sizeof(struct dvb_frontend_info));

  if(ioctl(tda->tda_fe_fd, FE_GET_INFO, tda->tda_fe_info)) {
    syslog(LOG_ALERT, "%s: Unable to query adapter\n", fname);
    close(fe);
    free(tda);
    return;
  }

  if(dvb_dvr_init(tda) < 0) {
    close(fe);
    free(tda);
    return;
  }
  
  pthread_mutex_init(&tda->tda_lock, NULL);
  pthread_cond_init(&tda->tda_cond, NULL);
  TAILQ_INIT(&tda->tda_fe_cmd_queue);

  LIST_INSERT_HEAD(&dvb_adapters, tda, tda_global_link);
  tda->tda_state = TDA_STATE_RUNNING;

  snprintf(buf, sizeof(buf), "%s_%s", tda->tda_rootpath, 
	   tda->tda_fe_info->name);

  r = strlen(buf);
  for(i = 0; i < r; i++)
    if(!isalnum((int)buf[i]))
      buf[i] = '_';

  tda->tda_identifier = strdup(buf);
  tda->tda_displayname = strdup(tda->tda_fe_info->name);

  syslog(LOG_INFO, "Found adapter %s (%s)", path, tda->tda_fe_info->name);
  dtimer_arm(&tda->tda_fec_monitor_timer, dvb_fec_monitor, tda, 1);
  dvb_fe_start(tda);
  dvb_tda_load(tda);
}




void
dvb_init(void)
{
  char path[200];
  int i;

  for(i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/dvb/adapter%d", i);
    dvb_add_adapter(path);
  }
}



/**
 * Find a transport based on 'serviceid' on the given mux
 *
 * If it cannot be found we create it  if 'pmt_pid' is also set
 */
th_transport_t *
dvb_find_transport(th_dvb_mux_instance_t *tdmi, uint16_t sid, int pmt_pid,
		   int *created)
{
  th_transport_t *t;
  char tmp[200];

  if(created != NULL)
    *created = 0;

  LIST_FOREACH(t, &tdmi->tdmi_transports, tht_mux_link) {
    if(t->tht_dvb_service_id == sid)
      return t;
  }
  
  if(pmt_pid == 0)
    return NULL;

  if(created != NULL)
    *created = 1;

  snprintf(tmp, sizeof(tmp), "%s_%04x", tdmi->tdmi_identifier, sid);

  t = transport_create(tmp, TRANSPORT_DVB, THT_MPEG_TS);

  t->tht_dvb_service_id = sid;
  t->tht_pmt            = pmt_pid;

  t->tht_start_feed = dvb_start_feed;
  t->tht_stop_feed  = dvb_stop_feed;
  t->tht_config_change = dvb_transport_config_change;
  t->tht_sourcename = dvb_source_name;
  t->tht_dvb_mux_instance = tdmi;
  
  LIST_INSERT_HEAD(&tdmi->tdmi_transports, t, tht_mux_link);
  return t;
}



/**
 *
 */
static void
dvb_fec_monitor(void *aux, int64_t now)
{
  th_dvb_adapter_t *tda = aux;
  th_dvb_mux_instance_t *tdmi;
  int i, v, vv;
  const char *s;

  dtimer_arm(&tda->tda_fec_monitor_timer, dvb_fec_monitor, tda, 1);

  tdmi = tda->tda_mux_current;
  if(tdmi == NULL)
    return;
  if(tdmi->tdmi_status == NULL) {

    v = vv = 0;
    for(i = 0; i < TDMI_FEC_ERR_HISTOGRAM_SIZE; i++) {
      if(tdmi->tdmi_fec_err_histogram[i] > DVB_FEC_ERROR_LIMIT)
	v++;
      vv += tdmi->tdmi_fec_err_histogram[i];
    }
    vv /= TDMI_FEC_ERR_HISTOGRAM_SIZE;

    if(v == TDMI_FEC_ERR_HISTOGRAM_SIZE) {
      if(LIST_FIRST(&tda->tda_transports) != NULL) {
	syslog(LOG_ERR, 
	       "\"%s\": Constant rate of FEC errors (average at %d / s), "
	       "last %d seconds, flushing subscribers\n", 
	       tdmi->tdmi_identifier, vv,
	       TDMI_FEC_ERR_HISTOGRAM_SIZE);

	dvb_adapter_clean(tdmi->tdmi_adapter);
      }
    }
  }
  
  s = dvb_mux_status(tdmi);
  if(s != tdmi->tdmi_last_status) {
    tdmi->tdmi_last_status = s;
    notify_tdmi_status_change(tdmi);
  }
}

/**
 * If nobody is subscribing, cycle thru all muxes to get some stats
 * and EIT updates
 */
static void
dvb_mux_scanner(void *aux, int64_t now)
{
  th_dvb_adapter_t *tda = aux;
  th_dvb_mux_instance_t *tdmi;

  dtimer_arm(&tda->tda_mux_scanner_timer, dvb_mux_scanner, tda, 10);

  if(transport_compute_weight(&tda->tda_transports) > 0)
    return; /* someone is here */

  tdmi = tda->tda_mux_current;
  tdmi = tdmi != NULL ? LIST_NEXT(tdmi, tdmi_adapter_link) : NULL;
  tdmi = tdmi != NULL ? tdmi : LIST_FIRST(&tda->tda_muxes);

  if(tdmi == NULL)
    return; /* no instances */

  dvb_tune_tdmi(tdmi, 0, TDMI_IDLESCAN);
}

/**
 *
 */
static int
tdmi_inssort(th_dvb_mux_instance_t *a, th_dvb_mux_instance_t *b)
{
  return a->tdmi_fe_params->frequency - b->tdmi_fe_params->frequency;
}

/**
 * Create a new mux on the given adapter, return NULL if it already exists
 */
th_dvb_mux_instance_t *
dvb_mux_create(th_dvb_adapter_t *tda, struct dvb_frontend_parameters *fe_param,
	       int polarisation, int switchport, int save, uint16_t tsid)
{
  th_dvb_mux_instance_t *tdmi;
  char buf[200];
  char qpsktxt[20];

  LIST_FOREACH(tdmi, &tda->tda_muxes, tdmi_adapter_link) {
    if(tdmi->tdmi_fe_params->frequency == fe_param->frequency &&
       tdmi->tdmi_polarisation         == polarisation &&
       tdmi->tdmi_switchport           == switchport)
      return NULL;
  }

  tdmi = calloc(1, sizeof(th_dvb_mux_instance_t));
  pthread_mutex_init(&tdmi->tdmi_table_lock, NULL);
  tdmi->tdmi_state = TDMI_IDLE;
  tdmi->tdmi_transport_stream_id = tsid;
  tdmi->tdmi_adapter = tda;

  if(LIST_FIRST(&tda->tda_muxes) == NULL) {
    /* First mux, start scanner */
    dtimer_arm(&tda->tda_mux_scanner_timer, dvb_mux_scanner, tda, 1);
  }


  tdmi->tdmi_fe_params = malloc(sizeof(struct dvb_frontend_parameters));
  tdmi->tdmi_polarisation = polarisation;
  tdmi->tdmi_switchport = switchport;
  memcpy(tdmi->tdmi_fe_params, fe_param,
	 sizeof(struct dvb_frontend_parameters));

  if(tda->tda_fe_info->type == FE_QPSK)
    snprintf(qpsktxt, sizeof(qpsktxt), "_%s_%d",
	     dvb_polarisation_to_str(polarisation), switchport);
  else
    qpsktxt[0] = 0;

  snprintf(buf, sizeof(buf), "%s%d%s", 
	   tda->tda_identifier,fe_param->frequency, qpsktxt);

  LIST_INSERT_SORTED(&tda->tda_muxes, tdmi, tdmi_adapter_link, tdmi_inssort);
  LIST_INSERT_HEAD(&dvb_muxes, tdmi, tdmi_global_link);

  tdmi->tdmi_identifier = strdup(buf);

  if(save) {
    dvb_tda_save(tda);
    notify_tda_change(tda);
  } else {
    dvb_tdmi_load(tdmi);
  }

  return tdmi;
}




/**
 * Save config for the given adapter
 */
void
dvb_tda_save(th_dvb_adapter_t *tda)
{
  th_dvb_mux_instance_t *tdmi;
  FILE *fp;
  char buf[400];

  snprintf(buf, sizeof(buf), "%s/dvbadapters/%s",
	   settings_dir, tda->tda_identifier);
  if((fp = settings_open_for_write(buf)) == NULL)
    return;

  LIST_FOREACH(tdmi, &tda->tda_muxes, tdmi_adapter_link) {

    fprintf(fp, "mux {\n");
    dvb_mux_store(fp, tdmi);
    fprintf(fp, "}\n");
  }
  fclose(fp);
}

/**
 * Load config for the given adapter
 */
static void
dvb_tda_load(th_dvb_adapter_t *tda)
{
  struct config_head cl;
  config_entry_t *ce;
  char buf[400];
  const char *v;

  snprintf(buf, sizeof(buf), "%s/dvbadapters/%s",
	   settings_dir, tda->tda_identifier);

  TAILQ_INIT(&cl);
  config_read_file0(buf, &cl);

  TAILQ_FOREACH(ce, &cl, ce_link) {
    if(ce->ce_type != CFG_SUB || strcasecmp("mux", ce->ce_key))
      continue;

    v = dvb_mux_create_str(tda,
			   config_get_str_sub(&ce->ce_sub,
					      "transportstreamid", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "frequency", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "symbol_rate", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "constellation", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "fec", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "fec_hi", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "fec_lo", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "bandwidth", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "transmission_mode", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "guard_interval", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "hierarchy", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "polarisation", NULL),
			   config_get_str_sub(&ce->ce_sub,
					      "switchport", NULL),
			   0);

    if(v != NULL)
      syslog(LOG_ALERT, "Unable to init saved mux on %s -- %s\n",
	     tda->tda_identifier, v);
  }
  config_free0(&cl);
}

/**
 * Save config for the given mux
 */
void
dvb_tdmi_save(th_dvb_mux_instance_t *tdmi)
{
  th_transport_t *t;
  FILE *fp;
  char buf[400];

  snprintf(buf, sizeof(buf), "%s/dvbmuxes/%s",
	   settings_dir, tdmi->tdmi_identifier);
  if((fp = settings_open_for_write(buf)) == NULL)
    return;

  LIST_FOREACH(t, &tdmi->tdmi_transports, tht_mux_link) {
    fprintf(fp, "service {\n");
    fprintf(fp, "\tservice_id = %d\n", t->tht_dvb_service_id);
    fprintf(fp, "\tpmt = %d\n", t->tht_pmt);
    fprintf(fp, "\tstype = %d\n", t->tht_servicetype);
    fprintf(fp, "\tscrambled = %d\n", t->tht_scrambled);

    if(t->tht_provider != NULL)
      fprintf(fp, "\tprovider = %s\n", t->tht_provider);

    if(t->tht_servicename)
      fprintf(fp, "\tservicename = %s\n", t->tht_servicename);

    if(t->tht_channelname)
      fprintf(fp, "\tchannelname = %s\n", t->tht_channelname);

    fprintf(fp, "\tmapped = %d\n", t->tht_channel ? 1 : 0);

    psi_save_transport(fp, t);

    fprintf(fp, "}\n");
  }
  fclose(fp);
}

/**
 * Load config for the given mux
 */
static void
dvb_tdmi_load(th_dvb_mux_instance_t *tdmi)
{
  struct config_head cl;
  config_entry_t *ce;
  char buf[400];
  const char *v;
  int sid, pmt;
  th_transport_t *t;

  snprintf(buf, sizeof(buf), "%s/dvbmuxes/%s",
	   settings_dir, tdmi->tdmi_identifier);

  TAILQ_INIT(&cl);
  config_read_file0(buf, &cl);

  TAILQ_FOREACH(ce, &cl, ce_link) {
    if(ce->ce_type != CFG_SUB || strcasecmp("service", ce->ce_key))
      continue;

    sid = atoi(config_get_str_sub(&ce->ce_sub, "service_id", "0"));
    pmt = atoi(config_get_str_sub(&ce->ce_sub, "pmt",        "0"));
    if(sid < 1 || pmt < 1)
      continue;
    
    t = dvb_find_transport(tdmi, sid, pmt, NULL);
    
    t->tht_servicetype = atoi(config_get_str_sub(&ce->ce_sub, "stype", "0"));
    t->tht_scrambled = atoi(config_get_str_sub(&ce->ce_sub, "scrambled", "0"));

    v = config_get_str_sub(&ce->ce_sub, "provider", "unknown");
    free((void *)t->tht_provider);
    t->tht_provider = strdup(v);

    v = config_get_str_sub(&ce->ce_sub, "servicename", "unknown");
    free((void *)t->tht_servicename);
    t->tht_servicename = strdup(v);

    v = config_get_str_sub(&ce->ce_sub, "channelname", NULL);
    if(v != NULL) {
      free((void *)t->tht_channelname);
      t->tht_channelname = strdup(v);
    } else {
      t->tht_channelname = strdup(t->tht_servicename);
    }

    psi_load_transport(&ce->ce_sub, t);

    if(atoi(config_get_str_sub(&ce->ce_sub, "mapped", "0"))) {
      transport_set_channel(t, t->tht_channelname);
    }
  }
  config_free0(&cl);
}

/**
 * Called when config changes for the given transport
 */
static void
dvb_transport_config_change(th_transport_t *t)
{
  th_dvb_mux_instance_t *tdmi = t->tht_dvb_mux_instance;

  dvb_tdmi_save(tdmi);
}


/**
 * Generate a descriptive name for the source
 */
static const char *
dvb_source_name(th_transport_t *t)
{
  th_dvb_mux_instance_t *tdmi;
  static char buf[200];

  tdmi = t->tht_dvb_mux_instance;

  snprintf(buf, sizeof(buf), "\"%s\" on \"%s\"",
	   tdmi->tdmi_network ?: "Unknown network",
	   tdmi->tdmi_adapter->tda_rootpath);

  return buf;
}
