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

struct th_dvb_mux_list dvb_muxes;
struct th_dvb_adapter_list dvb_adapters_probing;
struct th_dvb_adapter_list dvb_adapters_running;

static void dvb_tdt_add_demux(th_dvb_mux_instance_t *tdmi);
static void dvb_eit_add_demux(th_dvb_mux_instance_t *tdmi);
static void dvb_sdt_add_demux(th_dvb_mux_instance_t *tdmi);
static void dvb_pat_add_demux(th_dvb_mux_instance_t *tdmi);
static void dvb_cat_add_demux(th_dvb_mux_instance_t *tdmi);

static void tdmi_check_scan_status(th_dvb_mux_instance_t *tdmi);

static void dvb_start_initial_scan(th_dvb_mux_instance_t *tdmi);

static void tdmi_activate(th_dvb_mux_instance_t *tdmi);

static void dvb_mux_scanner(void *aux, int64_t now);

static void dvb_fec_monitor(void *aux, int64_t now);

static void
tda_parse_status(th_dvb_adapter_t *tda, fe_status_t fe_status)
{
  th_dvb_mux_instance_t *tdmi;
  int v;

  tdmi = tda->tda_mux_current;
  if(tdmi != NULL) {

    if(fe_status & FE_HAS_LOCK) {
      tdmi->tdmi_status = NULL;
    } else if(fe_status & FE_HAS_SYNC)
      tdmi->tdmi_status = "No lock, but sync ok";
    else if(fe_status & FE_HAS_VITERBI)
      tdmi->tdmi_status = "No lock, but FEC stable";
    else if(fe_status & FE_HAS_CARRIER)
      tdmi->tdmi_status = "No lock, but carrier present";
    else if(fe_status & FE_HAS_SIGNAL)
      tdmi->tdmi_status = "No lock, but faint signal present";
    else
      tdmi->tdmi_status = "No signal";

    /* Reset FEC Error counter */

    ioctl(tda->tda_fe_fd, FE_READ_UNCORRECTED_BLOCKS, &v);
  }
}



static void
dvb_frontend_event(int events, void *opaque, int fd)
{
  th_dvb_adapter_t *tda = opaque;
  struct dvb_frontend_event ev;
  int r;

  if(!(events & DISPATCH_PRI))
    return;

  pthread_mutex_lock(&tda->tda_mux_lock);

  r = ioctl(fd, FE_GET_EVENT, &ev);
  if(r == 0)
    tda_parse_status(tda, ev.status);

  pthread_mutex_unlock(&tda->tda_mux_lock);
}



static void
dvb_add_adapter(const char *path)
{
  char fname[256];
  int fe;
  th_dvb_adapter_t *tda;

  snprintf(fname, sizeof(fname), "%s/frontend0", path);
  
  fe = open(fname, O_RDWR | O_NONBLOCK);
  if(fe == -1) {
    if(errno != ENOENT)
      syslog(LOG_ALERT, "Unable to open %s -- %s\n", fname, strerror(errno));
    return;
  }
  tda = calloc(1, sizeof(th_dvb_adapter_t));
  tda->tda_path = strdup(path);
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
  
  pthread_mutex_init(&tda->tda_mux_lock, NULL);
  LIST_INSERT_HEAD(&dvb_adapters_probing, tda, tda_link);
  startupcounter++;

  tda->tda_info = strdup(tda->tda_fe_info->name);

  dispatch_addfd(tda->tda_fe_fd, dvb_frontend_event, tda, DISPATCH_PRI);

  syslog(LOG_INFO, "Adding adapter %s (%s)", path, tda->tda_fe_info->name);
  dtimer_arm(&tda->tda_fec_monitor_timer, dvb_fec_monitor, tda, 1);
}




