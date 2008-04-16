/*
 *  DVB Table support
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
#include "dvb_support.h"
#include "epg.h"
#include "transports.h"
#include "channels.h"
#include "psi.h"
#include "notify.h"

#define TDT_NOW 0x1

/**
 *
 */
void
dvb_tdt_destroy(th_dvb_table_t *tdt)
{
  free(tdt->tdt_fparams);
  LIST_REMOVE(tdt, tdt_link);
  close(dispatch_delfd(tdt->tdt_handle));
  free(tdt->tdt_name);
  free(tdt);
}


/**
 *
 */
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

  /* It seems some hardware (or is it the dvb API?) does not honour the
     DMX_CHECK_CRC flag, so we check it again */

  if(psi_crc32(sec, r))
    return;

  r -= 3;
  tableid = sec[0];
  len = ((sec[1] & 0x0f) << 8) | sec[2];
  
  if(len < r)
    return;

  ptr = &sec[3];
  len -= 4;   /* Strip trailing CRC */

  tdt->tdt_callback(tdt->tdt_tdmi, ptr, len, tableid, tdt->tdt_opaque);
}





/**
 * Add a new DVB table
 */
static void
tdt_add(th_dvb_mux_instance_t *tdmi, struct dmx_sct_filter_params *fparams,
	void (*callback)(th_dvb_mux_instance_t *tdmi, uint8_t *buf, int len,
			 uint8_t tableid, void *opaque), void *opaque,
	char *name, int flags)
{
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  th_dvb_table_t *tdt;
  int fd;

  if((fd = open(tda->tda_demux_path, O_RDWR)) == -1) 
    return;

  tdt = calloc(1, sizeof(th_dvb_table_t));
  tdt->tdt_fd = fd;
  tdt->tdt_name = strdup(name);
  tdt->tdt_callback = callback;
  tdt->tdt_opaque = opaque;
  tdt->tdt_tdmi = tdmi;
  tdt->tdt_handle = dispatch_addfd(fd, dvb_table_recv, tdt, DISPATCH_READ);
 
  if(flags & TDT_NOW) {
    ioctl(fd, DMX_SET_FILTER, fparams);
    free(fparams);
  } else {
    tdt->tdt_fparams = fparams;
  }

  pthread_mutex_lock(&tdmi->tdmi_table_lock);
  LIST_INSERT_HEAD(&tdmi->tdmi_tables, tdt, tdt_link);
  pthread_mutex_unlock(&tdmi->tdmi_table_lock);
}


/**
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


/**
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


/**
 * DVB EIT (Event Information Table)
 */
static void
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
  epg_content_type_t *ect;

  if(tableid < 0x4e || tableid > 0x6f || len < 11)
    return;

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

  t = dvb_find_transport(tdmi, serviceid, 0);
  if(t == NULL)
    return;

  ch = t->tht_channel;
  if(ch == NULL)
    return;

  epg_lock();

  while(len >= 12) {
    event_id                  = ptr[0] << 8 | ptr[1];
    start_time                = dvb_convert_date(&ptr[2]);
    duration                  = bcdtoint(ptr[7] & 0xff) * 3600 +
                                bcdtoint(ptr[8] & 0xff) * 60 +
                                bcdtoint(ptr[9] & 0xff);
    dllen                     = ((ptr[10] & 0x0f) << 8) | ptr[11];

    len -= 12;
    ptr += 12;

    if(dllen > len)
      break;

    ect = NULL;
    *title = 0;
    *desc = 0;
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

      case DVB_DESC_CONTENT:
	if(dlen >= 2)
	  /* We only support one content type per event atm. */
	  ect = epg_content_type_find_by_dvbcode(*ptr);
	break;
      }

      len -= dlen; ptr += dlen; dllen -= dlen;
    }

    if(duration > 0) {
      epg_update_event_by_id(ch, event_id, start_time, duration,
			     title, desc, ect);
      
    }
  }
  
  epg_unlock();
}


/**
 * DVB SDT (Service Description Table)
 */
static void
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
  char chname0[256], *chname;
  uint8_t stype;
  int l;
  int change = 0;

  if(len < 8)
    return;

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

    chname = NULL;

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
			    chname0, sizeof(chname0)) == 0) {
	  chname = chname0;
	  /* Some providers insert spaces.
	     Clean up that (both heading and trailing) */
	  while(*chname <= 32 && *chname != 0)
	    chname++;

	  l = strlen(chname);
	  while(l > 1 && chname[l - 1] <= 32) {
	    chname[l - 1] = 0;
	    l--;
	  }

	  if(l == 0) {
	    chname = chname0;
	    snprintf(chname0, sizeof(chname0), "noname-sid-0x%x", service_id);
	  }

	  t = dvb_find_transport(tdmi, service_id, 0);
	  if(t == NULL)
	    break;

	  change |=
	    t->tht_servicetype != stype ||
	    t->tht_scrambled != free_ca_mode ||
	    strcmp(t->tht_provider    ?: "", provider) ||
	    strcmp(t->tht_servicename ?: "", chname  );

	  t->tht_servicetype = stype;
	  t->tht_scrambled = free_ca_mode;

	  free((void *)t->tht_provider);
	  t->tht_provider = strdup(provider);

	  free((void *)t->tht_servicename);
	  t->tht_servicename = strdup(chname);

	  if(t->tht_channelname == NULL)
	    t->tht_channelname = strdup(chname);
	}
	break;
      }

      len -= dlen; ptr += dlen; dllen -= dlen;
    }
  }
  if(change)
    dvb_tdmi_save(tdmi);
}


