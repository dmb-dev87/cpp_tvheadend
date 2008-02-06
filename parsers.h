/*
 *  Elementary stream functions
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

#ifndef PARSERS_H
#define PARSERS_H

void parse_raw_mpeg(th_transport_t *t, th_stream_t *st, uint8_t *data, 
		    int len, int start, int err);

void parser_compute_duration(th_transport_t *t, th_stream_t *st,
			     th_pkt_t *pkt);

void parse_compute_pts(th_transport_t *t, th_stream_t *st, th_pkt_t *pkt);

void parser_enqueue_packet(th_transport_t *t, th_stream_t *st, th_pkt_t *pkt);

#endif /* PARSERS_H */
