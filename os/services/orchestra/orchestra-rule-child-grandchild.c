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
 */

/**
 * \file
 *         Autonomous parent/child TSCH cell scheduling, WITHOUT implicit
 *         acknowledgment and WITHOUT the dual (short/regular) timeslot
 *         template -- every cell here is a completely standard TSCH cell,
 *         using the regular timeslot template and TSCH's normal,
 *         synchronous, explicit-ACK confirmation/retry logic unchanged.
 *
 *         SIZING RULE (deliberately simple): every node allocates cells with
 *         its OWN parent sized by (that node's own direct RPL child count)
 *         + 1 for its own traffic. Nothing deeper than direct children is
 *         ever tracked, estimated, or signaled -- no grandchild addresses,
 *         no per-grandchild shard counts, no RPL routing-table traversal at
 *         all. A node's own direct child is relaying an entire subtree
 *         behind it, however deep -- but from this node's point of view
 *         that is one relationship, sized once, not one relationship per
 *         descendant.
 *
 *         This replaces an earlier design (see git history/session notes)
 *         that instead estimated each relay relationship's traffic from the
 *         RPL storing-mode routing table -- a true whole-subtree traffic
 *         proxy, but one that fluctuates on every DAO/route change anywhere
 *         in a node's entire subtree. That value needed to be signaled one
 *         hop (parent needs to know a child's exact shard count to size a
 *         matching Rx cell) and changed far faster than the EB channel
 *         carrying it could keep up with at scale, and every one of a dozen
 *         different attempts to close that gap (faster signaling, larger
 *         receive-side headroom, hysteresis, dedicated slotframes, ...)
 *         made things worse, not better. Own-child-count is a far more
 *         stable quantity -- it only changes when this node's own set of
 *         direct RPL children actually changes, a comparatively rare
 *         topology event -- so the same one-hop EB signaling that failed for
 *         the old metric should be adequate for this one.
 *
 *         Every child of a given parent gets its own dedicated cell(s),
 *         HASH2(parent, child) + shard spreading, rather than sharing one
 *         position Orchestra-style -- but there is only ever one such
 *         relationship per direct child, never one per descendant.
 *
 *         The own-shard-count signal reuses the same children-list Enhanced
 *         Beacon Information Element orchestra-rule-implicit-ack.c already
 *         defines (see MLME_SHORT_IE_TSCH_IA_CHILDREN, frame802154e-ie.c),
 *         piggybacking one byte per direct child -- that byte used to carry
 *         a 0/1 has-descendants flag there, and here instead carries this
 *         node's own current shard count. No framer, tsch.c, or tsch.h
 *         change was needed to support this reuse -- see this file's own
 *         TSCH_CALLBACK_IA_OWN_CHILDREN/TSCH_CALLBACK_IA_CHILD_EB
 *         definitions below.
 *
 *         Every other TSCH_CALLBACK_IA_* hook that orchestra-rule-implicit-
 *         ack.c wires up is either unneeded here (grandparent discovery,
 *         congestion-adaptive confirmation deadlines: both exist solely to
 *         support implicit-ack, which this file does not use) or must still
 *         be defined as a harmless stub purely to satisfy tsch.h's
 *         unconditional auto-wiring under TSCH_WITH_IMPLICIT_ACK=1 (needed
 *         only for the children-list IE above) -- see the "unused IA
 *         callback stubs" section near the end of this file. Critically,
 *         TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE() always returns 0 here, which
 *         by itself is sufficient to keep every other piece of the
 *         implicit-ack machinery in tsch-slot-operation.c permanently
 *         dormant regardless of TSCH_WITH_IMPLICIT_ACK's value: every one of
 *         those code paths is gated on PACKETBUF_ATTR_MAC_ACK having been
 *         left unset for a given frame, which never happens when this
 *         function always returns 0 (see send_packet(), tsch.c, and the
 *         do_wait_for_ack computation in tsch_tx_slot(),
 *         tsch-slot-operation.c). No cell installed by this file ever calls
 *         tsch_ia_link_use_short_timing() either, so every cell also keeps
 *         the regular, full-length timeslot template by construction.
 *
 *         CG_CONF_EWMA_SHARD_COUNT (default 0, off): a build-time comparison
 *         variant that replaces direct adoption of a child's EB-reported
 *         shard count with a Q8 fixed-point EWMA of it, installing the
 *         rounded average instead of the raw report -- see
 *         cg_child_eb_input()'s own comment for the mechanism and the
 *         expected trade-off this is designed to measure.
 */

#include "contiki.h"
#include "orchestra.h"
#include "net/packetbuf.h"
#include "net/mac/tsch/tsch-roots.h"
#include "net/routing/rpl-classic/rpl.h"
#include "sys/ctimer.h"
#include "sys/log.h"

#define LOG_MODULE "Orchestra"
#define LOG_LEVEL  LOG_LEVEL_MAC

#if UIP_MAX_ROUTES != 0

#ifndef CG_CONF_MAX_CHILDREN
#define CG_MAX_CHILDREN 6
#else
#define CG_MAX_CHILDREN CG_CONF_MAX_CHILDREN
#endif

/* Rank-aware cap (default 0, off): every node's cap has been a single flat
 * constant so far, which the companion paper's own 300-node extended-scale
 * check found to be the actual cause of that scale's collapse -- once a
 * node's own direct child count pushes it against the flat cap, its uplink
 * toward its parent is saturated, and that saturation compounds hop by hop
 * as it propagates outward through a deep, dense tree (a wedge of failing
 * nodes at the tree's periphery, concentrated exactly where this compounds
 * the most). The direct mitigation the paper points to: the flat cap is
 * least appropriate right where the tree carries the most traffic, near
 * the root, so give nodes within a few hops of the root a larger one. RPL
 * rank is free -- every node already computes it for routing, nothing new
 * is signaled -- so this needs no coordination with anyone, not even a
 * one-hop signal: a node decides its own cap entirely from its own rank. */
#ifndef CG_CONF_RANK_CAP
#define CG_RANK_CAP 0
#else
#define CG_RANK_CAP CG_CONF_RANK_CAP
#endif

#if CG_RANK_CAP
/* A node's own rank must be at or below this value to get the larger,
 * near-root cap. Expressed as a raw rpl_rank_t threshold rather than a
 * hop count: RPL rank is ETX-weighted, not a pure hop counter, so a fixed
 * multiple of RPL_MIN_HOPRANKINC would silently mean something different
 * on a lossy topology than on a clean one. The default below assumes the
 * library default RPL_MIN_HOPRANKINC (256) and targets roughly the root's
 * immediate neighbors and their own children (about 2 hops); retune to
 * your own deployment's actual RPL_CONF_MIN_HOPRANKINC and link quality. */
#ifndef CG_CONF_RANK_CAP_THRESHOLD
#define CG_RANK_CAP_THRESHOLD 512
#else
#define CG_RANK_CAP_THRESHOLD CG_CONF_RANK_CAP_THRESHOLD
#endif

/* The boosted cap for nodes at or below CG_RANK_CAP_THRESHOLD. Becomes the
 * array size below (CG_RELAY_SHARDS_MAX) since every per-child cell array
 * must be sized for the largest cap ANY node in the network might use --
 * far-from-root nodes still only ever request up to CG_RELAY_SHARDS_FAR
 * (see cg_effective_cap()), the array is just sized to allow more. */
#ifndef CG_CONF_RELAY_SHARDS_NEAR_ROOT
#define CG_RELAY_SHARDS_NEAR_ROOT 6
#else
#define CG_RELAY_SHARDS_NEAR_ROOT CG_CONF_RELAY_SHARDS_NEAR_ROOT
#endif

/* The ordinary, away-from-root cap -- unchanged from every measured result
 * in the companion paper's Tables (all of which used a flat cap of 4). */
