/*
 * Copyright (c) 2015, SICS Swedish ICT.
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
 * This file is part of the Contiki operating system.
 *
 */

/**
* \ingroup link-layer
* \defgroup tsch 802.15.4 TSCH
The IEEE 802.15.4-2015 TimeSlotted Channel Hopping (TSCH) protocol. Provides
scheduled communication on top of a globally-synchronized network. Performs
frequency hopping for enhanced reliability.
* @{
* \file
*	Main API declarations for TSCH.
*/

#ifndef TSCH_H_
#define TSCH_H_

/********** Includes **********/

#include "contiki.h"
#include "net/mac/mac.h"
#include "net/linkaddr.h"

#include "net/mac/tsch/tsch-conf.h"
#include "net/mac/tsch/tsch-const.h"
#include "net/mac/tsch/tsch-types.h"
#include "net/mac/tsch/tsch-adaptive-timesync.h"
#include "net/mac/tsch/tsch-slot-operation.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/mac/tsch/tsch-log.h"
#include "net/mac/tsch/tsch-packet.h"
#include "net/mac/tsch/tsch-security.h"
#include "net/mac/tsch/tsch-schedule.h"
#include "net/mac/tsch/tsch-stats.h"
#include "net/mac/tsch/tsch-roots.h"
#if UIP_CONF_IPV6_RPL
#include "net/mac/tsch/tsch-rpl.h"
#endif /* UIP_CONF_IPV6_RPL */


/* Include Arch-Specific conf */
#ifdef TSCH_CONF_ARCH_HDR_PATH
#include TSCH_CONF_ARCH_HDR_PATH
#endif /* TSCH_CONF_ARCH_HDR_PATH */

/*********** Callbacks *********/

/* Link callbacks to RPL in case RPL is enabled */
#if UIP_CONF_IPV6_RPL

#ifndef TSCH_CALLBACK_JOINING_NETWORK
#define TSCH_CALLBACK_JOINING_NETWORK tsch_rpl_callback_joining_network
#endif /* TSCH_CALLBACK_JOINING_NETWORK */

#ifndef TSCH_CALLBACK_LEAVING_NETWORK
#define TSCH_CALLBACK_LEAVING_NETWORK tsch_rpl_callback_leaving_network
#endif /* TSCH_CALLBACK_LEAVING_NETWORK */

#ifndef TSCH_CALLBACK_KA_SENT
#define TSCH_CALLBACK_KA_SENT tsch_rpl_callback_ka_sent
#endif /* TSCH_CALLBACK_KA_SENT */

#ifndef TSCH_RPL_CHECK_DODAG_JOINED
#define TSCH_RPL_CHECK_DODAG_JOINED tsch_rpl_check_dodag_joined
#endif /* TSCH_RPL_CHECK_DODAG_JOINED */

#endif /* UIP_CONF_IPV6_RPL */

#if BUILD_WITH_ORCHESTRA

#ifndef TSCH_CALLBACK_NEW_TIME_SOURCE
#define TSCH_CALLBACK_NEW_TIME_SOURCE orchestra_callback_new_time_source
#endif /* TSCH_CALLBACK_NEW_TIME_SOURCE */

#ifndef TSCH_CALLBACK_PACKET_READY
#define TSCH_CALLBACK_PACKET_READY orchestra_callback_packet_ready
#endif /* TSCH_CALLBACK_PACKET_READY */

#ifndef TSCH_CALLBACK_ROOT_NODE_UPDATED
#define TSCH_CALLBACK_ROOT_NODE_UPDATED orchestra_callback_root_node_updated
#endif /* TSCH_CALLBACK_ROOT_NODE_UPDATED */

#if TSCH_WITH_IMPLICIT_ACK
#ifndef TSCH_CALLBACK_NEW_ASFN
#define TSCH_CALLBACK_NEW_ASFN orchestra_ia_new_asfn
#endif /* TSCH_CALLBACK_NEW_ASFN */

#ifndef TSCH_CALLBACK_UNICAST_DATA_INPUT
#define TSCH_CALLBACK_UNICAST_DATA_INPUT orchestra_ia_data_input
#endif /* TSCH_CALLBACK_UNICAST_DATA_INPUT */

#ifndef TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE
#define TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE orchestra_ia_implicit_ack_active
#endif /* TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE */

#ifndef TSCH_CALLBACK_IA_OVERHEAR
#define TSCH_CALLBACK_IA_OVERHEAR orchestra_ia_overhear
#endif /* TSCH_CALLBACK_IA_OVERHEAR */

