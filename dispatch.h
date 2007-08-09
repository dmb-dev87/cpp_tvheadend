/*
 *  socket fd dispathcer
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

#ifndef DISPATCH_H
#define DISPATCH_H

#define DISPATCH_READ  0x1
#define DISPATCH_WRITE 0x2
#define DISPATCH_ERR   0x4

int dispatch_init(void);
void *dispatch_addfd(int fd, 
		     void (*callback)(int events, void *opaque, int fd),
		     void *opaque, int flags);
void dispatch_delfd(void *handle);
void dispatch_set(void *handle, int flags);
void dispatch_clr(void *handle, int flags);
void dispatcher(void);

void *dispatch_add_1sec_event(void (*callback)(void *aux), void *aux);
void dispatch_del_1sec_event(void *p);

#endif /* DISPATCH_H */
