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
 *         as a single entry (this node's address + one byte) in every EB,
 *         leaves included -- that byte used to carry a 0/1 has-descendants
 *         flag there, and here instead carries this node's own current
 *         shard count (see cg_get_own_children()). No framer, tsch.c, or tsch.h
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
#include "net/mac/framer/frame802154.h"
#include "net/mac/tsch/tsch-roots.h"
#include "net/routing/rpl-classic/rpl.h"
#include "net/nbr-table.h"
#include "lib/list.h"
#include "lib/memb.h"
#include "sys/ctimer.h"
#include "sys/log.h"

#define LOG_MODULE "Orchestra"
#define LOG_LEVEL  LOG_LEVEL_MAC

#if UIP_MAX_ROUTES != 0

/* Structural provisioning (default 1, ON): whether cg_own_shard_count()
 * counts this node's own direct RPL children at all. This is APAS's central
 * claim -- a relay's uplink capacity toward its own parent grows the instant
 * a child attaches (child_added(), zero traffic observed) rather than only
 * after that child's traffic has been observed and estimated. Setting this
 * to 0 is an ABLATION BUILD, not a real deployment option: with it off, the
 * child-count loop below is skipped entirely, and every node uses a single
 * cell toward its parent -- everything else in this file (idle reclaim,
 * hold-before-use) is untouched, so this isolates the one variable the
 * structural-provisioning claim depends on for a controlled comparison
 * (full APAS vs. APAS-without-structural). */
#ifndef CG_CONF_STRUCTURAL_PROVISIONING
#define CG_STRUCTURAL_PROVISIONING 1
#else
#define CG_STRUCTURAL_PROVISIONING CG_CONF_STRUCTURAL_PROVISIONING
#endif

/* Dynamic K (default 0, off): no K_max at all. A node's shard count is
 * exactly K_S = (own direct child count k) + 1 (its own traffic) + load
 * bonus, however large k is -- a node with 7 real children gets 8 shards
 * toward its parent, a node with 2 gets 3, and the BR installs, for each of
 * its own children, exactly the k+1 Rx cells that child advertises. There
 * is no policy cap (the fixed 4, the rank-tiered 6/4) and no compile-time
 * per-relationship array bound: cells are kept in per-relationship lists
 * drawn from one shared pool (cg_cell_memb, sized to TSCH's own link pool),
 * and the children table is sized to the neighbor table, since a node can
 * never have more direct children than neighbors. The only remaining
 * bounds are physical ones no policy can lift: a relationship cannot use
 * more cells than its slotframe has timeslots, the whole node cannot hold
 * more cells than TSCH_SCHEDULE_MAX_LINKS, and K_S rides a one-byte EB
 * field (255). CG_RANK_CAP is ignored when this is on, since there is no
 * cap left for a rank tier to lift a node above.
 *
 * k deliberately counts DIRECT children only. Counting a relay's whole
 * subtree (one cell per descendant, via a descendant count in the EB) was
 * tried and dropped: stale children after parent switches made the counts
 * count-to-infinity (60 nodes: 31.9% PDR), and even correct counts would
 * need the BR to hold one cell per node in the network, more than a
 * slotframe has timeslots once N exceeds its length. */
#ifndef CG_CONF_DYNAMIC_KMAX
#define CG_DYNAMIC_KMAX 0
#else
#define CG_DYNAMIC_KMAX CG_CONF_DYNAMIC_KMAX
#endif

#if CG_DYNAMIC_KMAX
/* Not a policy cap: the largest value the one-byte EB shard-count field
 * can carry. */
#define CG_RELAY_SHARDS_MAX 255
#endif /* CG_DYNAMIC_KMAX */

/* Length of the BR-facing slotframe (sf_root), in which the BR installs
 * k_c + 1 Rx cells for each of its children c. The fixed-cap builds use
 * ORCHESTRA_IA_ROOT_PERIOD (7): with at most 4-6 cells per child that fit,
 * but it silently limits the BR to 7 distinct cells in total however many
 * children it has -- beyond that, children's cells pile onto the same
 * timeslots and the BR can only listen on one of them. Under
 * CG_DYNAMIC_KMAX the BR therefore uses the same, full-length slotframe as
 * every other parent (TSCH_IA_SFS_SIZE), so the BR is sized like any relay:
 * sum over its children of (k_c + 1) cells. */
#ifdef CG_CONF_ROOT_PERIOD
#define CG_ROOT_PERIOD CG_CONF_ROOT_PERIOD
#elif CG_DYNAMIC_KMAX
#define CG_ROOT_PERIOD TSCH_IA_SFS_SIZE
#else
#define CG_ROOT_PERIOD ORCHESTRA_IA_ROOT_PERIOD
#endif

/* Distinct shard timeslots (default: on under CG_DYNAMIC_KMAX). The fixed-
 * cap builds place shard k at h + k*0x85EBCA6B (mod 2^32, mod period). At
 * period 67 that constant acts as a stride and a link's shards stay distinct
 * in practice (0 of 20000 simulated links up to K = 20), but nothing
 * guarantees it: in the old 7-slot root frame every link with K >= 4 had two
 * shards on one timeslot, where the parent can only listen on one. Under
 * CG_DYNAMIC_KMAX shard k is placed at base + k*stride (mod period) instead,
 * which gives every one of a relationship's first `period` shards its own
 * timeslot for any period (see cg_shard_slot()). Off in the fixed-cap
 * builds so their schedule is unchanged. */
#ifdef CG_CONF_DISTINCT_SHARD_SLOTS
#define CG_DISTINCT_SHARD_SLOTS CG_CONF_DISTINCT_SHARD_SLOTS
#else
#define CG_DISTINCT_SHARD_SLOTS CG_DYNAMIC_KMAX
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

/* The boosted cap for nodes at or below CG_RANK_CAP_THRESHOLD; far-from-
 * root nodes only ever request up to CG_RELAY_SHARDS_FAR (see
 * cg_effective_cap()). */
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

#if !CG_DYNAMIC_KMAX
#define CG_RELAY_SHARDS_MAX CG_RELAY_SHARDS_NEAR_ROOT
#endif
#else /* CG_RANK_CAP */
/* Flat policy cap on a node's own shard count (own_child_count + 1), so a
 * node with more direct children than this still only ever uses this many
 * uplink shards. Not used under CG_DYNAMIC_KMAX, which has no cap. */
#if !CG_DYNAMIC_KMAX
#define CG_RELAY_SHARDS_MAX 4
#endif
#endif /* CG_RANK_CAP */

/* ABLATION-ONLY switches (default 0): each reinstates one rejected design so
 * the paper's ablation runs exercise real code. Never enable in a deployment.
 *
 * CG_CONF_ABLATION_SUBTREE_COUNT: k counts every descendant instead of the
 * direct children; each node advertises its descendant count d as a second
 * EB entry and a parent sums (1 + d_c) over its children. Known to diverge
 * (stale children after parent switches are counted twice).
 *
 * CG_CONF_ABLATION_ANY_FRAME_ACTIVITY: idle reclaim treats any received
 * frame as activity (the original test, packetbuf_datalen() on the raw
 * frame, which is never 0), so keepalives and RPL control keep every child
 * active. */
#ifndef CG_CONF_ABLATION_SUBTREE_COUNT
#define CG_ABLATION_SUBTREE_COUNT 0
#else
#define CG_ABLATION_SUBTREE_COUNT CG_CONF_ABLATION_SUBTREE_COUNT
#endif
#ifndef CG_CONF_ABLATION_ANY_FRAME_ACTIVITY
#define CG_ABLATION_ANY_FRAME_ACTIVITY 0
#else
#define CG_ABLATION_ANY_FRAME_ACTIVITY CG_CONF_ABLATION_ANY_FRAME_ACTIVITY
#endif

#ifndef CG_CONF_MAX_CHILDREN
#if CG_DYNAMIC_KMAX
/* A node's direct RPL children are all its neighbors, so the neighbor table
 * is the real bound -- no separate, smaller child limit. */
#define CG_MAX_CHILDREN NBR_TABLE_MAX_NEIGHBORS
#else
#define CG_MAX_CHILDREN 6
#endif
#else
#define CG_MAX_CHILDREN CG_CONF_MAX_CHILDREN
#endif

/* Every cell this rule installs (own uplink/root-adjacent, every child's
 * relay_rx/root_adjacent_rx) is tracked through one entry of this shared
 * pool. Defaults to TSCH's own link pool size: TSCH cannot hold more links
 * than that anyway, so this adds no bound of its own. */
#ifndef CG_CONF_CELL_POOL_SIZE
#define CG_CELL_POOL_SIZE TSCH_SCHEDULE_MAX_LINKS
#else
#define CG_CELL_POOL_SIZE CG_CONF_CELL_POOL_SIZE
#endif

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

/* Idle reclaim (default 0, off): a node that has sent no application data
 * to its parent for several minutes moves to the shared cell. The node
 * decides this itself, because only it knows whether it has anything to
 * send: it keeps an idle score (1 = sent data recently), folded every
 * CG_IDLE_CHECK_PERIOD (decay to 3/4 per silent check), and once the score
 * drops below CG_IDLE_RECLAIM_THRESHOLD it sets K_S = 0. It then removes its
 * own uplink cells at once -- select_packet() finds none and leaves its
 * frames to the next rule, the shared cell of default_common, which its
 * parent always listens on -- and announces K_S = 0 in its next EB, on which
 * the parent removes every receive cell it kept for it. Shrinking in this
 * order (child first, parent after the EB) never leaves the child sending in
 * a cell its parent has stopped listening on.
 *
 * The moment the node queues application data for its parent again, it is
 * active: K_S goes back to k + 1, and CG_HOLD_BEFORE_USE keeps it on the
 * shared cell until an EB has stated the new count, after which both ends
 * install the same cells. Keepalives and RPL control never count as
 * activity (see cg_frame_is_app_data()). */
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
#if CG_ABLATION_SUBTREE_COUNT
  uint8_t descendants; /* as last advertised by this child (ablation) */
#endif
#if CG_EWMA_SHARD_COUNT
  /* Q8 fixed-point running average of every shard count c has reported in
   * its EB, updated in cg_child_eb_input(); shard_count above tracks its
   * rounded value instead of the raw report under this build variant. */
  uint16_t shard_ewma_q8;
#endif
#if CG_IDLE_RECLAIM
  /* Set while this child's EBs announce K = 0 (it is idle and uses the
   * shared cell): no receive cell is kept for it. */
  uint8_t idle;
#endif
  /* Lists of struct cg_cell, shard k at list position k (see
   * cg_set_cells()). */
  LIST_STRUCT(relay_rx);
  /* Only ever used when this node is the DAG root and this child is one of
   * its root-adjacent neighbors -- see update_child_root_adjacent_rx(). */
  LIST_STRUCT(root_adjacent_rx);
};