#ifndef TSCH_CALLBACK_IA_OWN_PARENT
#define TSCH_CALLBACK_IA_OWN_PARENT orchestra_ia_get_own_parent
#endif /* TSCH_CALLBACK_IA_OWN_PARENT */

#ifndef TSCH_CALLBACK_IA_PARENT_EB
#define TSCH_CALLBACK_IA_PARENT_EB orchestra_ia_parent_eb_input
#endif /* TSCH_CALLBACK_IA_PARENT_EB */

#ifndef TSCH_CALLBACK_IA_CONGESTION
#define TSCH_CALLBACK_IA_CONGESTION orchestra_ia_get_own_congestion
#endif /* TSCH_CALLBACK_IA_CONGESTION */

#ifndef TSCH_CALLBACK_IA_PARENT_CONGESTION
#define TSCH_CALLBACK_IA_PARENT_CONGESTION orchestra_ia_parent_congestion_input
#endif /* TSCH_CALLBACK_IA_PARENT_CONGESTION */

#ifndef TSCH_CALLBACK_IA_CONFIRMATION_CYCLES
#define TSCH_CALLBACK_IA_CONFIRMATION_CYCLES orchestra_ia_confirmation_cycles
#endif /* TSCH_CALLBACK_IA_CONFIRMATION_CYCLES */

#ifndef TSCH_CALLBACK_IA_OWN_CHILDREN
#define TSCH_CALLBACK_IA_OWN_CHILDREN orchestra_ia_get_own_children
#endif /* TSCH_CALLBACK_IA_OWN_CHILDREN */

#ifndef TSCH_CALLBACK_IA_CHILD_EB
#define TSCH_CALLBACK_IA_CHILD_EB orchestra_ia_child_eb_input
#endif /* TSCH_CALLBACK_IA_CHILD_EB */
#endif /* TSCH_WITH_IMPLICIT_ACK */

#endif /* BUILD_WITH_ORCHESTRA */

/* Called by TSCH when joining a network */
#ifdef TSCH_CALLBACK_JOINING_NETWORK
void TSCH_CALLBACK_JOINING_NETWORK(void);
#endif

/* Called by TSCH when leaving a network */
#ifdef TSCH_CALLBACK_LEAVING_NETWORK
void TSCH_CALLBACK_LEAVING_NETWORK(void);
#endif

/* Called by TSCH after sending a keep-alive */
#ifdef TSCH_CALLBACK_KA_SENT
void TSCH_CALLBACK_KA_SENT(int status, int transmissions);
#endif

/* Called by TSCH before sending a EB */
#ifdef TSCH_RPL_CHECK_DODAG_JOINED
int TSCH_RPL_CHECK_DODAG_JOINED(void);
#endif

/* Called by TSCH form interrupt after receiving a frame, enabled upper-layer to decide
 * whether to ACK or NACK */
#ifdef TSCH_CALLBACK_DO_NACK
int TSCH_CALLBACK_DO_NACK(struct tsch_link *link, linkaddr_t *src, linkaddr_t *dst);
#endif

/* Called by TSCH when switching time source */
#ifdef TSCH_CALLBACK_NEW_TIME_SOURCE
struct tsch_neighbor;
void TSCH_CALLBACK_NEW_TIME_SOURCE(const struct tsch_neighbor *old, const struct tsch_neighbor *new);
#endif

/* Called by TSCH every time a packet is ready to be added to the send queue */
#ifdef TSCH_CALLBACK_PACKET_READY
int TSCH_CALLBACK_PACKET_READY(void);
#endif

/* Called when a new root node, including the local node, is detected to be added or removed */
#ifdef TSCH_CALLBACK_ROOT_NODE_UPDATED
void TSCH_CALLBACK_ROOT_NODE_UPDATED(const linkaddr_t *, uint8_t is_added);
#endif /* TSCH_CALLBACK_ROOT_NODE_UPDATED */

/* Called by the scheduler (tsch_schedule_get_next_active_link) exactly once whenever
 * the Absolute Slotframe Number (ASFN = ASN / TSCH_IA_SFS_SIZE) advances, so that
 * autonomous, ASFN-rotating cells can be recomputed */
#ifdef TSCH_CALLBACK_NEW_ASFN
void TSCH_CALLBACK_NEW_ASFN(uint32_t asfn);
#endif /* TSCH_CALLBACK_NEW_ASFN */