void
dvb_init(void)
{
  th_dvb_adapter_t *tda;
  th_dvb_mux_instance_t *tdmi;
  char path[200];
  int i;

  for(i = 0; i < 32; i++) {
    snprintf(path, sizeof(path), "/dev/dvb/adapter%d", i);
    dvb_add_adapter(path);
  }

  dvb_mux_setup();

  LIST_FOREACH(tda, &dvb_adapters_probing, tda_link) {
    tdmi = LIST_FIRST(&tda->tda_muxes_configured);
    if(tdmi == NULL) {
      syslog(LOG_WARNING,
	     "No muxes configured on \"%s\" DVB adapter unused",
	     tda->tda_path);
      startupcounter--;
    } else {
      dvb_start_initial_scan(tdmi);
    }
  }
}






static void
tdt_destroy(th_dvb_table_t *tdt)
{
  LIST_REMOVE(tdt, tdt_link);
  close(dispatch_delfd(tdt->tdt_handle));
  free(tdt->tdt_name);
  free(tdt);
}


static void
tdmi_stop(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_table_t *tdt;
  while((tdt = LIST_FIRST(&tdmi->tdmi_tables)) != NULL)
    tdt_destroy(tdt);

  tdmi->tdmi_state = TDMI_IDLE;
  time(&tdmi->tdmi_lost_adapter);
}


static void
dvb_table_recv(int events, void *opaque, int fd)
{
  th_dvb_table_t *tdt = opaque;
  uint8_t sec[4096], *ptr;
  int r, len;
  uint8_t tableid;

  if(!(events & DISPATCH_READ))
    return;

  r = read(fd, sec, sizeof(sec));
  if(r < 3)
    return;

  r -= 3;

  tableid = sec[0];
  len = ((sec[1] & 0x0f) << 8) | sec[2];
  
  if(len < r)
    return;

  ptr = &sec[3];
  len -= 3;
  if(!tdt->tdt_callback(tdt->tdt_tdmi, ptr, len, tableid, tdt->tdt_opaque)) {
    tdt->tdt_count++;
  }
  tdmi_check_scan_status(tdt->tdt_tdmi);
}





static void
tdt_add(th_dvb_mux_instance_t *tdmi, int fd, 
	int (*callback)(th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len,
			uint8_t tableid, void *opaque), void *opaque,
	int initial_count, char *name)
{
  th_dvb_table_t *tdt = malloc(sizeof(th_dvb_table_t));

  LIST_INSERT_HEAD(&tdmi->tdmi_tables, tdt, tdt_link);
  tdt->tdt_name = strdup(name);
  tdt->tdt_callback = callback;
  tdt->tdt_opaque = opaque;
  tdt->tdt_tdmi = tdmi;
  tdt->tdt_handle = dispatch_addfd(fd, dvb_table_recv, tdt, DISPATCH_READ);
  tdt->tdt_count = initial_count;
}





int
dvb_tune_tdmi(th_dvb_mux_instance_t *tdmi, int maylog, tdmi_state_t state)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  th_dvb_mux_t *tdm  = tdmi->tdmi_mux;
  fe_status_t fe_status;
  int i;
  
  tdmi->tdmi_state = state;

  if(tda->tda_mux_current == tdmi)
    return 0;

  pthread_mutex_lock(&tda->tda_mux_lock);

  if(tda->tda_mux_current != NULL)
    tdmi_stop(tda->tda_mux_current);

  tda->tda_mux_current = tdmi;

  if(maylog)
    syslog(LOG_DEBUG, "\"%s\" tuning to mux \"%s\"", 
	   tda->tda_path, tdmi->tdmi_mux->tdm_title);

  i = ioctl(tda->tda_fe_fd, FE_SET_FRONTEND, tdm->tdm_fe_params);
  if(i != 0) {
    if(maylog)
      syslog(LOG_ERR, "\"%s\" tuning to transport \"%s\""
	     " -- Front configuration failed -- %s",
	     tda->tda_path, tdm->tdm_title,
	     strerror(errno));
    return -1;
  }

  dvb_tdt_add_demux(tdmi);
  dvb_eit_add_demux(tdmi);
  dvb_sdt_add_demux(tdmi);
  dvb_pat_add_demux(tdmi);
  dvb_cat_add_demux(tdmi);

  time(&tdmi->tdmi_got_adapter);

  if(ioctl(tda->tda_fe_fd, FE_READ_STATUS, &fe_status) < 0)
    fe_status = 0;

  tda_parse_status(tda, fe_status);

  pthread_mutex_unlock(&tda->tda_mux_lock);
  return 0;
}