/* One installed cell; lists of these replace fixed per-relationship link
 * arrays, so a relationship's shard count has no compile-time bound. */
struct cg_cell {
  struct cg_cell *next;
  struct tsch_link *link;
};
MEMB(cg_cell_memb, struct cg_cell, CG_CELL_POOL_SIZE);

static uint16_t slotframe_handle;
static struct tsch_slotframe *sf_short; /* uplink + relay_rx, every child */
static struct tsch_slotframe *sf_root;  /* root-adjacent cell(s) */

LIST(l_uplink);
LIST(l_root_adjacent);

static struct cg_child children[CG_MAX_CHILDREN];

#if CG_IDLE_RECLAIM
/* This node's own idle state toward its parent (see CG_IDLE_RECLAIM). */
static uint8_t own_heard;          /* sent application data since the last check */
static uint16_t own_idle_q8 = 1 << 8;
static uint8_t own_idle;           /* K_S = 0: using the shared cell */
static struct ctimer idle_timer;
static struct ctimer wake_timer;   /* re-sizes our cells outside select_packet() */
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
#if CG_DISTINCT_SHARD_SLOTS
static uint16_t
cg_gcd(uint16_t a, uint16_t b)
{
  while(b != 0) {
    uint16_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}
#endif /* CG_DISTINCT_SHARD_SLOTS */
/*---------------------------------------------------------------------------*/
/* Timeslot of shard k of the relationship hashed to h. Under
 * CG_DISTINCT_SHARD_SLOTS: base + k*stride (mod period), with stride
 * coprime to period, so shards 0..period-1 all get different timeslots --
 * both endpoints compute the same sequence from h alone. Otherwise the
 * fixed-cap builds' independent per-shard hash. */
static uint16_t
cg_shard_slot(uint32_t h, uint16_t k, uint16_t period)
{
#if CG_DISTINCT_SHARD_SLOTS
  uint16_t base, stride;
  if(period <= 1) {
    return 0;
  }
  base = cg_slot(h, period);
  stride = 1 + (uint16_t)((h / period) % (period - 1));
  while(cg_gcd(stride, period) != 1) {
    stride = stride % (period - 1) + 1; /* reaches 1 at worst, always coprime */
  }
  return (uint16_t)((base + (uint32_t)k * stride) % period);
#else
  return cg_slot(h + (uint32_t)k * 0x85EBCA6Bu, period);
#endif
}
/*---------------------------------------------------------------------------*/
static uint16_t
cg_channel(uint32_t hash)
{
  return tsch_hopping_sequence_length.val > 0 ? (uint16_t)(hash % tsch_hopping_sequence_length.val) : 0;
}
/*---------------------------------------------------------------------------*/
#if CG_RANK_CAP && !CG_DYNAMIC_KMAX
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
#endif /* CG_RANK_CAP && !CG_DYNAMIC_KMAX */
/*---------------------------------------------------------------------------*/
/* This node's own shard count: (own direct RPL child count) + 1 for its own
 * traffic, capped at cg_effective_cap() (CG_RANK_CAP) or the flat
 * CG_RELAY_SHARDS_MAX otherwise -- or, under CG_DYNAMIC_KMAX, not capped at
 * all (CG_RELAY_SHARDS_MAX is then just the one-byte EB field's range, 255,
 * far beyond any real child count). Entirely local -- Orchestra's own
 * child_added()/child_removed() callbacks already maintain children[], no
 * routing-table access or any other signal needed. Sizes this node's own
 * uplink/root-adjacent cells to its parent (update_uplink()/update_root_
 * adjacent() below) and is what this node advertises in its own EB
 * (cg_get_own_children()) for its parent to size a matching Rx cell.
 *
 * Under the CG_STRUCTURAL_PROVISIONING=0 ablation build, the child-count
 * loop is skipped: n starts at 1 (own traffic only) regardless of how many
 * children this node actually has, so a newly-attached child earns this
 * relay no additional uplink capacity at all. */
#if CG_ABLATION_SUBTREE_COUNT
/* Ablation: nodes below this one, sum over direct children of (1 + d_c). */
static uint8_t
cg_own_descendants(void)
{
  int i;
  uint16_t d = 0;
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(children[i].in_use) {
      d += 1 + children[i].descendants;
    }
  }
  return d > 254 ? 254 : (uint8_t)d;
}
#endif /* CG_ABLATION_SUBTREE_COUNT */
/*---------------------------------------------------------------------------*/
static uint8_t
cg_own_shard_count(void)
{
#if CG_STRUCTURAL_PROVISIONING && !CG_ABLATION_SUBTREE_COUNT
  int i;
#endif
  uint16_t n = 0;
  uint8_t cap;
#if CG_STRUCTURAL_PROVISIONING
#if CG_ABLATION_SUBTREE_COUNT
  n = cg_own_descendants();
#else
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(children[i].in_use) {
      n++;
    }
  }
