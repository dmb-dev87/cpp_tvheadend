/*
 *  Tvheadend - structures
 *  Copyright (C) 2007 Andreas Öman
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

#ifndef TVHEADEND_CLOCK_H
#define TVHEADEND_CLOCK_H

#include <time.h>

#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

#ifdef PLATFORM_DARWIN
#define CLOCK_MONOTONIC 0
#define CLOCK_REALTIME 0

static inline int clock_gettime(int clk_id, struct timespec* t) {
    struct timeval now;
    int rv = gettimeofday(&now, NULL);
    if (rv) return rv;
    t->tv_sec  = now.tv_sec;
    t->tv_nsec = now.tv_usec * 1000;
    return 0;
}
#endif

extern int64_t mdispatch_clock;
extern time_t  gdispatch_clock;

#define MONOCLOCK_RESOLUTION 1000000LL /* microseconds */

static inline int64_t
mono4sec(int64_t sec)
{
  return sec * MONOCLOCK_RESOLUTION;
}

static inline int64_t
sec4mono(int64_t monosec)
{
  return monosec / MONOCLOCK_RESOLUTION;
}

static inline int64_t
mono4ms(int64_t ms)
{
  return ms * (MONOCLOCK_RESOLUTION / 1000LL);
}

static inline int64_t
ms4mono(int64_t monosec)
{
  return monosec / (MONOCLOCK_RESOLUTION / 1000LL);
}

static inline int64_t
getmonoclock(void)
{
  struct timespec tp;

  clock_gettime(CLOCK_MONOTONIC, &tp);

  return tp.tv_sec * MONOCLOCK_RESOLUTION +
         (tp.tv_nsec / (1000000000LL/MONOCLOCK_RESOLUTION));
}

static inline int64_t
getfastmonoclock(void)
{
  struct timespec tp;

  clock_gettime(CLOCK_MONOTONIC_COARSE, &tp);

  return tp.tv_sec * MONOCLOCK_RESOLUTION +
         (tp.tv_nsec / (1000000000LL/MONOCLOCK_RESOLUTION));
}

time_t  gdispatch_clock_update(void);
int64_t mdispatch_clock_update(void);

void time_t_out_of_range_notify(int64_t val);

static inline time_t time_t_out_of_range(uint64_t val)
{
  time_t r = val;
  if ((int64_t)r != val) {
    time_t_out_of_range_notify(val);
    r = INT32_MAX;
  }
  return r;
}

#endif /* TVHEADEND_CLOCK_H */