/*
 *
 */
static int
dvb_service_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		     uint8_t tableid, void *opaque)
{
  th_transport_t *t = opaque;

  return psi_parse_pmt(t, ptr, len, 1);
}


/*
 *
 */

th_transport_t *
dvb_find_transport(th_dvb_mux_instance_t *tdmi, uint16_t tid,
		   uint16_t sid, int pmt_pid)
{
  th_transport_t *t;
  th_dvb_mux_t *tdm = tdmi->tdmi_mux;
  struct dmx_sct_filter_params fparams;
  char pmtname[50];
  int fd;

  LIST_FOREACH(t, &all_transports, tht_global_link) {
    if(t->tht_dvb_transport_id == tid &&
       t->tht_dvb_service_id   == sid)
      return t;
  }
  
  if(pmt_pid == 0)
    return NULL;

  t = calloc(1, sizeof(th_transport_t));
  transport_monitor_init(t);

  t->tht_dvb_transport_id = tid;
  t->tht_dvb_service_id   = sid;

  t->tht_type = TRANSPORT_DVB;
  t->tht_start_feed = dvb_start_feed;
  t->tht_stop_feed  = dvb_stop_feed;
  t->tht_dvb_mux = tdm;

  t->tht_prio = 50;

 
  fd = open(tdmi->tdmi_adapter->tda_demux_path, O_RDWR);
  if(fd == -1) {
    free(t);
    return NULL;
  }

  memset(&fparams, 0, sizeof(fparams));
  fparams.pid = pmt_pid;
  fparams.timeout = 0;
  fparams.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
  fparams.filter.filter[0] = 0x02;
  fparams.filter.mask[0] = 0xff;

  if(ioctl(fd, DMX_SET_FILTER, &fparams) < 0) {
    close(fd);
    free(t);
    return NULL;
  }
  
  snprintf(pmtname, sizeof(pmtname), "PMT(%d), service:%d", 
	   pmt_pid, sid);
	   

  tdt_add(tdmi, fd, dvb_service_callback, t, 0, pmtname);
  t->tht_name = strdup(tdm->tdm_title);
  LIST_INSERT_HEAD(&all_transports, t, tht_global_link);
  return t;
}






/*
 * DVB time and date functions
 */

#define bcdtoint(i) ((((i & 0xf0) >> 4) * 10) + (i & 0x0f))

static time_t
convert_date(uint8_t *dvb_buf)
{
  int i;
  int year, month, day, hour, min, sec;
  long int mjd;
  struct tm dvb_time;

  mjd = (dvb_buf[0] & 0xff) << 8;
  mjd += (dvb_buf[1] & 0xff);
  hour = bcdtoint(dvb_buf[2] & 0xff);
  min = bcdtoint(dvb_buf[3] & 0xff);
  sec = bcdtoint(dvb_buf[4] & 0xff);
  /*
   * Use the routine specified in ETSI EN 300 468 V1.4.1,
   * "Specification for Service Information in Digital Video Broadcasting"
   * to convert from Modified Julian Date to Year, Month, Day.
   */
  year = (int) ((mjd - 15078.2) / 365.25);
  month = (int) ((mjd - 14956.1 - (int) (year * 365.25)) / 30.6001);
  day = mjd - 14956 - (int) (year * 365.25) - (int) (month * 30.6001);
  if (month == 14 || month == 15)
    i = 1;
  else
    i = 0;
  year += i;
  month = month - 1 - i * 12;

  dvb_time.tm_sec = sec;
  dvb_time.tm_min = min;
  dvb_time.tm_hour = hour;
  dvb_time.tm_mday = day;
  dvb_time.tm_mon = month - 1;
  dvb_time.tm_year = year;
  dvb_time.tm_isdst = -1;
  dvb_time.tm_wday = 0;
  dvb_time.tm_yday = 0;
  return (timegm(&dvb_time));
}