#endif
#endif
  n++; /* +1 for this node's own traffic */
#if CG_IDLE_RECLAIM
  if(own_idle) {
    return 0; /* idle: no dedicated cell, the shared cell carries our frames */
  }
#endif
#if CG_RANK_CAP && !CG_DYNAMIC_KMAX
  cap = cg_effective_cap();
#else
  cap = CG_RELAY_SHARDS_MAX;
#endif
  return n > cap ? cap : (uint8_t)n;
}
/*---------------------------------------------------------------------------*/
/* Logs this node's own K_S whenever it changes. */
static void
cg_log_own_k(uint8_t k)
{
  static uint8_t last_k;
  if(k != last_k) {
#if CG_ABLATION_SUBTREE_COUNT
    LOG_INFO("cg: own K=%u (descendants=%u)\n", k, cg_own_descendants());
#else
    LOG_INFO("cg: own K=%u\n", k);
#endif
    last_k = k;
  }
}
/*---------------------------------------------------------------------------*/
/* Removes every cell in list cells from slotframe sf. */
static void
cg_clear_cells(struct tsch_slotframe *sf, list_t cells)
{
  struct cg_cell *cell;
  while((cell = list_pop(cells)) != NULL) {
    if(sf != NULL) {
      tsch_schedule_remove_link(sf, cell->link);
    }
    memb_free(&cg_cell_memb, cell);
  }
}
/*---------------------------------------------------------------------------*/
/* Makes list cells hold exactly want_shards cells in slotframe sf, shard k
 * at cg_shard_slot(h, k) (k = 0..want_shards-1): existing shards are retargeted
 * in place, missing ones added, surplus ones removed from the tail. Nothing
 * here bounds want_shards except physical limits: a relationship cannot use
 * more cells than the slotframe has timeslots, and an add fails only when
 * TSCH's own link pool (or cg_cell_memb, sized to match) is exhausted. */
