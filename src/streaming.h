/*
 *  Stream plumbing, connects individual streaming components to each other
 *  Copyright (C) 2008 Andreas Öman
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

#ifndef STREAMING_H_
#define STREAMING_H_

#include "tvhead.h"
#include "packet.h"
#include "htsmsg.h"

/**
 *
 */
void streaming_pad_init(streaming_pad_t *sp);

void streaming_target_init(streaming_target_t *st,
			   st_callback_t *cb, void *opaque);

void streaming_queue_init(streaming_queue_t *sq);

void streaming_queue_clear(struct streaming_message_queue *q);

void streaming_target_connect(streaming_pad_t *sp, streaming_target_t *st);

void streaming_target_disconnect(streaming_pad_t *sp, streaming_target_t *st);

void streaming_pad_deliver(streaming_pad_t *sp, streaming_message_t *sm);

void streaming_msg_free(streaming_message_t *sm);

streaming_message_t *streaming_msg_clone(streaming_message_t *src);

streaming_message_t *streaming_msg_create(streaming_message_type_t type);

streaming_message_t *streaming_msg_create_msg(streaming_message_type_t type, 
					      htsmsg_t *msg);

streaming_message_t *streaming_msg_create_code(streaming_message_type_t type, 
					       int code);

streaming_message_t *streaming_msg_create_pkt(th_pkt_t *pkt);

#define streaming_target_deliver(st, sm) ((st)->st_cb((st)->st_opaque, (sm)))
     
#endif /* STREAMING_H_ */