static int
dvb_tdt_callback(th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len,
		 uint8_t tableid, void *opaque)
{
  time_t t;
  t = convert_date(buf);
  tdmi->tdmi_time = t;
  time(&t);
  
  t = tdmi->tdmi_time - t;

  if(abs(t) > 5) {
    syslog(LOG_NOTICE, 
	   "\"%s\" DVB network clock is %lds off from system clock",
	   tdmi->tdmi_mux->tdm_name, t);
  }
  return 0;
}


static void
dvb_tdt_add_demux(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  struct dmx_sct_filter_params fparams;
  int fd;

  fd = open(tda->tda_demux_path, O_RDWR);
  if(fd == -1)
    return;

  memset(&fparams, 0, sizeof(fparams));
  fparams.pid = 0x14;
  fparams.timeout = 0;
  fparams.flags = DMX_IMMEDIATE_START;
  fparams.filter.filter[0] = 0x70;
  fparams.filter.mask[0] = 0xff;
  
  if(ioctl(fd, DMX_SET_FILTER, &fparams) < 0) {
    close(fd);
    return;
  }
  tdt_add(tdmi, fd, dvb_tdt_callback, NULL, 1, "tdt");
}





/*
 * DVB Descriptor; Short Event
 */

static int
dvb_desc_short_event(uint8_t *ptr, int len, 
		     char *title, size_t titlelen,
		     char *desc,  size_t desclen)
{
  int r;

  if(len < 4)
    return -1;
  ptr += 3; len -= 3;

  if((r = dvb_get_string_with_len(title, titlelen, ptr, len, "UTF8")) < 0)
    return -1;
  ptr += r; len -= r;

  if((r = dvb_get_string_with_len(desc, desclen, ptr, len, "UTF8")) < 0)
    return -1;

  return 0;
}


/*
 * DVB Descriptor; Service
 */



static int
dvb_desc_service(uint8_t *ptr, int len, uint8_t *typep, 
		 char *provider, size_t providerlen,
		 char *name, size_t namelen)
{
  int r;

  if(len < 2)
    return -1;

  *typep = ptr[0];

  ptr++;
  len--;

  if((r = dvb_get_string_with_len(provider, providerlen, ptr, len, 
				  "UTF8")) < 0)
    return -1;
  ptr += r; len -= r;

  if((r = dvb_get_string_with_len(name, namelen, ptr, len,
				  "UTF8")) < 0)
    return -1;
  ptr += r; len -= r;
  return 0;
}







/*
 * DVB EIT (Event Information Table)
 */


static int
dvb_eit_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  th_transport_t *t;
  th_channel_t *ch;

  uint16_t serviceid;
  int version;
  int current_next_indicator;
  uint8_t section_number;
  uint8_t last_section_number;
  uint16_t transport_stream_id;
  uint16_t original_network_id;
  uint8_t segment_last_section_number;
  uint8_t last_table_id;

  uint16_t event_id;
  time_t start_time;

  int duration;
  int dllen;
  uint8_t dtag, dlen;

  char title[256];
  char desc[5000];

  if(tableid < 0x4e || tableid > 0x6f)
    return -1;

  if(len < 11)
    return -1;

  serviceid                   = ptr[0] << 8 | ptr[1];
  version                     = ptr[2] >> 1 & 0x1f;
  current_next_indicator      = ptr[2] & 1;
  section_number              = ptr[3];
  last_section_number         = ptr[4];
  transport_stream_id         = ptr[5] << 8 | ptr[6];
  original_network_id         = ptr[7] << 8 | ptr[8];
  segment_last_section_number = ptr[9];
  last_table_id               = ptr[10];

  len -= 11;
  ptr += 11;

  t = dvb_find_transport(tdmi, transport_stream_id, serviceid, 0);
  if(t == NULL)
    return -1;
  ch = t->tht_channel;
  if(ch == NULL)
    return -1;

  epg_lock();

  while(len >= 12) {
    event_id                  = ptr[0] << 8 | ptr[1];
    start_time                = convert_date(&ptr[2]);
    duration                  = bcdtoint(ptr[7] & 0xff) * 3600 +
                                bcdtoint(ptr[8] & 0xff) * 60 +
                                bcdtoint(ptr[9] & 0xff);
    dllen                     = ((ptr[10] & 0x0f) << 8) | ptr[11];

    len -= 12;
    ptr += 12;



    if(dllen > len)
      break;
    
    while(dllen > 0) {
      dtag = ptr[0];
      dlen = ptr[1];

      len -= 2; ptr += 2; dllen -= 2; 

      if(dlen > len)
	break;

      switch(dtag) {
      case DVB_DESC_SHORT_EVENT:
	if(dvb_desc_short_event(ptr, dlen,
				title, sizeof(title),
				desc,  sizeof(desc)) < 0)
	  duration = 0;
	break;
      }

      len -= dlen; ptr += dlen; dllen -= dlen;
    }

    if(duration > 0) {
      epg_update_event_by_id(ch, event_id, start_time, duration,
			     title, desc);
      
    }
  }
  
  epg_unlock();
  return 0;
}