static void
cg_set_cells(struct tsch_slotframe *sf, list_t cells, uint16_t want_shards,
             uint8_t link_options, const linkaddr_t *addr, uint32_t h, uint16_t period)
{
  struct cg_cell *cell;
  uint16_t k = 0;

  if(want_shards > period) {
    LOG_WARN("cg: %u shards wanted but slotframe has only %u slots, using %u\n",
             want_shards, period, period);
    want_shards = period;
  }
  while(list_length(cells) > want_shards) {
    cell = list_chop(cells);
    tsch_schedule_remove_link(sf, cell->link);
    memb_free(&cg_cell_memb, cell);
  }
  for(cell = list_head(cells); cell != NULL; cell = list_item_next(cell), k++) {
    uint32_t kh = h + (uint32_t)k * 0x85EBCA6Bu;
    /* Retarget in place rather than remove+re-add: for the uplink our own
     * parent may have changed since this link was created (reconcile_parent_
     * address() no longer tears these down on a parent switch -- see its
     * own comment), and h already folds the *current* parent in, so only
     * the destination address field needs an explicit copy to match. */
    linkaddr_copy(&cell->link->addr, addr);
    cell->link->timeslot = cg_shard_slot(h, k, period);
    cell->link->channel_offset = cg_channel(kh);
  }
  for(; k < want_shards; k++) {
    uint32_t kh = h + (uint32_t)k * 0x85EBCA6Bu;
    cell = memb_alloc(&cg_cell_memb);
    if(cell == NULL) {
      LOG_ERR("cg: cell pool exhausted at shard %u of %u\n", k, want_shards);
      return;
    }
    cell->link = tsch_schedule_add_link(sf, link_options, LINK_TYPE_NORMAL, addr,
                                        cg_shard_slot(h, k, period), cg_channel(kh), 1);
    if(cell->link == NULL) {
      LOG_ERR("cg: TSCH link pool exhausted at shard %u of %u "
              "(raise TSCH_SCHEDULE_CONF_MAX_LINKS)\n", k, want_shards);
      memb_free(&cg_cell_memb, cell);
      return;
    }
    list_add(cells, cell);
  }
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
  int i;
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    if(!children[i].in_use) {
      children[i].in_use = 1;
      linkaddr_copy(&children[i].addr, addr);
      children[i].shard_count = 0;
#if CG_ABLATION_SUBTREE_COUNT
      children[i].descendants = 0;
#endif
#if CG_EWMA_SHARD_COUNT
      children[i].shard_ewma_q8 = 1 << 8; /* matches the shard_count=1 bootstrap default below */
#endif
#if CG_IDLE_RECLAIM
      children[i].idle = 0;
#endif
      LIST_STRUCT_INIT(&children[i], relay_rx);
      LIST_STRUCT_INIT(&children[i], root_adjacent_rx);
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
#if CG_HOLD_BEFORE_USE
  static uint8_t last_held;
#endif
  if(sf_short == NULL || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || tsch_roots_is_root(&orchestra_parent_linkaddr) || !orchestra_parent_knows_us) {
    cg_clear_cells(sf_short, l_uplink);
    return;
  }
  want_shards = cg_own_shard_count();
  cg_log_own_k(want_shards);
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
  cg_set_cells(sf_short, l_uplink, want_shards, LINK_OPTION_TX | LINK_OPTION_SHARED,
               &orchestra_parent_linkaddr,
               ORCHESTRA_LINKADDR_HASH2(&orchestra_parent_linkaddr, &linkaddr_node_addr),
               TSCH_IA_SFS_SIZE);
}
/*---------------------------------------------------------------------------*/
/* Our own root-adjacent cell(s), used instead of UPLINK when our parent is
 * the root: same sizing rule, own (typically shorter) slotframe/period. */
static void
update_root_adjacent(void)
{
  uint8_t want_shards;
#if CG_HOLD_BEFORE_USE
  static uint8_t last_held;
#endif
  if(sf_root == NULL || linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)
     || !tsch_roots_is_root(&orchestra_parent_linkaddr) || !orchestra_parent_knows_us) {
    cg_clear_cells(sf_root, l_root_adjacent);
    return;
  }
  want_shards = cg_own_shard_count();
  cg_log_own_k(want_shards);
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
  cg_set_cells(sf_root, l_root_adjacent, want_shards, LINK_OPTION_TX | LINK_OPTION_SHARED,
               &orchestra_parent_linkaddr,
               ORCHESTRA_LINKADDR_HASH2(&orchestra_parent_linkaddr, &linkaddr_node_addr),
               CG_ROOT_PERIOD);
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
  uint8_t want_shards = c->shard_count > 0 ? c->shard_count : 1;
#if CG_IDLE_RECLAIM
  if(c->idle) {
    want_shards = 0;
  }
#endif
  if(sf_short == NULL) {
    return;
  }
  cg_set_cells(sf_short, c->relay_rx, want_shards, LINK_OPTION_RX, &c->addr,
               ORCHESTRA_LINKADDR_HASH2(&linkaddr_node_addr, &c->addr), TSCH_IA_SFS_SIZE);
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
  uint8_t want_shards = c->shard_count > 0 ? c->shard_count : 1;
#if CG_IDLE_RECLAIM
  if(c->idle) {
    want_shards = 0;
  }
#endif
  if(sf_root == NULL || !tsch_is_coordinator) {
    cg_clear_cells(sf_root, c->root_adjacent_rx);
    return;
  }
  cg_set_cells(sf_root, c->root_adjacent_rx, want_shards, LINK_OPTION_RX, &c->addr,
               ORCHESTRA_LINKADDR_HASH2(&linkaddr_node_addr, &c->addr), CG_ROOT_PERIOD);
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
/* Adopts shard count v that child c advertised in its EB. Under
 * CG_IDLE_RECLAIM, v = 0 means c is idle and uses the shared cell: every
 * receive cell kept for it is removed until it announces a count again. */
static void
cg_adopt_child_count(struct cg_child *c, uint8_t v)
{
#if CG_IDLE_RECLAIM
  if(v == 0) {
    if(!c->idle) {
      LOG_INFO("cg: reclaiming idle child's shards (was %u) -- ", c->shard_count > 0 ? c->shard_count : 1);
      LOG_INFO_LLADDR(&c->addr);
      LOG_INFO_("\n");
      c->idle = 1;
      update_child(c);
    }
    return;
  }
  if(c->idle) {
    c->idle = 0;
    c->shard_count = v;
    LOG_INFO("cg: child active again, restoring %u shards -- ", v);
    LOG_INFO_LLADDR(&c->addr);
    LOG_INFO_("\n");
    update_child(c);
    return;
  }
#else
  if(v == 0) {
    v = 1;
  }
#endif
  if(c->shard_count != v) {
    c->shard_count = v;
    update_child(c);
  }
}
/*---------------------------------------------------------------------------*/
#if CG_IDLE_RECLAIM
/* Whether the frame in packetbuf carries application data (UDP), as
 * opposed to an empty keepalive or RPL control traffic (ICMPv6 DAO/DIO/DIS/
 * probes), which a node keeps sending to its parent while it has nothing
 * to report. At TSCH_CALLBACK_UNICAST_DATA_INPUT time packetbuf still holds
 * the whole raw frame, MAC header included, so it is parsed here first. A
 * compressed IPv6 header (6LoWPAN IPHC) sets its NH bit only when the next
 * header is compressed with NHC, which Contiki does for UDP and never for
 * ICMPv6. */
#if !CG_ABLATION_ANY_FRAME_ACTIVITY
static int
cg_frame_is_app_data(const uint8_t *buf, int buflen)
{
  frame802154_t frame;
  const uint8_t *p;
  int len;

  if(frame802154_parse((uint8_t *)buf, buflen, &frame) == 0) {
    return 0;
  }
  p = frame.payload;
  len = frame.payload_len;
  if(len <= 0) {
    return 0; /* keepalive */
  }
  if((p[0] & 0xf8) == 0xe0) {
    return 1; /* FRAGN: a piece of a datagram too large for control traffic */
  }
  if((p[0] & 0xf8) == 0xc0) { /* FRAG1: IPHC follows the 4-byte header */
    if(len <= 4) {
      return 0;
    }
    p += 4;
  }
  return (p[0] & 0xe0) == 0x60 && (p[0] & 0x04) != 0; /* IPHC with NH=1 */
}
#endif /* !CG_ABLATION_ANY_FRAME_ACTIVITY */
/* Whether the outgoing frame in packetbuf counts as activity toward our
 * parent: application data only (any data frame in the ablation build). */
static int
cg_is_activity(void)
{
#if CG_ABLATION_ANY_FRAME_ACTIVITY
  return 1;
#else
  /* Outgoing packet, already framed (select_packet() runs from
   * tsch_queue_add_packet(), after NETSTACK_FRAMER.create()): the MAC header
   * sits in front of the payload, at packetbuf_hdrptr(). */
  return cg_frame_is_app_data(packetbuf_hdrptr(), packetbuf_totlen());
#endif
}
/*---------------------------------------------------------------------------*/
static void
wake_cb(void *ptr)
{
  update_uplink();
  update_root_adjacent();
}
/*---------------------------------------------------------------------------*/
/* Every CG_IDLE_CHECK_PERIOD: fold whether we sent application data to our
 * parent this period into own_idle_q8 (back to 1 at once on data, 3/4 per
 * silent check), and move to the shared cell (K_S = 0) once it is below
 * CG_IDLE_RECLAIM_THRESHOLD. Also logs, per child, the receive cells kept
 * for it (0 while it is idle). */
static void
idle_tick(void *ptr)
{
  int i;
  if(own_heard) {
    own_idle_q8 = 1 << 8;
  } else {
    own_idle_q8 -= own_idle_q8 >> CG_IDLE_EWMA_DECAY_SHIFT;
  }
  own_heard = 0;
  LOG_INFO("cg: own idle score=%u/256 idle=%u\n", own_idle_q8, own_idle);
  if(!own_idle && own_idle_q8 < CG_IDLE_RECLAIM_THRESHOLD
     && !linkaddr_cmp(&orchestra_parent_linkaddr, &linkaddr_null)) {
    own_idle = 1;
    LOG_INFO("cg: idle, moving to the shared cell (K_S=0)\n");
    update_uplink();
    update_root_adjacent();
  }
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    struct cg_child *c = &children[i];
    if(!c->in_use) {
      continue;
    }
    LOG_INFO("cg: idle_tick DIAG child shard_count=%u ewma=%u/256 -- ",
             c->idle ? 0 : (c->shard_count > 0 ? c->shard_count : 1), c->idle ? 0 : 256);
    LOG_INFO_LLADDR(&c->addr);
    LOG_INFO_("\n");
  }
  ctimer_set(&idle_timer, CG_IDLE_CHECK_PERIOD, idle_tick, NULL);
}
#endif /* CG_IDLE_RECLAIM */
/*---------------------------------------------------------------------------*/
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
  if(c == NULL) {
    return;
  }
  cg_clear_cells(sf_short, c->relay_rx);
  cg_clear_cells(sf_root, c->root_adjacent_rx);
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
#if CG_IDLE_RECLAIM
    if(cg_is_activity()) {
      own_heard = 1;
      if(own_idle) {
        /* Active again: ask for k + 1 cells. Hold-before-use keeps this and
         * the next frames on the shared cell until an EB has stated the
         * new count. Re-sized from a timer, not inside this callback. */
        own_idle = 0;
        own_idle_q8 = 1 << 8;
        LOG_INFO("cg: active again, requesting K_S=%u\n", cg_own_shard_count());
        ctimer_set(&wake_timer, 0, wake_cb, NULL);
      }
    }
