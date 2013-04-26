/*
 *  Tvheadend - MPEGTS multiplex
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

#include "idnode.h"
#include "queue.h"
#include "input/mpegts.h"

const idclass_t mpegts_mux_class =
{
  .ic_class      = "mpegts_mux",
  .ic_caption    = "MPEGTS Multiplex",
  .ic_properties = (const property_t[]){
  }
};

mpegts_mux_t *
mpegts_mux_create0  
  ( const char *uuid, mpegts_network_t *net, uint16_t onid, uint16_t tsid )
{
  mpegts_mux_t *mm = idnode_create(mpegts_mux, uuid);

  /* Identification */
  mm->mm_onid                = onid;
  mm->mm_tsid                = tsid;

  /* Add to network */
  mm->mm_network             = net;
  mm->mm_initial_scan_status = MM_SCAN_PENDING;
  TAILQ_INSERT_TAIL(&net->mn_initial_scan_pending_queue, mm,
                    mm_initial_scan_link);

  return mm;
}


/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