static void
dvb_eit_add_demux(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  struct dmx_sct_filter_params fparams;
  int fd;

  fd = open(tda->tda_demux_path, O_RDWR);
  if(fd == -1)
    return;

  memset(&fparams, 0, sizeof(fparams));
  fparams.pid = 0x12;
  fparams.timeout = 0;
  fparams.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
  
  if(ioctl(fd, DMX_SET_FILTER, &fparams) < 0) {
    close(fd);
    return;
  }

  tdt_add(tdmi, fd, dvb_eit_callback, NULL, 1, "eit");
}





/*
 * DVB SDT (Service Description Table)
 */


static int
dvb_sdt_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  th_transport_t *t;
  int version;
  int current_next_indicator;
  uint8_t section_number;
  uint8_t last_section_number;
  uint16_t service_id;
  uint16_t transport_stream_id;
  uint16_t original_network_id;

  int reserved;
  int running_status, free_ca_mode;
  int dllen;
  uint8_t dtag, dlen;

  char provider[256];
  char chname[256];
  uint8_t stype;
  int ret = 0;

  if(len < 8)
    return -1;

  transport_stream_id         = ptr[0] << 8 | ptr[1];
  version                     = ptr[2] >> 1 & 0x1f;
  current_next_indicator      = ptr[2] & 1;
  section_number              = ptr[3];
  last_section_number         = ptr[4];
  original_network_id         = ptr[5] << 8 | ptr[6];
  reserved                    = ptr[7];

  len -= 8;
  ptr += 8;


  while(len >= 5) {
    service_id                = ptr[0] << 8 | ptr[1];
    reserved                  = ptr[2];
    running_status            = (ptr[3] >> 5) & 0x7;
    free_ca_mode              = (ptr[3] >> 4) & 0x1;
    dllen                     = ((ptr[3] & 0x0f) << 8) | ptr[4];

    len -= 5;
    ptr += 5;

    if(dllen > len)
      break;

    stype = 0;
    
    while(dllen > 2) {
      dtag = ptr[0];
      dlen = ptr[1];

      len -= 2; ptr += 2; dllen -= 2; 

      if(dlen > len)
	break;

      switch(dtag) {
      case DVB_DESC_SERVICE:
	if(dvb_desc_service(ptr, dlen, &stype,
			    provider, sizeof(provider),
			    chname, sizeof(chname)) < 0)
	  stype = 0;
	break;
      }

      len -= dlen; ptr += dlen; dllen -= dlen;
    }

    switch(stype) {

    case DVB_ST_SDTV:
    case DVB_ST_HDTV:
    case DVB_ST_AC_SDTV:
    case DVB_ST_AC_HDTV:
      /* TV service */

      t = dvb_find_transport(tdmi, transport_stream_id, service_id, 0);
      
      if(t == NULL)
	ret |= 1;
      else if(t->tht_channel == NULL) {
	/* Not yet mapped to a channel */
	if(LIST_FIRST(&t->tht_streams) != NULL) {
	  /* We have streams, map it */
	  transport_set_channel(t, channel_find(chname, 1, NULL));
	} else {
	  if(t->tht_pmt_seen == 0)
	    ret |= 1; /* Return error (so scanning wont continue yet) */
	}
      }
      break;

    }
  }
  return ret;
}