#endif
    if(tsch_roots_is_root(&orchestra_parent_linkaddr)) {
      if(list_head(l_root_adjacent) != NULL) {
        *slotframe = sf_root->handle;
        *timeslot = 0xffff;
        *channel_offset = 0xffff;
        return 1;
      }
      return 0;
    }
    if(list_head(l_uplink) != NULL) {
      *slotframe = sf_short->handle;
      *timeslot = 0xffff;
      *channel_offset = 0xffff;
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
  memb_init(&cg_cell_memb);
  list_init(l_uplink);
  list_init(l_root_adjacent);
  for(i = 0; i < CG_MAX_CHILDREN; i++) {
    children[i].in_use = 0;
  }
  sf_short = tsch_schedule_add_slotframe(slotframe_handle, TSCH_IA_SFS_SIZE);
  sf_root = tsch_schedule_add_slotframe(slotframe_handle | 0x4000, CG_ROOT_PERIOD);
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
#if CG_HOLD_BEFORE_USE
  announced_shards = 1; /* matches the safe bootstrap default everywhere else */
#endif
}
/*---------------------------------------------------------------------------*/
/* TSCH_CALLBACK_IA_OWN_CHILDREN: fired from tsch_packet_create_eb() to fill
 * our EB's children-list IE, reusing orchestra-rule-implicit-ack.c's wire
 * format. This node has exactly one shard count K_S (cg_own_shard_count()),
 * for its one uplink relationship with its own parent, so it is sent as
 * exactly ONE entry (our own address + K_S) -- always, even with no
 * children. Earlier versions wrote one entry per child, all carrying the
 * same value, which meant (a) a leaf sent an empty list and its parent never
 * learned its K_S, so a leaf's load-bonus shard was transmitted on a cell
 * the parent never listened on, and (b) the IE's TSCH_IA_IE_MAX_CHILDREN
 * limit bounded EB size by child count for no benefit. The parent only
 * reads entry 0 and keys on the EB's source address (cg_child_eb_input()),
 * so the entry's address field is informational. */
