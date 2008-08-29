/*
 *  tvheadend, transport and subscription functions
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
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "tvhead.h"
#include "dispatch.h"
#include "transports.h"
#include "subscriptions.h"

struct th_subscription_list subscriptions;


static int
subscription_sort(th_subscription_t *a, th_subscription_t *b)
{
  return b->ths_weight - a->ths_weight;
}

/**
 *
 */
void
subscription_reschedule(void)
{
  th_subscription_t *s;
  th_transport_t *t;

  lock_assert(&global_lock);

  LIST_FOREACH(s, &subscriptions, ths_global_link) {
    if(s->ths_transport != NULL)
      continue; /* Got a transport, we're happy */

    if(s->ths_channel == NULL)
      continue; /* stale entry, channel has been destroyed */

    t = transport_find(s->ths_channel, s->ths_weight);

    if(t == NULL)
      continue;

    s->ths_transport = t;
    LIST_INSERT_HEAD(&t->tht_subscriptions, s, ths_transport_link);
    s->ths_callback(s, TRANSPORT_AVAILABLE, s->ths_opaque);
  }
}

/**
 *
 */
void 
subscription_unsubscribe(th_subscription_t *s)
{
  th_transport_t *t = s->ths_transport;

  lock_assert(&global_lock);

  LIST_REMOVE(s, ths_global_link);

  if(s->ths_channel != NULL)
    LIST_REMOVE(s, ths_channel_link);

  if(t != NULL)
    transport_remove_subscriber(t, s);

  free(s->ths_title);
  free(s);

  subscription_reschedule();
}


/**
 *
 */
th_subscription_t *
subscription_create(channel_t *ch, unsigned int weight, const char *name,
		    subscription_callback_t *cb, void *opaque, uint32_t u32)
{
  th_subscription_t *s;

  s = calloc(1, sizeof(th_subscription_t));
  s->ths_callback  = cb;
  s->ths_opaque    = opaque;
  s->ths_title     = strdup(name);
  s->ths_total_err = 0;
  s->ths_weight    = weight;
  s->ths_u32       = u32;

  time(&s->ths_start);
  LIST_INSERT_SORTED(&subscriptions, s, ths_global_link, subscription_sort);

  s->ths_channel = ch;
  LIST_INSERT_HEAD(&ch->ch_subscriptions, s, ths_channel_link);

  s->ths_transport = NULL;

  subscription_reschedule();

  if(s->ths_transport == NULL)
    tvhlog(LOG_NOTICE, "subscription", 
	   "No transponder available for subscription \"%s\" "
	   "to channel \"%s\"",
	   s->ths_title, s->ths_channel->ch_name);

  return s;
}


/**
 *
 */
void
subscriptions_init(void)
{

}