#ifndef CG_CONF_RELAY_SHARDS_FAR
#define CG_RELAY_SHARDS_FAR 4
#else
#define CG_RELAY_SHARDS_FAR CG_CONF_RELAY_SHARDS_FAR
#endif

#define CG_RELAY_SHARDS_MAX CG_RELAY_SHARDS_NEAR_ROOT
#else /* CG_RANK_CAP */
/* Paper equation (3): K_S = m_S + 1, with NO fixed upper bound other than
 * the slotframe length L ("Equation (3) -- the core allocation rule, NO
 * CAP" per the paper text). The only ceiling this build still needs is a
 * structural one: CG_MAX_CHILDREN is itself just an array-sizing limit (the
 * largest number of direct children this implementation can track at all,
 * not a deliberate traffic policy), so a node's own shard count -- (its own
 * direct child count) + 1 -- can never exceed CG_MAX_CHILDREN + 1 anyway.
 * Sizing CG_RELAY_SHARDS_MAX to exactly that bound (rather than a small flat
 * constant like 4, which WOULD be an A3-style artificial cap the paper
 * explicitly contrasts itself against in its own Fig 6a) means cg_own_
 * shard_count()'s final "n > cap ? cap : n" clamp below is a no-op for every
 * reachable value of n -- i.e. truly uncapped, modulo this implementation
 * limit, not the library default of 4. Override via CG_CONF_RELAY_SHARDS_MAX
 * only if a deployment's slotframe length L is itself smaller than
 * CG_MAX_CHILDREN + 1 and an explicit, smaller structural ceiling is wanted
 * instead. */
#ifndef CG_CONF_RELAY_SHARDS_MAX
#define CG_RELAY_SHARDS_MAX (CG_MAX_CHILDREN + 1)
#else
#define CG_RELAY_SHARDS_MAX CG_CONF_RELAY_SHARDS_MAX
#endif
#endif /* CG_RANK_CAP */

/* Comparison variant, off by default: instead of adopting a child's
 * EB-advertised shard count directly (the design documented above and used
 * for every result in the companion paper's main evaluation), run it
 * through a Q8 fixed-point EWMA and install the *rounded, smoothed* value
 * instead. Still event-triggered only on EB reception -- nothing here adds
 * polling or changes when the parent looks at a report, only what it does
 * with one once it has it. See cg_child_eb_input()'s own comment for why
 * this is expected to trade (and, per the companion paper's Attempt 10
 * finding on hysteresis, a structurally similar prior attempt already did
 * trade) responsiveness for smoothness in a regime where responsiveness was
 * the binding constraint. */
#ifndef CG_CONF_EWMA_SHARD_COUNT
#define CG_EWMA_SHARD_COUNT 0
#else
#define CG_EWMA_SHARD_COUNT CG_CONF_EWMA_SHARD_COUNT
#endif

/* Idle reclaim (default 0, off): the mechanisms above all react to what a
 * child SAYS about itself in its EB (its own structural child count). This
 * one instead watches whether the parent has actually HEARD anything from a
 * child recently, and if not for a long time, shrinks that child's cells
 * back down to the guaranteed minimum of 1 -- freeing the rest for other
 * children -- without waiting for RPL to decide the child is gone
 * (child_removed() keeps working exactly as before, independently; this is
 * a faster, additional, and reversible reaction to silence, not a
 * replacement for RPL's own removal).
 *
 * Deliberately does NOT touch the child's own uplink behavior: the child
 * still transmits on however many shards ITS OWN child count says it needs,
 * unaware the parent shrank its Rx side. If the child is genuinely still
 * alive and later sends again, the mismatch is caught -- and corrected --
 * the same way every other shard-count change already is here: the next
 * time the parent hears that child's EB and finds its reported count no
 * longer matches c->shard_count, cg_child_eb_input() restores it directly.
 * Some frames sent on a just-reclaimed shard in that window are lost, the
 * same bounded, one-EB-period class of loss this design already accepts
 * whenever a shard count needs to grow (see update_child_relay_rx()'s own
 * comment on the bootstrap default). CG_IDLE_CHECK_PERIOD and the EWMA
 * decay shift below are chosen so this only fires after several minutes of
 * true silence, not a single missed application-layer send. */
#ifndef CG_CONF_IDLE_RECLAIM
#define CG_IDLE_RECLAIM 0
#else
#define CG_IDLE_RECLAIM CG_CONF_IDLE_RECLAIM
#endif

#if CG_IDLE_RECLAIM
/* How often we sample "did we hear from this child since the last check".
 * Kept well above the EB period (~12-16s measured) and the application
 * send interval used throughout this paper's evaluation (30s), so a single
 * missed EB or a single missed send never counts as idle on its own. */
#ifndef CG_CONF_IDLE_CHECK_PERIOD
#define CG_IDLE_CHECK_PERIOD (CLOCK_SECOND * 120)
#else
#define CG_IDLE_CHECK_PERIOD CG_CONF_IDLE_CHECK_PERIOD
#endif

/* Asymmetric like the shard-count EWMA above, but in the opposite sense:
 * hearing from a child snaps its activity estimate back up to fully-active
 * immediately (shift 0 -- one real packet is enough evidence, no reason to
 * hesitate), while silence decays it slowly (shift 2, weight 1/4 per check)
 * so a handful of missed checks in a row -- not one -- is what it takes to
 * cross the reclaim threshold. At the defaults below (period 120s, decay
 * shift 2, threshold 64/256) that is 5 consecutive idle checks, i.e. about
 * 10 minutes of true silence. */
#ifndef CG_CONF_IDLE_EWMA_DECAY_SHIFT
#define CG_IDLE_EWMA_DECAY_SHIFT 2
#else
#define CG_IDLE_EWMA_DECAY_SHIFT CG_CONF_IDLE_EWMA_DECAY_SHIFT
#endif

#ifndef CG_CONF_IDLE_RECLAIM_THRESHOLD
#define CG_IDLE_RECLAIM_THRESHOLD 64 /* out of 256 (Q8): 25% recent activity */
#else
#define CG_IDLE_RECLAIM_THRESHOLD CG_CONF_IDLE_RECLAIM_THRESHOLD
#endif
#endif /* CG_IDLE_RECLAIM */

/* Load bonus (default 0, off): everything above only ever grows a node's
 * own shard count because its own DIRECT CHILD COUNT grew -- a node that
 * has no more children than before, but is genuinely relaying or
 * generating a lot of real traffic, has no way to ask for more than its
 * structural minimum. This adds exactly one more reason to ask for more:
 * this node's OWN recent send activity toward its OWN parent, smoothed and
 * asymmetric the same way as everything else in this file.
 *
 * Deliberately scoped to be safe against the exact failure this whole
 * design replaced (see the file's own header comment and "Root Cause" in
 * the companion paper): that failure came from signaling a WHOLE-SUBTREE
 * aggregate, whose rate of change grows with subtree size and therefore
 * with network scale. What is measured here is only this one node's own
 * local send activity -- bounded by how much traffic THIS node personally
 * has, never by how large its subtree is or how the network happens to be
 * sized. It rides the identical one-hop EB channel and the identical
 * shard-count field everything else here already uses; nothing new is
 * added to the wire. */
#ifndef CG_CONF_LOAD_BONUS
#define CG_LOAD_BONUS 0
#else
#define CG_LOAD_BONUS CG_CONF_LOAD_BONUS
#endif

#if CG_LOAD_BONUS
/* How often we fold "how much did I send this window" into the load EWMA. */
#ifndef CG_CONF_LOAD_CHECK_PERIOD
#define CG_LOAD_CHECK_PERIOD (CLOCK_SECOND * 60)
#else
#define CG_LOAD_CHECK_PERIOD CG_CONF_LOAD_CHECK_PERIOD
#endif

/* A node that sends this many (or more) of its own packets toward its
 * parent within one check window counts as "fully busy" (sample = 256/256)
 * for that window; fewer sends scale down proportionally, 0 sends = 0. */