uint8_t
cg_get_own_children(linkaddr_t *out, uint8_t *out_shard_count, uint8_t max_children)
{
  uint8_t own_shards = cg_own_shard_count();
#if CG_HOLD_BEFORE_USE
  /* This EB is about to state own_shards -- record it as announced
   * regardless of whether we have any children entries below to write
   * (a leaf with no children still sends this in every one of its EBs,
   * and still needs its own single shard's increase, if any, held the
   * same way). */
  announced_shards = own_shards;
#endif
  if(max_children == 0) {
    return 0;
  }
  linkaddr_copy(&out[0], &linkaddr_node_addr);
  out_shard_count[0] = own_shards;
#if CG_ABLATION_SUBTREE_COUNT
  if(max_children >= 2) {
    linkaddr_copy(&out[1], &linkaddr_node_addr);
    out_shard_count[1] = cg_own_descendants();
    return 2;
  }
#endif
  return 1;
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
#if CG_ABLATION_SUBTREE_COUNT
  if(num_children >= 2 && c->descendants != children_shard_count[1]) {
    c->descendants = children_shard_count[1];
    update_uplink();
    update_root_adjacent();
  }
#endif
  {
    uint8_t reported = children_shard_count[0];
#if CG_IDLE_RECLAIM
    if(reported == 0) {
      cg_adopt_child_count(c, 0); /* idle announcement: never smoothed */
      return;
    }
#endif
    if(reported == 0) {
      reported = 1;
    }
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
    cg_adopt_child_count(c, smoothed);
#else
    cg_adopt_child_count(c, reported);
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
/* TSCH_CALLBACK_UNICAST_DATA_INPUT: every received unicast data frame, raw
 * frame in packetbuf. A child that becomes active again sends its first
 * application data on the shared cell (it holds its dedicated cells until an
 * EB has stated its new count), so the parent sees that data before any
 * dedicated cell is used. It then treats the child like a newly joined one:
 * one fresh cell at once, and the child's CURRENT count from its next EB --
 * not the count it advertised before going idle, which may be out of date
 * if it gained or lost children meanwhile. */
void
cg_data_input(const linkaddr_t *source)
{
#if CG_IDLE_RECLAIM
  struct cg_child *c = find_child(source);
  if(c == NULL || !c->idle) {
    return;
  }
#if !CG_ABLATION_ANY_FRAME_ACTIVITY
  if(!cg_frame_is_app_data(packetbuf_dataptr(), packetbuf_datalen())) {
    return;
  }
#endif
  cg_adopt_child_count(c, 1);
#endif
}
/*---------------------------------------------------------------------------*/
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
