/*
 * AVC helper functions for muxers
 * Copyright (c) 2006 Baptiste Coudurier <baptiste.coudurier@smartjog.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVC_H__
#define AVC_H__

#include "tvheadend.h"
#include "packet.h"

int avc_parse_nal_units(sbuf_t *sb, const uint8_t *buf_in, int size);

int isom_write_avcc(sbuf_t *sb, const uint8_t *src, int len);

th_pkt_t *avc_convert_pkt(th_pkt_t *src);

#endif 
