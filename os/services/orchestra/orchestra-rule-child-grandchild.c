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

 NEW UPDATE MANAGED BY ANINDITA SARKAR
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
 * child-count loop below is skipped entirely, and this node's shard count
 * comes only from CG_LOAD_BONUS (traffic actually observed) exactly as a
 * purely reactive scheme like A3 would size it -- everything else in this
 * file (idle reclaim, rank cap, hold-before-use) is untouched, so this
 * isolates the one variable the structural-provisioning claim depends on
 * for a controlled comparison (full APAS vs. APAS-without-structural). */
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
 * unaware the parent shrank its Rx side. A reclaimed child is then held at
 * 1 shard even though its EBs keep advertising more (it still beacons while
 * idle -- adopting those EBs would undo the reclaim within one EB period);
 * the moment it sends real data again, cg_data_input() restores the count
 * its EBs advertise. Its shard 0 is always kept, so that first data frame
 * does get through; frames it sent on its other shards before the restore
 * are retried on its next cell. CG_IDLE_CHECK_PERIOD and the EWMA
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


#ifndef CG_CONF_LOAD_BONUS
#define CG_LOAD_BONUS 0
#else
#define CG_LOAD_BONUS CG_CONF_LOAD_BONUS
#endif

#if CG_LOAD_BONUS

#ifdef CG_CONF_LOAD_QUEUE_BASED
#define CG_LOAD_QUEUE_BASED CG_CONF_LOAD_QUEUE_BASED
#else
#define CG_LOAD_QUEUE_BASED CG_DYNAMIC_KMAX
#endif

#if CG_LOAD_QUEUE_BASED

#ifndef CG_CONF_LOAD_RELEASE_WINDOWS
#define CG_LOAD_RELEASE_WINDOWS 5
#else
#define CG_LOAD_RELEASE_WINDOWS CG_CONF_LOAD_RELEASE_WINDOWS
#endif
#endif /* CG_LOAD_QUEUE_BASED */

/* Queue-based: the release window. Rate-based: how often we fold "how much
 * did I send this window" into the load EWMA. */
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
  /* Set by cg_data_input() the moment real data is heard from this child;
   * cleared by idle_tick() every CG_IDLE_CHECK_PERIOD once it has been
   * folded into idle_ewma_q8 below. */
  uint8_t heard_since_check;
  /* Q8 fixed-point recent-activity estimate, 256 = just heard from it,
   * decaying toward 0 under sustained silence (see idle_tick()). */
  uint16_t idle_ewma_q8;
  /* Set when idle_tick() reclaimed this child: its cells stay at 1, even
   * though its EBs keep advertising more, until it sends real data again
   * (cg_data_input()). advertised holds what its EBs say meanwhile. */
  uint8_t reclaimed;
  uint8_t advertised;
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

#if CG_LOAD_BONUS
/* Node-wide, not per-child: this is about THIS node's own outgoing traffic
 * toward its OWN parent, not anything about any specific child. Counted in
 * select_packet() every time a real data frame for our parent is matched
 * to one of our own cells; folded into own_load_ewma_q8 and, if that
 * estimate is sustained above CG_LOAD_BUSY_THRESHOLD, own_load_bonus is set
 * to 1 and cg_own_shard_count() adds it on top of the structural count. */
static uint8_t own_load_bonus;
#if CG_LOAD_QUEUE_BASED
/* Whether the previous per-slotframe sample already found more packets
 * queued to our parent than we have uplink cells. */
static uint8_t last_backlogged;
/* Set by queue_tick() when a head packet survived a full slotframe;
 * cleared every CG_LOAD_CHECK_PERIOD by load_tick(). */
static uint8_t shortage_seen;
static uint8_t quiet_windows;
#else
static uint16_t own_load_tx_count;
static uint16_t own_load_ewma_q8;
#endif
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
 * relay no additional uplink capacity until CG_LOAD_BONUS's own traffic
 * observation later grants it -- the reactive-only comparison point. */
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
#if CG_LOAD_BONUS
  n += own_load_bonus; /* +1 more if sustained real load earned it (load_tick()) */
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
      /* Start "fully active": a just-joined child gets the same grace
       * period every other bootstrap default in this file already gives
       * it, rather than becoming reclaim-eligible before it has ever had a
       * chance to send anything. */
      children[i].heard_since_check = 1;
      children[i].idle_ewma_q8 = 1 << 8;
      children[i].reclaimed = 0;
      children[i].advertised = 0;
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
/* Adopts shard count v that child c advertised in its EB -- unless c was
 * reclaimed for being idle, in which case it stays at 1 until it sends
 * real data again. Without that hold, c's very next EB (~15 s later) would
 * restore every reclaimed cell while c is still silent. */
