/*
 *  API - Common functions for control/query API
 *
 *  Copyright (C) 2013 Adam Sutton
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
#include "api.h"
#include "access.h"

#include <string.h>

typedef struct api_link {
  const api_hook_t   *hook;
  RB_ENTRY(api_link)  link;
} api_link_t;

RB_HEAD(,api_link) api_hook_tree;

static int ah_cmp
  ( api_link_t *a, api_link_t *b )
{
  return strcmp(a->hook->ah_subsystem, b->hook->ah_subsystem);
}

void
api_register ( const api_hook_t *hook )
{
  static api_link_t *t, *skel = NULL;
  if (!skel)
    skel = calloc(1, sizeof(api_link_t));
  skel->hook = hook;
  t = RB_INSERT_SORTED(&api_hook_tree, skel, link, ah_cmp);
  if (t)
    tvherror("api", "trying to re-register subsystem");
  else
    skel = NULL;
}

void
api_register_all ( const api_hook_t *hooks )
{
  while (hooks->ah_subsystem) {
    api_register(hooks);
    hooks++;
  }
}

int
api_exec ( const char *subsystem, htsmsg_t *args, htsmsg_t **resp )
{
  api_hook_t h;
  api_link_t *ah, skel;
  const char *op;

  /* Args and response must be set */
  if (!args || !resp)
    return EINVAL;

  // Note: there is no locking while checking the hook tree, its assumed
  //       this is all setup during init (if this changes the code will
  //       need updating)
  h.ah_subsystem = subsystem;
  skel.hook      = &h;
  ah = RB_FIND(&api_hook_tree, &skel, link, ah_cmp);

  if (!ah) {
    tvhwarn("api", "failed to find subsystem [%s]", subsystem);
    return ENOSYS; // TODO: is this really the right error code?
  }

  /* Extract method */
  op = htsmsg_get_str(args, "method");
  if (!op)
    op = htsmsg_get_str(args, "op");
  // Note: this is not required (so no final validation)

  /* Execute */
  return ah->hook->ah_callback(ah->hook->ah_opaque, op, args, resp);
}

static int
api_serverinfo
  ( void *opaque, const char *op, htsmsg_t *args, htsmsg_t **resp )
{
  *resp = htsmsg_create_map();
  htsmsg_add_str(*resp, "sw_version",   tvheadend_version);
  htsmsg_add_u32(*resp, "api_version",  TVH_API_VERSION);
  htsmsg_add_str(*resp, "name",         "Tvheadend");
  if (tvheadend_webroot)
    htsmsg_add_str(*resp, "webroot",      tvheadend_webroot);
  htsmsg_add_msg(*resp, "capabilities", tvheadend_capabilities_list(1));
  return 0;
}

void api_init ( void )
{
  static api_hook_t h[] = {
    { "serverinfo", ACCESS_ANONYMOUS, api_serverinfo, NULL },
    { NULL, 0, NULL, NULL }
  };
  api_register_all(h);

  /* Subsystems */
  api_idnode_init();
  api_input_init();
  api_mpegts_init();
  api_service_init();
  api_channel_init();
  api_epg_init();
  api_epggrab_init();
}
