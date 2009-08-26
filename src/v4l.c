/*
 *  TV Input - Linux analogue (v4lv2) interface
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

#include <assert.h>
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
#include <poll.h>
#include <inttypes.h>

#define __user
#include <linux/videodev2.h>

#include "tvhead.h"
#include "transports.h"
#include "v4l.h"
#include "parsers.h"

LIST_HEAD(v4l_adapter_list, v4l_adapter);

struct v4l_adapter_list v4l_adapters;

typedef struct v4l_adapter {

  LIST_ENTRY(v4l_adapter) va_global_link;

  char *va_path;

  char *va_identifier;

  struct v4l2_capability va_caps;

  struct th_transport *va_current_transport;


  int va_fd;

  pthread_t va_thread;

  int va_pipe[2];

  /** Mpeg stream parsing */
  uint32_t va_startcode;
  int va_lenlock;

} v4l_adapter_t;


/**
 *
 */
static void
v4l_input(v4l_adapter_t *va)
{
  th_transport_t *t = va->va_current_transport;
  th_stream_t *st;
  uint8_t buf[4000];
  uint8_t *ptr, *pkt;
  int len, l, r;

  len = read(va->va_fd, buf, 4000);
  if(len < 1)
    return;

  ptr = buf;

  pthread_mutex_lock(&t->tht_stream_mutex);

  while(len > 0) {

    switch(va->va_startcode) {
    default:
      va->va_startcode = va->va_startcode << 8 | *ptr;
      va->va_lenlock = 0;
      ptr++; len--;
      continue;

    case 0x000001e0:
      st = t->tht_video;
      break;
    case 0x000001c0:
      st = t->tht_audio;
      break;
    }

    if(va->va_lenlock == 2) {
      l = st->st_buffer2_size;
      st->st_buffer2 = pkt = realloc(st->st_buffer2, l);
      
      r = l - st->st_buffer2_ptr;
      if(r > len)
	r = len;
      memcpy(pkt + st->st_buffer2_ptr, ptr, r);
      
      ptr += r;
      len -= r;

      st->st_buffer2_ptr += r;
      if(st->st_buffer2_ptr == l) {
	parse_mpeg_ps(t, st, pkt + 6, l - 6);

	st->st_buffer2_size = 0;
	va->va_startcode = 0;
      } else {
	assert(st->st_buffer2_ptr < l);
      }
      
    } else {
      st->st_buffer2_size = st->st_buffer2_size << 8 | *ptr;
      va->va_lenlock++;
      if(va->va_lenlock == 2) {
	st->st_buffer2_size += 6;
	st->st_buffer2_ptr = 6;
      }
      ptr++; len--;
    }
  }
  pthread_mutex_unlock(&t->tht_stream_mutex);
}


/**
 *
 */
static void *
v4l_thread(void *aux)
{
  v4l_adapter_t *va = aux;
  struct pollfd pfd[2];
  int r;

  pfd[0].fd = va->va_pipe[0];
  pfd[0].events = POLLIN;
  pfd[1].fd = va->va_fd;
  pfd[1].events = POLLIN;

  while(1) {

    r = poll(pfd, 2, -1);
    if(r < 0) {
      tvhlog(LOG_ALERT, "v4l", "%s: poll() error %s, sleeping one second",
	     va->va_path, strerror(errno));
      sleep(1);
      continue;
    }

    if(pfd[0].revents & POLLIN) {
      // Message on control pipe, used to exit thread, do so
      break;
    }

    if(pfd[1].revents & POLLIN) {
      v4l_input(va);
    }
  }

  close(va->va_pipe[0]);
  return NULL;
}



/**
 *
 */
static int
v4l_transport_start(th_transport_t *t, unsigned int weight, int status, 
		    int force_start)
{
  v4l_adapter_t *va = t->tht_v4l_adapter;
  int fd;

  fd = open(va->va_path, O_RDWR | O_NONBLOCK);
  if(fd == -1) {
    tvhlog(LOG_ERR, "v4l",
	   "%s: Unable to open device: %s\n", va->va_path, 
	   strerror(errno));
    return -1;
  }

  int frequency = 182250000;
  struct v4l2_frequency vf;
  //  struct v4l2_tuner vt;
  int result;


  v4l2_std_id std = 0xff;

  result = ioctl(fd, VIDIOC_S_STD, &std);
  if(result < 0) {
    tvhlog(LOG_ERR, "v4l",
	   "%s: Unable to set PAL -- %s", va->va_path, strerror(errno));
    close(fd);
    return -1;
  }

  memset(&vf, 0, sizeof(vf));

  vf.tuner = 0;
  vf.type = V4L2_TUNER_ANALOG_TV;
  vf.frequency = (frequency * 16) / 1000000;
  result = ioctl(fd, VIDIOC_S_FREQUENCY, &vf);
  if(result < 0) {
    tvhlog(LOG_ERR, "v4l",
	   "%s: Unable to tune to %dHz", va->va_path, frequency);
    close(fd);
    return -1;
  }

  tvhlog(LOG_DEBUG, "v4l",
	 "%s: Tuned to %dHz", va->va_path, frequency);

  if(pipe(va->va_pipe)) {
    tvhlog(LOG_ERR, "v4l",
	   "%s: Unable to create control pipe", va->va_path, strerror(errno));
    close(fd);
    return -1;
  }


  va->va_fd = fd;
  va->va_current_transport = t;
  t->tht_status = status;
  pthread_create(&va->va_thread, NULL, v4l_thread, va);
  return 0;
}


/**
 *
 */
static void
v4l_transport_refresh(th_transport_t *t)
{

}