#ifndef CG_CONF_LOAD_SATURATE_COUNT
#define CG_LOAD_SATURATE_COUNT 4
#else
#define CG_LOAD_SATURATE_COUNT CG_CONF_LOAD_SATURATE_COUNT
#endif

/* Asymmetric like everything else here, but tuned differently on purpose:
 * genuine, sustained demand should earn its extra shard reasonably fast
 * (attack shift 1, weight 1/2 per window) -- unlike the shard-count EWMA's
 * instant attack, this is not a correctness-critical direction (a node
 * without its bonus yet simply keeps using its structural minimum, exactly
 * as it always has -- no cell is lost, only a possible extra one is
 * delayed), so a little averaging first is cheap insurance against a
 * single busy window granting a shard that is not really needed. Losing
 * the bonus decays much slower still (shift 3, weight 1/8), so one quiet
 * window does not immediately give back capacity a node is still using
 * most of the time. */
#ifndef CG_CONF_LOAD_EWMA_ATTACK_SHIFT
#define CG_LOAD_EWMA_ATTACK_SHIFT 1
#else
#define CG_LOAD_EWMA_ATTACK_SHIFT CG_CONF_LOAD_EWMA_ATTACK_SHIFT
#endif

#ifndef CG_CONF_LOAD_EWMA_DECAY_SHIFT
#define CG_LOAD_EWMA_DECAY_SHIFT 3
#else
#define CG_LOAD_EWMA_DECAY_SHIFT CG_CONF_LOAD_EWMA_DECAY_SHIFT
#endif

#ifndef CG_CONF_LOAD_BUSY_THRESHOLD
#define CG_LOAD_BUSY_THRESHOLD 160 /* out of 256 (Q8): ~63% of saturate-count, sustained */
#else
#define CG_LOAD_BUSY_THRESHOLD CG_CONF_LOAD_BUSY_THRESHOLD
#endif
#endif /* CG_LOAD_BONUS */

/* Hold-before-use (default 0, off): closes the one remaining gap between
 * this design and the original proposal it started from. Everywhere
 * above, the instant this node's own shard count grows, update_uplink()/
 * update_root_adjacent() start using the new shard(s) right away -- before
 * this node's own next EB has told its parent about the increase. Every
 * one of the companion paper's measured results (Table~mainresults etc.)
 * used exactly that immediate-use behavior and still reached >=98.5% PDR,
 * because a shard-count increase is a comparatively rare event; this
 * option is for deployments that want the exposure closed anyway.
 *
 * Mechanism: cg_get_own_children() (called once per outgoing EB, from
 * tsch_packet_create_eb()) records what this node's own EB actually just
 * announced, in announced_shards. update_uplink()/update_root_adjacent()
 * then never claim more shards than announced_shards allows, regardless
 * of what cg_own_shard_count() itself would currently justify -- so a
 * fresh increase is only ever installed after it has actually gone out in
 * an EB at least once. A decrease is never held back this way (see the
 * clamp itself: it can only ever lower want_shards, never raise it), which
 * matches the same asymmetric reasoning already used throughout this file
 * -- only an increase has a real correctness cost from being used early.
 *
 * As with the companion paper's own hold-before-use discussion: TSCH EBs
 * are unacknowledged broadcasts, so "announced" here means "this node
 * transmitted an EB claiming this many shards," not "the parent is known
 * to have received it." If that specific EB is lost, the exposure this
 * closes is bounded to roughly one more EB period (the next EB re-states
 * the same count), not eliminated outright. */
#ifndef CG_CONF_HOLD_BEFORE_USE
#define CG_HOLD_BEFORE_USE 0
#else
#define CG_HOLD_BEFORE_USE CG_CONF_HOLD_BEFORE_USE
#endif

/* Asymmetric (fast-attack / slow-decay) time constants, replacing a single
 * symmetric CG_EWMA_SHIFT. The two directions of a shard-count change are
 * NOT equally costly to get wrong (Section "Root Cause" of the companion
 * paper): lagging on an INCREASE reproduces the original signaling-lag
 * defect exactly -- the parent is not yet listening on a shard the child
 * has already started using, and every frame on it is lost outright, no
 * retry possible. Lagging on a DECREASE costs only a briefly idle extra
 * receive shard -- the same, already-tolerated over-provisioning every
 * bootstrap default and every bootstrap race in this file already accepts
 * elsewhere. So: attack (shard count going up) uses a short time constant
 * -- CG_EWMA_ATTACK_SHIFT 0 means adopt the new value immediately, exactly
 * matching the direct-adoption design's behavior for the one direction
 * where responsiveness has a genuine correctness cost -- while decay (shard
 * count going down) keeps real smoothing, CG_EWMA_DECAY_SHIFT 2 (weight
 * 1/4), which absorbs a single noisy/corrupted EB under-report without
 * prematurely shrinking a still-needed receive shard. */
#ifndef CG_CONF_EWMA_ATTACK_SHIFT
#define CG_EWMA_ATTACK_SHIFT 0
#else
#define CG_EWMA_ATTACK_SHIFT CG_CONF_EWMA_ATTACK_SHIFT
#endif

#ifndef CG_CONF_EWMA_DECAY_SHIFT
#define CG_EWMA_DECAY_SHIFT 2
#else
#define CG_EWMA_DECAY_SHIFT CG_CONF_EWMA_DECAY_SHIFT
#endif

/* One entry per direct child. relay_rx[0..shard_count-1] are Rx cells at
 * HASH2(self, child) + shard spreading, dedicated to this one child's
 * traffic (its own, plus everything it relays for its own subtree, however
 * deep -- all of that rides through this same, single relationship).
 * shard_count here is never computed locally: it is exactly what this child
 * signaled about itself in its own EB (cg_child_eb_input()), since only the
 * child knows its own direct child count. Defaults to 1 (see update_child_
 * relay_rx()'s own comment) until the child's first EB is heard, since every
 * node's shard count is guaranteed to be at least 1 regardless of its own
 * child count. */
struct cg_child {
  linkaddr_t addr;
  uint8_t in_use;
  uint8_t shard_count;
#if CG_EWMA_SHARD_COUNT
  /* Q8 fixed-point running average of every shard count c has reported in
   * its EB, updated in cg_child_eb_input(); shard_count above tracks its
   * rounded value instead of the raw report under this build variant. */
  uint16_t shard_ewma_q8;
#endif
#if CG_IDLE_RECLAIM
  /* Set by cg_data_input() the moment real data is heard from this child;
   * cleared by idle_tick() every CG_IDLE_CHECK_PERIOD once it has been
   * folded into idle_ewma_q8 below. */
  uint8_t heard_since_check;
  /* Q8 fixed-point recent-activity estimate, 256 = just heard from it,
   * decaying toward 0 under sustained silence (see idle_tick()). */
  uint16_t idle_ewma_q8;
#endif
  struct tsch_link *relay_rx[CG_RELAY_SHARDS_MAX];
  /* Only ever used when this node is the DAG root and this child is one of
   * its root-adjacent neighbors -- see update_child_root_adjacent_rx(). */
  struct tsch_link *root_adjacent_rx[CG_RELAY_SHARDS_MAX];
};

static uint16_t slotframe_handle;
static struct tsch_slotframe *sf_short; /* uplink + relay_rx, every child */
static struct tsch_slotframe *sf_root;  /* root-adjacent cell(s) */

static struct tsch_link *l_uplink[CG_RELAY_SHARDS_MAX];
static struct tsch_link *l_root_adjacent[CG_RELAY_SHARDS_MAX];

static struct cg_child children[CG_MAX_CHILDREN];

#if CG_LOAD_BONUS
/* Node-wide, not per-child: this is about THIS node's own outgoing traffic
 * toward its OWN parent, not anything about any specific child. Counted in
 * select_packet() every time a real data frame for our parent is matched
 * to one of our own cells; folded into own_load_ewma_q8 and, if that
 * estimate is sustained above CG_LOAD_BUSY_THRESHOLD, own_load_bonus is set
 * to 1 and cg_own_shard_count() adds it on top of the structural count. */
