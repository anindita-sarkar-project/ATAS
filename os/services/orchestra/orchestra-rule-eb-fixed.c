/*
 * Copyright (c) 2026, RISE Research Institutes of Sweden.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */
/**
 * \file
 *         Orchestra: a single EB slot, fixed and identical on every node,
 *         instead of orchestra-rule-eb-per-time-source.c's per-address hash.
 *
 * Diagnosed root cause this replaces: eb_per_time_source's new_time_source()
 * recomputes a receiver's EB-listening timeslot as hash(new_time_source_addr)
 * % ORCHESTRA_EBSF_PERIOD every time TSCH's time source changes (which
 * tsch_rpl_callback_parent_switch() ties directly to every RPL preferred-
 * parent switch). Confirmed in testing: a node whose parent flapped between
 * two candidates twice within ~90s had its EB-listening slot relocated twice
 * in that window and, despite each relocation correctly matching the new
 * target's own fixed Tx slot on paper, never actually received a single EB
 * from either candidate for 240+ real seconds -- double
 * TSCH_DESYNC_THRESHOLD -- and was forcibly disassociated by tsch.c's
 * "last sync" desync-timeout, which in turn forced RPL to nullify its
 * parent. Root cause: every parent switch (or churn of the two switching
 * back and forth) throws away however much time was already invested
 * relocking onto the old target, restarting the acquisition clock from
 * zero, indefinitely, if the churn keeps happening faster than a full
 * re-lock completes.
 *
 * Fix: don't tie the EB slot to *who* the time source is at all. One fixed
 * (timeslot, channel_offset), identical on every node for both its own EB Tx
 * and its Rx listening, network-wide -- the same convention already used by
 * default_common's and orchestra-rule-implicit-ack.c's l_control cells (see
 * their own comments), extended here to the EB slotframe itself. A parent
 * switch no longer moves anything: the listening position was never a
 * function of the parent's address to begin with, so there's nothing to
 * re-acquire. LINK_OPTION_SHARED's usual per-occurrence random backoff is
 * what keeps multiple nodes' own periodic EB transmissions (already staggered
 * by TSCH_EB_PERIOD's own random jitter, see tsch.c) from colliding on this
 * now-common slot -- the same tolerance default_common's shared cell already
 * relies on for arbitrary contending unicast/broadcast traffic.
 *
 * Data-plane traffic (this project's own UPLINK/RELAY_RX/RELAY_TX/
 * ROOT_ADJACENT cells in orchestra-rule-implicit-ack.c) is untouched by this
 * file and stays pair-wise hash-derived (HASH2/HASH3 of the specific
 * sender/receiver addresses) exactly as before -- only the EB control-plane
 * slot moves to a fixed, network-wide-common position.
 *
 */

#include "contiki.h"
#include "orchestra.h"
#include "net/packetbuf.h"

static uint16_t slotframe_handle = 0;

/* Every node uses this exact (timeslot, channel_offset) for both its own EB
 * Tx and its Rx listening, regardless of identity or current time source.
 * channel_offset 1 matches ORCHESTRA_EB_MIN/MAX_CHANNEL_OFFSET's existing
 * default (both 1), so this doesn't change which physical channels EBs use,
 * only that the timeslot is no longer an address-dependent hash. */
#define ORCHESTRA_EB_FIXED_TIMESLOT       0
#define ORCHESTRA_EB_FIXED_CHANNEL_OFFSET 1

/*---------------------------------------------------------------------------*/
static int
select_packet(uint16_t *slotframe, uint16_t *timeslot, uint16_t *channel_offset)
{
  /* Select EBs only */
  if(packetbuf_attr(PACKETBUF_ATTR_FRAME_TYPE) == FRAME802154_BEACONFRAME) {
    if(slotframe != NULL) {
      *slotframe = slotframe_handle;
    }
    if(timeslot != NULL) {
      *timeslot = ORCHESTRA_EB_FIXED_TIMESLOT;
    }
    /* no need to set the channel offset: it's taken automatically from the link */
    return 1;
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static void
init(uint16_t sf_handle)
{
  struct tsch_slotframe *sf_eb;
  slotframe_handle = sf_handle;
  sf_eb = tsch_schedule_add_slotframe(slotframe_handle, ORCHESTRA_EBSF_PERIOD);
  /* One combined Tx+Rx+Shared link, fixed and identical on every node: covers
   * both our own periodic EB transmission and listening for anyone else's
   * (in particular our current time source's) -- see file header. */
  tsch_schedule_add_link(sf_eb,
                          LINK_OPTION_TX | LINK_OPTION_RX | LINK_OPTION_SHARED,
                          LINK_TYPE_ADVERTISING_ONLY, &tsch_broadcast_address,
                          ORCHESTRA_EB_FIXED_TIMESLOT, ORCHESTRA_EB_FIXED_CHANNEL_OFFSET, 0);
}
/*---------------------------------------------------------------------------*/
struct orchestra_rule eb_fixed_shared = {
  init,
  NULL, /* no new_time_source callback needed: nothing ever moves */
  select_packet,
  NULL,
  NULL,
  NULL,
  NULL,
  "EB fixed shared",
  ORCHESTRA_EBSF_PERIOD,
};