/* Called from tsch_rx_process_pending() with the link-layer source address of an
 * incoming unicast DATA frame, just before it is passed to packet_input(). Runs
 * synchronously with any IP-layer forwarding decision made for that same frame,
 * so it can be used to tag "who this outgoing (relayed) packet came from" */
#ifdef TSCH_CALLBACK_UNICAST_DATA_INPUT
void TSCH_CALLBACK_UNICAST_DATA_INPUT(const linkaddr_t *source);
#endif /* TSCH_CALLBACK_UNICAST_DATA_INPUT */

/* Called by send_packet() (tsch.c) to decide whether a unicast frame to addr
 * should have its ACK request suppressed -- addr is reached via an implicit-
 * ack-eligible autonomous cell (not the explicit-ack path near the root, and
 * only once our own grandparent is known, since without it we could never
 * overhear a matching relay to confirm the frame). This is the only point
 * where the frame's on-air ack_required bit is still mutable. */
#ifdef TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE
int TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE(const linkaddr_t *addr);
#endif /* TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE */

/* Called by tsch_rx_slot() when a frame is successfully received on a link
 * with LINK_OPTION_IA_OVERHEAR set -- source/destination are neither us nor
 * addressed to us. Implementation should check whether source == our parent
 * and destination == our grandparent, and if so, confirm the corresponding
 * pending packet (see struct tsch_packet's ia_pending field). payload/
 * payload_len are the frame's raw bytes from immediately after the 802.15.4
 * MAC header (i.e. the 6LoWPAN-compressed IP packet) -- needed to check the
 * ORIGINAL IP-layer sender, since a RELAY_TX[child] position is shared by
 * that child's entire subtree (see orchestra_ia_overhear()'s own comment):
 * a source/destination match alone does not mean the frame carries THIS
 * node's own packet rather than a passed-through descendant's. */
#ifdef TSCH_CALLBACK_IA_OVERHEAR
void TSCH_CALLBACK_IA_OVERHEAR(const linkaddr_t *source, const linkaddr_t *destination,
                                struct tsch_link *link,
                                const uint8_t *payload, uint16_t payload_len);
#endif /* TSCH_CALLBACK_IA_OVERHEAR */

/* Called by tsch_packet_create_eb() (tsch-packet.c) to fetch our own current
 * parent's address, for inclusion in our EB so our children can learn their
 * grandparent. Returns nonzero and writes *out and *out_is_root if we have a
 * parent, 0 otherwise (e.g. we are the root). *out_is_root must be filled in
 * here (via our own, locally-valid tsch_roots_is_root(&our_parent) check)
 * rather than left for the receiving child to re-derive: tsch_roots_is_root()
 * is only ever true for a root the *local* node is directly 1 hop from (its
 * list is populated solely by hearing an EB with join_priority 0, see
 * eb_input()/tsch-roots.c) -- a child 2+ hops from the root would always get
 * a false negative asking about its own grandparent directly. */
#ifdef TSCH_CALLBACK_IA_OWN_PARENT
int TSCH_CALLBACK_IA_OWN_PARENT(linkaddr_t *out, uint8_t *out_is_root);
#endif /* TSCH_CALLBACK_IA_OWN_PARENT */

/* Called by eb_input() (tsch.c) when an EB from our own time source carries
 * a grandparent-address IE, with that address (our own grandparent) and
 * whether the sender (our parent) says that address is itself the root --
 * see TSCH_CALLBACK_IA_OWN_PARENT's comment for why that bit must come from
 * the sender rather than be re-derived locally. */
#ifdef TSCH_CALLBACK_IA_PARENT_EB
void TSCH_CALLBACK_IA_PARENT_EB(const linkaddr_t *grandparent, uint8_t grandparent_is_root);
#endif /* TSCH_CALLBACK_IA_PARENT_EB */

/* Called by tsch_packet_create_eb() (tsch-packet.c) to fetch our own current
 * queue depth toward our own parent (self-originated + relayed traffic
 * combined, 0-255), for inclusion in our EB so our children can size their
 * own implicit-ack confirmation deadline against how backed up we actually
 * are -- a child's own local confirm/timeout history can never see this by
 * itself, since a relayed frame resolves synchronously at Tx time regardless
 * of the child's deadline (see the comment in tsch_tx_slot() gating the
 * deferred ia_pending path on self-originated traffic only). */
#ifdef TSCH_CALLBACK_IA_CONGESTION
uint8_t TSCH_CALLBACK_IA_CONGESTION(void);
#endif /* TSCH_CALLBACK_IA_CONGESTION */