/**
 * PAT - Program Allocation table
 */
static void
dvb_pat_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  uint16_t service, pmt, tid;
  th_transport_t *t;

  if(len < 5)
    return;

  tid = (ptr[0] << 8) | ptr[1];

  ptr += 5;
  len -= 5;

  while(len >= 4) {
    service =  ptr[0]         << 8 | ptr[1];
    pmt     = (ptr[2] & 0x1f) << 8 | ptr[3];

    if(service != 0) {
      t = dvb_find_transport(tdmi, service, pmt);
      dvb_table_add_transport(tdmi, t, pmt); /* Add PMT to our table scanner */
    }
    ptr += 4;
    len -= 4;
  }
}


/**
 * CAT - Condition Access Table
 */
static void
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
}


/**
 * Tables for delivery descriptor parsing
 */
static const fe_code_rate_t fec_tab [8] = {
  FEC_AUTO, FEC_1_2, FEC_2_3, FEC_3_4,
  FEC_5_6, FEC_7_8, FEC_NONE, FEC_NONE
};


static const fe_modulation_t qam_tab [6] = {
	 QAM_AUTO, QAM_16, QAM_32, QAM_64, QAM_128, QAM_256
};

/**
 * Cable delivery descriptor
 */
static void
dvb_table_cable_delivery(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len)
{
  int freq, symrate;
  struct dvb_frontend_parameters fe_param;

  if(len < 11) {
    printf("Invalid CABLE DESCRIPTOR\n");
    return;
  }
  memset(&fe_param, 0, sizeof(fe_param));
  fe_param.inversion = INVERSION_AUTO;

  freq =
    bcdtoint(ptr[0]) * 1000000 + bcdtoint(ptr[1]) * 10000 + 
    bcdtoint(ptr[2]) * 100     + bcdtoint(ptr[3]);

  fe_param.frequency = freq * 100;

  symrate =
    bcdtoint(ptr[7]) * 100000 + bcdtoint(ptr[8]) * 1000 + 
    bcdtoint(ptr[9]) * 10     + (ptr[10] >> 4);

  fe_param.u.qam.symbol_rate = symrate * 100;


  if((ptr[6] & 0x0f) > 5)
    fe_param.u.qam.modulation = QAM_AUTO;
  else
    fe_param.u.qam.modulation = qam_tab[ptr[6] & 0x0f];

  fe_param.u.qam.fec_inner = fec_tab[ptr[10] & 0x07];

  dvb_mux_create(tdmi->tdmi_adapter, &fe_param, 0, 0, 1);
}

/**
 * Satellite delivery descriptor
 */
static void
dvb_table_sat_delivery(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len)
{
  int freq, symrate, pol;
  struct dvb_frontend_parameters fe_param;

  if(len < 11)
    return;

  memset(&fe_param, 0, sizeof(fe_param));
  fe_param.inversion = INVERSION_AUTO;

  freq =
    bcdtoint(ptr[0]) * 1000000 + bcdtoint(ptr[1]) * 10000 + 
    bcdtoint(ptr[2]) * 100     + bcdtoint(ptr[3]);
  fe_param.frequency = freq * 10;

  symrate =
    bcdtoint(ptr[7]) * 100000 + bcdtoint(ptr[8]) * 1000 + 
    bcdtoint(ptr[9]) * 10     + (ptr[10] >> 4);
  fe_param.u.qam.symbol_rate = symrate * 100;

  fe_param.u.qam.fec_inner = fec_tab[ptr[10] & 0x07];

  pol = (ptr[6] >> 5) & 0x03;

  dvb_mux_create(tdmi->tdmi_adapter, &fe_param, pol, tdmi->tdmi_switchport, 1);
}



/**
 * NIT - Network Information Table
 */