static uint16_t own_load_tx_count;
static uint16_t own_load_ewma_q8;
static uint8_t own_load_bonus;
#endif

#if CG_HOLD_BEFORE_USE
/* Node-wide: the largest shard count this node has actually stated in one
 * of its own transmitted EBs so far. update_uplink()/update_root_adjacent()
 * never claim more than this, regardless of what cg_own_shard_count()
 * currently justifies -- see the constant block's own comment. */
static uint8_t announced_shards;
#endif

/*---------------------------------------------------------------------------*/
static uint16_t
cg_slot(uint32_t hash, uint16_t period)
{
  return period > 0 ? (uint16_t)(hash % period) : 0;
}
/*---------------------------------------------------------------------------*/
static uint16_t
cg_channel(uint32_t hash)
{
  return tsch_hopping_sequence_length.val > 0 ? (uint16_t)(hash % tsch_hopping_sequence_length.val) : 0;
}
/*---------------------------------------------------------------------------*/
/* Local collision avoidance for sf_short (own uplink shards + every tracked
 * child's relay_rx shards). Root cause this exists for (confirmed live via
 * COOJA.testlog analysis, nc_N25_seed1): tsch_schedule_add_link()'s own
 * comment is explicit -- "We currently support only one link per timeslot
 * in a given slotframe" -- and with do_remove=1 (what every add_link() call
 * in this file passes), it finds and silently evicts whatever is ALREADY
 * installed at that TIMESLOT, "We don't check for channel offset because
 * only one link per timeslot is allowed" (tsch-schedule.c). This means the
 * hash formula's channel dimension (mod N_c) provides NO collision
 * protection at all in this TSCH schedule implementation -- two of THIS
 * NODE'S OWN cells (e.g. its own uplink shard and one child's relay_rx
 * shard) that hash to the same timeslot but different channels will
 * silently evict each other, repeatedly, every time either side's cell is
 * recomputed -- confirmed live: the same two relationships evicting each
 * other back and forth at the identical timeslot, 3 separate times over
 * one run. Worse, the "retarget in place" code path in update_uplink() et
 * al. does not go through add_link()/remove_link() at all (it mutates an
 * existing struct tsch_link's fields directly for efficiency) -- if THAT
 * cell's own pointer was itself the one silently evicted and freed by some
 * OTHER cell's add_link() call, the next retarget writes through a
 * dangling pointer into freed (and, via memb's free-list reuse, likely
 * already reallocated) memory. Checking local occupancy before ever
 * finalizing a position prevents both problems at once, since it is
 * exactly the undetected collision itself that causes the eviction -- if
 * this node's own calls never contend for the same timeslot, add_link()
 * never has anything of ours to evict, and no pointer in l_uplink[]/
 * children[].relay_rx[] is ever invalidated out from under this file's own
 * bookkeeping. Does not and cannot help with collisions against a
 * completely different, remote node's cells (no coordination exists to
 * detect those, and the paper's own design explicitly tolerates that
 * class of collision) -- this only closes the much more damaging
 * self-inflicted case, which is entirely decidable from state this node
 * already has locally, at zero signaling cost. */
static int
cg_short_timeslot_taken(uint16_t ts, const struct tsch_link *exclude)
{
  int i, k;
  for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
    if(l_uplink[k] != NULL && l_uplink[k] != exclude && l_uplink[k]->timeslot == ts) {
      return 1;
    }
  }
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(!children[i].in_use) {
      continue;
    }
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(children[i].relay_rx[k] != NULL && children[i].relay_rx[k] != exclude
         && children[i].relay_rx[k]->timeslot == ts) {
        return 1;
      }
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Same idea as cg_short_timeslot_taken(), for sf_root (own root-adjacent
 * shards + every tracked child's root_adjacent_rx shards) -- a completely
 * separate slotframe/timeslot space from sf_short, so checked separately. */
static int
cg_root_timeslot_taken(uint16_t ts, const struct tsch_link *exclude)
{
  int i, k;
  for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
    if(l_root_adjacent[k] != NULL && l_root_adjacent[k] != exclude && l_root_adjacent[k]->timeslot == ts) {
      return 1;
    }
  }
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(!children[i].in_use) {
      continue;
    }
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(children[i].root_adjacent_rx[k] != NULL && children[i].root_adjacent_rx[k] != exclude
         && children[i].root_adjacent_rx[k]->timeslot == ts) {
        return 1;
      }
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Shared probing logic: starting from this shard's natural hash position
 * (probe 0), if it's locally taken by one of this node's OWN other cells,
 * deterministically try alternate positions from the SAME relationship's
 * own hash sequence (jumping a full CG_RELAY_SHARDS_MAX steps ahead each
 * time, so it can never coincide with this same relationship's own other
 * shards, which occupy k=0..CG_RELAY_SHARDS_MAX-1) until a free one is
 * found or a bounded number of attempts is exhausted (falls back to the
 * natural position if every probe is also taken -- extremely unlikely
 * given L is always well above this node's total local shard count, and
 * strictly better than not trying at all). Deterministic (no randomness,
 * no extra signaling) so every node reaches the same answer independently
 * from the same local state, exactly like the base hash formula itself. */
#define CG_COLLISION_PROBE_ATTEMPTS 4
static void
cg_resolve_position(uint32_t h, int k, uint16_t period,
                     int (*taken)(uint16_t, const struct tsch_link *),
                     const struct tsch_link *exclude,
                     uint16_t *out_ts, uint16_t *out_ch)
{
  int probe;
  uint16_t ts = 0, ch = 0;
  for(probe = 0; probe < CG_COLLISION_PROBE_ATTEMPTS; probe++) {
    uint32_t trial_k = (uint32_t)k + (uint32_t)probe * CG_RELAY_SHARDS_MAX;
    uint32_t kh = h + trial_k * 0x85EBCA6Bu;
    ts = cg_slot(kh, period);
    ch = cg_channel(kh);
    if(!taken(ts, exclude)) {
      break;
    }
  }
  *out_ts = ts;
  *out_ch = ch;
}
/*---------------------------------------------------------------------------*/
#if CG_RANK_CAP
/* This node's own cap on shard count: CG_RELAY_SHARDS_NEAR_ROOT if our own
 * RPL rank is at or below CG_RANK_CAP_THRESHOLD, else the ordinary
 * CG_RELAY_SHARDS_FAR. Rank is already computed by RPL for routing; no
 * signaling of any kind is needed to know it. A node with no DAG yet
 * (rpl_get_any_dag() returns NULL, e.g. during initial bootstrap) gets the
 * conservative, ordinary cap until it has one. */
static uint8_t
cg_effective_cap(void)
{
  static uint8_t last_logged;
  uint8_t cap;
  rpl_dag_t *dag = rpl_get_any_dag();
  if(dag != NULL && dag->rank <= CG_RANK_CAP_THRESHOLD) {
    cap = CG_RELAY_SHARDS_NEAR_ROOT;
  } else {
    cap = CG_RELAY_SHARDS_FAR;
  }
  if(cap != last_logged) {
    LOG_INFO("cg: rank cap now %u (rank=%u)\n", cap, dag != NULL ? dag->rank : 0);
    last_logged = cap;
  }
  return cap;
}
#endif /* CG_RANK_CAP */
/*---------------------------------------------------------------------------*/
/* This node's own shard count: (own direct RPL child count) + 1 for its own
 * traffic, capped at cg_effective_cap() (CG_RANK_CAP) or the flat
 * CG_RELAY_SHARDS_MAX otherwise. Entirely local -- Orchestra's own
 * child_added()/child_removed() callbacks already maintain children[], no
 * routing-table access or any other signal needed. Sizes this node's own
 * uplink/root-adjacent cells to its parent (update_uplink()/update_root_
 * adjacent() below) and is what this node advertises in its own EB
 * (cg_get_own_children()) for its parent to size a matching Rx cell. */
static uint8_t
cg_own_shard_count(void)
{
  int i;
  uint8_t n = 0;
  uint8_t cap;
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(children[i].in_use) {
      n++;
    }
  }
  n++; /* +1 for this node's own traffic */
#if CG_LOAD_BONUS
  n += own_load_bonus; /* +1 more if sustained real load earned it (load_tick()) */
#endif
#if CG_RANK_CAP
  cap = cg_effective_cap();
#else
  cap = CG_RELAY_SHARDS_MAX;
#endif
  return n > cap ? cap : n;
}
/*---------------------------------------------------------------------------*/
static struct cg_child *
find_child(const linkaddr_t *addr)
{
  int i;
  if(addr == NULL || linkaddr_cmp(addr, &linkaddr_null)) {
    return NULL;
  }
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(children[i].in_use && linkaddr_cmp(&children[i].addr, addr)) {
      return &children[i];
    }
  }
  return NULL;
}
/*---------------------------------------------------------------------------*/
static struct cg_child *
alloc_child(const linkaddr_t *addr)
{
  int i, j;
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(!children[i].in_use) {
      children[i].in_use = 1;
      linkaddr_copy(&children[i].addr, addr);
      children[i].shard_count = 0;
#if CG_EWMA_SHARD_COUNT
      children[i].shard_ewma_q8 = 1 << 8; /* matches the shard_count=1 bootstrap default below */
#endif
#if CG_IDLE_RECLAIM
      /* Start "fully active": a just-joined child gets the same grace
       * period every other bootstrap default in this file already gives
       * it, rather than becoming reclaim-eligible before it has ever had a
       * chance to send anything. */
      children[i].heard_since_check = 1;
      children[i].idle_ewma_q8 = 1 << 8;
#endif
      for(j = 0; j < CG_RELAY_SHARDS_MAX; j++) {
        children[i].relay_rx[j] = NULL;
        children[i].root_adjacent_rx[j] = NULL;
      }
      return &children[i];
    }
  }
  LOG_ERR("cg: no free child slot for ");
  LOG_ERR_LLADDR(addr);
  LOG_ERR_("\n");
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* This node's own uplink cell(s) to its parent -- standard cells, regular
 * timing, explicit ACK. Shard count is this node's own (cg_own_shard_count()),
 * position HASH2(parent, self) + k*kappa for k=0..shard_count-1: every one
 * of this node's own children (and, transitively, everything they relay)
 * rides through this same set of cells -- there is no separate cell per
 * child or per descendant on the Tx side. */