/* Called by eb_input() (tsch.c) when an EB from our own time source carries
 * our parent's own congestion byte (see TSCH_CALLBACK_IA_CONGESTION above).
 * Unlike TSCH_CALLBACK_IA_PARENT_EB, not gated on the sender having a
 * grandparent to report -- our parent's congestion is meaningful even if our
 * parent is root-adjacent. */
#ifdef TSCH_CALLBACK_IA_PARENT_CONGESTION
void TSCH_CALLBACK_IA_PARENT_CONGESTION(uint8_t parent_congestion);
#endif /* TSCH_CALLBACK_IA_PARENT_CONGESTION */

/* Called by tsch_tx_slot() (tsch-slot-operation.c) when arming a self-
 * originated implicit-ack-pending packet, to pick how many TSCH_IA_SFS_SIZE
 * cycles to wait before declaring it unconfirmed. Implementation should
 * derive this from the most recently received TSCH_CALLBACK_IA_PARENT_CONGESTION
 * value, not from any local confirm/timeout history (see that callback's
 * comment for why a local signal can't work). */
#ifdef TSCH_CALLBACK_IA_CONFIRMATION_CYCLES
uint8_t TSCH_CALLBACK_IA_CONFIRMATION_CYCLES(void);
#endif /* TSCH_CALLBACK_IA_CONFIRMATION_CYCLES */

/* Called by tsch_packet_create_eb() (tsch-packet.c) to fetch our own direct
 * children's addresses, for inclusion in our EB so our own parent (their
 * grandparent) can install a matching Rx cell for each of our RELAY_TX
 * cells. Returns the number of addresses written into out (capped at
 * max_children). */
#ifdef TSCH_CALLBACK_IA_OWN_CHILDREN
uint8_t TSCH_CALLBACK_IA_OWN_CHILDREN(linkaddr_t *out, uint8_t *out_has_descendants, uint8_t max_children);
#endif /* TSCH_CALLBACK_IA_OWN_CHILDREN */

/* Called by eb_input() (tsch.c) for every received EB, with its source
 * address and the (possibly empty) list of children addresses it carries.
 * Unlike TSCH_CALLBACK_IA_PARENT_EB, not gated on the EB being from our own
 * time source: this fires for an EB from any neighbor, and the callback
 * itself must check whether that neighbor is actually one of our own
 * children before acting on the list. */
#ifdef TSCH_CALLBACK_IA_CHILD_EB
void TSCH_CALLBACK_IA_CHILD_EB(const linkaddr_t *source, const linkaddr_t *children,
                                const uint8_t *children_has_descendants, uint8_t num_children);
#endif /* TSCH_CALLBACK_IA_CHILD_EB */


/***** External Variables *****/

/* Are we coordinator of the TSCH network? */
extern int tsch_is_coordinator;
/* Are we associated to a TSCH network? */
extern int tsch_is_associated;
/* Is the PAN running link-layer security? */
extern int tsch_is_pan_secured;
/* The TSCH MAC driver */
extern const struct mac_driver tschmac_driver;
/* 802.15.4 broadcast MAC address */
extern const linkaddr_t tsch_broadcast_address;
/* The address we use to identify EB queue */
extern const linkaddr_t tsch_eb_address;
/* The current Absolute Slot Number (ASN) */
extern struct tsch_asn_t tsch_current_asn;
extern uint8_t tsch_join_priority;
extern struct tsch_link *current_link;
/* If we are inside a slot, these tell the current channel and channel offset */
extern uint8_t tsch_current_channel;
extern uint8_t tsch_current_channel_offset;
/* TSCH channel hopping sequence */
extern uint8_t tsch_hopping_sequence[TSCH_HOPPING_SEQUENCE_MAX_LEN];
extern struct tsch_asn_divisor_t tsch_hopping_sequence_length;
/* TSCH timeslot timing (in micro-second) */
extern tsch_timeslot_timing_usec tsch_timing_us;
/* TSCH timeslot timing (in rtimer ticks) */
extern tsch_timeslot_timing_ticks tsch_timing;
/* Statistics on the current session */
extern unsigned long tx_count;
extern unsigned long rx_count;
extern unsigned long sync_count;
extern int32_t min_drift_seen;
extern int32_t max_drift_seen;
/* The TSCH standard 10ms timeslot timing */
extern const tsch_timeslot_timing_usec tsch_timeslot_timing_us_10000;
#if TSCH_WITH_IMPLICIT_ACK
/* The short (5ms) implicit-ack-only timeslot timing -- see its definition
 * in tsch-timeslot-timing.c for the timing budget this assumes. */
