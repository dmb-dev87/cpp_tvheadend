/*
 *  TV Input - Linux DVB interface - Support
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

/* 
 * Based on:
 *
 * ITU-T Recommendation H.222.0 / ISO standard 13818-1
 * EN 300 468 - V1.7.1
 */

#ifndef DVB_SUPPORT_H
#define DVB_SUPPORT_H

#define DVB_DESC_CA           0x09
#define DVB_DESC_LANGUAGE     0x0a

/* Descriptors defined in EN 300 468 */

#define DVB_DESC_NETWORK_NAME 0x40
#define DVB_DESC_SERVICE_LIST 0x41
#define DVB_DESC_SHORT_EVENT  0x4d
#define DVB_DESC_SERVICE      0x48
#define DVB_DESC_TELETEXT     0x56
#define DVB_DESC_SUBTITLE     0x59
#define DVB_DESC_AC3          0x6a


/* Service types defined in EN 300 468 */

#define DVB_ST_SDTV           0x1    /* SDTV (MPEG2) */
#define DVB_ST_RADIO          0x2
#define DVB_ST_HDTV           0x11   /* HDTV (MPEG2) */
#define DVB_ST_AC_SDTV        0x16   /* Advanced codec SDTV */
#define DVB_ST_AC_HDTV        0x19   /* Advanced codec HDTV */

int dvb_get_string(char *dst, size_t dstlen, const uint8_t *src, 
		   const size_t srclen, const char *target_encoding);

int dvb_get_string_with_len(char *dst, size_t dstlen, 
			    const uint8_t *buf, size_t buflen, 
			    const char *target_encoding);

#define bcdtoint(i) ((((i & 0xf0) >> 4) * 10) + (i & 0x0f))

time_t dvb_convert_date(uint8_t *dvb_buf);

#endif /* DVB_SUPPORT_H */