static void
dvb_sdt_add_demux(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  struct dmx_sct_filter_params fparams;
  int fd;

  fd = open(tda->tda_demux_path, O_RDWR);
  if(fd == -1)
    return;

  memset(&fparams, 0, sizeof(fparams));
  fparams.pid = 0x11;
  fparams.timeout = 0;
  fparams.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
  fparams.filter.filter[0] = 0x42;
  fparams.filter.mask[0] = 0xff;

  if(ioctl(fd, DMX_SET_FILTER, &fparams) < 0) {
    close(fd);
    return;
  }

  tdt_add(tdmi, fd, dvb_sdt_callback, NULL, 0, "sdt");
}

/**
 *
 */


static int
dvb_pat_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  uint16_t service, pmt, tid;

  if(len < 5)
    return -1;

  tid = (ptr[0] << 8) | ptr[1];

  ptr += 5;
  len -= 5;

  while(len >= 4) {
    service =  ptr[0]         << 8 | ptr[1];
    pmt     = (ptr[2] & 0x1f) << 8 | ptr[3];
    
    if(service != 0)
      dvb_find_transport(tdmi, tid, service, pmt);

    ptr += 4;
    len -= 4;
  }
  return 0;
}



static void
dvb_pat_add_demux(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  struct dmx_sct_filter_params fparams;
  int fd;

  fd = open(tda->tda_demux_path, O_RDWR);
  if(fd == -1)
    return;

  memset(&fparams, 0, sizeof(fparams));
  fparams.pid = 0;
  fparams.timeout = 0;
  fparams.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
  fparams.filter.filter[0] = 0x00;
  fparams.filter.mask[0] = 0xff;

  if(ioctl(fd, DMX_SET_FILTER, &fparams) < 0) {
    close(fd);
    return;
  }

  tdt_add(tdmi, fd, dvb_pat_callback, NULL, 0, "pat");
}


/**
 * CAT - Condition Access Table
 */
static int
dvb_cat_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  int tag, tlen;
  uint16_t caid;
  uint16_t pid;

  ptr += 5;
  len -= 5;

  while(len > 2) {
    tag = *ptr++;
    tlen = *ptr++;
    len -= 2;
    switch(tag) {
    case DVB_DESC_CA:
      caid =  (ptr[0]        << 8)  | ptr[1];
      pid  = ((ptr[2] & 0x1f << 8)) | ptr[3];
      break;

    default:
      break;
    }

    ptr += tlen;
    len -= tlen;
  }


  return 0;
}



static void
dvb_cat_add_demux(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  struct dmx_sct_filter_params fparams;
  int fd;

  fd = open(tda->tda_demux_path, O_RDWR);
  if(fd == -1)
    return;

  memset(&fparams, 0, sizeof(fparams));
  fparams.pid = 1;
  fparams.timeout = 0;
  fparams.flags = DMX_IMMEDIATE_START | DMX_CHECK_CRC;
  fparams.filter.filter[0] = 0x01;
  fparams.filter.mask[0] = 0xff;

  if(ioctl(fd, DMX_SET_FILTER, &fparams) < 0) {
    close(fd);
    return;
  }

  tdt_add(tdmi, fd, dvb_cat_callback, NULL, 1, "cat");
}


static void
tdmi_activate(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;

  dtimer_disarm(&tdmi->tdmi_initial_scan_timer);

  tdmi->tdmi_state = TDMI_IDLE;
  
  LIST_REMOVE(tdmi, tdmi_adapter_link);
  LIST_INSERT_HEAD(&tda->tda_muxes_active, tdmi, tdmi_adapter_link);

  /* tune to next configured (but not yet active) mux */

  tdmi = LIST_FIRST(&tda->tda_muxes_configured);
  
  if(tdmi == NULL) {
    startupcounter--;
    syslog(LOG_INFO,
	   "\"%s\" Initial scan completed, adapter available",
	   tda->tda_path);
    /* no more muxes to probe, link adapter to the world */
    LIST_REMOVE(tda, tda_link);
    LIST_INSERT_HEAD(&dvb_adapters_running, tda, tda_link);
    dtimer_arm(&tda->tda_mux_scanner_timer, dvb_mux_scanner, tda, 10);
    return;
  }
  dvb_start_initial_scan(tdmi);
}


