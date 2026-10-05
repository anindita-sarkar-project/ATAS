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
 *         Orchestra: an autonomous, ASFN-rotating tree scheduler with implicit ACKs.
 *
 *         A child overhears the frame its parent forwards on to the grandparent and
 *         treats it as an implicit ACK of its own earlier uplink transmission, since
 *         both child and grandparent are within the parent's Tx range. Cell placement
 *         is autonomous (no control-packet exchange): parent and child independently
 *         compute the same SlotOffset/ChannelOffset from a hash of their addresses and
 *         the current ASFN (Absolute Slotframe Number = ASN / TSCH_IA_SFS_SIZE), so the
 *         hash rotates every slotframe cycle and address collisions between different
 *         node pairs don't persist indefinitely.
 *
 *         Cells (all in the short slotframe "sf_short", except ROOT_ADJACENT; no
 *         longer ASFN-rotating -- see ia_hash1_shard()'s own comment for why):
 *           UPLINK           Tx to parent            HASH(parent)   -- receiver-based,
 *                                                     shared by every sibling with the
 *                                                     same parent (see update_uplink())
 *           RELAY_RX         Rx from any direct       HASH(self)    -- one cell shared by
 *                             child                                  every child, mirrors
 *                                                                     stock Orchestra's own
 *                                                                     receiver-based Rx cell
 *                                                                     (update_relay_rx_shared())
 *           RELAY_TX[child]  Tx to parent, relaying   HASH3(self, child, parent) -- kept
 *                             a child's frame onward   per-child (see struct ia_child)
 *           ROOT_ADJACENT    Tx to a 1-hop root       HASH(root)    -- receiver-based
 *                             (regular timing, explicit ACK -- root has no further
 *                             grandparent, so no frame terminating at it can ever be
 *                             confirmed implicitly, whether self-originated or relayed)
 *
 *         This milestone (M1) installs all of the above and rotates them every ASFN,
 *         but every cell still uses the regular timeslot timing and synchronous
 *         explicit ACKs -- the implicit-ack overhear/deferred-confirmation machinery
 *         and the short timing template are added in later milestones.
 *
 *         Known M1 limitation: a node only learns of its own direct children (via
 *         child_added/child_removed) and its own RPL parent/grandparent-of-root
 *         status; it has no way yet to learn about grandchildren relayed by a
 *         non-root parent (that needs the grandparent/children-tags EB IE, added in
 *         a later milestone). RELAY_TX is therefore only installed when our own
 *         parent is not the root -- any frame destined directly to a root (whether
 *         self-originated or relayed by us) always takes the ROOT_ADJACENT path
 *         instead, which every root already listens for via its own per-child Rx
 *         cells (installed below when tsch_is_coordinator). This means topologies no
 *         deeper than 2 hops from the root (root - 1-hop - 2-hop) are fully handled
 *         by this milestone; a relay through a non-root intermediate (3+ hops from
 *         root) is not yet correctly received until the EB IE milestone lands.
 *
 */

#include "contiki.h"
#include "orchestra.h"
#include "net/packetbuf.h"
#include "net/mac/tsch/tsch-roots.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/ipv6/uip-icmp6.h"
#include "net/routing/rpl-classic/rpl-private.h"
#include "net/ipv6/sicslowpan.h"
#include "sys/ctimer.h"

/*
 * The body of this rule should be compiled only when "nbr_routes" is available,
 * otherwise a link error causes build failure. "nbr_routes" is compiled if
 * UIP_MAX_ROUTES != 0. See uip-ds6-route.c.
 */
#if UIP_MAX_ROUTES != 0

#include "sys/log.h"
#define LOG_MODULE "Orchestra"
#define LOG_LEVEL  LOG_LEVEL_MAC

/* Largest ORCHESTRA_IA_RELAY_TX_SHARDS this build supports; see that
 * constant's own comment (orchestra-conf.h). A fixed cap, not a VLA, so
 * struct ia_child has a constant size regardless of the configured value. */
#define ORCHESTRA_IA_RELAY_TX_SHARDS_MAX 4

/* One entry per grandchild (a direct child's own direct child), learned from
 * that child's own EB -- see orchestra_ia_child_eb_input(). relay_rx (shard
 * 0) and relay_rx_extra (shards 1..ORCHESTRA_IA_RELAY_TX_SHARDS-1) are
 * *normal* (non-overhear) Rx cells in sf_short at HASH3(child, grandchild,
 * self) and its extra-shard variants, matching the positions the child's own
 * RELAY_TX[grandchild] cell(s) transmit on: without every one of them
 * installed, we have no way to ever receive a grandchild's relayed frame
 * that happens to go out on a shard we're not listening to -- this was a
 * real, silent-data-loss bug, not just a missed opportunity: relay_tx_extra
 * (struct ia_child) and self_overhear_extra (this file's own static state)
 * were both already shard-aware when this was found, but this Rx side had
 * never been extended to match, so any relay actually sent on shard k>=1
 * radio-transmitted successfully, was correctly overheard (and confirmed!)
 * by the relaying child's own SELF_OVERHEAR -- which does listen at every
 * shard -- while the *intended* grandparent recipient was never listening
 * there at all and silently never received it. The child's confirmation is
 * therefore not false in what it observes (the frame really was
 * transmitted), but the packet is gone anyway, permanently, with no retry
 * ever triggered, because the sender legitimately believes it succeeded. See
 * MLME_SHORT_IE_TSCH_IA_CHILDREN's comment in frame802154e-ie.c for the full
 * "grandparent fan-out gap" this cell exists to close. */
struct ia_grandchild {
  linkaddr_t addr;
  uint8_t in_use;
  /* Signaled by our own child (this grandchild's parent) in its EB --
   * ie_ia_children_has_descendants (frame802154e-ie.h) -- and mirrors
   * exactly what that child's own child_has_descendants() check locally
   * decided when it chose whether to install relay_tx_extra for this
   * grandchild. Gates relay_rx_extra below the same way, so we only pay for
   * the extra Rx position(s) when the sender could actually ever use them --
   * installing them unconditionally was itself a real, if less severe, cost:
   * every unused Rx cell still occupies a position in the shared,
   * never-rotating hash address space, adding to the same network-wide
   * collision pressure this whole mechanism already has to pay for the
   * positions it actually needs. */
  uint8_t has_descendants;
  struct tsch_link *relay_rx;
  struct tsch_link *relay_rx_extra[ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1];
  /* Last (timeslot<<16|channel) actually logged for relay_rx/relay_rx_extra[k],
   * purely diagnostic -- lets update_grandchild_relay_rx() emit an IATRACE
   * line on every *real* repositioning (including the silent in-place
   * mutate branch, which tsch_schedule_add_link()'s own creation-only log
   * never captures), without spamming one line per EB/ASFN tick when the
   * position hasn't actually changed. 0xffffffff sentinel so the first real
   * position always logs, mirroring add_link's own creation log. */
  uint32_t logged_rx_pos;
  uint32_t logged_rx_extra_pos[ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1];
};

/* One entry per direct child. relay_tx (shard 0) and relay_tx_extra (shards
 * 1..ORCHESTRA_IA_RELAY_TX_SHARDS-1, unused slots left NULL) are the
 * child's RELAY_TX cell(s), all in its own dedicated relay_tx_sf slotframe
 * (see update_child_links() for why it can't share sf_short with our own
 * UPLINK) -- kept per-child, unlike RELAY_RX below, because the
 * implicit-ack confirmation mechanism depends on it: a child's
 * SELF_OVERHEAR only knows a relay is even *potentially* its own frame
 * because RELAY_TX[that child]'s position(s) are unique to it (see
 * update_child_links()'s RELAY_TX comment) -- "potentially", not
 * "definitely", since with ORCHESTRA_IA_RELAY_TX_SHARDS==1 this position is
 * *also* shared by everything relayed through this child from its own
 * descendants, which is why final confirmation additionally requires
 * checking the original IP-layer sender (see orchestra_ia_overhear()'s and
 * ia_overhear_source_is_own()'s comments). grandchildren is unused on the
 * root (a root has no further grandparent-fan-out concern of its own to
 * solve). */
struct ia_child {
  linkaddr_t addr;
  uint8_t in_use;
  struct tsch_link *relay_tx;
  struct tsch_link *relay_tx_extra[ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1];
  struct tsch_slotframe *relay_tx_sf;
  struct ia_grandchild grandchildren[ORCHESTRA_IA_MAX_CHILDREN];
  /* Same diagnostic purpose as struct ia_grandchild's logged_rx_pos, for
   * relay_tx/relay_tx_extra[k]. */
  uint32_t logged_tx_pos;
  uint32_t logged_tx_extra_pos[ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1];
};

static uint16_t slotframe_handle;
static struct tsch_slotframe *sf_short; /* implicit-ack-eligible cells */
static struct tsch_slotframe *sf_root;  /* root-adjacent, regular-timing cells */

static struct tsch_link *l_uplink;
/* Failure-triggered (not periodic) UPLINK rehash -- see uplink_shard_for()'s
 * comment for why UPLINK specifically is a safe target for this and why the
 * earlier, unconditional per-ASFN rotation of every cell was reverted
 * instead of kept. uplink_fail_streak counts consecutive unconfirmed
 * attempts on our own UPLINK (reset to 0 by any confirm); on hitting
 * UPLINK_REHASH_FAIL_THRESHOLD, uplink_epoch advances and UPLINK is
 * remapped to a new shard. */
#define UPLINK_REHASH_FAIL_THRESHOLD 3
static uint8_t uplink_epoch;
#if TSCH_WITH_IMPLICIT_ACK
/* Only read/written from check_ia_timeout()/orchestra_ia_overhear(), both
 * TSCH_WITH_IMPLICIT_ACK-only -- guarded the same way to avoid an unused-
 * variable error when this file is compiled without that feature (e.g.
 * stock-Orchestra examples that pull in this whole module but never
 * select the implicit_ack_tree rule). uplink_epoch itself stays
 * unconditional since uplink_shard_for() (used unconditionally by
 * update_uplink()) reads it regardless. */
static uint8_t uplink_fail_streak;
#endif /* TSCH_WITH_IMPLICIT_ACK */
static struct tsch_link *l_root_adjacent;
static uint8_t have_root;
static linkaddr_t root_linkaddr;
/* ORCHESTRA_IA_UPLINK_SHARDS shared Rx cells receiving from *every* direct
 * child (or, on the root, every root-adjacent child), spread across that
 * many separate CSMA-contention groups -- see update_relay_rx_shared()'s
 * long comment for why this is safe to consolidate (unlike RELAY_TX) and
 * how it mirrors (a multiple of) stock Orchestra's own receiver-based
 * convention. All shards live in the same slotframe (l_relay_rx_shared_sf,
 * one pointer suffices). */
static struct tsch_link *l_relay_rx_shared[ORCHESTRA_IA_UPLINK_SHARDS];
static struct tsch_slotframe *l_relay_rx_shared_sf;

#if TSCH_WITH_IMPLICIT_ACK
/* Our own grandparent (our parent's own parent), learned from our parent's
 * EB -- see orchestra_ia_parent_eb_input(). Needed to compute SELF_OVERHEAR
 * and to know when it is safe to suppress an ACK request at all.
 * grandparent_is_root is handed down by our parent's EB rather than
 * re-derived via tsch_roots_is_root(&grandparent_linkaddr) locally: that
 * list is only ever populated by directly hearing an EB with join_priority
 * 0 (eb_input(), tsch-roots.c), i.e. it only ever holds roots the *local*
 * node is 1 hop from. A grandparent 2+ hops away always reads as "not root"
 * there even when it genuinely is -- confirmed in testing by a node whose
 * grandparent is the root installing a SELF_OVERHEAR cell targeting it
 * anyway (since the too-local check said "not root"), which then never gets
 * anything but the parent's own explicit ACK reception (parent always uses
 * ROOT_ADJACENT for anything destined to the actual root, never a HASH3
 * relay cell), silently corrupting radio state for the rest of that node's
 * traffic (the overhear cell's promiscuous RX_MODE toggle firing every ASFN
 * for no reason). */
static uint8_t have_grandparent;
static linkaddr_t grandparent_linkaddr;
static uint8_t grandparent_is_root;
/* Our parent's own last-reported queue depth toward its own parent (piggy-
 * backed on the same EB IE as the grandparent tag above), used by
 * orchestra_ia_confirmation_cycles() to size our own implicit-ack
 * confirmation deadline against how backed up our parent actually is --
 * see that function's long comment for why this must be a real signal from
 * the parent rather than anything derived from our own local history.
 * Starts at 0 (assume no congestion) so a freshly-joined node uses the
 * shortest deadline until its parent's first EB says otherwise -- matches
 * the empirically-confirmed fact that the short deadline is the right
 * default absent any evidence of real congestion (small/shallow networks). */
static uint8_t parent_congestion;
/* Promiscuous Rx cell where we overhear our parent's relay of our own
 * earlier frame to our grandparent, HASH3(parent, self, grandparent).
 * l_self_overhear_extra mirrors c->relay_tx_extra on the parent side one
 * for one -- with ORCHESTRA_IA_RELAY_TX_SHARDS > 1 our parent's relay of us
 * may go out on any of its shards, so we must listen at every one of them
 * too, or a relay on shard k>0 would be invisible to us entirely (see
 * update_self_overhear()'s own comment). */
static struct tsch_link *l_self_overhear;
static struct tsch_link *l_self_overhear_extra[ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1];
/* Diagnostic only -- same purpose as struct ia_child's logged_tx_pos, for
 * l_self_overhear/l_self_overhear_extra[k]. */
static uint32_t logged_selfoh_pos = 0xffffffffu;
static uint32_t logged_selfoh_extra_pos[ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1] = {0xffffffffu, 0xffffffffu, 0xffffffffu};
/* Fixed (never-rotating) shared Tx|Rx cell at (timeslot=0, channel_offset=0)
 * in whichever of sf_short/sf_root is currently our own uplink-style
 * slotframe, dedicated to RPL control traffic (DIS/DIO/DAO/DAO-ACK) --
 * update_control_link()'s comment has the full rationale. l_control_sf
 * tracks which of the two it currently lives in, since that can change if
 * our own root-adjacency status changes. */
static struct tsch_link *l_control;
static struct tsch_slotframe *l_control_sf;
/* Additional Rx-only (1,0) cell for hearing a CHILD's control traffic, in
 * whichever slotframe/size *that child* transmits it on -- see
 * update_control_rx()'s comment for why this is only ever needed for the
 * root and for root-adjacent nodes, never for anything deeper. */
static struct tsch_link *l_control_rx;
static struct tsch_slotframe *l_control_rx_sf;
#endif /* TSCH_WITH_IMPLICIT_ACK */

static struct ia_child children[ORCHESTRA_IA_MAX_CHILDREN];

/* Multi-cell targeting: an extra RELAY_TX shard (and its matching
 * SELF_OVERHEAR listen position) only helps a child whose own subtree
 * actually generates enough combined traffic to saturate a single shard --
 * a leaf child's RELAY_TX[child] only ever carries that one child's own
 * traffic, no different from stock Orchestra's single per-neighbor cell, so
 * giving it a second shard only adds a permanently-static extra position to
 * the network-wide hash address space (doubling the birthday-paradox
 * collision odds against every *other* cell in the network, our own
 * included -- see ORCHESTRA_IA_RELAY_TX_SHARDS's comment in orchestra-
 * conf.h) for zero throughput benefit. child_has_descendants()/
 * self_has_children() gate extra-shard installation on whether a subtree
 * actually exists at all, using information both sides already have
 * locally and independently -- a parent learns a child's grandchildren via
 * that child's own EB (orchestra_ia_child_eb_input()), and a child knows
 * its own children[] directly -- so both sides reach the same conclusion
 * without any additional signaling. */
static uint8_t
child_has_descendants(const struct ia_child *c)
{
  int i;
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(c->grandchildren[i].in_use) {
      return 1;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
#if TSCH_WITH_IMPLICIT_ACK
/* Guarded: its only caller, update_self_overhear(), is itself entirely
 * TSCH_WITH_IMPLICIT_ACK-only, so this would otherwise trip
 * -Werror=unused-function on a build (e.g. stock-Orchestra examples) that
 * pulls in this whole rule module but never enables that feature. */
static uint8_t
self_has_children(void)
{
  int i;
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(children[i].in_use) {
      return 1;
    }
  }
  return 0;
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
/* Current ASFN, updated once per slotframe cycle via TSCH_CALLBACK_NEW_ASFN.
 * Not folded into ia_hash1_shard()/ia_hash3() (see their own comments) -- it
 * still exists purely so orchestra_ia_new_asfn() has a periodic trigger for
 * check_ia_timeout() and update_self_overhear()'s defensive re-check; the
 * value itself no longer affects any cell's computed position. Folding it
 * back in was tried twice (see ia_hash1_shard()'s comment for the second,
 * measured attempt) and reverted both times. */
static uint32_t current_asfn;

/* A per-node adaptive confirmation deadline (shrink/grow TSCH_IA_
 * CONFIRMATION_TIMEOUT_CYCLES based on each node's own observed confirm/
 * timeout outcomes, TCP-RTO-style) was designed, implemented, and tested
 * here across three iterations -- a latency EWMA, a latency EWMA with
 * multiplicative backoff on timeout, and finally a direct timeout-rate
 * controller with a conservative cold-start value -- and reverted after all
 * three landed at essentially the same ~25-27% PDR at 60-node scale despite
 * behaving very differently internally (confirmed via the armed/deadline
 * IATRACE log: the final version really was escalating individual nodes'
 * own cycles values up to the configured max, not stuck). Root cause: a
 * node's own confirmation timeout only governs *its own* self-originated
 * uplink traffic -- a relayed child's frame is resolved synchronously at
 * radio-Tx-success time regardless of this setting (see tsch_tx_slot()'s
 * own comment, tsch-slot-operation.c) -- so when a node's deadline is too
 * short, the *harm* lands on its PARENT (an extra, unnecessary retry
 * competing for that parent's own relay capacity), not on the node whose
 * own stats are being measured. A purely local signal can't see congestion
 * it causes one hop away, which is exactly where the 60-node bottleneck
 * lives (see the root-cause investigation earlier in this file's history).
 * Fixing this properly would need a congestion signal that actually
 * propagates between parent and child, not a per-node local counter --
 * left as a known limitation rather than force-fitting a bigger redesign
 * under time pressure. TSCH_CONF_IA_CONFIRMATION_TIMEOUT_CYCLES (project-
 * conf.h) is back to a plain fixed constant, at the value (3) that tested
 * reasonably across every scale tried (8/25/49/60 nodes) even though it
 * isn't the single best value for any one of them individually. */

/* Link-layer source address of the unicast DATA frame currently being forwarded,
 * if any -- see orchestra_ia_data_input() and its use in select_packet() below. */
static linkaddr_t last_rx_source;

/* Periodically (safe process context, not slot-operation) checks for
 * orchestra_parent_knows_us flipping 0->1, the one state transition that
 * needs a real cell install but isn't otherwise announced by any of the
 * existing safe callbacks -- see update_uplink(). */
#define IA_BOOTSTRAP_CHECK_PERIOD (CLOCK_SECOND / 4)
static struct ctimer bootstrap_timer;
static uint8_t last_parent_knows_us;

/* Hysteresis for the RPL/TSCH parent-address reconciliation in
 * check_parent_knows_us() was tried and reverted -- see that function's own
 * long comment for the bug being corrected (TSCH's resynchronize() silently
 * moving the time source to an arbitrary EB sender, bypassing RPL) and the
 * full measured sweep. Summary: requiring a divergence to persist for a
 * delay before forcing the correction does cut real disassociations a lot
 * (e.g. 1s delay: 60->29 at 100 nodes, 277->166 at 150), but at every delay
 * tried (0.5s/1s/3s) and at both 100 and 150 nodes, PDR came out *lower*
 * than the plain, correct-immediately version -- disassociations were never
 * actually costing PDR in the first place, so trading them down by waiting
 * only left cells hashed against the wrong parent for longer each time,
 * which did cost PDR. Reverted to unconditional, immediate correction
 * (this file's actual behavior, see check_parent_knows_us()) as the version
 * that measurably wins on the metric that matters; disassoc/RDC upticks
 * are a real but secondary cost of that choice. */

/*---------------------------------------------------------------------------*/
/* current_asfn (ASFN = floor(ASN / TSCH_IA_SFS_SIZE)) was folded back into
 * this hash a second time this session, specifically to fix a diagnosed
 * failure mode at short TSCH_IA_SFS_SIZE: this rule's larger per-node cell
 * count saturates the (slotframe x channel) address space against the
 * fixed, network-wide-shared l_control cell (update_control_link()), and
 * without rotation such a collision never self-resolves. The bug class that
 * caused the *first* revert (see below) turned out to already be moot by
 * the time this was retried: select_packet() now deliberately leaves
 * *timeslot unpinned (0xffff, matching on PACKETBUF_ATTR_TSCH_SLOTFRAME
 * alone -- its own comment) and every conceptual cell role has its own
 * dedicated slotframe handle, so a packet queued for, say, RELAY_TX[child]
 * is still only ever matched against RELAY_TX[child]'s (now-rotated) link
 * wherever ASFN has most recently moved it -- no stale-snapshot hazard.
 *
 * Measured with real Cooja runs (8/25/60 nodes, same seed, rotation vs. the
 * static hash below, steady-state 30-minute window) before being reverted a
 * second time:
 *
 *   TSCH_IA_SFS_SIZE=101 (the paper's own matched-length configuration):
 *     8-node:  PDR 72.9%->67.1%, RDC 1.50%->2.88%   (both worse)
 *    25-node:  PDR 72.7%->68.5%, RDC 1.29%->1.76%   (both worse)
 *
 *   TSCH_IA_SFS_SIZE=17 (the short-slotframe collision scenario rotation
 *   specifically targets -- "not able to re-synchronize" event counts and
 *   RDC in parentheses):
 *    25-node:  PDR 24.9%->27.8% (marginal), RDC 30.3%->5.6% (much better),
 *              resync-fail 217->69, disassoc 157->54, real parent-switches
 *              7->19 (churn roughly tripled)
 *    60-node:  PDR 26.9%->17.6% (worse), RDC 63.4%->13.7% (much better),
 *              resync-fail 600->453 (only modestly better), disassoc
 *              262->258 (unchanged), real parent-switches 23->41 (churn
 *              roughly doubled)
 *
 * So the diagnosis was correct -- rotation does resolve the specific
 * permanent-collision-with-l_control mechanism, and RDC/resync-failure
 * counts confirm it convincingly at every scale tried -- but it introduces
 * a new cost of its own: every cell relocating once per ASFN (~170ms at
 * SFS_SIZE=17) compounds with this topology's existing RPL rank flapping
 * into substantially more real parent switches, and each switch still
 * carries the usual reconvergence cost (see PARENT_SWITCH_THRESHOLD_CONF's
 * comment). That new cost erases most of the PDR upside at 25 nodes and
 * outweighs it entirely at 60 -- and, more importantly, regresses the
 * already-good SFS_SIZE=101 configuration the paper's own results are built
 * on, at both scales tried. Reverted back to the static, non-rotating hash
 * below on that basis: a fix that resolves its target mechanism but costs
 * more than it saves isn't a net improvement, however cleanly it isolates
 * the original diagnosis. Left as a known limitation rather than pursued
 * further (e.g. rotating less often than every ASFN, or only rotating the
 * specific cells most likely to collide with l_control) without a concrete
 * next hypothesis to test.
 *
 * Single-address hash, folding in a shard index so ORCHESTRA_IA_UPLINK_
 * SHARDS distinct positions can exist for the same address instead of one
 * (shard 0 reduces to the plain, unsharded hash this rule used before
 * ORCHESTRA_IA_UPLINK_SHARDS existed) -- see update_relay_rx_shared()'s
 * comment for why: a parent installs one Rx cell per shard, and each child
 * deterministically picks one shard (uplink_shard_for(), based on the
 * child's own address, so it never changes on a parent switch) for its own
 * UPLINK. This splits however many children share one parent across
 * ORCHESTRA_IA_UPLINK_SHARDS separate CSMA-contention groups instead of one,
 * directly addressing the "duplicate retransmission" cost of a single
 * shared cell (see ORCHESTRA_IA_UPLINK_SHARDS's comment in orchestra-
 * conf.h) at a small, fixed (not per-child) increase in cell count -- still
 * O(1) in the number of children, just a larger constant than plain
 * consolidation's 1. 2654435761 is Knuth's multiplicative hash constant
 * (2^32 / golden ratio): multiplying the shard index by it before adding
 * spreads different shards' results across the full 32-bit range rather
 * than clustering them near each other, so ia_slot()/ia_channel()'s modulo
 * reduction doesn't correlate shard 0 and shard 1's positions. */
/* IA_ASFN_EXPERIMENT: re-adds the global per-ASFN rotation term (reverted
 * earlier this session at SF=101/SF=17, see current_asfn's own comment) on
 * top of the UPLINK-only failure-triggered rehash above. Tested at SF=167
 * and SF=397 (60/100/150 nodes) to check whether the SF=101/17 verdict
 * ("costs more in churn than it saves") generalizes -- it doesn't, cleanly,
 * either way: at SF=167 it was flat-to-worse at every scale tried (100-node
 * PDR 23.9%->15.3%, real parent-switches 27->127 at 60 nodes); at SF=397 it
 * *helped* at 100/150 nodes (PDR 5.8%->9.7% and 4.1%->7.5%, both with
 * similar or better RDC) but not at 60 (PDR 14.2%->11.8%). No single
 * on/off setting of this knob is best across every (SFS_SIZE, scale) pair
 * tried so far -- left disabled (matching the UPLINK-only rehash already
 * adopted) since that is the configuration actually in the paper's results;
 * flip to 1 to reproduce the SF=397/100-150-node improvement specifically. */
#define IA_ASFN_EXPERIMENT 0
static uint32_t
ia_hash1_shard(const linkaddr_t *addr, uint8_t shard)
{
  return (uint32_t)ORCHESTRA_LINKADDR_HASH(addr) + (uint32_t)shard * 2654435761u
#if IA_ASFN_EXPERIMENT
         + current_asfn * 0x85EBCA6Bu
#endif
         ;
}
/*---------------------------------------------------------------------------*/
/* Which shard (0..ORCHESTRA_IA_UPLINK_SHARDS-1) this node's own UPLINK (and
 * therefore whichever of its parent's ORCHESTRA_IA_UPLINK_SHARDS Rx cells it
 * targets) uses -- a function of this node's own address (stable across
 * parent switches, only the *parent* term in ia_hash1_shard() changes then)
 * plus uplink_epoch, a purely local, failure-triggered counter (see its own
 * declaration comment). Folding epoch in additively before the modulo means
 * each rehash steps to the *next* shard in a fixed cycle -- guaranteed to
 * visit every one of the ORCHESTRA_IA_UPLINK_SHARDS candidates at least once
 * every ORCHESTRA_IA_UPLINK_SHARDS rehashes, rather than risk landing back
 * on an already-tried bad shard by chance.
 *
 * This is safe as a *unilateral*, uncoordinated change specifically because
 * of how update_relay_rx_shared() already works: the parent unconditionally
 * installs all ORCHESTRA_IA_UPLINK_SHARDS Rx cells regardless of which
 * shard any given child actually uses, so whichever shard this node's own
 * epoch happens to select, the corresponding Rx cell already exists and is
 * already being listened on -- no need for the parent to learn this node's
 * current epoch value at all. RELAY_TX[child]/SELF_OVERHEAR (ia_hash3(),
 * its own comment) don't have this property -- there's exactly one
 * position, not an N-way pre-installed menu -- which is why the earlier,
 * unconditional per-ASFN rotation attempt (folded into every cell type via
 * current_asfn, then reverted) isn't reapplied to those here: a unilateral
 * rehash on either the parent or child side alone would desynchronize the
 * pair with nothing to fix it. */
static uint8_t
uplink_shard_for(const linkaddr_t *addr)
{
  return (uint8_t)((ORCHESTRA_LINKADDR_HASH(addr) + uplink_epoch) % ORCHESTRA_IA_UPLINK_SHARDS);
}
/*---------------------------------------------------------------------------*/
/* Sender-based single-address hash, applied to l_uplink/ROOT_ADJACENT/
 * RELAY_RX (mirrors stock Orchestra's ORCHESTRA_CONF_UNICAST_SENDER_BASED
 * convention): a child's own Tx position depends only on its own address,
 * never its parent's, so it never relocates on a parent switch -- the same
 * class of fix as orchestra-rule-eb-fixed.c's.
 *
 * Re-enabled after being tried and reverted on an 8-node scattered topology
 * (where it measured 95.6% PDR vs. pairwise HASH2's 97.5%, a wash, since
 * that topology had almost no parent switching to begin with -- see git
 * history for that A/B). Grid topologies (25/49-node square grids) showed
 * why it matters: 129 parent-switch churns logged across just 24 non-root
 * nodes in one 60-minute run -- over 5 per node -- because a grid's regular
 * structure puts many nodes equidistant from multiple candidate parents,
 * so RPL/MRHOF's rank comparisons flip-flop far more than in a scattered or
 * linear topology. Every one of those switches, under pairwise HASH2(parent,
 * child), relocated l_uplink's (and every affected RELAY_RX's) position --
 * compounding with this file's existing SFS_SIZE=31 collision pressure (see
 * project-conf.h) into a disassociation cascade that pairwise hashing alone
 * doesn't have a way to avoid. ia_hash1(child) removes that specific
 * compounding factor: the position itself is now immune to how often the
 * parent changes, regardless of *why* it keeps changing. */
static uint32_t
ia_hash3(const linkaddr_t *parent, const linkaddr_t *child, const linkaddr_t *grandparent)
{
  return ORCHESTRA_LINKADDR_HASH3(parent, child, grandparent)
#if IA_ASFN_EXPERIMENT
         + current_asfn * 0x85EBCA6Bu
#endif
         ;
}
/*---------------------------------------------------------------------------*/
static uint16_t
ia_slot(uint32_t hash, uint16_t period)
{
  return period > 0 ? (uint16_t)(hash % period) : 0;
}
/*---------------------------------------------------------------------------*/
static uint16_t
ia_channel(uint32_t hash)
{
  return tsch_hopping_sequence_length.val > 0 ? (uint16_t)(hash % tsch_hopping_sequence_length.val) : 0;
}
/*---------------------------------------------------------------------------*/
#if TSCH_WITH_IMPLICIT_ACK
/* Whether the packet currently in packetbuf is RPL control traffic (DIS,
 * DIO, DAO, or DAO-ACK) rather than application data or a relayed frame.
 * PACKETBUF_ATTR_NETWORK_ID/_CHANNEL are populated by sicslowpan.c's
 * set_packet_attrs() -- from the *uncompressed* IPv6/ICMPv6 header, before
 * 6LoWPAN compression -- specifically because a netstack sniffer is
 * registered (orchestra_init() always registers one), and survive
 * unmodified all the way to send_packet()/orchestra_callback_packet_ready(),
 * i.e. exactly when select_packet() and orchestra_ia_implicit_ack_active()
 * run. Same check already used (for a different purpose) in orchestra.c's
 * own sniffer callback.
 *
 * DAO/DIS/unicast-DIO are addressed exactly like regular uplink traffic
 * (destination == our parent), so without this check they'd ride the same
 * cell as app data -- including, for a 3+-hop node, an implicit-ack cell
 * with its ACK suppressed. That's a bad trade for control-plane traffic
 * specifically: losing one app data packet is harmless, but a lost/delayed
 * DAO destabilizes RPL routing and, via the same parent link, the very
 * time-sync refresh (a successful ACK'd Tx to our time source refreshes
 * last_sync_asn, tsch-slot-operation.c) that keeps us associated at all --
 * confirmed in testing as a contributing cause of periodic disassociation
 * once DAO/control traffic started sharing unreliable implicit-ack cells.
 * Keeping DIO/EB/DAO/DIS on the regular, always-explicit-ack path removes
 * that feedback loop entirely regardless of any other collision/reliability
 * variance in the implicit-ack cells themselves. */
static uint8_t
ia_is_rpl_control_packet(void)
{
  if(packetbuf_attr(PACKETBUF_ATTR_NETWORK_ID) != UIP_PROTO_ICMP6) {
    return 0;
  }
  {
    uint16_t rpl_code = packetbuf_attr(PACKETBUF_ATTR_CHANNEL);
    return rpl_code == (ICMP6_RPL << 8 | RPL_CODE_DIS)
        || rpl_code == (ICMP6_RPL << 8 | RPL_CODE_DIO)
        || rpl_code == (ICMP6_RPL << 8 | RPL_CODE_DAO)
        || rpl_code == (ICMP6_RPL << 8 | RPL_CODE_DAO_ACK);
  }
}
/* Whether the packet currently in packetbuf is a TSCH keep-alive
 * (keepalive_send(), tsch.c): a bare MAC frame with no payload at all
 * (packetbuf_clear() followed by nothing else), the only frame type in this
 * whole stack with datalen 0 -- every IP-layer packet, even a bare DIS, has
 * at least a compressed IPv6/ICMPv6 header. packetbuf_hdralloc() (used by
 * NETSTACK_FRAMER.create(), already run by the time select_packet()/
 * orchestra_ia_implicit_ack_active() see this packet) only grows hdrlen, never
 * buflen, so this stays a reliable 0 for a KA both before and after framing.
 *
 * KAs need exactly the same exclusion as RPL control traffic above, for the
 * same reason: a KA's entire purpose is to solicit a real ACK so its
 * time-correction IE can refresh last_sync_asn (tsch-slot-operation.c) --
 * TSCH's only per-transmission clock resync opportunity, EB reception being
 * far rarer. An implicit-ack UPLINK/RELAY_TX cell has no SELF_OVERHEAR
 * watching for a KA (nothing to overhear -- the parent doesn't relay a KA
 * anywhere) and no ACK budget in its short timing template, so a suppressed-
 * ACK KA can only ever resolve via the multi-cycle ia-timeout sweep, never a
 * real ACK -- starving resync down to EB-period-only. Confirmed in testing:
 * grid runs showing "leaving the network, last sync NNNN" (full
 * disassociation) despite otherwise-healthy relay traffic, each event
 * flushing every packet -- including every child's in-flight relayed
 * frames, not just this node's own -- queued to the old parent
 * (tsch_queue_free_packets_to(), tsch-queue.c). */
static uint8_t
ia_is_keepalive_packet(void)
{
  return packetbuf_datalen() == 0;
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
#if TSCH_WITH_IMPLICIT_ACK
/* Whether our own Tx traffic to our parent (UPLINK, RELAY_TX[child]) is
 * implicit-ack-eligible right now -- must exactly mirror
 * orchestra_ia_implicit_ack_active()'s condition (minus the address-match
 * and orchestra_parent_knows_us checks, both already guaranteed by the
 * caller's own gating), so a cell's timing template and a packet's actual
 * ACK requirement (decided per-send in send_packet()) never disagree.
 * Disagreement here is dangerous, not just inefficient: a packet that DOES
 * need an explicit ACK, sent on a link whose short template has zero ACK
 * budget (RxAckDelay/AckWait/MaxAck all 0), can never receive that ACK no
 * matter how many times it's retried.
 *
 * Only OUR OWN parent's root-ness matters here -- not our grandparent's.
 * Only the root and nodes one hop from it (root-adjacent, whose own parent
 * IS root) ever need regular timing/explicit ack for their own uplink:
 * root-adjacent nodes have no grandparent to overhear a confirming relay
 * through at all, so ROOT_ADJACENT must stay a real, synchronous ack. Every
 * other node -- 2 hops out and deeper -- has a real grandparent, and its
 * *parent*'s own relay of its frame onward (RELAY_TX[us], on the parent's
 * side) can use short timing regardless of whether that parent is itself
 * root-adjacent: root only needs to *receive* the relayed frame, not ack it,
 * so the parent's own root-adjacency is irrelevant to whether relaying THIS
 * child's frame can use implicit ack. (An earlier version of this function
 * also required the grandparent to not be root, on the theory that a
 * root-adjacent parent could only ever relay via its own ROOT_ADJACENT cell
 * -- see update_child_links()'s RELAY_TX comment for why that assumption was
 * wrong and why a root-adjacent parent gets its own dedicated, short-timed
 * RELAY_TX[child] cell instead, separate from its own ROOT_ADJACENT
 * traffic.) Re-derived on every call (creation and ASFN-driven mutation
 * alike) rather than cached, since have_grandparent can change over the
 * link's lifetime (a parent switch can gain/lose a grandparent). */
static uint8_t
ia_tx_short_timing_ok(void)
{
  return have_grandparent
      && !linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
      && !tsch_roots_is_root(&orchestra_parent_linkaddr);
}
/* Whether a direct child's Tx to us -- RELAY_RX[child] (child's own uplink)
 * *and* RELAY_TX[child] (us relaying that same child's frame onward) -- is
 * implicit-ack-eligible: mirrors that condition as evaluated *by the
 * child*, whose parent is us (never root here -- update_child_links() only
 * installs RELAY_RX/RELAY_TX at all when we're not the coordinator) and
 * whose grandparent is our own parent. RELAY_TX[child]'s timing must use
 * this -- the CHILD's eligibility -- not ia_tx_short_timing_ok() (OUR OWN
 * eligibility): those are unrelated facts. Getting this wrong is exactly
 * why implicit ack silently never confirms for a 3+-hop node whose relaying
 * parent is itself only 2 hops from root: RELAY_TX[child] would follow the
 * *parent's* own (regular) timing while the child's SELF_OVERHEAR -- gated
 * by the child's own, different eligibility -- keeps expecting short timing,
 * so the child's listen window closes long before the parent's regular-
 * timing TxOffset (~2120us vs ~700us) even begins transmitting: the overhear
 * can never land regardless of address matching (confirmed in testing: 0
 * confirmed / 216 timeouts over a 15-minute run once this mismatch was
 * introduced). RELAY_RX[child] and RELAY_TX[child] must always agree with
 * each other and with the child's own SELF_OVERHEAR/UPLINK decision, since
 * all three are about the exact same fact: is this specific child eligible.
 *
 * Deliberately does *not* also require our own parent to not be root: doing
 * so used to mean a root-adjacent parent (whose own parent literally IS
 * root) could never relay a child with short timing at all, forcing every
 * 2-hop node onto full explicit-ack regardless of how deep the tree is
 * below the root. But whether root-adjacent parent P can relay child C's
 * frame with implicit ack has nothing to do with P's own distance to root:
 * root just needs to *receive* the relayed frame, not ack it, so P's own
 * RELAY_TX[C] cell (a separate cell from P's own ROOT_ADJACENT traffic, see
 * update_child_links()) can use short timing exactly like any other relay's
 * would. Only the root and root-adjacent nodes' *own* uplink traffic
 * (ia_tx_short_timing_ok()) needs to stay on regular timing -- relaying a
 * child one level further out is a different question entirely. */
static uint8_t
ia_rx_short_timing_ok(void)
{
  return !tsch_is_coordinator
      && !linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null);
}
/* Whether a grandchild's relay through an intermediate child -- received on
 * our own dedicated grandchild-relay-rx cell -- is implicit-ack-eligible:
 * mirrors that condition as evaluated *by the grandchild*, whose parent is
 * the intermediate child (never root -- it has a parent, us, by
 * definition) and whose grandparent is us. Deliberately *not*
 * ia_rx_short_timing_ok(): that also requires *our own* parent to not be
 * root, which is irrelevant here -- the grandchild only cares whether its
 * own uplink to the intermediate child is eligible (always true: its
 * parent, the intermediate child, is structurally never root), not whether
 * WE (the grandparent evaluating this cell) happen to be root ourselves.
 * Unconditionally eligible for exactly the same reason update_child_links()'s
 * RELAY_TX creation no longer excludes a root-adjacent relayer: whoever
 * ultimately receives a relayed frame only needs to *receive* it, never ack
 * it, so our own root-ness is irrelevant to whether the relay below us can
 * use implicit ack. This used to return !tsch_is_coordinator, disabling
 * short timing (and, via update_grandchild_relay_rx()'s matching gate at the
 * time, the cell's very existence) specifically when evaluated by root --
 * exactly the case root now needs this cell for, once root-adjacent nodes
 * also relay via RELAY_TX (see git history). */
static uint8_t
ia_grandchild_short_timing_ok(void)
{
  return 1;
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
static struct ia_child *
find_child(const linkaddr_t *addr)
{
  int i;
  if(addr == NULL || linkaddr_cmp(addr, &linkaddr_null)) {
    return NULL;
  }
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(children[i].in_use && linkaddr_cmp(&children[i].addr, addr)) {
      return &children[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
static struct ia_child *
alloc_child(const linkaddr_t *addr)
{
  int i, j;
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(!children[i].in_use) {
      children[i].in_use = 1;
      linkaddr_copy(&children[i].addr, addr);
      children[i].relay_tx = NULL;
      children[i].relay_tx_sf = NULL;
      children[i].logged_tx_pos = 0xffffffffu;
      for(j = 0; j < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; j++) {
        children[i].relay_tx_extra[j] = NULL;
        children[i].logged_tx_extra_pos[j] = 0xffffffffu;
      }
      for(j = 0; j < ORCHESTRA_IA_MAX_CHILDREN; j++) {
        int k;
        children[i].grandchildren[j].in_use = 0;
        children[i].grandchildren[j].relay_rx = NULL;
        children[i].grandchildren[j].logged_rx_pos = 0xffffffffu;
        for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
          children[i].grandchildren[j].relay_rx_extra[k] = NULL;
          children[i].grandchildren[j].logged_rx_extra_pos[k] = 0xffffffffu;
        }
      }
      return &children[i];
    }
  }
  LOG_ERR("ia: no free child slot for ");
  LOG_ERR_LLADDR(addr);
  LOG_ERR_("\n");
  return NULL;
}
/*---------------------------------------------------------------------------*/
#if TSCH_WITH_IMPLICIT_ACK
/* Does timeslot ts collide with any of our own SELF_OVERHEAR positions
 * (shard 0 through ORCHESTRA_IA_RELAY_TX_SHARDS-1)? Computed directly from
 * the (parent, self, grandparent) triple rather than read off l_self_
 * overhear/l_self_overhear_extra, so this has no dependency on whether
 * update_self_overhear() has already run on this same tick -- both
 * functions can independently derive the identical, deterministic answer.
 * Used by update_uplink() below to detect, and where possible avoid, a
 * genuine scheduling conflict confirmed to occur in testing: if our own
 * UPLINK ends up on the same timeslot as our own SELF_OVERHEAR, TSCH's
 * scheduler tie-break always favors the Tx link, so SELF_OVERHEAR would
 * silently and permanently never fire, and every implicit-ack confirmation
 * we're waiting on would time out and retry instead, forever, for as long
 * as this (parent, self, grandparent, epoch) combination holds. */
static uint8_t
uplink_ts_conflicts_self_overhear(uint16_t ts)
{
  uint8_t k;
  if(!have_grandparent) {
    return 0;
  }
  for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS && k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX; k++) {
    uint32_t h = ia_hash3(&orchestra_parent_linkaddr, &linkaddr_node_addr, &grandparent_linkaddr);
    if(k > 0) {
      h = (h + (uint32_t)k * 0x85EBCA6Bu) & 0xFFFFFFFFu;
    }
    if(ia_slot(h, TSCH_IA_SFS_SIZE) == ts) {
      return 1;
    }
  }
  return 0;
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
/* Our own uplink to our parent. Skipped (and torn down) whenever our parent is
 * a root: any frame destined directly to a root always uses ROOT_ADJACENT
 * instead, self-originated or relayed alike (see file header).
 *
 * allow_install must be 0 when called from the TSCH_CALLBACK_NEW_ASFN path
 * (orchestra_ia_new_asfn(), invoked from inside
 * tsch_schedule_get_next_active_link() while tsch_in_slot_operation may
 * still be set) -- tsch_schedule_add_link()/_remove_link() call
 * tsch_get_lock(), which busy-waits for tsch_in_slot_operation to clear, and
 * that flag can only be cleared by the very slot-operation call we'd be
 * blocking inside of, deadlocking the mote solid. From that path this
 * function may only mutate fields of an already-installed link. It's 1 from
 * every other (safe, non-slot-operation) caller: new_time_source(),
 * root_node_updated(), child_added(), and check_parent_knows_us_transition(). */
static void
update_uplink(uint8_t allow_install)
{
  if(sf_short == NULL || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || tsch_roots_is_root(&orchestra_parent_linkaddr)
     || !orchestra_parent_knows_us) {
    /* Installing this link -- addressed to our parent specifically, not to
     * tsch_broadcast_address -- bumps our parent's tx_links_count on our
     * side (tsch_schedule_add_link() -> tsch_queue_add_nbr()), which
     * disqualifies it from tsch_queue_get_unicast_packet_for_any()'s
     * fallback scan (it only considers neighbors with tx_links_count == 0).
     * That fallback is exactly what lets a DAO go out via default_common
     * before any dedicated cell exists on either side. Until our parent has
     * ACKed a DAO from us (confirming it now knows about us and, for
     * root-adjacent parents, has had the chance to install its own Rx cell
     * via child_added()), we must not install this link at all -- not just
     * decline to select it in select_packet(). */
    if(allow_install && l_uplink != NULL) {
      tsch_schedule_remove_link(sf_short, l_uplink);
      l_uplink = NULL;
    }
    return;
  }
  {
    /* Receiver-based: hash of our PARENT's address (plus our own shard, see
     * uplink_shard_for()), not our own address -- mirrors stock Orchestra's
     * own receiver-based convention (ORCHESTRA_CONF_UNICAST_SENDER_BASED=0),
     * and is what makes update_relay_rx_shared()'s consolidation possible:
     * every one of our parent's children that picks the same shard we do
     * computes this exact same hash (since it depends only on the shared
     * parent's address and the shard, not on our own address beyond that),
     * so they converge on the same one of our parent's ORCHESTRA_IA_UPLINK_
     * SHARDS Rx positions instead of each needing a dedicated Rx cell. The
     * tradeoff, same one stock Orchestra itself accepts (just divided by
     * ORCHESTRA_IA_UPLINK_SHARDS instead of avoided entirely): this position
     * relocates on every parent switch (see git history for the earlier
     * sender-based ia_hash1(child) variant, kept for RELAY_TX/SELF_OVERHEAR
     * below where consolidation isn't safe -- see struct ia_child's
     * comment). PARENT_SWITCH_THRESHOLD_CONF (project-conf.h) already raises
     * RPL's own switch hysteresis to cushion this.
     * A separate, shorter-period slotframe for just this (now-shared,
     * now-contended) cell was tried and reverted: it caused a severe,
     * unexplained regression (even the unrelated fixed l_control cell's own
     * success rate collapsed from ~normal to 13%, 55/426 attempts, in one
     * 45-minute 8-node run) that wasn't tracked down before time ran out --
     * see git history. Left at TSCH_IA_SFS_SIZE/sf_short pending further
     * investigation into that regression; ORCHESTRA_IA_UPLINK_SHARDS is the
     * lever this session settled on instead for cutting contention on this
     * cell without touching slotframe size. */
    uint32_t h = ia_hash1_shard(&orchestra_parent_linkaddr, uplink_shard_for(&linkaddr_node_addr));
    uint16_t ts = ia_slot(h, TSCH_IA_SFS_SIZE);
    uint16_t ch = ia_channel(h);
#if TSCH_WITH_IMPLICIT_ACK
    /* Proactive self-collision avoidance (see uplink_ts_conflicts_self_
     * overhear()'s own comment): if our currently-chosen shard lands on our
     * own SELF_OVERHEAR's timeslot, permanently advance uplink_epoch (the
     * same state check_ia_timeout()'s existing failure-triggered rehash
     * mutates) to move to the next shard instead, before ever installing
     * this position -- rather than waiting for the resulting confirmation-
     * timeout streak to eventually trigger that reactive path. Advancing
     * the real epoch here, rather than substituting a locally-computed
     * alternative just for this call, keeps every other reader of
     * uplink_shard_for() (in particular check_ia_timeout()'s own streak
     * bookkeeping) consistent with which shard we actually installed. Tried
     * at most once: with ORCHESTRA_IA_UPLINK_SHARDS==2 there is exactly one
     * alternative, and if it also collides we keep the original rather than
     * loop -- a residual, lower-probability conflict is still better odds
     * than guaranteeing one by refusing to pick anything. This only ever
     * changes *which* of the already-existing ORCHESTRA_IA_UPLINK_SHARDS
     * positions we use -- our parent listens on all of them unconditionally
     * regardless of why we picked one over the other, so this is exactly as
     * safe/uncoordinated as the reactive rehash it complements. */
    if(uplink_ts_conflicts_self_overhear(ts)) {
      uint32_t h2 = ia_hash1_shard(&orchestra_parent_linkaddr,
                                    (uint8_t)((uplink_shard_for(&linkaddr_node_addr) + 1) % ORCHESTRA_IA_UPLINK_SHARDS));
      uint16_t ts2 = ia_slot(h2, TSCH_IA_SFS_SIZE);
      if(!uplink_ts_conflicts_self_overhear(ts2)) {
        uplink_epoch++;
        h = h2;
        ts = ts2;
        ch = ia_channel(h2);
      }
    }
#endif /* TSCH_WITH_IMPLICIT_ACK */
    if(l_uplink == NULL) {
      if(!allow_install) {
        return;
      }
      l_uplink = tsch_schedule_add_link(sf_short, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                         LINK_TYPE_NORMAL, &orchestra_parent_linkaddr, ts, ch, 1);
    } else {
      l_uplink->timeslot = ts;
      l_uplink->channel_offset = ch;
    }
#if TSCH_WITH_IMPLICIT_ACK
    /* Re-derived every call (not just at creation): see ia_tx_short_timing_ok()'s
     * comment for why this must track the same live condition send_packet()
     * uses to decide the ACK bit, not be fixed once at cell creation. */
    if(ia_tx_short_timing_ok()) {
      tsch_ia_link_use_short_timing(l_uplink);
    } else if(l_uplink != NULL) {
      l_uplink->timing_us = NULL;
      l_uplink->timing_ticks = NULL;
    }
#endif /* TSCH_WITH_IMPLICIT_ACK */
  }
}
/*---------------------------------------------------------------------------*/
/* Our own ROOT_ADJACENT cell towards a 1-hop root neighbor (regular timing,
 * explicit ack -- installed/updated once we've heard that root's EB).
 * allow_install: see update_uplink(). */
static void
update_root_adjacent(uint8_t allow_install)
{
  if(!have_root || sf_root == NULL || !orchestra_parent_knows_us) {
    /* Same reasoning as update_uplink(): don't install a link addressed to
     * root until root has ACKed a DAO from us, or we permanently block the
     * default_common fallback that DAO itself needs to travel on. */
    if(allow_install && l_root_adjacent != NULL) {
      tsch_schedule_remove_link(sf_root, l_root_adjacent);
      l_root_adjacent = NULL;
    }
    return;
  }
  {
    /* Receiver-based: hash of the root's address, shard 0 only -- see
     * update_uplink()'s matching comment on sharding in general, and
     * update_relay_rx_shared()'s on why ORCHESTRA_IA_UPLINK_SHARDS isn't
     * applied to the root-adjacent tier: ORCHESTRA_IA_ROOT_PERIOD is small
     * (7 by default) precisely because few nodes are ever root-adjacent, so
     * there's little CSMA contention to split in the first place -- doubling
     * cell density in that already-tiny (period * channels) address space
     * measurably hurt it instead (confirmed in testing: a root-adjacent
     * node repeatedly disassociating, stuck at "last sync" timeouts, once
     * this tier was also sharded). */
    uint32_t h = ia_hash1_shard(&root_linkaddr, 0);
    uint16_t ts = ia_slot(h, ORCHESTRA_IA_ROOT_PERIOD);
    uint16_t ch = ia_channel(h);
    if(l_root_adjacent == NULL) {
      if(!allow_install) {
        return;
      }
      l_root_adjacent = tsch_schedule_add_link(sf_root, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                                LINK_TYPE_NORMAL, &root_linkaddr, ts, ch, 1);
    } else {
      l_root_adjacent->timeslot = ts;
      l_root_adjacent->channel_offset = ch;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* ORCHESTRA_IA_UPLINK_SHARDS Rx-only cells, together shared by *every* direct
 * child (or, on the root, every root-adjacent child), at ia_hash1_shard(our
 * own address, 0..ORCHESTRA_IA_UPLINK_SHARDS-1) -- the exact positions every
 * one of our children's own UPLINK cells now converges on (each child using
 * whichever shard uplink_shard_for() picks for its own address), since
 * update_uplink() hashes the PARENT's address (plus that shard), not the
 * child's. This mirrors (a multiple of) stock Orchestra's own receiver-based
 * convention (each node has Rx slot(s) at its own hash position(s), and
 * any/all of its neighbors converge their Tx there), replacing what used to
 * be one dedicated RELAY_RX cell per child.
 *
 * Safe to consolidate, unlike RELAY_TX/SELF_OVERHEAR: this cell only needs
 * to *receive* a child's frame, not confirm delivery of a specific one --
 * the implicit-ack confirmation for that same frame happens one hop further
 * along (RELAY_TX[child]/SELF_OVERHEAR, still per-child dedicated, see
 * struct ia_child's comment), which is entirely unaffected by how many other
 * children share one of our RELAY_RX shards. Addressed to tsch_broadcast_
 * address, not a specific neighbor, since (unlike l_control) these cells are
 * Rx-only and must accept traffic from whichever child happens to send --
 * mirrors orchestra-rule-unicast-per-neighbor-rpl-storing.c's own broadcast-
 * addressed receiver-based cells.
 *
 * Position never actually rotates in practice: unlike UPLINK/ROOT_ADJACENT
 * (which relocate on a parent switch) or RELAY_TX (which relocates when a
 * *child's* parent -- us -- doesn't change, but is created/destroyed per
 * child), these cells' hash input is just our own fixed address, a fixed
 * shard index, and our own (fixed-after-boot) coordinator status -- called
 * from the same lifecycle points as update_control_rx() purely for
 * consistency, not because anything here actually changes over time.
 * allow_install: see update_uplink().
 *
 * Installed unconditionally (not gated on currently having a child) once we
 * know our own role, deliberately: a lazy have-at-least-one-child gate was
 * tried and reverted -- it does cut RDC for leaf nodes as expected (confirmed
 * in testing, roughly halving it on a 25/49-node grid), but toggling this
 * cell in and out of the schedule on every child-count transition (0 <-> 1+)
 * measurably destabilized delivery instead (25-node PDR dropped from ~70% to
 * ~51%, 49-node from ~51% back down to ~35%, undoing this whole
 * consolidation's own gain) -- installing/removing a link mid-convergence,
 * exactly when a child is most likely to be about to use it, is apparently
 * a worse trade than the wasted leaf-node wake-up. */
static void
update_relay_rx_shared(uint8_t allow_install)
{
  struct tsch_slotframe *target_sf;
  uint16_t period;
  int shard;
  int num_shards;

  if(tsch_is_coordinator) {
    /* Root-adjacent tier: 1 shard only -- see update_root_adjacent()'s
     * matching comment on why ORCHESTRA_IA_UPLINK_SHARDS isn't applied
     * here. */
    target_sf = sf_root;
    period = ORCHESTRA_IA_ROOT_PERIOD;
    num_shards = 1;
  } else if(sf_short != NULL) {
    target_sf = sf_short;
    period = TSCH_IA_SFS_SIZE;
    num_shards = ORCHESTRA_IA_UPLINK_SHARDS;
  } else {
    target_sf = NULL;
    period = 0;
    num_shards = 0;
  }

  if(target_sf == NULL) {
    if(allow_install) {
      for(shard = 0; shard < ORCHESTRA_IA_UPLINK_SHARDS; shard++) {
        if(l_relay_rx_shared[shard] != NULL) {
          tsch_schedule_remove_link(l_relay_rx_shared_sf, l_relay_rx_shared[shard]);
          l_relay_rx_shared[shard] = NULL;
        }
      }
      l_relay_rx_shared_sf = NULL;
    }
    return;
  }

  if(l_relay_rx_shared_sf != NULL && l_relay_rx_shared_sf != target_sf) {
    /* Coordinator status changed (shouldn't happen after boot, but mirrors
     * update_control_rx()'s defensive handling of the same case). */
    if(!allow_install) {
      return;
    }
    for(shard = 0; shard < ORCHESTRA_IA_UPLINK_SHARDS; shard++) {
      if(l_relay_rx_shared[shard] != NULL) {
        tsch_schedule_remove_link(l_relay_rx_shared_sf, l_relay_rx_shared[shard]);
        l_relay_rx_shared[shard] = NULL;
      }
    }
    l_relay_rx_shared_sf = NULL;
  }

  /* Defensive: tear down any shard beyond num_shards that may have been
   * installed under a different num_shards value (only possible if
   * tsch_is_coordinator itself changed, which "shouldn't happen after
   * boot" per the comment above, but costs nothing to handle). */
  if(allow_install) {
    for(shard = num_shards; shard < ORCHESTRA_IA_UPLINK_SHARDS; shard++) {
      if(l_relay_rx_shared[shard] != NULL) {
        tsch_schedule_remove_link(target_sf, l_relay_rx_shared[shard]);
        l_relay_rx_shared[shard] = NULL;
      }
    }
  }

  for(shard = 0; shard < num_shards; shard++) {
    uint32_t h = ia_hash1_shard(&linkaddr_node_addr, (uint8_t)shard);
    uint16_t ts = ia_slot(h, period);
    uint16_t ch = ia_channel(h);
    if(l_relay_rx_shared[shard] == NULL) {
      if(!allow_install) {
        continue;
      }
      l_relay_rx_shared[shard] = tsch_schedule_add_link(target_sf, LINK_OPTION_RX, LINK_TYPE_NORMAL,
                                                         &tsch_broadcast_address, ts, ch, 1);
      l_relay_rx_shared_sf = target_sf;
    } else {
      l_relay_rx_shared[shard]->timeslot = ts;
      l_relay_rx_shared[shard]->channel_offset = ch;
    }
#if TSCH_WITH_IMPLICIT_ACK
    /* Same live condition as RELAY_TX/RELAY_RX always used (ia_rx_short_
     * timing_ok() doesn't vary per child or per shard, so consolidating
     * doesn't change this decision at all): short timing whenever we're not
     * the coordinator and our own parent is known. */
    if(!tsch_is_coordinator && ia_rx_short_timing_ok()) {
      tsch_ia_link_use_short_timing(l_relay_rx_shared[shard]);
    } else if(l_relay_rx_shared[shard] != NULL) {
      l_relay_rx_shared[shard]->timing_us = NULL;
      l_relay_rx_shared[shard]->timing_ticks = NULL;
    }
#endif /* TSCH_WITH_IMPLICIT_ACK */
  }
}
#if TSCH_WITH_IMPLICIT_ACK
/*---------------------------------------------------------------------------*/
/* Fixed (timeslot=0, channel_offset=0) shared Tx|Rx cell for RPL control
 * traffic (DIS/DIO/DAO/DAO-ACK), living in whichever of sf_short/sf_root is
 * currently our own uplink-style slotframe. Two earlier approaches were
 * tried and abandoned this session: sharing UPLINK's/ROOT_ADJACENT's own
 * hash-derived cell tag (works, but ties control traffic's queue position
 * to whatever's already queued ahead of it there), and a fully separate
 * always-regular slotframe with its own hash-derived cell (broke: its
 * formula was bit-identical to UPLINK's own, HASH2(parent,self)+asfn, so it
 * permanently aliased UPLINK's cell and always lost the tie-break -- see
 * git history). A *fixed* position sidesteps both: it never rotates, so it
 * can never permanently alias a hash-derived cell (at most it occasionally
 * coincides with one that happens to hash to 0, tolerated like any other
 * autonomous collision), and it needs no new slotframe/handle to win a
 * tie-break against.
 *
 * LINK_OPTION_TX|RX together (not just Tx, like UPLINK/ROOT_ADJACENT) since
 * this same fixed slot must also serve as our Rx opportunity for any child's
 * control traffic addressed to us -- unlike RELAY_RX, this isn't scoped to
 * one specific child, so one generic Rx-capable cell covers all of them.
 * Every node's sf_short/sf_root share the same size and ASN phase, so
 * timeslot 0 lands on the same absolute ASN network-wide; LINK_OPTION_SHARED
 * (with its usual backoff) is what keeps that from being a hard collision
 * every single occurrence -- this is the same tradeoff the standard 6TiSCH
 * minimal schedule's own single shared cell already makes, and control
 * traffic (trickle-timer DIO, periodic DAO refresh) is infrequent enough
 * for it to matter little in practice.
 *
 * Fixed at (1,0), not (0,0): default_common's own shared cell (a different,
 * same-size slotframe, handle 2) is unconditionally hardcoded to (0,0) too.
 * tsch_schedule_get_next_active_link()'s tie-break picks purely by
 * link_options/slotframe_handle, decided before either link's queue is even
 * looked at (tsch-schedule.c) -- when both links have LINK_OPTION_TX, the
 * lower slotframe_handle always wins. sf_short's handle (1) is lower than
 * default_common's (2), so an l_control placed at (0,0) would permanently
 * win that tie-break on every non-root-adjacent node (whose l_control lives
 * in sf_short, Tx+Rx) -- not an occasional collision resolved by
 * LINK_OPTION_SHARED backoff like the cross-node case below, but a
 * deterministic, every-single-cycle starvation of default_common, since
 * neither cell ever rotates. Confirmed in testing: a non-root-adjacent
 * node's queued DIOs sat in the TSCH queue forever, 0 ever reaching the
 * radio, silently blocking every node further down the tree from ever
 * hearing a DIO from it. Root-adjacent nodes were never affected -- their
 * l_control lives in sf_root instead, leaving only the Rx-only
 * l_control_rx at sf_short's fixed slot, which *loses* the same tie-break to
 * default_common's Tx-capable cell (Tx beats non-Tx) -- masking the bug
 * until a 2+-hop node's own l_control was exercised. (1,0) has no such
 * structural collision with any other fixed-position cell in this file.
 *
 * Deliberately addressed directly to orchestra_parent_linkaddr/root_linkaddr
 * (not tsch_broadcast_address, unlike default_common's own shared cell):
 * tsch_queue_get_unicast_packet_for_any() -- the mechanism that lets a
 * broadcast-addressed shared cell also serve arbitrary unicast traffic --
 * explicitly skips any neighbor with tx_links_count > 0, and our parent
 * always has at least one (UPLINK or ROOT_ADJACENT), so a broadcast-
 * addressed cell would never find our queued control packet at all. Direct
 * addressing means this cell's Tx side shares the exact same physical
 * per-neighbor queue as UPLINK/ROOT_ADJACENT -- safe only because
 * tsch_queue_get_packet_for_nbr() (tsch-queue.c) now scans past a
 * mismatched head to find and promote a matching-tag packet, rather than
 * only ever checking the literal head position; without that fix this would
 * reintroduce the exact head-of-queue starvation this session already found
 * and fixed once (see git history).
 *
 * Unlike UPLINK/ROOT_ADJACENT, this cell does *not* wait for
 * orchestra_parent_knows_us: that gate exists because the *parent's own*
 * matching Rx cell for us is only installed once our route exists
 * (child_added(), triggered by our DAO's ACK) -- a genuinely one-sided
 * problem for a per-neighbor hash-derived cell. This cell has no such
 * problem: every node installs its own generic (1,0) Rx-capable cell as
 * soon as it merely knows its own parent/root, regardless of whether its
 * children (if any) have DAO'd yet -- so our very first DAO, sent before
 * orchestra_parent_knows_us is even true, can safely ride this cell too,
 * instead of falling back to default_common's much rarer opportunity. */
static void
update_control_link(uint8_t allow_install)
{
  struct tsch_slotframe *target_sf;
  const linkaddr_t *target_addr;

  if(have_root && sf_root != NULL) {
    target_sf = sf_root;
    target_addr = &root_linkaddr;
  } else if(sf_short != NULL && !linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)) {
    target_sf = sf_short;
    target_addr = &orchestra_parent_linkaddr;
  } else {
    target_sf = NULL;
    target_addr = NULL;
  }

  if(target_sf == NULL) {
    if(allow_install && l_control != NULL) {
      tsch_schedule_remove_link(l_control_sf, l_control);
      l_control = NULL;
      l_control_sf = NULL;
    }
    return;
  }

  if(l_control != NULL && l_control_sf != target_sf) {
    /* Our root-adjacency status changed: move to the other slotframe. */
    if(!allow_install) {
      return;
    }
    tsch_schedule_remove_link(l_control_sf, l_control);
    l_control = NULL;
    l_control_sf = NULL;
  }

  if(l_control == NULL) {
    if(!allow_install) {
      return;
    }
    l_control = tsch_schedule_add_link(target_sf, LINK_OPTION_TX | LINK_OPTION_RX | LINK_OPTION_SHARED,
                                        LINK_TYPE_NORMAL, target_addr, 1, 0, 1);
    l_control_sf = target_sf;
  } else if(!linkaddr_cmp(&l_control->addr, target_addr)) {
    linkaddr_copy(&l_control->addr, target_addr);
  }
  /* timeslot/channel_offset are always (1,0) -- never rotates, so nothing
   * else to update here on every ASFN unlike every other cell in this file. */
}
/*---------------------------------------------------------------------------*/
/* update_control_link()'s (1,0) cell above covers *our own* control Tx to
 * *our own* parent, in whichever of sf_short/sf_root that parent expects
 * (matching the parent's own choice, computed the same way on its side).
 * That is also automatically our Rx opportunity for a *child's* control
 * traffic addressed to us, in the common case: any non-root-adjacent node's
 * own l_control lives in sf_short, and every one of its children -- which,
 * structurally, can only ever be non-root-adjacent themselves too, since
 * only root's own direct children are root-adjacent -- also transmits
 * control traffic in sf_short. Same slotframe, same size, same phase:
 * already aligned, no extra cell needed.
 *
 * That symmetry breaks in exactly two cases, both handled here:
 *  - A root-adjacent node's own l_control lives in sf_root (to reach root),
 *    but its children are one hop further out and therefore NOT
 *    root-adjacent -- they transmit their own control cell in sf_short.
 *    Since sf_root (size ORCHESTRA_IA_ROOT_PERIOD) and sf_short (size
 *    TSCH_IA_SFS_SIZE) are different sizes, their (timeslot=1) instants
 *    only coincide once every lcm(size_root, size_short) ASN ticks --
 *    confirmed in testing as a near-total loss of child-to-parent control
 *    delivery once a root-adjacent node's children tried to use it, with
 *    resulting DAO loss visibly corrupting RPL rank/parent selection
 *    network-wide. A root-adjacent node therefore needs a SEPARATE,
 *    additional Rx-only (1,0) cell in sf_short to hear its own children.
 *  - The root itself never runs update_control_link() at all (it has no
 *    parent to send control traffic to -- root_node_updated() returns
 *    early on self), so it has no (1,0) cell in sf_root whatsoever despite
 *    needing one to hear its root-adjacent children's control traffic.
 *
 * Address is tsch_broadcast_address, not a specific neighbor: unlike
 * update_control_link()'s cell (which also transmits, and so needs a real
 * neighbor to look up a Tx queue for), this cell is Rx-only and must accept
 * control traffic from any of our (potentially several) children -- exactly
 * mirroring default_common's own generic-address convention for a cell not
 * scoped to one specific neighbor. allow_install: see update_uplink(). */
static void
update_control_rx(uint8_t allow_install)
{
  struct tsch_slotframe *target_sf = NULL;

  if(tsch_is_coordinator) {
    target_sf = sf_root;
  } else if(have_root) {
    /* Root-adjacent: our own l_control lives in sf_root, but our children
     * (if any) are not root-adjacent and transmit in sf_short instead. */
    target_sf = sf_short;
  }
  /* Deeper (non-root-adjacent) nodes need nothing extra here: their own
   * l_control already lives in sf_short, same as their children's. */

  if(target_sf == NULL) {
    if(allow_install && l_control_rx != NULL) {
      tsch_schedule_remove_link(l_control_rx_sf, l_control_rx);
      l_control_rx = NULL;
      l_control_rx_sf = NULL;
    }
    return;
  }

  if(l_control_rx != NULL && l_control_rx_sf != target_sf) {
    if(!allow_install) {
      return;
    }
    tsch_schedule_remove_link(l_control_rx_sf, l_control_rx);
    l_control_rx = NULL;
    l_control_rx_sf = NULL;
  }

  if(l_control_rx == NULL) {
    if(!allow_install) {
      return;
    }
    l_control_rx = tsch_schedule_add_link(target_sf, LINK_OPTION_RX | LINK_OPTION_SHARED,
                                           LINK_TYPE_NORMAL, &tsch_broadcast_address, 1, 0, 0);
    l_control_rx_sf = target_sf;
  }
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
#if TSCH_WITH_IMPLICIT_ACK
/* Our own SELF_OVERHEAR cell: promiscuous Rx at HASH3(parent, self,
 * grandparent), where we expect to overhear our parent relaying our own
 * earlier frame on to our grandparent. Skipped (and torn down) whenever we
 * don't yet know our grandparent, or our parent is a root (root has no
 * further grandparent, so nothing to overhear -- see update_uplink()).
 * Deliberately *not* also skipped when our grandparent is root: our parent
 * relays our frame via its own dedicated RELAY_TX[us] cell (a HASH3-derived
 * cell separate from our parent's own ROOT_ADJACENT traffic, see
 * update_child_links()), regardless of whether our parent is itself
 * root-adjacent -- only the root and root-adjacent nodes' own uplink
 * traffic needs to stay on regular timing, not every relay they perform for
 * a child one hop further out. allow_install: see update_uplink(); this
 * link is Rx-only so it never touches tx_links_count, but the same
 * slot-operation-safety rule applies to tsch_schedule_add_link()/
 * _remove_link() regardless of link direction. */
static void
update_self_overhear(uint8_t allow_install)
{
  if(sf_short == NULL || !have_grandparent
     || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || tsch_roots_is_root(&orchestra_parent_linkaddr)) {
    if(allow_install) {
      int k;
      if(l_self_overhear != NULL) {
        tsch_schedule_remove_link(sf_short, l_self_overhear);
        l_self_overhear = NULL;
        if(logged_selfoh_pos != 0xffffffffu) {
          logged_selfoh_pos = 0xffffffffu;
          LOG_INFO("IATRACE selfoh removed\n");
        }
      }
      for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
        if(l_self_overhear_extra[k] != NULL) {
          tsch_schedule_remove_link(sf_short, l_self_overhear_extra[k]);
          l_self_overhear_extra[k] = NULL;
        }
        if(logged_selfoh_extra_pos[k] != 0xffffffffu) {
          logged_selfoh_extra_pos[k] = 0xffffffffu;
          LOG_INFO("IATRACE selfoh-extra removed k=%u\n", k + 1);
        }
      }
    }
    return;
  }
  {
    uint32_t h = ia_hash3(&orchestra_parent_linkaddr, &linkaddr_node_addr, &grandparent_linkaddr);
    uint16_t ts = ia_slot(h, TSCH_IA_SFS_SIZE);
    uint16_t ch = ia_channel(h);
    if(l_self_overhear == NULL) {
      if(!allow_install) {
        return;
      }
      l_self_overhear = tsch_schedule_add_link(sf_short, LINK_OPTION_RX | LINK_OPTION_IA_OVERHEAR,
                                                LINK_TYPE_NORMAL, &grandparent_linkaddr, ts, ch, 1);
      tsch_ia_link_use_short_timing(l_self_overhear);
    } else {
      l_self_overhear->timeslot = ts;
      l_self_overhear->channel_offset = ch;
    }
    {
      uint32_t pos = ((uint32_t)ts << 16) | ch;
      if(pos != logged_selfoh_pos) {
        logged_selfoh_pos = pos;
        LOG_INFO("IATRACE selfoh ts=%u ch=%u gp=", ts, ch);
        LOG_INFO_LLADDR(&grandparent_linkaddr);
        LOG_INFO_("\n");
      }
    }
    /* Extra listen positions matching our parent's extra RELAY_TX shards
     * (ORCHESTRA_IA_RELAY_TX_SHARDS > 1) one for one -- same h+k*0x85EBCA6B
     * formula as update_child_links()'s extra shards, so a relay our parent
     * sends on shard k lands exactly on our k-th listen position. Gated on
     * self_has_children(): our parent only installs its own extra shard for
     * us if it has learned (via our EB) that we have descendants of our own
     * (child_has_descendants() in update_child_links()) -- and we know
     * whether that's true directly, without waiting on any EB round trip,
     * since it's just our own children[] -- so this stays in lockstep with
     * our parent's decision using purely local information on both ends. */
    {
      uint8_t k;
      uint8_t want_extra = self_has_children();
      for(k = 1; k < ORCHESTRA_IA_RELAY_TX_SHARDS && k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX; k++) {
        if(!want_extra) {
          if(l_self_overhear_extra[k - 1] != NULL) {
            tsch_schedule_remove_link(sf_short, l_self_overhear_extra[k - 1]);
            l_self_overhear_extra[k - 1] = NULL;
          }
          logged_selfoh_extra_pos[k - 1] = 0xffffffffu;
          continue;
        }
        {
          uint32_t eh = h + (uint32_t)k * 0x85EBCA6Bu;
          uint16_t ets = ia_slot(eh, TSCH_IA_SFS_SIZE);
          uint16_t ech = ia_channel(eh);
          if(l_self_overhear_extra[k - 1] == NULL) {
            if(allow_install) {
              l_self_overhear_extra[k - 1] = tsch_schedule_add_link(sf_short, LINK_OPTION_RX | LINK_OPTION_IA_OVERHEAR,
                                                                     LINK_TYPE_NORMAL, &grandparent_linkaddr, ets, ech, 1);
              if(l_self_overhear_extra[k - 1] != NULL) {
                tsch_ia_link_use_short_timing(l_self_overhear_extra[k - 1]);
              }
            }
          } else {
            l_self_overhear_extra[k - 1]->timeslot = ets;
            l_self_overhear_extra[k - 1]->channel_offset = ech;
          }
          if(l_self_overhear_extra[k - 1] != NULL) {
            uint32_t epos = ((uint32_t)ets << 16) | ech;
            if(epos != logged_selfoh_extra_pos[k - 1]) {
              logged_selfoh_extra_pos[k - 1] = epos;
              LOG_INFO("IATRACE selfoh-extra k=%u ts=%u ch=%u gp=", k, ets, ech);
              LOG_INFO_LLADDR(&grandparent_linkaddr);
              LOG_INFO_("\n");
            }
          }
        }
      }
    }
  }
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
/* Tx cell to relay a direct child's frame onward to our own parent
 * (RELAY_TX) -- only meaningful, and only installed, when our own parent is
 * not itself a root (see update_uplink() and the file header). The Rx side
 * (receiving the child's own uplink transmission in the first place) is no
 * longer handled here at all -- see update_relay_rx_shared(), which
 * replaced what used to be one dedicated RELAY_RX cell per child with one
 * shared cell for all of them. RELAY_TX stays per-child, unlike RELAY_RX:
 * see struct ia_child's comment for why consolidating it would break
 * implicit-ack confirmation. allow_install: see update_uplink(). */
static void
update_child_links(struct ia_child *c, uint8_t allow_install)
{
  if(c == NULL || tsch_is_coordinator || sf_short == NULL) {
    /* Root has no further parent to relay a child's frame onward to -- its
     * children's own uplink/root-adjacent traffic terminates at
     * update_relay_rx_shared()'s cell, nothing further needed here. */
    return;
  }

  /* RELAY_TX: only if our own parent is known and knows about us (same
   * tx_links_count/default_common reasoning as update_uplink()). Installed
   * even when our own parent IS root (root-adjacent): relaying a child's
   * frame onward is a separate concern from our own uplink traffic to that
   * same parent -- root just needs to receive the relayed frame, not ack
   * it, so a root-adjacent node's RELAY_TX[child] is exactly as valid (and,
   * per ia_rx_short_timing_ok(), exactly as implicit-ack-eligible) as any
   * deeper relay's. Forcing this to fall back to our own ROOT_ADJACENT cell
   * instead (by disallowing RELAY_TX here) was an earlier, overly
   * conservative choice that made every 2-hop node's traffic use full
   * explicit ack regardless of tree depth below it -- see
   * ia_tx_short_timing_ok()'s comment.
   *
   * This cannot live in sf_short alongside our own UPLINK: both are Tx cells
   * addressed to the same destination (our parent), and tsch_queue_get_
   * packet_for_nbr() only ever peeks the *head* of that neighbor's single
   * shared tx_ringbuf, filtering solely on PACKETBUF_ATTR_TSCH_SLOTFRAME/
   * _TIMESLOT (tsch-queue.c). Since select_packet() deliberately leaves
   * *timeslot unpinned (0xffff, see its own long comment on why), a packet
   * tagged with sf_short's handle is eligible for *any* sf_short cell to
   * that neighbor -- meaning whichever of UPLINK or RELAY_TX happens to
   * occur first in a given ASFN silently grabs the head-of-queue packet,
   * regardless of which one it was actually enqueued for. A relayed child
   * frame that goes out over our own UPLINK cell instead of this child's
   * RELAY_TX cell is invisible to that child's SELF_OVERHEAR (which listens
   * at RELAY_TX's specific, HASH3-derived slot) -- confirmed in testing via
   * an armed/deadline/overhear ASN trace showing genuine overhears landing
   * long after (and independent of) the implicit-ack deadline, i.e. the
   * relay was going out on the wrong cell entirely most of the time. Giving
   * each child's RELAY_TX its own dedicated slotframe (still unpinned on
   * timeslot/channel within it) makes the two Tx roles mutually exclusive
   * via PACKETBUF_ATTR_TSCH_SLOTFRAME alone, with no reintroduction of the
   * enqueue-time (timeslot, channel) staleness bug this design avoids. */
  if(!linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     && orchestra_parent_knows_us) {
    uint32_t h = ia_hash3(&linkaddr_node_addr, &c->addr, &orchestra_parent_linkaddr);
    uint16_t ts = ia_slot(h, TSCH_IA_SFS_SIZE);
    uint16_t ch = ia_channel(h);
    if(c->relay_tx_sf == NULL) {
      if(!allow_install) {
        return;
      }
      c->relay_tx_sf = tsch_schedule_add_slotframe((slotframe_handle | 0x2000) + (uint16_t)(c - children),
                                                    TSCH_IA_SFS_SIZE);
      if(c->relay_tx_sf == NULL) {
        LOG_ERR("ia: failed to add relay_tx slotframe for ");
        LOG_ERR_LLADDR(&c->addr);
        LOG_ERR_("\n");
        return;
      }
    }
    if(c->relay_tx == NULL) {
      if(allow_install) {
        c->relay_tx = tsch_schedule_add_link(c->relay_tx_sf, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                              LINK_TYPE_NORMAL, &orchestra_parent_linkaddr, ts, ch, 1);
      }
    } else {
      c->relay_tx->timeslot = ts;
      c->relay_tx->channel_offset = ch;
    }
    if(c->relay_tx != NULL) {
      uint32_t pos = ((uint32_t)ts << 16) | ch;
      if(pos != c->logged_tx_pos) {
        c->logged_tx_pos = pos;
        LOG_INFO("IATRACE relaytx ts=%u ch=%u child=", ts, ch);
        LOG_INFO_LLADDR(&c->addr);
        LOG_INFO_(" gp=");
        LOG_INFO_LLADDR(&orchestra_parent_linkaddr);
        LOG_INFO_("\n");
      }
    }
#if TSCH_WITH_IMPLICIT_ACK
    /* NOT ia_tx_short_timing_ok() (our own eligibility) -- this cell's
     * timing must match child c's own eligibility, the exact same fact
     * RELAY_RX[c] above uses, since c's SELF_OVERHEAR is what's actually
     * listening for this transmission. See ia_rx_short_timing_ok()'s long
     * comment for why conflating the two silently breaks implicit ack for
     * any child whose relaying parent (us) doesn't itself qualify. */
    if(ia_rx_short_timing_ok()) {
      tsch_ia_link_use_short_timing(c->relay_tx);
    } else if(c->relay_tx != NULL) {
      c->relay_tx->timing_us = NULL;
      c->relay_tx->timing_ticks = NULL;
    }
#endif /* TSCH_WITH_IMPLICIT_ACK */
    /* Extra RELAY_TX shards (ORCHESTRA_IA_RELAY_TX_SHARDS > 1, see its own
     * comment in orchestra-conf.h): additional, simultaneously-active
     * positions in the same relay_tx_sf, each at h+k*0x85EBCA6B mod
     * SFS_SIZE/channels -- same spreading-constant convention as
     * uplink_shard_for()'s epoch term, chosen to decorrelate shard k's
     * position from shard 0's rather than clustering nearby. Any packet
     * tagged for this slotframe can be served by *any* link inside it
     * (matching is by slotframe handle alone, per this function's own
     * comment on PACKETBUF_ATTR_TSCH_SLOTFRAME) -- no selection logic is
     * needed on the sending side at all; TSCH's own scheduler just serves
     * whichever shard's timeslot comes up next.
     *
     * Gated on child_has_descendants(): a leaf child's RELAY_TX only ever
     * carries that one child's own traffic (no subtree to combine), so a
     * second shard there is a permanently-static extra position added to
     * the network-wide hash address space for zero throughput benefit --
     * see this file's own child_has_descendants()/self_has_children()
     * comment for the full reasoning and why both ends of the relationship
     * reach the same conclusion independently. */
    {
      uint8_t k;
      uint8_t want_extra = child_has_descendants(c);
      for(k = 1; k < ORCHESTRA_IA_RELAY_TX_SHARDS && k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX; k++) {
        if(!want_extra) {
          if(c->relay_tx_extra[k - 1] != NULL) {
            tsch_schedule_remove_link(c->relay_tx_sf, c->relay_tx_extra[k - 1]);
            c->relay_tx_extra[k - 1] = NULL;
          }
          c->logged_tx_extra_pos[k - 1] = 0xffffffffu;
          continue;
        }
        {
          uint32_t eh = h + (uint32_t)k * 0x85EBCA6Bu;
          uint16_t ets = ia_slot(eh, TSCH_IA_SFS_SIZE);
          uint16_t ech = ia_channel(eh);
          if(c->relay_tx_extra[k - 1] == NULL) {
            if(allow_install) {
              c->relay_tx_extra[k - 1] = tsch_schedule_add_link(c->relay_tx_sf, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                                                 LINK_TYPE_NORMAL, &orchestra_parent_linkaddr, ets, ech, 1);
            }
          } else {
            c->relay_tx_extra[k - 1]->timeslot = ets;
            c->relay_tx_extra[k - 1]->channel_offset = ech;
          }
          if(c->relay_tx_extra[k - 1] != NULL) {
            uint32_t epos = ((uint32_t)ets << 16) | ech;
            if(epos != c->logged_tx_extra_pos[k - 1]) {
              c->logged_tx_extra_pos[k - 1] = epos;
              LOG_INFO("IATRACE relaytx-extra k=%u ts=%u ch=%u child=", k, ets, ech);
              LOG_INFO_LLADDR(&c->addr);
              LOG_INFO_("\n");
            }
          }
#if TSCH_WITH_IMPLICIT_ACK
          if(c->relay_tx_extra[k - 1] != NULL) {
            if(ia_rx_short_timing_ok()) {
              tsch_ia_link_use_short_timing(c->relay_tx_extra[k - 1]);
            } else {
              c->relay_tx_extra[k - 1]->timing_us = NULL;
              c->relay_tx_extra[k - 1]->timing_ticks = NULL;
            }
          }
#endif /* TSCH_WITH_IMPLICIT_ACK */
        }
      }
    }
  } else if(allow_install && c->relay_tx_sf != NULL) {
    /* tsch_schedule_remove_slotframe() removes every link inside it too. */
    int k;
    tsch_schedule_remove_slotframe(c->relay_tx_sf);
    c->relay_tx_sf = NULL;
    c->relay_tx = NULL;
    for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
      c->relay_tx_extra[k] = NULL;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* find_grandchild()/alloc_grandchild(): guarded by TSCH_WITH_IMPLICIT_ACK
 * since their only caller, orchestra_ia_child_eb_input(), lives entirely
 * inside that same guard further below -- without this, a build with
 * TSCH_WITH_IMPLICIT_ACK left at its default (0) leaves these two genuinely
 * unused, tripping -Werror=unused-function. */
#if TSCH_WITH_IMPLICIT_ACK
static struct ia_grandchild *
find_grandchild(struct ia_child *c, const linkaddr_t *addr)
{
  int i;
  if(c == NULL || addr == NULL || linkaddr_cmp(addr, &linkaddr_null)) {
    return NULL;
  }
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(c->grandchildren[i].in_use && linkaddr_cmp(&c->grandchildren[i].addr, addr)) {
      return &c->grandchildren[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
static struct ia_grandchild *
alloc_grandchild(struct ia_child *c, const linkaddr_t *addr)
{
  int i;
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(!c->grandchildren[i].in_use) {
      int k;
      c->grandchildren[i].in_use = 1;
      linkaddr_copy(&c->grandchildren[i].addr, addr);
      /* Fail-safe default: assume the grandchild *does* have descendants
       * (install the extra Rx position) until an actual EB from our own
       * child explicitly says otherwise (orchestra_ia_child_eb_input() sets
       * this from ie_ia_children_has_descendants on every EB). Defaulting
       * to 0 instead was measured to regress delivery at 60/100-node scale
       * relative to the unconditional-install version this whole mechanism
       * replaced: under the same EB loss/delay that already drives this
       * scheme's RPL churn at that scale, a grandchild that genuinely has
       * descendants could sit with has_descendants still unset for a real
       * stretch of time, during which we'd silently under-provision exactly
       * the way the original missing-loop bug did -- just gated by unlucky
       * signaling timing instead of a missing code path. A wrong "assume
       * yes" here only costs one briefly-unused extra Rx cell; a wrong
       * "assume no" costs silently dropped packets. */
      c->grandchildren[i].has_descendants = 1;
      c->grandchildren[i].relay_rx = NULL;
      c->grandchildren[i].logged_rx_pos = 0xffffffffu;
      for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
        c->grandchildren[i].relay_rx_extra[k] = NULL;
        c->grandchildren[i].logged_rx_extra_pos[k] = 0xffffffffu;
      }
      return &c->grandchildren[i];
    }
  }
  LOG_ERR("ia: no free grandchild slot for ");
  LOG_ERR_LLADDR(addr);
  LOG_ERR_("\n");
  return NULL;
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
static void
remove_grandchild(struct ia_child *c, struct ia_grandchild *gc, uint8_t allow_install)
{
  int k;
  if(allow_install && gc->relay_rx != NULL) {
    tsch_schedule_remove_link(sf_short, gc->relay_rx);
  }
  gc->relay_rx = NULL;
  for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
    if(allow_install && gc->relay_rx_extra[k] != NULL) {
      tsch_schedule_remove_link(sf_short, gc->relay_rx_extra[k]);
    }
    gc->relay_rx_extra[k] = NULL;
  }
  gc->in_use = 0;
}
/*---------------------------------------------------------------------------*/
/* Normal (non-overhear) Rx cell matching a grandchild's relayed frame: our
 * direct child c actually transmits it at HASH3(c, grandchild, self) (its
 * own RELAY_TX[grandchild] cell), so we must listen there with the exact
 * same hash inputs, in the same order, to land on the same (timeslot,
 * channel) -- see ia_hash3()'s callers in update_child_links() (the
 * transmitting side) for the formula this must mirror. Addressed to c (the
 * actual sender), not the grandchild (which never transmits to us
 * directly). allow_install: see update_uplink(); only ever called from
 * orchestra_ia_child_eb_input() (safe, non-slot-operation context) or the
 * ASFN callback (allow_install=0, mutate-only). */
static void
update_grandchild_relay_rx(struct ia_child *c, struct ia_grandchild *gc, uint8_t allow_install)
{
  uint32_t h = ia_hash3(&c->addr, &gc->addr, &linkaddr_node_addr);
  uint16_t ts = ia_slot(h, TSCH_IA_SFS_SIZE);
  uint16_t ch = ia_channel(h);
  if(sf_short == NULL) {
    return;
  }
  /* Deliberately *not* also excluded when tsch_is_coordinator (root): root
   * creates sf_short unconditionally in init() just like everyone else, and
   * needs this cell exactly as much as any other grandparent -- a
   * root-adjacent child's RELAY_TX[grandchild] cell (update_child_links())
   * lives in its own dedicated, HASH3-derived relay_tx_sf, a completely
   * different slotframe and position from that same child's own
   * ROOT_ADJACENT/relay_rx cell (which root already listens for via
   * update_child_links()'s own tsch_is_coordinator branch, in sf_root).
   * Without this cell, root has literally no Rx opportunity positioned to
   * ever hear a root-adjacent child's relayed grandchild traffic at all --
   * confirmed in testing as radio-level transmissions that succeeded (a
   * clean RADIO_TX_OK every single retry, channel-hopping normally) yet
   * were never once received at the application layer, because root was
   * simply never listening at that cell's position. */
  if(gc->relay_rx == NULL) {
    if(!allow_install) {
      return;
    }
    gc->relay_rx = tsch_schedule_add_link(sf_short, LINK_OPTION_RX, LINK_TYPE_NORMAL,
                                           &c->addr, ts, ch, 1);
  } else {
    gc->relay_rx->timeslot = ts;
    gc->relay_rx->channel_offset = ch;
  }
  if(gc->relay_rx != NULL) {
    uint32_t pos = ((uint32_t)ts << 16) | ch;
    if(pos != gc->logged_rx_pos) {
      gc->logged_rx_pos = pos;
      LOG_INFO("IATRACE gcrx ts=%u ch=%u relayer=", ts, ch);
      LOG_INFO_LLADDR(&c->addr);
      LOG_INFO_(" grandchild=");
      LOG_INFO_LLADDR(&gc->addr);
      LOG_INFO_("\n");
    }
  }
#if TSCH_WITH_IMPLICIT_ACK
  /* NOT ia_rx_short_timing_ok() -- this cell's timing must match the
   * *grandchild's* own eligibility (see ia_grandchild_short_timing_ok()'s
   * comment), not "is our own parent also not root", which is what
   * ia_rx_short_timing_ok() additionally (and wrongly, for this cell)
   * requires. */
  if(ia_grandchild_short_timing_ok()) {
    tsch_ia_link_use_short_timing(gc->relay_rx);
  } else if(gc->relay_rx != NULL) {
    gc->relay_rx->timing_us = NULL;
    gc->relay_rx->timing_ticks = NULL;
  }
#endif /* TSCH_WITH_IMPLICIT_ACK */
  /* Extra Rx positions matching child c's own extra RELAY_TX shards
   * (ORCHESTRA_IA_RELAY_TX_SHARDS > 1) one for one -- same h+k*0x85EBCA6B
   * formula as update_child_links()'s extra shards and update_self_overhear()'s
   * matching listen positions. This loop was missing entirely until this
   * comment was added (see struct ia_grandchild's own comment for the
   * silent-data-loss bug that caused): c's own SELF_OVERHEAR already listens
   * at every shard, but without this loop we never did, so any relay c
   * actually sent on shard k>=1 vanished here -- received by nobody -- while
   * c's own overhear-based confirmation, watching a *different* cell, still
   * (correctly, from its own vantage point) saw the transmission succeed and
   * confirmed it.
   *
   * Gating this on gc->has_descendants (signaled by c, our child, via
   * ie_ia_children_has_descendants -- see that field's own comment, and
   * child_has_descendants()'s use in update_child_links() for the matching
   * Tx-side decision) was tried: measured as a large, clean win at 25/49
   * nodes (PDR 54.1%->54.8%/37.7%->68.8%) but a real regression at 60/100
   * (47.9%->43.4%/25.7%->15.1%) relative to installing unconditionally --
   * and unaffected by which way has_descendants defaults before the first
   * authoritative EB arrives (checked: identical result either way), so the
   * regression isn't a signaling-timing artifact, just a real, scale-
   * dependent cost of the install/remove churn this gating adds whenever a
   * grandchild's reported status changes. Left ungated (always install)
   * here as the more robust default across scales; gc->has_descendants
   * itself is still tracked and kept in sync via the EB in case a future,
   * more targeted use for it (e.g. gating only on a stable/settled value
   * rather than every EB) is worth revisiting. */
  if(1) {
    uint8_t k;
    for(k = 1; k < ORCHESTRA_IA_RELAY_TX_SHARDS && k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX; k++) {
      uint32_t eh = h + (uint32_t)k * 0x85EBCA6Bu;
      uint16_t ets = ia_slot(eh, TSCH_IA_SFS_SIZE);
      uint16_t ech = ia_channel(eh);
      if(gc->relay_rx_extra[k - 1] == NULL) {
        if(allow_install) {
          gc->relay_rx_extra[k - 1] = tsch_schedule_add_link(sf_short, LINK_OPTION_RX, LINK_TYPE_NORMAL,
                                                              &c->addr, ets, ech, 1);
        }
      } else {
        gc->relay_rx_extra[k - 1]->timeslot = ets;
        gc->relay_rx_extra[k - 1]->channel_offset = ech;
      }
      if(gc->relay_rx_extra[k - 1] != NULL) {
        uint32_t epos = ((uint32_t)ets << 16) | ech;
        if(epos != gc->logged_rx_extra_pos[k - 1]) {
          gc->logged_rx_extra_pos[k - 1] = epos;
          LOG_INFO("IATRACE gcrx-extra k=%u ts=%u ch=%u relayer=", k, ets, ech);
          LOG_INFO_LLADDR(&c->addr);
          LOG_INFO_(" grandchild=");
          LOG_INFO_LLADDR(&gc->addr);
          LOG_INFO_("\n");
        }
      }
#if TSCH_WITH_IMPLICIT_ACK
      if(gc->relay_rx_extra[k - 1] != NULL) {
        if(ia_grandchild_short_timing_ok()) {
          tsch_ia_link_use_short_timing(gc->relay_rx_extra[k - 1]);
        } else {
          gc->relay_rx_extra[k - 1]->timing_us = NULL;
          gc->relay_rx_extra[k - 1]->timing_ticks = NULL;
        }
      }
#endif /* TSCH_WITH_IMPLICIT_ACK */
    }
  } else {
    uint8_t k;
    for(k = 1; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX; k++) {
      if(gc->relay_rx_extra[k - 1] != NULL) {
        if(allow_install) {
          tsch_schedule_remove_link(sf_short, gc->relay_rx_extra[k - 1]);
        }
        gc->relay_rx_extra[k - 1] = NULL;
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
static void
child_added(const linkaddr_t *addr)
{
  struct ia_child *c = find_child(addr);
  if(c == NULL) {
    c = alloc_child(addr);
  }
  update_child_links(c, 1);
#if TSCH_WITH_IMPLICIT_ACK
  /* Gaining our first child flips self_has_children() from false to true --
   * re-evaluate whether we should now also listen at the extra
   * SELF_OVERHEAR position(s) our own parent may start using for us (see
   * update_self_overhear()'s own comment). A no-op if we already had at
   * least one child. */
  update_self_overhear(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
}
/*---------------------------------------------------------------------------*/
static void
child_removed(const linkaddr_t *addr)
{
  struct ia_child *c = find_child(addr);
  if(c == NULL) {
    return;
  }
  if(c->relay_tx_sf != NULL) {
    int k;
    tsch_schedule_remove_slotframe(c->relay_tx_sf);
    c->relay_tx_sf = NULL;
    c->relay_tx = NULL;
    for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
      c->relay_tx_extra[k] = NULL;
    }
  }
  {
    int i;
    for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
      if(c->grandchildren[i].in_use) {
        remove_grandchild(c, &c->grandchildren[i], 1);
      }
    }
  }
  tsch_queue_free_packets_to(addr);
  c->in_use = 0;
#if TSCH_WITH_IMPLICIT_ACK
  /* Mirror of child_added()'s call: losing our last child flips
   * self_has_children() from true to false, so tear down our own extra
   * SELF_OVERHEAR listen position(s) if we no longer have any children to
   * justify them. Must run after c->in_use = 0 above so self_has_children()
   * sees the post-removal state. */
  update_self_overhear(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
}
/*---------------------------------------------------------------------------*/
static void
reconcile_parent_address(const linkaddr_t *new_addr)
{
  if(!linkaddr_cmp(new_addr != NULL ? new_addr : &linkaddr_null, &orchestra_parent_linkaddr)) {
    const linkaddr_t *old_addr = &orchestra_parent_linkaddr;
    int i;

    /* UPLINK and every child's RELAY_TX are bound to the old parent's address;
     * tear them down before switching (their formulas depend on the parent). */
    if(l_uplink != NULL) {
      tsch_schedule_remove_link(sf_short, l_uplink);
      l_uplink = NULL;
    }
    for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
      if(children[i].in_use && children[i].relay_tx_sf != NULL) {
        int k;
        tsch_schedule_remove_slotframe(children[i].relay_tx_sf);
        children[i].relay_tx_sf = NULL;
        children[i].relay_tx = NULL;
        if(children[i].logged_tx_pos != 0xffffffffu) {
          children[i].logged_tx_pos = 0xffffffffu;
          LOG_INFO("IATRACE relaytx removed child=");
          LOG_INFO_LLADDR(&children[i].addr);
          LOG_INFO_("\n");
        }
        for(k = 0; k < ORCHESTRA_IA_RELAY_TX_SHARDS_MAX - 1; k++) {
          children[i].relay_tx_extra[k] = NULL;
          if(children[i].logged_tx_extra_pos[k] != 0xffffffffu) {
            children[i].logged_tx_extra_pos[k] = 0xffffffffu;
            LOG_INFO("IATRACE relaytx-extra removed k=%u child=", k + 1);
            LOG_INFO_LLADDR(&children[i].addr);
            LOG_INFO_("\n");
          }
        }
      }
    }
    if(old_addr != NULL) {
      tsch_queue_free_packets_to(old_addr);
    }

#if TSCH_WITH_IMPLICIT_ACK
    /* Our old grandparent info was learned from our OLD parent's EB; it's
     * meaningless for a new parent until its own EB tells us otherwise. */
    have_grandparent = 0;
    if(l_self_overhear != NULL) {
      tsch_schedule_remove_link(sf_short, l_self_overhear);
      l_self_overhear = NULL;
      if(logged_selfoh_pos != 0xffffffffu) {
        logged_selfoh_pos = 0xffffffffu;
        LOG_INFO("IATRACE selfoh removed\n");
      }
    }
#endif /* TSCH_WITH_IMPLICIT_ACK */

    if(new_addr != NULL) {
      linkaddr_copy(&orchestra_parent_linkaddr, new_addr);
    } else {
      linkaddr_copy(&orchestra_parent_linkaddr, &linkaddr_null);
    }

    update_uplink(1);
#if TSCH_WITH_IMPLICIT_ACK
    update_control_link(1);
    update_control_rx(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
    for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
      if(children[i].in_use) {
        update_child_links(&children[i], 1);
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_NEW_TIME_SOURCE: thin wrapper resolving the neighbor
 * pointers TSCH gives us down to addresses, then handing off to
 * reconcile_parent_address() -- the actual teardown/rebuild logic, shared
 * with check_parent_knows_us()'s own address-only reconciliation below (see
 * that function's comment for why a *second* caller of this logic exists:
 * TSCH's resynchronize() can silently move the time source to a neighbor
 * RPL never chose, entirely bypassing this callback's own only-fires-on-
 * pointer-change guard, since it still goes through tsch_queue_update_time_
 * source() -- meaning this callback *does* fire, just with a "new" that
 * disagrees with RPL, and reconcile_parent_address() correctly hashes every
 * cell against that phantom neighbor because it has no way to know any
 * better from here alone. */
static void
new_time_source(const struct tsch_neighbor *old, const struct tsch_neighbor *new)
{
  if(new != old) {
    reconcile_parent_address(tsch_queue_get_nbr_address(new));
  }
}
/*---------------------------------------------------------------------------*/
static void
root_node_updated(const linkaddr_t *root, uint8_t is_added)
{
  if(linkaddr_cmp(root, &linkaddr_node_addr)) {
    /* Our own root status changed. We have no parent to send ROOT_ADJACENT
     * traffic to either way; the Rx side for our children now lives in
     * update_relay_rx_shared() (one shared cell, in sf_root once we're
     * coordinator), not per child. We still need our own control-Rx cell
     * here, though (update_control_rx() also checks tsch_is_coordinator,
     * redundantly with is_added here, but correctly): init()'s own
     * tsch_is_coordinator check fires too early to see this node as root,
     * so this callback, firing once root status is authoritatively known,
     * is the most immediate reliable place to install both (check_parent_
     * knows_us()'s periodic recheck is the fallback within IA_BOOTSTRAP_
     * CHECK_PERIOD if this callback's own timing is ever missed). */
    update_relay_rx_shared(1);
#if TSCH_WITH_IMPLICIT_ACK
    update_control_rx(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
    return;
  }
  if(sf_root == NULL) {
    return;
  }
  if(is_added) {
    linkaddr_copy(&root_linkaddr, root);
    have_root = 1;
    update_root_adjacent(1);
#if TSCH_WITH_IMPLICIT_ACK
    update_control_link(1);
    update_control_rx(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
  } else {
    have_root = 0;
    if(l_root_adjacent != NULL) {
      tsch_schedule_remove_link(sf_root, l_root_adjacent);
      l_root_adjacent = NULL;
    }
    tsch_queue_free_packets_to(root);
#if TSCH_WITH_IMPLICIT_ACK
    update_control_link(1);
    update_control_rx(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
  }
}
/*---------------------------------------------------------------------------*/
static int
select_packet(uint16_t *slotframe, uint16_t *timeslot, uint16_t *channel_offset)
{
  const linkaddr_t *dest = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);

  if(packetbuf_attr(PACKETBUF_ATTR_FRAME_TYPE) != FRAME802154_DATAFRAME || dest == NULL) {
    return 0;
  }

#if TSCH_WITH_IMPLICIT_ACK
  if((ia_is_rpl_control_packet() || ia_is_keepalive_packet())
     && l_control != NULL && l_control_sf != NULL
     && linkaddr_cmp(dest, &l_control->addr)) {
    /* RPL control traffic (DIS/DIO/DAO/DAO-ACK) *and* TSCH's own keep-alives
     * (see ia_is_keepalive_packet()'s comment -- same resync-starvation
     * hazard, same fix) unicast to our own next hop go out on the fixed
     * (1,0) shared cell -- see update_control_link()'s long comment. Checked
     * *before* the orchestra_parent_knows_us gate below and before the
     * root/parent branches further down: this cell doesn't need
     * orchestra_parent_knows_us (unlike UPLINK/ROOT_ADJACENT, see
     * update_control_link()), so even our very first DAO (or a KA sent
     * before our first DAO is ACKed) can ride it, and it would otherwise
     * also match the root/parent destination checks below and get
     * mis-tagged onto UPLINK's/ROOT_ADJACENT's own rotating cell instead.
     * timeslot/channel_offset are read directly off l_control (not a literal
     * constant) -- unlike every other branch in this function, reading the
     * live link fields here is correct (not a frozen-snapshot bug) precisely
     * because this cell's position never rotates. */
    if(slotframe != NULL) {
      *slotframe = l_control_sf->handle;
    }
    if(timeslot != NULL) {
      *timeslot = l_control->timeslot;
    }
    if(channel_offset != NULL) {
      *channel_offset = l_control->channel_offset;
    }
    return 1;
  }
#endif /* TSCH_WITH_IMPLICIT_ACK */

  if(!orchestra_parent_knows_us) {
    /* Our parent hasn't yet ACKed a DAO from us, which means it can't yet
     * have processed our route and installed its own matching Rx cell for
     * us (child_added() on its side is triggered by exactly that route
     * being created) -- our dedicated cells are one-sided until then. Fall
     * back to the always-mutual default_common slotframe so anything not
     * already claimed above can still get through; this mirrors the
     * identical bootstrap gate in orchestra-rule-unicast-per-neighbor-rpl-
     * storing.c's neighbor_has_uc_link() for the sender-based case. */
    return 0;
  }

  if(!linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     && linkaddr_cmp(dest, &orchestra_parent_linkaddr)) {
    /* Going to our own next hop (our parent, or root itself if we're
     * root-adjacent -- orchestra_parent_linkaddr is already whichever
     * applies): self-originated (UPLINK/ROOT_ADJACENT), or a relay of a
     * specific child's frame (RELAY_TX)? last_rx_source, set by
     * orchestra_ia_data_input() just before this same frame was forwarded,
     * tells us which -- consume it once so a later, unrelated send (e.g. an
     * app-generated packet) never gets mis-attributed to a stale relay.
     *
     * The relay_child check below must run *before* deciding between
     * sf_root/sf_short, not after: for a root-adjacent node, dest (our
     * parent) and root are the exact same address, so tsch_roots_is_root()
     * matches for both our own uplink AND anything we're relaying for a
     * child -- checking it first (as an earlier version of this function
     * did) unconditionally routed every relayed child frame onto our own
     * ROOT_ADJACENT/control cell instead of that child's RELAY_TX, since it
     * returned before ever consulting last_rx_source/relay_child. Confirmed
     * in testing: a root-adjacent node's RELAY_TX[child] cell existed in the
     * schedule but never had a single packet enqueued for it (0 "enqueued"
     * log lines for its slotframe tag, ever), and the child's own overhear
     * confirmation rate was consequently 0% despite normal delivery via
     * plain retry -- the relay was silently always going out over
     * ROOT_ADJACENT instead of the cell the child was actually listening on.
     *
     * *timeslot and *channel_offset are deliberately left unset (0xffff) for
     * both branches below, rather than pinned to relay_tx's/l_uplink's
     * current fields. Pinning them here was a real bug: TSCH_WITH_LINK_
     * SELECTOR freezes whatever numeric (timeslot, channel_offset) is
     * passed here permanently into the packet's queuebuf (PACKETBUF_ATTR_
     * TSCH_TIMESLOT/_CHANNEL_OFFSET), and tsch_queue_get_packet_for_nbr()/
     * tsch_get_channel_offset() (tsch-queue.c, tsch-slot-operation.c) use
     * that frozen snapshot, unconditionally, for every future retransmission
     * attempt -- they only fall back to the link's live field when the tag
     * is exactly 0xffff. But relay_tx/l_uplink's timeslot and channel_offset
     * are mutated every single ASFN tick (orchestra_ia_new_asfn(), ~170ms).
     * A packet queued at one ASFN keeps trying to send on that ASFN's
     * (timeslot, channel) long after the live cell has rotated elsewhere:
     * confirmed in testing by a packet's logged retries showing an identical
     * channel_offset across many seconds (frozen) while a live-value trace
     * showed the same link's channel_offset cycling every ~170ms the whole
     * time, and by retries only recurring roughly once per full SF(S)size-
     * epoch cycle (~2.9s at the default size 17) -- i.e. only when the hash
     * sequence happened to wrap back to the frozen value. Leaving these
     * unpinned means tsch_queue_get_packet_for_nbr()'s per-link check always
     * passes (packet_attr_timeslot == 0xffff short-circuits the comparison)
     * and tsch_get_channel_offset() always falls back to the link's live,
     * current channel_offset -- so every transmission attempt, on whichever
     * cell occurrence actually runs it, uses that cell's real current
     * position instead of a stale enqueue-time snapshot. *slotframe is still
     * pinned since slotframe handles don't rotate and this keeps the packet
     * correctly scoped to the right Tx role -- sf_short for our own UPLINK,
     * or this specific child's own relay_tx_sf for a relay (a *different*
     * slotframe handle per child, and different again from sf_short/UPLINK
     * -- see update_child_links()'s long comment on why the two Tx roles to
     * the same destination cannot share a slotframe despite both being
     * "sf_short-sized": tsch_queue_get_packet_for_nbr() only ever peeks the
     * head of the shared per-neighbor queue, filtered by slotframe handle,
     * so an unpinned-timeslot packet tagged for the wrong Tx role's
     * slotframe would silently go out over whichever cell -- UPLINK or a
     * relay -- happens to occur first, missing the specific child's
     * SELF_OVERHEAR slot entirely). */
    struct ia_child *relay_child = find_child(&last_rx_source);
    LOG_INFO("IATRACE select_packet to parent, last_rx_source=");
    LOG_INFO_LLADDR(&last_rx_source);
    LOG_INFO_(" relay_child=%p relay_tx=%p\n",
              (void *)relay_child, relay_child != NULL ? (void *)relay_child->relay_tx : NULL);
    linkaddr_copy(&last_rx_source, &linkaddr_null);

    if(relay_child != NULL && relay_child->relay_tx != NULL) {
      if(slotframe != NULL) {
        *slotframe = relay_child->relay_tx_sf->handle;
      }
      LOG_INFO("IATRACE select_packet RELAY branch, tagging slotframe=%u\n",
               relay_child->relay_tx_sf->handle);
      return 1;
    }
    /* Not a relay: this is our own uplink traffic. Root-adjacent (our own
     * parent IS root) uses sf_root/ROOT_ADJACENT; everyone else uses
     * sf_short/UPLINK. */
    if(tsch_roots_is_root(dest)) {
      if(have_root && l_root_adjacent != NULL) {
        if(slotframe != NULL) {
          *slotframe = sf_root->handle;
        }
        return 1;
      }
      return 0;
    }
    if(l_uplink != NULL) {
      if(slotframe != NULL) {
        *slotframe = sf_short->handle;
      }
      return 1;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static void
check_parent_knows_us(void *ptr)
{
  /* Unlike the cells below, update_relay_rx_shared() doesn't depend on
   * orchestra_parent_knows_us at all -- checked unconditionally, every tick,
   * so it gets installed shortly after boot regardless of node role, and
   * so a root's own tsch_is_coordinator transition (too early to observe at
   * this rule's own init(), see root_node_updated()'s comment) is picked up
   * here too if root_node_updated() doesn't fire first. Idempotent when
   * nothing has changed. */
  update_relay_rx_shared(1);
  /* Safety net against tsch.c's resynchronize(): when a node loses TSCH-level
   * sync with its current time source, that function grabs "the last
   * neighbor we received an EB from" as an emergency new time source --
   * entirely independent of RPL, and *not* through tsch_rpl_callback_
   * parent_switch(). It still flows through tsch_queue_update_time_source()
   * and this rule's own new_time_source(), which unconditionally treats any
   * time-source change as a parent change and recomputes/reinstalls every
   * cell keyed off orchestra_parent_linkaddr accordingly -- silently
   * hashing every one of this node's own cells (UPLINK, RELAY_TX, SELF_
   * OVERHEAR) against a neighbor RPL never chose and has no record of.
   * Confirmed at 100-node scale: 57 of 66 logged "re-synchronizing on"
   * events picked an address different from the node's own current RPL
   * preferred parent at that moment -- and it doesn't self-heal, since
   * RPL's own dag->preferred_parent pointer never actually changed, so
   * rpl_set_preferred_parent()'s unconditional early-return (dag->
   * preferred_parent == p) means the one callback that would otherwise fire
   * again and correct it never does; the node's cells stay hashed against
   * the phantom neighbor for the rest of the run. Verified concretely: a
   * child's SELF_OVERHEAR position recomputed via HASH3(phantom_parent,
   * self, grandparent) exactly, rather than HASH3(real_rpl_parent, self,
   * grandparent) -- the two disagree in both timeslot and channel, and the
   * real parent's own RELAY_TX naturally still uses its own correct
   * address, so the two sides of what's supposed to be one cell relationship
   * silently diverge with no error, no timeout, and no self-correction.
   *
   * Checked every IA_BOOTSTRAP_CHECK_PERIOD (4/s, same timer as the
   * bootstrap check above): if orchestra_parent_linkaddr has drifted from
   * RPL's own preferred parent, correct *only* our own address bookkeeping
   * via reconcile_parent_address() -- deliberately not touching TSCH's
   * actual time source (tsch_queue_update_time_source()) at all. First
   * attempt did exactly that (forcing TSCH's real sync target back onto
   * RPL's address too) and measured *worse* at 100 nodes (PDR 60.7%->48.1%,
   * disassoc 16->33): resynchronize() picks "last neighbor we could
   * actually hear an EB from" precisely because the node just lost sync
   * with something else, likely including the RPL parent itself in
   * marginal-link conditions -- forcibly redirecting the radio-level sync
   * target back onto a neighbor the node may not currently be able to
   * track pushed it back out of sync again, compounding the very problem
   * that mechanism exists to recover from. Correcting only *our own* cell
   * bookkeeping avoids that entirely: TSCH keeps syncing its clock to
   * whoever it can actually hear (its own legitimate job), while every IA
   * cell still gets hashed against the address RPL actually routes
   * through -- the one thing that was actually wrong.
   *
   * Full 6-scale validation (PDR, on top of ORCHESTRA_CONF_COMMON_SHARED_
   * PERIOD=11 above, library-default-31 baseline for reference):
   *
   *   scale     31 (orig)   11 alone   11 + this fix
   *      8        99.8%       99.8%       99.8%   (flat -- no churn)
   *     25        74.9%       73.4%       87.4%   (large win)
   *     49        77.3%       92.2%       88.4%   (gives back a little, still far above orig)
   *     60        71.1%       81.2%       80.6%   (flat)
   *    100        39.4%       60.7%       64.0%   (win)
   *    150        14.7%       10.6%       38.9%   (large win -- beats even the manual
   *                                                 SFS_SIZE=167 override above, 31.1%,
   *                                                 with zero per-scale tuning)
   *
   * Every scale improves or holds flat versus the original baseline; only
   * 49 gives back some of period=11-alone's own gain. Kept permanently.
   *
   * Hysteresis (requiring the divergence to persist before correcting) was
   * tried afterward and reverted -- see the long comment on the now-removed
   * IA_PARENT_RECONCILE_DELAY above for the full measured sweep. It did cut
   * disassociations substantially, but PDR was lower at every delay tried,
   * at both 100 and 150 nodes, than this plain, unconditional, immediate
   * version below -- disassociations were never the thing actually costing
   * PDR, so trading them down by waiting only left cells wrong for longer.
   * Correct as soon as a divergence is seen, every check_parent_knows_us()
   * tick (4/s): a no-op the vast majority of ticks, since orchestra_parent_
   * linkaddr and RPL's own preferred parent agree almost all the time. */
  {
    rpl_dag_t *dag = rpl_get_any_dag();
    if(dag != NULL && dag->preferred_parent != NULL) {
      uip_ipaddr_t *rpl_parent_ip = rpl_parent_get_ipaddr(dag->preferred_parent);
      const linkaddr_t *rpl_parent_lladdr = rpl_parent_ip != NULL ?
        (const linkaddr_t *)uip_ds6_nbr_lladdr_from_ipaddr(rpl_parent_ip) : NULL;
      if(rpl_parent_lladdr != NULL) {
        reconcile_parent_address(rpl_parent_lladdr);
      }
    }
  }
  if(orchestra_parent_knows_us && !last_parent_knows_us) {
    int i;
    update_uplink(1);
    update_root_adjacent(1);
    for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
      if(children[i].in_use) {
        update_child_links(&children[i], 1);
      }
    }
#if TSCH_WITH_IMPLICIT_ACK
    update_self_overhear(1);
#endif /* TSCH_WITH_IMPLICIT_ACK */
  }
  last_parent_knows_us = orchestra_parent_knows_us;
  ctimer_set(&bootstrap_timer, IA_BOOTSTRAP_CHECK_PERIOD, check_parent_knows_us, NULL);
}
/*---------------------------------------------------------------------------*/
static void
init(uint16_t sf_handle)
{
  int i;
  slotframe_handle = sf_handle;
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    children[i].in_use = 0;
  }
  linkaddr_copy(&last_rx_source, &linkaddr_null);
  sf_short = tsch_schedule_add_slotframe(slotframe_handle, TSCH_IA_SFS_SIZE);
  sf_root = tsch_schedule_add_slotframe(slotframe_handle | 0x4000, ORCHESTRA_IA_ROOT_PERIOD);
  if(sf_short == NULL) {
    LOG_ERR("failed to add the short (implicit-ack) slotframe\n");
  }
  if(sf_root == NULL) {
    LOG_ERR("failed to add the root-adjacent slotframe\n");
  }
  last_parent_knows_us = orchestra_parent_knows_us;
  ctimer_set(&bootstrap_timer, IA_BOOTSTRAP_CHECK_PERIOD, check_parent_knows_us, NULL);
}
#if TSCH_WITH_IMPLICIT_ACK
/*---------------------------------------------------------------------------*/
/* Give up on an implicit-ack-pending packet to our parent once its deadline
 * has passed with no matching overheard frame, handing it to the existing
 * retry/drop logic exactly as an explicit-ack failure would. Only our own
 * parent can ever have a pending packet (implicit ack is only active for
 * traffic to our parent, see orchestra_ia_implicit_ack_active()), so there is
 * no need to scan every neighbor. Safe to call from the ASFN callback:
 * tsch_queue_packet_sent() and its callees never call tsch_get_lock(). */
static void
check_ia_timeout(void)
{
  if(!linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)) {
    struct tsch_neighbor *n = tsch_queue_get_nbr(&orchestra_parent_linkaddr);
    struct tsch_packet *p = tsch_queue_get_head_packet(n);
    /* TSCH_ASN_DIFF's subtraction is on unsigned ls4b fields, so its result
     * must be cast to a signed type before comparing -- otherwise "asn1 in
     * the past relative to asn2" wraps around to a huge positive value and
     * "diff >= 0" is trivially always true, firing the timeout on the very
     * next ASFN check regardless of how far away the real deadline is
     * (every other TSCH_ASN_DIFF call site does this same cast). */
    if(p != NULL && p->ia_pending && (int32_t)TSCH_ASN_DIFF(tsch_current_asn, p->ia_deadline_asn) >= 0) {
      LOG_INFO("IATRACE timeout (no overhear match), transmissions=%u asn=%lu deadline=%lu\n",
               p->transmissions, (unsigned long)tsch_current_asn.ls4b, (unsigned long)p->ia_deadline_asn.ls4b);
      p->ia_pending = 0;
      if(p->ia_link == l_uplink && l_uplink != NULL) {
        uplink_fail_streak++;
        LOG_INFO("IATRACE uplink fail streak=%u epoch=%u\n", uplink_fail_streak, uplink_epoch);
        if(uplink_fail_streak >= UPLINK_REHASH_FAIL_THRESHOLD) {
          uplink_fail_streak = 0;
          uplink_epoch++;
          LOG_INFO("IATRACE uplink rehash, new epoch=%u\n", uplink_epoch);
          update_uplink(0);
        }
      }
      tsch_queue_packet_sent(n, p, p->ia_link, MAC_TX_NOACK);
    }
  }
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_NEW_ASFN: fired by the scheduler exactly once per ASFN
 * increment (tsch-schedule.c). Recompute and rotate every cell we own by
 * mutating its timeslot/channel_offset directly, rather than removing and
 * re-adding the link -- see the implementation plan for why. */
void
orchestra_ia_new_asfn(uint32_t asfn)
{
  int i;
  current_asfn = asfn;
  /* allow_install=0: this callback runs from inside
   * tsch_schedule_get_next_active_link(), where installing/removing a link
   * (tsch_get_lock()) can deadlock the mote -- see update_uplink(). Only
   * mutate fields of links that already exist; check_parent_knows_us() and
   * the other (safe-context) callbacks own actual install/removal. */
  update_uplink(0);
  update_root_adjacent(0);
  update_relay_rx_shared(0);
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(children[i].in_use) {
      int j;
      update_child_links(&children[i], 0);
      for(j = 0; j < ORCHESTRA_IA_MAX_CHILDREN; j++) {
        if(children[i].grandchildren[j].in_use) {
          update_grandchild_relay_rx(&children[i], &children[i].grandchildren[j], 0);
        }
      }
    }
  }
#if TSCH_WITH_IMPLICIT_ACK
  update_self_overhear(0);
  check_ia_timeout();
#endif /* TSCH_WITH_IMPLICIT_ACK */
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_UNICAST_DATA_INPUT: fired from tsch.c with the link-layer
 * source address of an incoming unicast DATA frame, just before it is passed
 * up to packet_input(). Any resulting forwarded frame is sent synchronously,
 * in the same call stack, so select_packet() above can rely on this being the
 * correct attribution for the very next outgoing packet it's asked about. */
void
orchestra_ia_data_input(const linkaddr_t *source)
{
  linkaddr_copy(&last_rx_source, source);
}
/*---------------------------------------------------------------------------*/
#if TSCH_WITH_IMPLICIT_ACK
/* TSCH_CALLBACK_IA_PARENT_EB: fired from eb_input() with the grandparent
 * address (and whether the sender says that address is the root -- see the
 * long comment on grandparent_is_root above for why this can't be
 * re-derived locally) carried in an EB from our own time source (our
 * parent). Refreshed on every such EB, not just once, in case our parent's
 * own parent (or its root-ness) changes. */
void
orchestra_ia_parent_eb_input(const linkaddr_t *grandparent, uint8_t is_root)
{
  uint8_t was_known = have_grandparent;
  uint8_t was_root = grandparent_is_root;
  linkaddr_copy(&grandparent_linkaddr, grandparent);
  grandparent_is_root = is_root;
  have_grandparent = 1;
  if(!was_known || was_root != is_root) {
    /* First time we learn our grandparent, or its root-ness just changed:
     * safe to install/tear down SELF_OVERHEAR right away, since eb_input()
     * runs from tsch_rx_process_pending(), a regular polled process, not
     * from inside slot-operation. */
    update_self_overhear(1);
  }
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_PARENT_CONGESTION: fired from eb_input() with our
 * parent's own current queue depth toward its own parent, carried on every
 * EB from our own time source (piggybacked on the same IE as the
 * grandparent tag, refreshed just as often). This is the real, cross-
 * boundary congestion signal that three prior local-only adaptive-timeout
 * attempts were missing -- see orchestra_ia_confirmation_cycles() below. */
void
orchestra_ia_parent_congestion_input(uint8_t congestion)
{
  parent_congestion = congestion;
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_OWN_PARENT: fired from tsch_packet_create_eb() (tsch-
 * packet.c) to fetch our own current parent's address, for inclusion in our
 * own EB so our children can learn their grandparent (us's parent) -- along
 * with whether that parent (from our own, locally-valid 1-hop point of
 * view) is itself the root, since our children cannot determine that fact
 * correctly on their own (see the long comment on grandparent_is_root). */
int
orchestra_ia_get_own_parent(linkaddr_t *out, uint8_t *out_is_root)
{
  if(linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)) {
    return 0;
  }
  linkaddr_copy(out, &orchestra_parent_linkaddr);
  *out_is_root = tsch_roots_is_root(&orchestra_parent_linkaddr);
  return 1;
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_CONGESTION: fired from tsch_packet_create_eb() (tsch-
 * packet.c) to fetch our own current queue depth toward our own parent
 * (self-originated + relayed traffic combined -- exactly the queue that
 * backs up if we can't relay fast enough), for inclusion in our own EB so
 * our children can see it via orchestra_ia_parent_congestion_input(). */
uint8_t
orchestra_ia_get_own_congestion(void)
{
  int count;
  if(linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)) {
    return 0;
  }
  count = tsch_queue_nbr_packet_count(tsch_queue_get_nbr(&orchestra_parent_linkaddr));
  if(count < 0) {
    return 0;
  }
  return count > 255 ? 255 : (uint8_t)count;
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_CONFIRMATION_CYCLES: fired from tsch_tx_slot() (tsch-
 * slot-operation.c) when arming a self-originated implicit-ack-pending
 * packet, to pick how many TSCH_IA_SFS_SIZE cycles to wait for an overheard
 * confirmation before declaring it unconfirmed and retrying -- see the long
 * comment on the congestion thresholds in orchestra-conf.h for why this is
 * driven by our PARENT's self-reported queue depth (parent_congestion)
 * rather than anything derived locally.
 *
 * Gated on ORCHESTRA_IA_CONGESTION_ADAPTIVE (orchestra-conf.h, default 1):
 * when 0, this always returns the plain fixed TSCH_IA_CONFIRMATION_TIMEOUT_
 * CYCLES constant instead of thresholding parent_congestion -- i.e. exactly
 * the plain fixed-cycles configuration swept earlier this session (cycles=1
 * measured 49.7% PDR at 49 nodes, the best of any single value <=49 nodes),
 * with all of the congestion-signaling plumbing left running but unused
 * rather than torn out, so this can be flipped back on later. */
uint8_t
orchestra_ia_confirmation_cycles(void)
{
#if ORCHESTRA_IA_CONGESTION_ADAPTIVE
  if(parent_congestion < ORCHESTRA_IA_CONGESTION_LOW_THRESHOLD) {
    return 1;
  } else if(parent_congestion < ORCHESTRA_IA_CONGESTION_HIGH_THRESHOLD) {
    return 2;
  } else {
    return 3;
  }
#else /* ORCHESTRA_IA_CONGESTION_ADAPTIVE */
  return TSCH_IA_CONFIRMATION_TIMEOUT_CYCLES;
#endif /* ORCHESTRA_IA_CONGESTION_ADAPTIVE */
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_OWN_CHILDREN: fired from tsch_packet_create_eb() (tsch-
 * packet.c) to fetch our own direct children's addresses, for inclusion in
 * our own EB so our parent (their grandparent) can install a matching Rx
 * cell for each of our RELAY_TX cells -- see the "grandparent fan-out gap"
 * comment on MLME_SHORT_IE_TSCH_IA_CHILDREN in frame802154e-ie.c. */
uint8_t
orchestra_ia_get_own_children(linkaddr_t *out, uint8_t *out_has_descendants, uint8_t max_children)
{
  int i;
  uint8_t n = 0;
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN && n < max_children; i++) {
    if(children[i].in_use) {
      linkaddr_copy(&out[n], &children[i].addr);
      out_has_descendants[n] = child_has_descendants(&children[i]);
      n++;
    }
  }
  if(n > 0) {
    LOG_INFO("IATRACE own_children n=%u first=", n);
    LOG_INFO_LLADDR(&out[0]);
    LOG_INFO_("\n");
  }
  return n;
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_CHILD_EB: fired from eb_input() for every received EB
 * (not just ones from our own time source), with the sender's address and
 * whatever children list it carries. Only acts if source is actually one of
 * our own known children -- an EB from an unrelated neighbor (e.g. our own
 * parent, or a neighbor we're not related to in the tree) is ignored here.
 * For each listed grandchild, installs (or keeps rotating, on refresh) the
 * matching Rx cell -- see update_grandchild_relay_rx(). Also tears down any
 * previously-tracked grandchild that's no longer in the list (e.g. it left
 * the network), mirroring child_removed()'s cleanup one level down. Safe to
 * install/remove links directly: eb_input() runs from tsch_rx_process_
 * pending(), a regular polled process, not from inside slot-operation. */
void
orchestra_ia_child_eb_input(const linkaddr_t *source, const linkaddr_t *children_list,
                             const uint8_t *children_has_descendants, uint8_t num_children)
{
  struct ia_child *c = find_child(source);
  int i;
  if(num_children > 0 || c != NULL) {
    LOG_INFO("IATRACE child_eb_input source=");
    LOG_INFO_LLADDR(source);
    LOG_INFO_(" is_child=%d num_children=%u\n", c != NULL, num_children);
  }
  uint8_t seen[ORCHESTRA_IA_MAX_CHILDREN] = {0};
  if(c == NULL) {
    return;
  }
  /* Deliberately not also excluded when tsch_is_coordinator: root has
   * grandchildren too (any child of one of root's own direct children), and
   * needs update_grandchild_relay_rx() to actually install its Rx cell for
   * them exactly as much as any other grandparent does -- see that
   * function's comment. This exclusion used to be harmless when root-
   * adjacent nodes never relayed via RELAY_TX at all (root would never have
   * had anything to receive here anyway); now that they do, this silently
   * discarded every grandchild root ever learned about, leaving root with
   * no matching Rx cell and no way to ever receive a 2+-hop node's relayed
   * traffic despite the sender's radio transmissions succeeding every time. */
  for(i = 0; i < num_children; i++) {
    struct ia_grandchild *gc = find_grandchild(c, &children_list[i]);
    if(gc == NULL) {
      gc = alloc_grandchild(c, &children_list[i]);
    }
    if(gc != NULL) {
      gc->has_descendants = children_has_descendants[i];
      update_grandchild_relay_rx(c, gc, 1);
      seen[gc - c->grandchildren] = 1;
    }
  }
  for(i = 0; i < ORCHESTRA_IA_MAX_CHILDREN; i++) {
    if(c->grandchildren[i].in_use && !seen[i]) {
      remove_grandchild(c, &c->grandchildren[i], 1);
    }
  }
  /* Re-evaluate child_has_descendants(c) now that this child's grandchild
   * set may have just changed -- installs/tears down its extra RELAY_TX
   * shard(s) accordingly (see update_child_links()'s own comment). Safe to
   * call unconditionally on every EB, not only on an actual change: it's
   * idempotent, matching every other periodic call site of this
   * function. */
  update_child_links(c, 1);
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE: fired from send_packet() (tsch.c),
 * *before* select_packet() runs, to decide whether to suppress a unicast
 * frame's ACK request. Only active once our dedicated cells to our parent
 * actually exist and are known on both ends (orchestra_parent_knows_us --
 * see update_uplink()'s long comment), we know our own grandparent (without
 * it we could never overhear a confirming relay), and the destination is
 * our own next hop. Never active for RPL control traffic (DIS/DIO/DAO/
 * DAO-ACK) or our own keep-alives: see ia_is_rpl_control_packet()'s and
 * ia_is_keepalive_packet()'s comments -- neither has a matching SELF_OVERHEAR
 * cell watching for it, so a suppressed ACK there would just time out every
 * single time instead of getting the immediate explicit ack it needs, which
 * is what keeps DAO -- and, for both, our own time-sync freshness -- reliable
 * regardless of this cell's *other* traffic.
 *
 * Beyond that, this must distinguish self-originated traffic from a relay of
 * a child's frame, using the exact same last_rx_source/find_child() check
 * select_packet() uses right after us in the same synchronous send (safe to
 * peek without consuming: select_packet() is the one that clears it, and it
 * runs later in this same call). Getting this wrong is exactly why a
 * root-adjacent node relaying a child's frame used to deadlock that frame
 * permanently: RELAY_TX[child]'s *cell* correctly uses short timing (per
 * ia_rx_short_timing_ok(), which doesn't care about our own root-adjacency),
 * but without this check every RELAY_TX packet's *destination* (our own
 * parent -- root, for a root-adjacent relay) also matches our own uplink's
 * address, so it fell through to ia_tx_short_timing_ok()'s logic instead --
 * which correctly refuses short timing for OUR OWN traffic to a root parent,
 * but wrongly did the same for a frame we're only relaying. The ACK request
 * was then never suppressed, but the cell it went out on had zero ACK
 * budget (RxAckDelay/AckWait/MaxAck all 0) -- a real ack that can never
 * arrive no matter how many times it's retried. Confirmed in testing: this
 * was the direct cause of a previously rock-solid root-adjacent node
 * repeatedly disassociating once it started actually relaying a child
 * (tx/rx/sync stats at disassociation showed heavy retry activity despite a
 * near-empty queue -- one packet retried over and over, never one that a
 * real ACK could ever resolve). Self-originated traffic still follows
 * ia_tx_short_timing_ok() unchanged. */
int
orchestra_ia_implicit_ack_active(const linkaddr_t *addr)
{
  if(!orchestra_parent_knows_us || ia_is_rpl_control_packet()
     || ia_is_keepalive_packet()
     || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || !linkaddr_cmp(addr, &orchestra_parent_linkaddr)) {
    return 0;
  }
  {
    struct ia_child *relay_child = find_child(&last_rx_source);
    if(relay_child != NULL && relay_child->relay_tx != NULL) {
      return ia_rx_short_timing_ok();
    }
  }
  return ia_tx_short_timing_ok();
}
/*---------------------------------------------------------------------------*/
/* Attempts to read the ORIGINAL IP-layer sender's terminal address byte out
 * of a 6LoWPAN IPHC-compressed frame, given a pointer to its bytes starting
 * immediately after the 802.15.4 MAC header (i.e. what tsch_rx_slot() reads
 * before any packetbuf/uip processing happens -- this deliberately never
 * touches packetbuf or uip_buf, so it cannot interact with, or be corrupted
 * by, genuine concurrent packet reception).
 *
 * Why this is needed at all: a RELAY_TX[child]/SELF_OVERHEAR position is
 * shared by that child's *entire* subtree, not just the child's own packets
 * -- every hop tags an outgoing relay by immediate previous-hop address
 * (last_rx_source), not by original sender, since a node only ever tracks
 * its own direct children, not arbitrary-depth descendants (see struct
 * ia_child's comment). So a source/destination match on this cell (checked
 * by the caller) is necessary but not sufficient: it is exactly as
 * consistent with "my parent is relaying MY packet" as with "my parent is
 * relaying something that merely passed through me from a child/grandchild
 * of mine." Confirmed empirically via direct log tracing (armed/timeout/
 * overhear ASN sequences on a live 8-node run): nodes with a non-trivial
 * subtree saw overhear-match-but-nothing-pending rates of 45-63%, while
 * leaf nodes (no subtree at all) saw 3-7% -- tracking subtree size, not
 * network size, and not explained by (and not fixed by lengthening) the
 * confirmation deadline, which only makes the shared queue backlog worse
 * (measured: PDR 88.3%->56.9%, timeout rate 29.9%->57.1% at 8 nodes/SF=101
 * under ORCHESTRA_CONF_IA_CONGESTION_ADAPTIVE). Disambiguation requires
 * actually reading who the original sender was.
 *
 * Deliberately minimal and fail-closed: handles SAM 00/01 (a full or
 * 64-bit-IID address inline, under either link-local or context-based
 * compression -- SAC only changes where the elided *prefix* comes from,
 * never the explicit-IID byte count or position, except at SAM=00 where
 * the two modes disagree and are handled separately below), covering the
 * long-address traffic this rule's own examples always produce. Any other
 * case (a 16-bit short address, context-SAM=00's reserved encoding, a
 * malformed or truncated header) returns 0 -- "cannot determine" -- and
 * the caller must treat
 * that exactly like "not a match": failing to confirm a genuinely-own
 * packet costs one extra retry (the existing timeout path already handles
 * that), but a false positive would silently and permanently lose a
 * different packet's confirmation, which is the one outcome this must
 * never risk. */
static uint8_t
ia_overhear_source_is_own(const uint8_t *payload, uint16_t len)
{
  uint16_t offset;
  uint8_t iphc0, iphc1, sam;

  if(payload == NULL || len < 2
     || (payload[0] & SICSLOWPAN_DISPATCH_IPHC_MASK) != SICSLOWPAN_DISPATCH_IPHC) {
    return 0;
  }
  iphc0 = payload[0];
  iphc1 = payload[1];
  /* SAC (context-based compression) is NOT bailed out on: Contiki-NG's
   * default RPL setup auto-registers a context (typically the DODAG's
   * global /64 prefix) as context 0, so SAC=1 is the COMMON case in
   * practice, confirmed empirically (every overheard frame in a live run
   * had SAC set). The context only supplies the elided *prefix* -- it does
   * not change where or how many explicit IID bytes follow for SAM=01/10/11,
   * which is all this function reads (see unc_llconf[]/unc_ctxconf[] in
   * sicslowpan.c: both tables read the same byte counts for tmp=1/2/3, they
   * only differ at tmp=0, handled below). */
  offset = 2;
  if(iphc1 & SICSLOWPAN_IPHC_CID) {
    offset += 1;
  }
  if((iphc0 & SICSLOWPAN_IPHC_FL_C) == 0) {
    offset += (iphc0 & SICSLOWPAN_IPHC_TC_C) == 0 ? 4 : 3;
  } else if((iphc0 & SICSLOWPAN_IPHC_TC_C) == 0) {
    offset += 1;
  }
  if((iphc0 & SICSLOWPAN_IPHC_NH_C) == 0) {
    offset += 1;
  }
  if((iphc0 & 0x03) == SICSLOWPAN_IPHC_TTL_I) {
    offset += 1;
  }

  sam = (iphc1 & SICSLOWPAN_IPHC_SAM_11) >> SICSLOWPAN_IPHC_SAM_BIT;
  switch(sam) {
  case 0:
    if(iphc1 & SICSLOWPAN_IPHC_SAC) {
      /* Context-based SAM=00 is "reserved/unspecified" (unc_ctxconf[0] ==
       * 0x00, 0 bits from packet) -- no address is recoverable here. */
      return 0;
    }
    /* Link-local SAM=00: full 128-bit address inline -- IID is its last
     * 8 bytes (unc_llconf[0] == 0x0f, 16 bytes from packet). */
    if(len < (uint16_t)(offset + 16)) {
      return 0;
    }
    return payload[offset + 15] == linkaddr_node_addr.u8[LINKADDR_SIZE - 1];
  case 1: /* SAM=01: 64-bit IID inline, same byte count under SAC=0 or 1
           * (unc_llconf[1] == 0x28, unc_ctxconf[1] == 0x88 -- both read
           * 8 bytes from the packet). */
    if(len < (uint16_t)(offset + 8)) {
      return 0;
    }
    return payload[offset + 7] == linkaddr_node_addr.u8[LINKADDR_SIZE - 1];
  default:
    /* SAM=10 (16-bit short address, unused by this rule's long-address
     * examples) or SAM=11 (fully elided -- reconstructed from THIS frame's
     * own link-layer sender, i.e. our parent; this can only be valid if
     * our parent's own address is the original source, meaning this is
     * our parent's own traffic, never a relay of ours or any sibling's).
     * Both cases are "not us". */
    return 0;
  }
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_OVERHEAR: fired from tsch_rx_slot() on a successful
 * receive on our SELF_OVERHEAR link. Confirm iff this is genuinely our
 * parent's relay to our grandparent (as opposed to some other collision on
 * this hash-derived cell, which -- like any autonomous-scheduling hash
 * collision -- is tolerated, not prevented: we simply don't confirm on it
 * and let the timeout path retry) AND the original IP-layer sender is us
 * specifically, not a descendant whose traffic merely passed through this
 * same, subtree-shared position (see ia_overhear_source_is_own()'s long
 * comment). At most one packet to our parent is ever in flight at a time
 * (do_wait_for_ack is false for implicit-ack links, so burst_link_requested/
 * TSCH_BURST_MAX_LEN never applies to them either), so the head of its
 * queue is unambiguous once we know the overheard frame is actually ours. */
void
orchestra_ia_overhear(const linkaddr_t *source, const linkaddr_t *destination,
                      struct tsch_link *link,
                      const uint8_t *payload, uint16_t payload_len)
{
  LOG_INFO("IATRACE overhear src=");
  LOG_INFO_LLADDR(source);
  LOG_INFO_(" dst=");
  LOG_INFO_LLADDR(destination);
  LOG_INFO_(" (parent match=%d, grandparent match=%d)\n",
            linkaddr_cmp(source, &orchestra_parent_linkaddr),
            have_grandparent && linkaddr_cmp(destination, &grandparent_linkaddr));
  if(have_grandparent && !linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     && linkaddr_cmp(source, &orchestra_parent_linkaddr)
     && linkaddr_cmp(destination, &grandparent_linkaddr)) {
    struct tsch_neighbor *n = tsch_queue_get_nbr(&orchestra_parent_linkaddr);
    struct tsch_packet *p = tsch_queue_get_head_packet(n);
    uint8_t is_own = ia_overhear_source_is_own(payload, payload_len);
    LOG_INFO("IATRACE overhear MATCH, pending packet=%p, ia_pending=%d is_own=%u asn=%lu deadline=%lu transmissions=%d\n",
             (void *)p, p != NULL ? p->ia_pending : -1, is_own, (unsigned long)tsch_current_asn.ls4b,
             p != NULL ? (unsigned long)p->ia_deadline_asn.ls4b : 0UL, p != NULL ? p->transmissions : -1);
    if(is_own && p != NULL && p->ia_pending) {
      p->ia_pending = 0;
      if(p->ia_link == l_uplink && l_uplink != NULL) {
        uplink_fail_streak = 0;
      }
      tsch_queue_packet_sent(n, p, p->ia_link, MAC_TX_OK);
      LOG_INFO("IATRACE overhear CONFIRMED implicit ack\n");
    }
  }
}
#endif /* TSCH_WITH_IMPLICIT_ACK */
/*---------------------------------------------------------------------------*/
struct orchestra_rule implicit_ack_tree = {
  init,
  new_time_source,
  select_packet,
  child_added,
  child_removed,
  NULL,
  root_node_updated,
  "implicit ack tree",
  TSCH_IA_SFS_SIZE,
};

#endif /* UIP_MAX_ROUTES */