static void
dvb_nit_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  uint8_t tag, tlen;
  int ntl;
  char networkname[256];
  uint16_t tsid;

  ptr += 5;
  len -= 5;

  if(tableid != 0x40)
    return;

  ntl = ((ptr[0] & 0xf) << 8) | ptr[1];
  ptr += 2;
  len -= 2;
  if(ntl > len)
    return;

  while(ntl > 2) {
    tag = *ptr++;
    tlen = *ptr++;
    len -= 2;
    ntl -= 2;

    switch(tag) {
    case DVB_DESC_NETWORK_NAME:
      if(dvb_get_string(networkname, sizeof(networkname), ptr, tlen, "UTF8"))
	return;

      if(strcmp(tdmi->tdmi_network ?: "", networkname)) {
	free((void *)tdmi->tdmi_network);
	tdmi->tdmi_network = strdup(networkname);
	notify_tdmi_name_change(tdmi);
      }
      break;
    }

    ptr += tlen;
    len -= tlen;
    ntl -= tlen;
  }

  if(len < 2)
    return;

  ntl =  ((ptr[0] & 0xf) << 8) | ptr[1];
  ptr += 2;
  len -= 2;

  if(len < ntl)
    return;

  while(len >= 6) {
    tsid = ( ptr[0]        << 8) | ptr[1];
    ntl =  ((ptr[4] & 0xf) << 8) | ptr[5];

    ptr += 6;
    len -= 6;
    if(ntl > len)
      break;

    while(ntl > 2) {
      tag = *ptr++;
      tlen = *ptr++;
      len -= 2;
      ntl -= 2;

      switch(tag) {
      case DVB_DESC_SAT:
	dvb_table_sat_delivery(tdmi, ptr, tlen);
	break;
      case DVB_DESC_CABLE:
	dvb_table_cable_delivery(tdmi, ptr, tlen);
	break;
      }

      ptr += tlen;
      len -= tlen;
      ntl -= tlen;
    }
  }
}



/**
 * PMT - Program Mapping Table
 */
static void
dvb_pmt_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  th_transport_t *t = opaque;
  int v = t->tht_pmt_seen;

  psi_parse_pmt(t, ptr, len, 1);
  v ^= t->tht_pmt_seen;
  if(v)
    dvb_tdmi_save(tdmi);
  return;
}


/**
 * RST - Running Status Table
 */
static void
dvb_rst_callback(th_dvb_mux_instance_t *tdmi, uint8_t *ptr, int len,
		 uint8_t tableid, void *opaque)
{
  int i;

  //  printf("Got RST on %s\t", tdmi->tdmi_uniquename);

  for(i = 0; i < len; i++)
    printf("%02x.", ptr[i]);
  printf("\n");
}


/**
 * Helper for preparing a section filter parameter struct
 */
struct dmx_sct_filter_params *
dvb_fparams_alloc(int pid, int flags)
{
  struct dmx_sct_filter_params *p;

  p = calloc(1, sizeof(struct dmx_sct_filter_params));
  p->pid = pid;
  p->timeout = 0;
  p->flags = flags;
  return p;
}



/**
 * Setup FD + demux for default DVB tables that we want
 */
void
dvb_table_add_default(th_dvb_mux_instance_t *tdmi)
{
  struct dmx_sct_filter_params *fp;

  /* Program Allocation Table */

  fp = dvb_fparams_alloc(0x0, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  fp->filter.filter[0] = 0x00;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_pat_callback, NULL, "pat", 0);

  /* Conditional Access Table */

  fp = dvb_fparams_alloc(0x1, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  fp->filter.filter[0] = 0x1;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_cat_callback, NULL, "cat", 0);

  /* Network Information Table */

  fp = dvb_fparams_alloc(0x10, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  tdt_add(tdmi, fp, dvb_nit_callback, NULL, "nit", 0);

  /* Service Descriptor Table */

  fp = dvb_fparams_alloc(0x11, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  fp->filter.filter[0] = 0x42;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_sdt_callback, NULL, "sdt", 0);

  /* Event Information table */

  fp = dvb_fparams_alloc(0x12, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  tdt_add(tdmi, fp, dvb_eit_callback, NULL, "eit", 0);

  /* Running Status Table */

  fp = dvb_fparams_alloc(0x13, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  fp->filter.filter[0] = 0x71;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_rst_callback, NULL, "rst", 0);

}


/**
 * Setup FD + demux for a services PMT
 */
void
dvb_table_add_transport(th_dvb_mux_instance_t *tdmi, th_transport_t *t,
			int pmt_pid)
{
  struct dmx_sct_filter_params *fp;
  char pmtname[100];

  snprintf(pmtname, sizeof(pmtname), "PMT(%d), service:%d", 
	   pmt_pid, t->tht_dvb_service_id);

  fp = dvb_fparams_alloc(pmt_pid, DMX_IMMEDIATE_START | DMX_CHECK_CRC);
  fp->filter.filter[0] = 0x02;
  fp->filter.mask[0] = 0xff;
  tdt_add(tdmi, fp, dvb_pmt_callback, t, pmtname, TDT_NOW);
}