static void
cg_adopt_child_count(struct cg_child *c, uint8_t v)
{
#if CG_IDLE_RECLAIM
  c->advertised = v;
  if(c->reclaimed) {
    return;
  }
#endif
  if(c->shard_count != v) {
    c->shard_count = v;
    update_child(c);
  }
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
 * populated, so the frame can be inspected -- but packetbuf then holds the
 * raw frame, MAC header included, so packetbuf_datalen() alone is never 0
 * (an earlier version relied on that and counted every keepalive as
 * activity); see cg_frame_is_app_data(). */
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
cg_frame_is_app_data(void)
{
  frame802154_t frame;
  const uint8_t *p;
  int len;

  if(frame802154_parse(packetbuf_dataptr(), packetbuf_datalen(), &frame) == 0) {
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
/*---------------------------------------------------------------------------*/
void
cg_data_input(const linkaddr_t *source)
{
  struct cg_child *c;
#if CG_ABLATION_ANY_FRAME_ACTIVITY
  if(packetbuf_datalen() == 0) {
    return; /* never true on the raw frame -- every frame counts (ablation) */
  }
#else
  if(!cg_frame_is_app_data()) {
    return; /* keepalive or RPL control -- not evidence of real activity */
  }
#endif
  c = find_child(source);
  if(c != NULL) {
    c->heard_since_check = 1;
    if(c->reclaimed) {
      /* Active again: give back what it advertises. */
      c->reclaimed = 0;
      LOG_INFO("cg: child active again, restoring %u shards -- ", c->advertised);
      LOG_INFO_LLADDR(&c->addr);
      LOG_INFO_("\n");
      cg_adopt_child_count(c, c->advertised > 0 ? c->advertised : 1);
    }
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
      c->reclaimed = 1;
      update_child(c);
    }
  }
  ctimer_set(&idle_timer, CG_IDLE_CHECK_PERIOD, idle_tick, NULL);
}
#endif /* CG_IDLE_RECLAIM */
/*---------------------------------------------------------------------------*/
#if CG_LOAD_BONUS
static struct ctimer load_timer;
#if CG_LOAD_QUEUE_BASED
static struct ctimer queue_timer;
/* One slotframe of whichever slotframe our uplink cells are in, in clock
 * ticks (rounded up): the time for all K_S of our cells to pass once. */
static clock_time_t
cg_slotframe_ticks(void)
{
  uint16_t len = tsch_roots_is_root(&orchestra_parent_linkaddr) ? CG_ROOT_PERIOD : TSCH_IA_SFS_SIZE;
  uint32_t us = (uint32_t)len * RTIMERTICKS_TO_US(tsch_timing[tsch_ts_timeslot_length]);
  clock_time_t t = (clock_time_t)(((uint64_t)us * CLOCK_SECOND + 999999) / 1000000);
  return t > 0 ? t : 1; /* never a zero-period (busy-looping) timer */
}
/*---------------------------------------------------------------------------*/
/* Every slotframe: shortage if more packets are queued to our parent than
 * our installed uplink cells can send in one slotframe, at this sample and
 * the previous one. Only sampled while our uplink cells are installed --
 * before that, packets wait for a missing cell, not for more cells.
 * Grants X_S immediately (fast rise). */
static void
queue_tick(void *ptr)
{
  int cells = list_length(tsch_roots_is_root(&orchestra_parent_linkaddr) ? l_root_adjacent : l_uplink);
  int queued = 0;
  uint8_t backlogged;

  if(orchestra_parent_knows_us && cells > 0) {
    struct tsch_neighbor *n = tsch_queue_get_nbr(&orchestra_parent_linkaddr);
    if(n != NULL) {
      queued = tsch_queue_nbr_packet_count(n);
    }
  }
  backlogged = cells > 0 && queued > cells;
  if(backlogged && last_backlogged) {
    shortage_seen = 1;
    quiet_windows = 0;
    if(!own_load_bonus) {
      LOG_INFO("cg: own load bonus granted (%d queued > %d cells, two slotframes)\n",
               queued, cells);
      own_load_bonus = 1;
      update_uplink();
      update_root_adjacent();
    }
  }
  last_backlogged = backlogged;
  ctimer_set(&queue_timer, cg_slotframe_ticks(), queue_tick, NULL);
}
/*---------------------------------------------------------------------------*/
/* Every CG_LOAD_CHECK_PERIOD: release X_S once CG_LOAD_RELEASE_WINDOWS
 * windows in a row have passed with no shortage at all (fall slow). */
static void
load_tick(void *ptr)
{
  if(shortage_seen) {
    quiet_windows = 0;
  } else if(quiet_windows < 255) {
    quiet_windows++;
  }
  shortage_seen = 0;
  if(own_load_bonus && quiet_windows >= CG_LOAD_RELEASE_WINDOWS) {
    LOG_INFO("cg: own load bonus released (%u quiet windows)\n", quiet_windows);
    own_load_bonus = 0;
    update_uplink();
    update_root_adjacent();
  }
  ctimer_set(&load_timer, CG_LOAD_CHECK_PERIOD, load_tick, NULL);
}
#else /* CG_LOAD_QUEUE_BASED */
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
#endif /* CG_LOAD_QUEUE_BASED */
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
    if(tsch_roots_is_root(&orchestra_parent_linkaddr)) {
      if(list_head(l_root_adjacent) != NULL) {
        *slotframe = sf_root->handle;
        *timeslot = 0xffff;
        *channel_offset = 0xffff;
#if CG_LOAD_BONUS && !CG_LOAD_QUEUE_BASED
        own_load_tx_count++;
#endif
        return 1;
      }
      return 0;
    }
    if(list_head(l_uplink) != NULL) {
      *slotframe = sf_short->handle;
      *timeslot = 0xffff;
      *channel_offset = 0xffff;
#if CG_LOAD_BONUS && !CG_LOAD_QUEUE_BASED
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
#if CG_LOAD_BONUS
  own_load_bonus = 0;
#if CG_LOAD_QUEUE_BASED
  last_backlogged = 0;
  shortage_seen = 0;
  quiet_windows = 0;
  ctimer_set(&queue_timer, cg_slotframe_ticks(), queue_tick, NULL);
#else
  own_load_tx_count = 0;
  own_load_ewma_q8 = 0;
#endif
  ctimer_set(&load_timer, CG_LOAD_CHECK_PERIOD, load_tick, NULL);
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