extern const tsch_timeslot_timing_usec tsch_timeslot_timing_us_short_5000;
/* Switch a link to the short timing template -- see its definition in
 * tsch.c for which links should (and shouldn't) call this. */
void tsch_ia_link_use_short_timing(struct tsch_link *l);
#endif /* TSCH_WITH_IMPLICIT_ACK */

/* Read timeslot timing element `elem` (a tsch_ts_* enum value) for a given
 * link, in microseconds/rtimer ticks respectively: the link's own override
 * if it set one (tsch_ia_link_use_short_timing()), otherwise the global
 * default (tsch_timing_us/tsch_timing) -- so every existing call site that
 * doesn't know about per-link timing continues to work unchanged, and only
 * the ~15 call sites in tsch-slot-operation.c's tx/rx slot state machines
 * (the ones that actually execute a specific link) need to switch to these. */
#define TSCH_LINK_TIMING_US(link, elem) \
  (((link) != NULL && (link)->timing_us != NULL) ? (link)->timing_us[elem] : tsch_timing_us[elem])
#define TSCH_LINK_TIMING(link, elem) \
  (((link) != NULL && (link)->timing_ticks != NULL) ? (link)->timing_ticks[elem] : tsch_timing[elem])

/* TSCH processes */
PROCESS_NAME(tsch_process);
PROCESS_NAME(tsch_send_eb_process);
PROCESS_NAME(tsch_pending_events_process);


/********** Functions *********/

/**
 * Set the TSCH join priority (JP)
 *
 * \param jp the new join priority
 */
void tsch_set_join_priority(uint8_t jp);
/**
 * Set the period at wich TSCH enhanced beacons (EBs) are sent. The period can
 * not be set to exceed TSCH_MAX_EB_PERIOD. Set to 0 to stop sending EBs.
 * Actual transmissions are jittered, spaced by a random number within
 * [period*0.75, period[
 * If RPL is used, the period will be automatically reset by RPL
 * equal to the DIO period whenever the DIO period changes.
 * Hence, calling `tsch_set_eb_period(0)` is NOT sufficient to disable sending EB!
 * To do that, either configure the node in RPL leaf mode, or
 * use static config for TSCH (`define TSCH_CONF_EB_PERIOD 0`).
 *
 * \param period The period in Clock ticks.
 */
void tsch_set_eb_period(uint32_t period);
/**
 * Set the desynchronization timeout after which a node sends a unicasst
 * keep-alive (KA) to its time source. Set to 0 to stop sending KAs. The
 * actual timeout is a random number within [timeout*0.9, timeout[
 * Can be called from an interrupt.
 *
 * \param timeout The timeout in Clock ticks.
 */
void tsch_set_ka_timeout(uint32_t timeout);
/**
 * Set the node as PAN coordinator
 *
 * \param enable 1 to be coordinator, 0 to be a node
 */
void tsch_set_coordinator(int enable);
/**
 * Enable/disable security. If done at the coordinator, the Information
 * will be included in EBs, and all nodes will adopt the same security level.
 * Enabling requires compilation with LLSEC802154_ENABLED set.
 * Note: when LLSEC802154_ENABLED is set, nodes boot with security enabled.
 *
 * \param enable 1 to enable security, 0 to disable it
 */
void tsch_set_pan_secured(int enable);
/**
  * Schedule a keep-alive transmission within [timeout*0.9, timeout[
  * Can be called from an interrupt.
  * @see tsch_set_ka_timeout
  *
  * \param immediate send immediately when 1, schedule using current timeout when 0
  */
void tsch_schedule_keepalive(int immediate);
/**
  * Get the time, in clock ticks, since the TSCH network was started.
  *
  * \return The network uptime, or -1 if the node is not part of a TSCH network.
  */
uint64_t tsch_get_network_uptime_ticks(void);

#ifdef TSCH_RX_RSSI_MIN
/**
 * \brief Emulated-range link filter: feed one received frame's RSSI and
 * return whether frames from this neighbour count as received. Decides on
 * the neighbour's averaged RSSI (with hysteresis), not the single frame.
 */
int tsch_rssi_filter_accept(const linkaddr_t *src, int16_t rssi);
#endif /* TSCH_RX_RSSI_MIN */
/**
  * Leave the TSCH network we are currently in
  */
void tsch_disassociate(void);

#endif /* TSCH_H_ */
/** @} */
