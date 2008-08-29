/*
 *  tvheadend, WEBUI / HTML user interface
 *  Copyright (C) 2008 Andreas �man
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

#define _GNU_SOURCE /* for splice() */
#include <fcntl.h>

#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/sendfile.h>

#include "tvhead.h"
#include "access.h"
#include "http.h"
#include "webui.h"

static int
is_client_simple(http_connection_t *hc)
{
  char *c;

  if((c = http_arg_get(&hc->hc_args, "UA-OS")) != NULL) {
    if(strstr(c, "Windows CE") || strstr(c, "Pocket PC"))
      return 1;
  }

  if((c = http_arg_get(&hc->hc_args, "x-wap-profile")) != NULL) {
    return 1;
  }
  return 0;
}



/**
 * Root page, we direct the client to different pages depending
 * on if it is a full blown browser or just some mobile app
 */
static int
page_root(http_connection_t *hc, const char *remain, void *opaque)
{
  if(is_client_simple(hc)) {
    http_redirect(hc, "/simple.html");
  } else {
    http_redirect(hc, "/extjs.html");
  }
  return 0;
}

/**
 * Root page, we direct the client to different pages depending
 * on if it is a full blown browser or just some mobile app
 */
static int
page_static(http_connection_t *hc, const char *remain, void *opaque)
{
  int fd;
  const char *rootpath = HTS_BUILD_ROOT "/tvheadend/webui/static";
  char path[500];
  struct stat st;
  const char *content = NULL, *postfix;

  if(strstr(remain, ".."))
    return HTTP_STATUS_BAD_REQUEST;

  postfix = strrchr(remain, '.');
  if(postfix != NULL) {
    postfix++;
    if(!strcmp(postfix, "js"))
      content = "text/javascript; charset=UTF-8";
  }

  snprintf(path, sizeof(path), "%s/%s", rootpath, remain);

  if((fd = open(path, O_RDONLY)) < 0)
    return 404;

  if(fstat(fd, &st) < 0) {
    close(fd);
    return 404;
  }

  http_send_header(hc, 200, content, st.st_size, NULL, NULL, 10);
  sendfile(hc->hc_fd, fd, NULL, st.st_size);
  close(fd);
  return 0;
}


/**
 * WEB user interface
 */
void
webui_init(void)
{
  http_path_add("/", NULL, page_root, ACCESS_WEB_INTERFACE);

  http_path_add("/static", NULL, page_static, ACCESS_WEB_INTERFACE);

  //  simpleui_start();
  extjs_start();
  comet_init();

}