static void
update_uplink(void)
{
  uint8_t want_shards;
  int k;
#if CG_HOLD_BEFORE_USE
  static uint8_t last_held;
#endif
  if(sf_short == NULL || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || tsch_roots_is_root(&orchestra_parent_linkaddr) || !orchestra_parent_knows_us) {
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(l_uplink[k] != NULL) {
        tsch_schedule_remove_link(sf_short, l_uplink[k]);
        l_uplink[k] = NULL;
      }
    }
    return;
  }
  want_shards = cg_own_shard_count();
#if CG_HOLD_BEFORE_USE
  if(want_shards > announced_shards) {
    if(want_shards != last_held) {
      LOG_INFO("cg: holding uplink at %u shards (want %u, not yet announced)\n",
               announced_shards, want_shards);
      last_held = want_shards;
    }
    want_shards = announced_shards; /* increase not yet stated in an EB -- hold */
  }
#endif
  {
    uint32_t h = ORCHESTRA_LINKADDR_HASH2(&orchestra_parent_linkaddr, &linkaddr_node_addr);
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(k >= want_shards) {
        if(l_uplink[k] != NULL) {
          tsch_schedule_remove_link(sf_short, l_uplink[k]);
          l_uplink[k] = NULL;
        }
        continue;
      }
      {
        uint16_t ts, ch;
        /* Validity check before ever touching an existing pointer: if some
         * OTHER local cell's add_link() call silently evicted this one
         * since we last touched it (see cg_short_timeslot_taken()'s own
         * comment), l_uplink[k] is a dangling pointer into freed (and,
         * via memb's free-list reuse, likely already reallocated) memory
         * -- confirm it is still the link actually installed at its own
         * last-known timeslot before dereferencing it any further; if not,
         * treat it as gone and fall through to a fresh add instead of
         * writing through a stale pointer. */
        if(l_uplink[k] != NULL
           && tsch_schedule_get_link_by_timeslot(sf_short, l_uplink[k]->timeslot) != l_uplink[k]) {
          l_uplink[k] = NULL;
        }
        cg_resolve_position(h, k, TSCH_IA_SFS_SIZE, cg_short_timeslot_taken, l_uplink[k], &ts, &ch);
        if(l_uplink[k] == NULL) {
          l_uplink[k] = tsch_schedule_add_link(sf_short, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                                LINK_TYPE_NORMAL, &orchestra_parent_linkaddr, ts, ch, 1);
        } else {
          /* Retarget in place rather than remove+re-add: our own parent may
           * have changed since this link was created (reconcile_parent_
           * address() no longer tears these down on a parent switch -- see
           * its own comment), and HASH2 above already folds the *current*
           * orchestra_parent_linkaddr in, so the position is already correct
           * here too; only the destination address field needs an explicit
           * copy to match. */
          linkaddr_copy(&l_uplink[k]->addr, &orchestra_parent_linkaddr);
          l_uplink[k]->timeslot = ts;
          l_uplink[k]->channel_offset = ch;
        }
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Our own root-adjacent cell(s), used instead of UPLINK when our parent is
 * the root: same sizing rule, own (typically shorter) slotframe/period. */
static void
update_root_adjacent(void)
{
  uint8_t want_shards;
  int k;
#if CG_HOLD_BEFORE_USE
  static uint8_t last_held;
#endif
  if(sf_root == NULL || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || !tsch_roots_is_root(&orchestra_parent_linkaddr) || !orchestra_parent_knows_us) {
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(l_root_adjacent[k] != NULL) {
        tsch_schedule_remove_link(sf_root, l_root_adjacent[k]);
        l_root_adjacent[k] = NULL;
      }
    }
    return;
  }
  want_shards = cg_own_shard_count();
#if CG_HOLD_BEFORE_USE
  if(want_shards > announced_shards) {
    if(want_shards != last_held) {
      LOG_INFO("cg: holding root-adjacent at %u shards (want %u, not yet announced)\n",
               announced_shards, want_shards);
      last_held = want_shards;
    }
    want_shards = announced_shards; /* increase not yet stated in an EB -- hold */
  }
#endif
  {
    uint32_t h = ORCHESTRA_LINKADDR_HASH2(&orchestra_parent_linkaddr, &linkaddr_node_addr);
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(k >= want_shards) {
        if(l_root_adjacent[k] != NULL) {
          tsch_schedule_remove_link(sf_root, l_root_adjacent[k]);
          l_root_adjacent[k] = NULL;
        }
        continue;
      }
      {
        uint16_t ts, ch;
        if(l_root_adjacent[k] != NULL
           && tsch_schedule_get_link_by_timeslot(sf_root, l_root_adjacent[k]->timeslot) != l_root_adjacent[k]) {
          l_root_adjacent[k] = NULL;
        }
        cg_resolve_position(h, k, ORCHESTRA_IA_ROOT_PERIOD, cg_root_timeslot_taken, l_root_adjacent[k], &ts, &ch);
        if(l_root_adjacent[k] == NULL) {
          l_root_adjacent[k] = tsch_schedule_add_link(sf_root, LINK_OPTION_TX | LINK_OPTION_SHARED,
                                                       LINK_TYPE_NORMAL, &orchestra_parent_linkaddr, ts, ch, 1);
        } else {
          linkaddr_copy(&l_root_adjacent[k]->addr, &orchestra_parent_linkaddr);
          l_root_adjacent[k]->timeslot = ts;
          l_root_adjacent[k]->channel_offset = ch;
        }
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Rx cell(s) matching one direct child's own uplink cell(s): child c
 * transmits at HASH2(self, c) (from c's own point of view, HASH2(parent,
 * self) -- self here IS the parent, so this matches exactly), shard count
 * c->shard_count, which is never computed locally: it is exactly what c
 * signaled about itself in its own EB (cg_child_eb_input()). Defaults to 1
 * (installed immediately on child_added(), before any EB has been heard)
 * since every node's own shard count is guaranteed to be at least 1
 * (0 children of its own + 1) regardless of c's own child count -- so the
 * common case (a leaf child) needs no bootstrap wait at all; only a second
 * or later shard, implying c has children of its own, waits on c's EB. */
static void
update_child_relay_rx(struct cg_child *c)
{
  int k;
  uint8_t want_shards = c->shard_count > 0 ? c->shard_count : 1;
  if(sf_short == NULL) {
    return;
  }
  {
    uint32_t h = ORCHESTRA_LINKADDR_HASH2(&linkaddr_node_addr, &c->addr);
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(k >= want_shards) {
        if(c->relay_rx[k] != NULL) {
          tsch_schedule_remove_link(sf_short, c->relay_rx[k]);
          c->relay_rx[k] = NULL;
        }
        continue;
      }
      {
        uint16_t ts, ch;
        if(c->relay_rx[k] != NULL
           && tsch_schedule_get_link_by_timeslot(sf_short, c->relay_rx[k]->timeslot) != c->relay_rx[k]) {
          c->relay_rx[k] = NULL;
        }
        cg_resolve_position(h, k, TSCH_IA_SFS_SIZE, cg_short_timeslot_taken, c->relay_rx[k], &ts, &ch);
        if(c->relay_rx[k] == NULL) {
          c->relay_rx[k] = tsch_schedule_add_link(sf_short, LINK_OPTION_RX, LINK_TYPE_NORMAL,
                                                   &c->addr, ts, ch, 1);
        } else {
          c->relay_rx[k]->timeslot = ts;
          c->relay_rx[k]->channel_offset = ch;
        }
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
/* The DAG root's own Rx side of a root-adjacent child's ROOT_ADJACENT
 * cell(s) -- without this, sf_root never has any link installed on the
 * root's end at all, so a root-adjacent child's frames arrive on a slot/
 * channel the root was never listening on and are silently lost every
 * single time. Same sizing/defaulting rule as update_child_relay_rx(). */
static void
update_child_root_adjacent_rx(struct cg_child *c)
{
  int k;
  uint8_t want_shards = c->shard_count > 0 ? c->shard_count : 1;
  if(sf_root == NULL || !tsch_is_coordinator) {
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(c->root_adjacent_rx[k] != NULL) {
        tsch_schedule_remove_link(sf_root, c->root_adjacent_rx[k]);
        c->root_adjacent_rx[k] = NULL;
      }
    }
    return;
  }
  {
    uint32_t h = ORCHESTRA_LINKADDR_HASH2(&linkaddr_node_addr, &c->addr);
    for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
      if(k >= want_shards) {
        if(c->root_adjacent_rx[k] != NULL) {
          tsch_schedule_remove_link(sf_root, c->root_adjacent_rx[k]);
          c->root_adjacent_rx[k] = NULL;
        }
        continue;
      }
      {
        uint16_t ts, ch;
        if(c->root_adjacent_rx[k] != NULL
           && tsch_schedule_get_link_by_timeslot(sf_root, c->root_adjacent_rx[k]->timeslot) != c->root_adjacent_rx[k]) {
          c->root_adjacent_rx[k] = NULL;
        }
        cg_resolve_position(h, k, ORCHESTRA_IA_ROOT_PERIOD, cg_root_timeslot_taken, c->root_adjacent_rx[k], &ts, &ch);
        if(c->root_adjacent_rx[k] == NULL) {
          c->root_adjacent_rx[k] = tsch_schedule_add_link(sf_root, LINK_OPTION_RX, LINK_TYPE_NORMAL,
                                                           &c->addr, ts, ch, 1);
        } else {
          c->root_adjacent_rx[k]->timeslot = ts;
          c->root_adjacent_rx[k]->channel_offset = ch;
        }
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Installs/updates whichever of update_child_relay_rx()/update_child_root_
 * adjacent_rx() applies to this node's own current role. */
static void
update_child(struct cg_child *c)
{
  if(c == NULL) {
    return;
  }
  if(tsch_is_coordinator) {
    update_child_root_adjacent_rx(c);
  } else {
    update_child_relay_rx(c);
  }
}
/*---------------------------------------------------------------------------*/
#define CG_BOOTSTRAP_CHECK_PERIOD (CLOCK_SECOND / 4)
static struct ctimer bootstrap_timer;
static uint8_t last_parent_knows_us;
/* orchestra_parent_knows_us flipping 0->1 is the one state transition that
 * needs UPLINK/ROOT_ADJACENT installed but isn't otherwise announced by any
 * callback this rule implements -- a parent switch installs these at the
 * moment of the switch, which is almost always *before* the very first DAO
 * to that parent has had a chance to succeed. Without this periodic catch-
 * up, a node would never install its own uplink cell at all once orchestra_
 * parent_knows_us does eventually become true.
 *
 * Without CG_HOLD_BEFORE_USE, this is the only thing that needs periodic
 * polling here: every other value in this file only ever changes on an
 * actual event and is recomputed directly from that event, not from a
 * poll. With CG_HOLD_BEFORE_USE on, this tick also re-applies
 * update_uplink()/update_root_adjacent() every period regardless of the
 * parent_knows_us edge, since that is how a newly-announced shard-count
 * increase (cg_get_own_children(), gated in update_uplink() itself) gets
 * picked up and actually installed once it clears the hold -- deliberately
 * from this already-existing, ordinary ctimer context rather than directly
 * inside cg_get_own_children() (called from deep in TSCH's own EB-building
 * path, not a safe place to mutate the schedule from). */
static void
bootstrap_tick(void *ptr)
{
  if(orchestra_parent_knows_us && !last_parent_knows_us) {
    update_uplink();
    update_root_adjacent();
  }
#if CG_HOLD_BEFORE_USE
  if(orchestra_parent_knows_us) {
    update_uplink();
    update_root_adjacent();
  }
#endif
  last_parent_knows_us = orchestra_parent_knows_us;
  ctimer_set(&bootstrap_timer, CG_BOOTSTRAP_CHECK_PERIOD, bootstrap_tick, NULL);
}
/*---------------------------------------------------------------------------*/
#if CG_IDLE_RECLAIM
static struct ctimer idle_timer;
/* TSCH_CALLBACK_UNICAST_DATA_INPUT: fires from tsch_rx_process_pending() for
 * every incoming DATA-type frame, right after its payload has been copied
 * into packetbuf and before it is passed up to packet_input(). We reuse it
 * as a "heard REAL traffic from src" tap for idle_tick() below.
 *
 * We originally tapped TSCH_CALLBACK_DO_NACK for this instead (fires
 * earlier, on any ack-required reception) and found by live-testing this
 * exact mechanism that it never actually detected a genuinely idle child:
 * TSCH's own keepalive frames are ordinary, ack-required, empty-payload
 * DATA frames sent specifically BECAUSE a node has had nothing real to
 * send for a while -- so a truly idle child, from the application's point
 * of view, looks maximally *active* to a tap that cannot see payload
 * length. TSCH_CALLBACK_UNICAST_DATA_INPUT fires after packetbuf is
 * populated, so packetbuf_datalen() lets us tell a real packet (own
 * traffic or relayed) from an empty keepalive and only count the former. */
void
cg_data_input(const linkaddr_t *source)
{
  struct cg_child *c;
  if(packetbuf_datalen() == 0) {
    return; /* keepalive or other empty frame -- not evidence of real activity */
  }
  c = find_child(source);
  if(c != NULL) {
    c->heard_since_check = 1;
  }
}
/*---------------------------------------------------------------------------*/
/* Every CG_IDLE_CHECK_PERIOD, for every tracked child: fold whether we
 * heard from it this period into idle_ewma_q8 (fast up on real activity,
 * slow decay under silence -- see the constant block's own comment), and
 * if that estimate has dropped below CG_IDLE_RECLAIM_THRESHOLD, shrink the
 * child back to its guaranteed-minimum 1 shard, freeing the rest. Never
 * removes the child outright -- that stays RPL's decision via
 * child_removed() -- and never shrinks a child already at 1. */
static void
idle_tick(void *ptr)
{
  int i;
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    struct cg_child *c = &children[i];
    uint16_t sample_q8;
    if(!c->in_use) {
      continue;
    }
    sample_q8 = c->heard_since_check ? (1 << 8) : 0;
    if(sample_q8 >= c->idle_ewma_q8) {
      c->idle_ewma_q8 = sample_q8; /* attack: instant */
    } else {
      c->idle_ewma_q8 += (int32_t)(sample_q8 - c->idle_ewma_q8) >> CG_IDLE_EWMA_DECAY_SHIFT;
    }
    c->heard_since_check = 0;
    LOG_INFO("cg: idle_tick DIAG child shard_count=%u ewma=%u/256 -- ", c->shard_count, c->idle_ewma_q8);
    LOG_INFO_LLADDR(&c->addr);
    LOG_INFO_("\n");
    if(c->idle_ewma_q8 < CG_IDLE_RECLAIM_THRESHOLD && c->shard_count > 1) {
      LOG_INFO("cg: reclaiming idle child's shards (was %u) -- ", c->shard_count);
      LOG_INFO_LLADDR(&c->addr);
      LOG_INFO_("\n");
      c->shard_count = 1;
      update_child(c);
    }
  }
  ctimer_set(&idle_timer, CG_IDLE_CHECK_PERIOD, idle_tick, NULL);
}
#endif /* CG_IDLE_RECLAIM */
/*---------------------------------------------------------------------------*/
#if CG_LOAD_BONUS
static struct ctimer load_timer;
/* Every CG_LOAD_CHECK_PERIOD: fold this window's own_load_tx_count into
 * own_load_ewma_q8 (fast-ish attack, slow decay -- see the constant
 * block's own comment), and if that estimate has stayed above
 * CG_LOAD_BUSY_THRESHOLD, set own_load_bonus so cg_own_shard_count()
 * starts including it. Only actually resizes our own uplink/root-adjacent
 * cells when the bonus bit itself changes -- most windows it won't -- and
 * the new count reaches our parent the normal way, through our own next
 * EB, exactly like a real child-count change already does. */
static void
load_tick(void *ptr)
{
  uint16_t sample_q8;
  uint8_t new_bonus;

  sample_q8 = (uint16_t)((uint32_t)MIN(own_load_tx_count, CG_LOAD_SATURATE_COUNT) * 256
                          / CG_LOAD_SATURATE_COUNT);
  if(sample_q8 >= own_load_ewma_q8) {
    own_load_ewma_q8 += (int32_t)(sample_q8 - own_load_ewma_q8) >> CG_LOAD_EWMA_ATTACK_SHIFT;
  } else {
    own_load_ewma_q8 += (int32_t)(sample_q8 - own_load_ewma_q8) >> CG_LOAD_EWMA_DECAY_SHIFT;
  }
  own_load_tx_count = 0;

  new_bonus = own_load_ewma_q8 >= CG_LOAD_BUSY_THRESHOLD ? 1 : 0;
  if(new_bonus != own_load_bonus) {
    LOG_INFO("cg: own load bonus %s (ewma=%u/256)\n", new_bonus ? "granted" : "released",
             own_load_ewma_q8);
    own_load_bonus = new_bonus;
    update_uplink();
    update_root_adjacent();
  }
  ctimer_set(&load_timer, CG_LOAD_CHECK_PERIOD, load_tick, NULL);
}
#endif /* CG_LOAD_BONUS */
/*---------------------------------------------------------------------------*/
static void
child_added(const linkaddr_t *addr)
{
  struct cg_child *c = find_child(addr);
  if(c == NULL) {
    c = alloc_child(addr);
  }
  if(c == NULL) {
    /* Table exhausted (CG_MAX_CHILDREN children already tracked) --
     * alloc_child() already logged this. This child was never recorded, so
     * it will never get a matching receive cell from us and its traffic
     * (and its whole subtree's) is lost outright until CG_CONF_MAX_CHILDREN
     * is raised; there is nothing further we can safely do for it here. Our
     * own tracked child count is unchanged, so no uplink/root-adjacent
     * resize is needed either. */
    return;
  }
  update_child(c);
  /* Our own child count just changed, which is what our own shard count is
   * based on -- resize our own uplink/root-adjacent cells to match. */
  update_uplink();
  update_root_adjacent();
}
/*---------------------------------------------------------------------------*/
static void
child_removed(const linkaddr_t *addr)
{
  struct cg_child *c = find_child(addr);
  int k;
  if(c == NULL) {
    return;
  }
  for(k = 0; k < CG_RELAY_SHARDS_MAX; k++) {
    if(c->relay_rx[k] != NULL) {
      tsch_schedule_remove_link(sf_short, c->relay_rx[k]);
      c->relay_rx[k] = NULL;
    }
    if(c->root_adjacent_rx[k] != NULL) {
      tsch_schedule_remove_link(sf_root, c->root_adjacent_rx[k]);
      c->root_adjacent_rx[k] = NULL;
    }
  }
  tsch_queue_free_packets_to(addr);
  c->in_use = 0;
  update_uplink();
  update_root_adjacent();
}
/*---------------------------------------------------------------------------*/
/* Common re-target logic for both an actual TSCH time-source change
 * (new_time_source(), below) and any future periodic reconciliation. A
 * no-op whenever new_addr already matches orchestra_parent_linkaddr.
 * Deliberately does not touch any child's relay_rx/root_adjacent_rx cells:
 * their position, HASH2(self, child), does not depend on our own parent at
 * all, so a parent switch never requires touching them. */
static void
reconcile_parent_address(const linkaddr_t *new_addr)
{
  if(!linkaddr_cmp(new_addr != NULL ? new_addr : &linkaddr_null, &orchestra_parent_linkaddr)) {
    const linkaddr_t *old_addr = &orchestra_parent_linkaddr;

    if(new_addr != NULL) {
      linkaddr_copy(&orchestra_parent_linkaddr, new_addr);
    } else {
      linkaddr_copy(&orchestra_parent_linkaddr, &linkaddr_null);
    }
    tsch_queue_free_packets_to(old_addr);

    update_uplink();
    update_root_adjacent();
  }
}
/*---------------------------------------------------------------------------*/
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
    return;
  }
  if(!linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     && linkaddr_cmp(root, &orchestra_parent_linkaddr)) {
    update_uplink();
    update_root_adjacent();
  }
}
/*---------------------------------------------------------------------------*/
/* Every frame to our parent -- self-originated or relayed for any of our
 * own children/descendants -- uses the same cell(s): there is no separate
 * per-child Tx cell to choose between any more, so (unlike the earlier,
 * per-descendant design) select_packet() needs no bookkeeping about which
 * neighbor a frame was last received from. */
static int
select_packet(uint16_t *slotframe, uint16_t *timeslot, uint16_t *channel_offset)
{
  const linkaddr_t *dst = packetbuf_addr(PACKETBUF_ADDR_RECEIVER);
  if(packetbuf_attr(PACKETBUF_ATTR_FRAME_TYPE) == FRAME802154_DATAFRAME
     && linkaddr_cmp(dst, &orchestra_parent_linkaddr)
     && !linkaddr_cmp(dst, &linkaddr_null)) {
    if(tsch_roots_is_root(&orchestra_parent_linkaddr)) {
      if(l_root_adjacent[0] != NULL) {
        *slotframe = sf_root->handle;
        *timeslot = 0xffff;
        *channel_offset = 0xffff;
#if CG_LOAD_BONUS
        own_load_tx_count++;
#endif
        return 1;
      }
      return 0;
    }
    if(l_uplink[0] != NULL) {
      *slotframe = sf_short->handle;
      *timeslot = 0xffff;
      *channel_offset = 0xffff;
#if CG_LOAD_BONUS
      own_load_tx_count++;
#endif
      return 1;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
static void
init(uint16_t sf_handle)
{
  int i;
  slotframe_handle = sf_handle;
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    children[i].in_use = 0;
  }
  sf_short = tsch_schedule_add_slotframe(slotframe_handle, TSCH_IA_SFS_SIZE);
  sf_root = tsch_schedule_add_slotframe(slotframe_handle | 0x4000, ORCHESTRA_IA_ROOT_PERIOD);
  if(sf_short == NULL) {
    LOG_ERR("cg: failed to add the shared slotframe\n");
  }
  if(sf_root == NULL) {
    LOG_ERR("cg: failed to add the root-adjacent slotframe\n");
  }
  ctimer_set(&bootstrap_timer, CG_BOOTSTRAP_CHECK_PERIOD, bootstrap_tick, NULL);
#if CG_IDLE_RECLAIM
  ctimer_set(&idle_timer, CG_IDLE_CHECK_PERIOD, idle_tick, NULL);
#endif
#if CG_LOAD_BONUS
  own_load_tx_count = 0;
  own_load_ewma_q8 = 0;
  own_load_bonus = 0;
  ctimer_set(&load_timer, CG_LOAD_CHECK_PERIOD, load_tick, NULL);
#endif
#if CG_HOLD_BEFORE_USE
  announced_shards = 1; /* matches the safe bootstrap default everywhere else */
#endif
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_OWN_CHILDREN: fired from tsch_packet_create_eb() to fetch
 * our own direct children's addresses for inclusion in our EB, reusing
 * orchestra-rule-implicit-ack.c's existing wire format exactly -- one byte
 * per child. Every entry carries the *same* value, this node's own overall
 * shard count (cg_own_shard_count()) -- not a per-child value, since there
 * is nothing to differentiate any more: this node has exactly one shard
 * count, used for its one uplink relationship with its own parent. */
uint8_t
cg_get_own_children(linkaddr_t *out, uint8_t *out_shard_count, uint8_t max_children)
{
  int i;
  uint8_t n = 0;
  uint8_t own_shards = cg_own_shard_count();
#if CG_HOLD_BEFORE_USE
  /* This EB is about to state own_shards -- record it as announced
   * regardless of whether we have any children entries below to write
   * (a leaf with no children still sends this in every one of its EBs,
   * and still needs its own single shard's increase, if any, held the
   * same way). */
  announced_shards = own_shards;
#endif
  for(i = 0; i < CG_MAX_CHILDREN && n < max_children; i++) {
    if(children[i].in_use) {
      linkaddr_copy(&out[n], &children[i].addr);
      out_shard_count[n] = own_shards;
      n++;
    }
  }
  return n;
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_CHILD_EB: fired from eb_input() for every received EB;
 * only acts if source is actually one of our own known children. Every
 * entry in the list carries the same value (see cg_get_own_children()'s own
 * comment) -- the source's own overall shard count -- so only the first
 * entry, if any, needs reading.
 *
 * Two build variants of what happens next, selected by CG_EWMA_SHARD_COUNT:
 *
 *   - Direct adoption (default, CG_EWMA_SHARD_COUNT=0): install exactly the
 *     value c just reported, the instant it is reported. This is the design
 *     documented at the top of this file and used for every result in the
 *     companion paper's main evaluation.
 *
 *   - EWMA-smoothed prediction (CG_EWMA_SHARD_COUNT=1): run every reported
 *     value through a Q8 fixed-point exponential moving average and install
 *     the *rounded* average instead of the raw report. The event that
 *     triggers a look (an EB arriving) is unchanged; only what is done with
 *     the value once received changes. Asymmetric by default (see
 *     CG_EWMA_ATTACK_SHIFT/CG_EWMA_DECAY_SHIFT above): a first, symmetric
 *     version of this (equal smoothing in both directions) measurably
 *     regressed delivery ratio and topology stability at 25+ nodes,
 *     structurally the same trade the paper's own Attempt 10 (commit-after-
 *     2s-of-consistent-readings hysteresis) already made and found to
 *     regress -- because symmetric smoothing delays a shard-count *increase*
 *     exactly as much as a decrease, and only the former has a correctness
 *     cost. The asymmetric version below adopts increases immediately
 *     (matching direct adoption's behavior for the dangerous direction) and
 *     only smooths decreases, which are safe to delay. */
void
cg_child_eb_input(const linkaddr_t *source, const linkaddr_t *children_list,
                  const uint8_t *children_shard_count, uint8_t num_children)
{
  struct cg_child *c = find_child(source);
  if(c == NULL || num_children == 0) {
    return;
  }
  {
    uint8_t reported = children_shard_count[0] > 0 ? children_shard_count[0] : 1;
#if CG_EWMA_SHARD_COUNT
    int32_t sample_q8 = (int32_t)reported << 8;
    int32_t ewma_q8 = (int32_t)c->shard_ewma_q8;
    uint8_t smoothed;
    if(sample_q8 >= ewma_q8) {
      ewma_q8 += (sample_q8 - ewma_q8) >> CG_EWMA_ATTACK_SHIFT;
    } else {
      ewma_q8 += (sample_q8 - ewma_q8) >> CG_EWMA_DECAY_SHIFT;
    }
    c->shard_ewma_q8 = (uint16_t)ewma_q8;
    smoothed = (uint8_t)((ewma_q8 + 128) >> 8);
    if(smoothed < 1) {
      smoothed = 1;
    } else if(smoothed > CG_RELAY_SHARDS_MAX) {
      smoothed = CG_RELAY_SHARDS_MAX;
    }
    if(c->shard_count != smoothed) {
      c->shard_count = smoothed;
      update_child(c);
    }
#else
    if(c->shard_count != reported) {
      c->shard_count = reported;
      update_child(c);
    }
#endif
  }
}
/*---------------------------------------------------------------------------*/
/* Unused IA callback stubs: required only to satisfy tsch.h's unconditional
 * auto-wiring of every TSCH_CALLBACK_IA_* hook under TSCH_WITH_IMPLICIT_ACK
 * (needed here purely for the children-list IE above) -- see this file's own
 * header comment for why TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE always returning
 * 0 is what actually keeps all of this dormant, regardless of these stubs. */
int
cg_implicit_ack_active(const linkaddr_t *addr)
{
  return 0;
}
void
cg_overhear(const linkaddr_t *source, const linkaddr_t *destination,
            struct tsch_link *link, const uint8_t *payload, uint16_t payload_len)
{
}
int
cg_get_own_parent(linkaddr_t *out, uint8_t *out_is_root)
{
  return 0;
}
void
cg_parent_eb_input(const linkaddr_t *grandparent, uint8_t grandparent_is_root)
{
}
uint8_t
cg_get_own_congestion(void)
{
  return 0;
}
void
cg_parent_congestion_input(uint8_t congestion)
{
}
uint8_t
cg_confirmation_cycles(void)
{
  return 1;
}
void
cg_new_asfn(uint32_t asfn)
{
}
/*---------------------------------------------------------------------------*/
struct orchestra_rule child_grandchild_tree = {
  init,
  new_time_source,
  select_packet,
  child_added,
  child_removed,
  NULL,
  root_node_updated,
  "child_grandchild_tree",
  -1,
};

#endif /* UIP_MAX_ROUTES != 0 */