/**
 *
 */
static void
v4l_transport_stop(th_transport_t *t)
{
  char c = 'q';
  v4l_adapter_t *va = t->tht_v4l_adapter;

  assert(va->va_current_transport != NULL);

  if(write(va->va_pipe[1], &c, 1) != 1)
    tvhlog(LOG_ERR, "v4l", "Unable to close video thread -- %s",
	   strerror(errno));
  
  pthread_join(va->va_thread, NULL);

  close(va->va_pipe[1]);
  close(va->va_fd);

  va->va_current_transport = NULL;
  t->tht_status = TRANSPORT_IDLE;
}


/**
 *
 */
static void
v4l_transport_save(th_transport_t *t)
{

}


/**
 *
 */
static int
v4l_transport_quality(th_transport_t *t)
{
  return 100;
}


/**
 * Generate a descriptive name for the source
 */
static htsmsg_t *
v4l_transport_sourceinfo(th_transport_t *t)
{
  htsmsg_t *m = htsmsg_create_map();
#if 0
  if(t->tht_v4l_iface != NULL)
    htsmsg_add_str(m, "adapter", t->tht_v4l_iface);
  htsmsg_add_str(m, "mux", inet_ntoa(t->tht_v4l_group));
#endif
  return m;
}


/**
 *
 */
static th_transport_t *
v4l_add_transport(v4l_adapter_t *va)
{
  th_transport_t *t;

  char id[256];

  snprintf(id, sizeof(id), "%s_%s", va->va_identifier, "foo"); 
  printf("Adding transport %s\n", id);

  t = transport_create(id, TRANSPORT_V4L, 0);
  t->tht_flags |= THT_DEBUG;

  t->tht_start_feed    = v4l_transport_start;
  t->tht_refresh_feed  = v4l_transport_refresh;
  t->tht_stop_feed     = v4l_transport_stop;
  t->tht_config_save   = v4l_transport_save;
  t->tht_sourceinfo    = v4l_transport_sourceinfo;
  t->tht_quality_index = v4l_transport_quality;

  t->tht_v4l_adapter = va;

  pthread_mutex_lock(&t->tht_stream_mutex);

  t->tht_video = transport_stream_create(t, -1, SCT_MPEG2VIDEO);
  t->tht_audio = transport_stream_create(t, -1, SCT_MPEG2AUDIO);

  pthread_mutex_unlock(&t->tht_stream_mutex);

  transport_map_channel(t, channel_find_by_name("alpha", 1), 0);

  //XXX  LIST_INSERT_HEAD(&v4l_all_transports, t, tht_group_link);
  
  return t;
}





/**
 *
 */
static void
v4l_adapter_check(const char *path, int fd)
{
  int r, i;

  v4l_adapter_t *va;
  struct v4l2_capability caps;

  r = ioctl(fd, VIDIOC_QUERYCAP, &caps);

  if(r) {
    tvhlog(LOG_DEBUG, "v4l", 
	   "Can not query capabilities on %s, device skipped", path);
    return;
  }

  if(!(caps.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
    tvhlog(LOG_DEBUG, "v4l", 
	   "Device %s not a video capture device, device skipped", path);
    return;
  }

  if(!(caps.capabilities & V4L2_CAP_TUNER)) {
    tvhlog(LOG_DEBUG, "v4l", 
	   "Device %s does not have a built-in tuner, device skipped", path);
    return;
  }

  /* Enum video standards */

  for(i = 0;; i++) {
    struct v4l2_standard standard;
    memset(&standard, 0, sizeof(standard));
    standard.index = i;

    if(ioctl(fd, VIDIOC_ENUMSTD, &standard))
      break;

    printf("%3d: %016llx %24s %d/%d %d lines\n",
	   standard.index, 
	   standard.id,
	   standard.name,
	   standard.frameperiod.numerator,
	   standard.frameperiod.denominator,
	   standard.framelines);
  }


  /* Enum formats */
  for(i = 0;; i++) {

    struct v4l2_fmtdesc fmtdesc;
    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.index = i;
    fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if(ioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc)) {
      tvhlog(LOG_DEBUG, "v4l", 
	     "Device %s has no suitable formats, device skipped", path);
      return; 
    }

    if(fmtdesc.pixelformat == V4L2_PIX_FMT_MPEG)
      break;
  }



  va = calloc(1, sizeof(v4l_adapter_t));

  va->va_identifier = strdup(path);

  r = strlen(va->va_identifier);
  for(i = 0; i < r; i++)
    if(!isalnum((int)va->va_identifier[i]))
      va->va_identifier[i] = '_';

  va->va_path = strdup(path);
  va->va_caps = caps;

  LIST_INSERT_HEAD(&v4l_adapters, va, va_global_link);

  tvhlog(LOG_INFO, "v4l", "Adding adapter %s: %s (%s) @ %s", 
	 path, caps.card, caps.driver, caps.bus_info, caps.bus_info);

  v4l_add_transport(va);
}


/**
 *
 */
static void 
v4l_adapter_probe(const char *path)
{
  int fd;

  fd = open(path, O_RDWR | O_NONBLOCK);

  if(fd == -1) {
    if(errno != ENOENT)
      tvhlog(LOG_ALERT, "v4l",
	     "Unable to open %s -- %s", path, strerror(errno));
    return;
  }

  v4l_adapter_check(path, fd);

  close(fd);
}






void
v4l_init(void)
{
  char buf[256];
  int i;

  for(i = 0; i < 1; i++) {
    snprintf(buf, sizeof(buf), "/dev/video%d", i);
    v4l_adapter_probe(buf);
  }
}