static void
tdmi_initial_scan_timeout(void *aux, int64_t now)
{
  th_dvb_mux_instance_t *tdmi = aux;
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  const char *err;
#if 0
  th_dvb_table_t *tdt;
  LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link) {
    printf("%s: %d\n", tdt->tdt_name, tdt->tdt_count);
  }
#endif
  dtimer_disarm(&tdmi->tdmi_initial_scan_timer);

  if(tdmi->tdmi_status != NULL)
    err = tdmi->tdmi_status;
  else
    err = "Missing PSI tables, scan will continue";

  syslog(LOG_DEBUG, "\"%s\" mux \"%s\" Initial scan timed out -- %s",
	 tda->tda_path, tdmi->tdmi_mux->tdm_name, err);

  tdmi_activate(tdmi);
}



static void
tdmi_check_scan_status(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_table_t *tdt;
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;

  if(tdmi->tdmi_state >= TDMI_IDLE)
    return;

  
  LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link)
    if(tdt->tdt_count == 0)
      return;

  /* All tables seen at least once */

  syslog(LOG_DEBUG, "\"%s\" on \"%s\" Initial scan completed",
	 tda->tda_path, tdmi->tdmi_mux->tdm_name);

  tdmi_activate(tdmi);
}



static void
dvb_start_initial_scan(th_dvb_mux_instance_t *tdmi)
{
  dvb_tune_tdmi(tdmi, 1, TDMI_INITIAL_SCAN);

  dtimer_arm(&tdmi->tdmi_initial_scan_timer,
	     tdmi_initial_scan_timeout, tdmi, 5);

}

/**
 *
 *
 */

static int
mux_sort_quality(th_dvb_mux_instance_t *a, th_dvb_mux_instance_t *b)
{
  return a->tdmi_fec_err_per_sec - b->tdmi_fec_err_per_sec;
}


static void
dvb_fec_monitor(void *aux, int64_t now)
{
  th_dvb_adapter_t *tda = aux;
  th_dvb_mux_instance_t *tdmi;
  th_dvb_mux_t *tdm;
  int v;

  dtimer_arm(&tda->tda_fec_monitor_timer, dvb_fec_monitor, tda, 1);

  tdmi = tda->tda_mux_current;

  if(tdmi != NULL && tdmi->tdmi_status == NULL) {
    if(ioctl(tda->tda_fe_fd, FE_READ_UNCORRECTED_BLOCKS, &v) < 0)
      v = 0;
    tdmi->tdmi_fec_err_per_sec = (tdmi->tdmi_fec_err_per_sec * 7 + v) / 8;

    if(tdmi->tdmi_fec_err_per_sec > DVB_FEC_ERROR_LIMIT) {

      if(LIST_FIRST(&tda->tda_transports) != NULL) {
	syslog(LOG_ERR, "%s: on %s: Too many FEC errors (%d / s), "
	       "flushing subscribers\n", 
	       tdmi->tdmi_mux->tdm_name, tda->tda_path,
	       tdmi->tdmi_fec_err_per_sec);
	dvb_adapter_clean(tdmi->tdmi_adapter);
      }
    }

    tdm = tdmi->tdmi_mux;
    LIST_REMOVE(tdmi, tdmi_mux_link);
    LIST_INSERT_SORTED(&tdm->tdm_instances, tdmi, tdmi_mux_link,
		       mux_sort_quality);
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
  tdmi = tdmi != NULL ? tdmi : LIST_FIRST(&tda->tda_muxes_active);

  if(tdmi == NULL)
    return; /* no instances */

  dvb_tune_tdmi(tdmi, 0, TDMI_IDLESCAN);

}
