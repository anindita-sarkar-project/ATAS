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

/*
 * This example ("child-grandchild-tree") is a parent/child hash-based cell
 * taxonomy, with implicit acknowledgment and the short/regular dual timeslot
 * template both removed -- every cell here is a completely standard TSCH
 * cell (regular timeslot, explicit ACK, TSCH's own normal retry/backoff).
 * See orchestra-rule-child-grandchild.c's own header comment for the full
 * design and for exactly how TSCH_WITH_IMPLICIT_ACK=1 below is reused
 * purely for its children-list Information Element without pulling in any
 * of the implicit-ack behavior that name otherwise implies.
 *
 * Cell sizing rule: every node allocates cells with its own parent sized by
 * (its own direct RPL child count) + 1 for its own traffic -- nothing
 * deeper than direct children is ever tracked or signaled. This replaced an
 * earlier design that instead estimated each relay relationship's traffic
 * from the RPL routing table (a deeper, whole-subtree traffic proxy that
 * turned out to change too fast for the one-hop EB signal carrying it to
 * keep up with at scale) -- see the rule file's own header comment for the
 * full history.
 */

#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* ====================================================================
 * FIT IoT-LAB m3 build (real hardware, grenoble/lille, 60 nodes).
 * STM32F103RE + AT86RF231 -- 64KB RAM / 512KB flash, a quarter of the
 * nRF52840's RAM budget, so several values below are trimmed down from
 * the Cooja-validated config (which ran on ContikiMote's effectively
 * unlimited RAM) rather than reused as-is. Everything else in this file
 * below this block is the same, unmodified, Cooja-validated APAS config.
 * ==================================================================== */

#if CONTIKI_TARGET_IOTLAB
/* Node identity via the DEPLOYMENT service (testbed-safe) -- real IoT-LAB
 * hardware only: deployment_init() maps THIS BOARD's real MAC address to a
 * logical id via deployment-map.c's table, which only ever contains real
 * M3 MAC addresses. Must stay out of the Cooja build entirely (see the
 * Makefile's own comment on the TARGET=iotlab guard around the deployment
 * MODULES/PROJECT_SOURCEFILES lines): a Cooja mote's simulated MAC never
 * matches any entry in that table, so including this module there silently
 * forced every node_id to 0 (confirmed live -- see the Makefile comment for
 * the exact failure mode observed). */
#define DEPLOYMENT_MAPPING iotlab_m3_map

/* Sized for 60 real nodes, but trimmed conservatively for 64KB RAM --
 * a worst-case RPL-storing-mode node near the root could in principle
 * need routes to close to all 59 others, so this is not padded with
 * extra headroom the way the nRF52840 build's MAX_NODE_NUM=16 was;
 * first real linker RAM-overflow (if any) is the actual, empirical
 * signal to trim this further. IoT-LAB only: Cooja's ContikiMoteType has no
 * comparable RAM budget, and the node-count scalability sweep (Table II)
 * goes up to 150 nodes, well past this 62-node ceiling -- see the #else
 * branch below for the Cooja-side value. (This real-hardware #if TARGET
 * guard needed to be added: the whole block used to apply unconditionally,
 * which capped UIP_CONF_MAX_ROUTES/NBR_TABLE_CONF_MAX_NEIGHBORS at 62 even
 * under Cooja and would have silently broken RPL storing-mode routing --
 * dropped DAO routes beyond the 62nd -- for the N=100 and N=150 points of
 * that sweep.) */
#define MAX_NODE_NUM 62
#undef UIP_CONF_MAX_ROUTES
#define UIP_CONF_MAX_ROUTES MAX_NODE_NUM
#undef NBR_TABLE_CONF_MAX_NEIGHBORS
#define NBR_TABLE_CONF_MAX_NEIGHBORS MAX_NODE_NUM
#else /* CONTIKI_TARGET_IOTLAB */
/* Cooja: ContikiMoteType motes run as native host processes/threads with no
 * real memory constraint, so this is sized with real headroom above the
 * paper's largest node-count-sweep point (150) rather than trimmed tight. */
#define MAX_NODE_NUM 200
#undef UIP_CONF_MAX_ROUTES
#define UIP_CONF_MAX_ROUTES MAX_NODE_NUM
#undef NBR_TABLE_CONF_MAX_NEIGHBORS
#define NBR_TABLE_CONF_MAX_NEIGHBORS MAX_NODE_NUM
#endif /* CONTIKI_TARGET_IOTLAB */

/* Real AT86RF231 radio supports TSCH channel hopping; Cooja's UDGM model is
 * driven by the same hopping sequence setting. Table II specifies 4
 * channels {15,20,25,26} for BOTH the real IoT-LAB deployment and the Cooja
 * simulations, so this override is intentionally unconditional (not inside
 * the #if CONTIKI_TARGET_IOTLAB block above) -- TSCH_HOPPING_SEQUENCE_4_4 is
 * exactly this channel set, just listed in a different order (channel order
 * within one hopping sequence doesn't change which channels are used). */
#undef TSCH_CONF_DEFAULT_HOPPING_SEQUENCE
#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4

#define UIP_CONF_IPV6 1
#define NETSTACK_CONF_WITH_IPV6 1
#define IEEE802154_CONF_PANID 0x81a5

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

/* Real radio conditions are noisier than Cooja's UDGM model -- same
 * bootstrap retransmission headroom as the nRF52840 build. */
#define RPL_CONF_DIO_INTERVAL_MIN        10
#define RPL_CONF_DIO_INTERVAL_DOUBLINGS  6
#define RPL_CONF_DAO_MAX_RETRANSMISSIONS 10
#define RPL_CONF_DAO_RETRANSMISSION_TIMEOUT (3 * CLOCK_SECOND)

/* RPL_CONF_MAX_RANKINC: library default is 7*RPL_MIN_HOPRANKINC (896 at our
 * MIN_HOPRANKINC=128, confirmed live via "MaxRankInc 896" in the log). Kept
 * raised (see below) as cheap, harmless insurance, but empirically it did
 * NOT fix the dominant failure mode -- see MRHOF_CONF_MAX_LINK_METRIC below
 * for the fix that actually matters and why this one doesn't apply to it:
 * acceptable_rank() (rpl-dag.c) has an earlier, UNCONDITIONAL check,
 * `rank != RPL_INFINITE_RANK`, that rejects a rank of exactly 0xffff
 * regardless of max_rankinc -- confirmed both by reading the source and
 * empirically (raising this to 4096 and rerunning nc_N49_seed2/nc_N25_seed2
 * reproduced bit-for-bit identical PDR/packet-count results to the pre-fix
 * runs, proving it had zero effect on those failures). Left at 4096 anyway
 * since it is a harmless, strictly-more-tolerant setting for the (rarer)
 * case of a merely-elevated-but-finite rank, not reverted purely to avoid
 * re-introducing noise into an already-large validation matrix. */
#define RPL_CONF_MAX_RANKINC 4096

/* MRHOF_CONF_MAX_LINK_METRIC: a second, deliberately-tried RPL/MRHOF
 * robustness lever, kept despite ALSO not resolving the dominant residual
 * failure mode (nc_N25/nc_N150/sf_L17@N40) -- documented honestly here
 * since it's still a real, harmless improvement for a different, narrower
 * case. Mechanism traced via rpl-mrhof.c: rpl_parent_is_reachable() calls
 * parent_has_usable_link(), which rejects a parent outright once its
 * ETX-based link_metric exceeds MAX_LINK_METRIC (library default 1024, ETX
 * 8) -- confirmed live via RPL's own "Unacceptable link %u, removing routes
 * via: ..." warning (rpl_process_parent_event(), rpl-dag.c) firing
 * immediately before the "Unacceptable rank 65535...Nullifying parent" +
 * "Loop detected" sequence found in nc_N25_seed1. Raised this 16x (ETX~128)
 * expecting it to give RPL more patience with a congested, hash-collision-
 * prone link before tearing the route down -- re-ran nc_N25_seed1 with
 * BOTH this and RPL_CONF_MAX_RANKINC active: 42.5% PDR, "Unacceptable
 * link/rank" events still fired 1543 times, essentially unchanged. Root
 * cause why: MAX_LINK_METRIC only gates an ACCEPTANCE check on an
 * already-computed metric -- the metric's own computation,
 * parent_path_cost() -> `MIN(base + link_metric, 0xffff)`, is a separate,
 * unconditional numeric clip this override cannot reach, and
 * parent_link_metric() additionally returns 0xffff outright, unconditionally,
 * whenever the neighbor's link-stats entry is simply missing
 * (rpl_get_parent_link_stats() returns NULL) -- a different trigger again,
 * tied to generic link-stats/nbr_table state rather than any RPL/MRHOF
 * threshold. That is genuinely RPL's own 16-bit rank representation hitting
 * an arithmetic ceiling under sustained TSCH congestion, not a tunable
 * value -- reaching further (e.g. patching nbr_table/link-stats internals)
 * would mean editing shared infrastructure several other subsystems depend
 * on, a materially bigger and riskier change than anything else in this
 * file. Left at 16384 anyway: confirmed harmless (no regression on any
 * passing config) and it measurably helped the *different*, non-infinite-
 * rank case in the traffic-rate sweep (rt_R10@N=40 went from one seed at
 * 83.1% to both seeds clearing 90%, 99.4%/95.0%, once both this and
 * RPL_CONF_MAX_RANKINC were active). */
#define MRHOF_CONF_MAX_LINK_METRIC 16384

/* Conservative memory trims for 64KB RAM (vs. Cooja's unlimited) are
 * applied at the source further down in this file (QUEUEBUF_CONF_NUM)
 * and in node.c (MAX_AGG_ITEMS) rather than here, since both already
 * have their own #define further along that would otherwise win --
 * see those sites' own comments. Starting points, not a verified
 * budget: the plan is to try the build and let a real RAM-overflow
 * linker error (if any) tell us how much further to cut. */

/* Do not start TSCH at init, wait for NETSTACK_MAC.on() */
#define TSCH_CONF_AUTOSTART 0

/* RFC6719's own recommended hysteresis rather than MRHOF's more aggressive
 * default, since every RPL parent switch relocates this scheme's own
 * parent-keyed autonomous cells too (uplink/root-adjacent). */
#define PARENT_SWITCH_THRESHOLD_CONF 192

/* 6TiSCH minimal schedule length, used only until this rule's own
 * slotframes take over scheduling of unicast/data traffic. */
#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 3

/* sf_short + sf_root, plus one slotframe each for the EB and default-common
 * rules. No per-child slotframes needed: every child's relay_rx cells live
 * in sf_short at their own hash-derived position (HASH2(self, child)),
 * distinct from every other child's by construction, with collisions
 * tolerated the same way stock Orchestra already tolerates them. */
#define TSCH_SCHEDULE_CONF_MAX_SLOTFRAMES 4

/* Bumped from the library default (4): a grid node has up to 4 orthogonal
 * neighbors in radio range, any of which could pick it as RPL parent, plus
 * headroom for transient churn during convergence. Raised further from 6 to
 * 10 after a code review found alloc_child() (orchestra-rule-child-
 * grandchild.c) silently drops any child beyond this cap -- permanent,
 * unmitigated 100% loss for that child and its whole subtree, with only a
 * LOG_ERR as any trace -- and this evaluation now runs topologies dense and
 * deep enough (Section~sec:extendedscale, 200-300 nodes) that the risk of a
 * single node attracting more than 6 direct RPL children is no longer
 * negligible. 10 is not a structural fix (the cap is still fixed-size), just
 * a wider margin; see child_added()'s own comment for the defensive check
 * added alongside this. */
#define CG_CONF_MAX_CHILDREN 10

/* Reused purely for its children-list Enhanced Beacon Information Element
 * (MLME_SHORT_IE_TSCH_IA_CHILDREN, frame802154e-ie.c) -- the only piece of
 * orchestra-rule-implicit-ack.c's shared infrastructure this example needs.
 * See orchestra-rule-child-grandchild.c's header comment for the full
 * argument that this does NOT, by itself, enable any implicit-ack behavior:
 * every one of the other TSCH_CALLBACK_IA_* hooks this flag auto-wires is
 * explicitly redirected below to this file's own harmless stubs, and
 * TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE always returning 0 is what actually
 * keeps tsch-slot-operation.c's deferred-confirmation/short-timing paths
 * permanently dormant regardless of this flag's value. */
#define TSCH_CONF_WITH_IMPLICIT_ACK 1

/* Governing slotframe length for sf_short (uplink + every child's relay_rx).
 * Matched to the same value used throughout the implicit-ack-tree-multicell
 * evaluation for a fair, same-budget comparison. Also directly compared
 * against 167 across all 6 scales: 167 gave slightly lower RDC (~0.2-1.0
 * points, longer slotframe means fewer active slots per second for the same
 * cell count) but slightly worse PDR at every scale from 25 nodes up
 * (e.g. 150 nodes: 98.5%->98.1%) and higher latency throughout (longer
 * average wait to reach one's own cell) -- not a beneficial trade given 101
 * already keeps PDR at or above 98.5% and RDC at 3-4% everywhere. Kept at
 * 101.
 *
 * Guarded with #ifndef so the Makefile/experiment driver can override it
 * per-run with -DTSCH_CONF_IA_SFS_SIZE=<L> for the paper's Table II
 * slotframe-length sweep (L in {17,43,67,83,101}); 67 below is just the
 * default used when nothing overrides it (node-count and traffic-rate
 * sweeps both fix L=67 per Table II). */
#ifndef TSCH_CONF_IA_SFS_SIZE
#define TSCH_CONF_IA_SFS_SIZE 67
#endif

/* Comparison variant, off by default: EWMA-smooth a child's EB-reported
 * shard count instead of adopting it directly (see orchestra-rule-child-
 * grandchild.c's own header comment and cg_child_eb_input() for the
 * mechanism). Asymmetric fast-attack/slow-decay: 0 = adopt an increase
 * immediately (matching direct adoption for the direction that has a
 * correctness cost), 2 = alpha=1/4 smoothing on a decrease (safe to delay). */
/* #define CG_CONF_EWMA_SHARD_COUNT 1 */
/* #define CG_CONF_EWMA_ATTACK_SHIFT 0 */
/* #define CG_CONF_EWMA_DECAY_SHIFT 2 */

/* Idle reclaim, ENABLED (see orchestra-rule-child-grandchild.c's own
 * header comment on CG_IDLE_RECLAIM for the mechanism and the bounded,
 * self-correcting mismatch window it accepts). This watches whether a
 * child has actually been heard from lately -- independent of what it
 * self-reports in its EB -- and shrinks it back to its guaranteed-minimum
 * 1 shard after sustained silence, freeing capacity for others. It never
 * removes the child outright; that stays RPL's job via child_removed().
 * Both this and TSCH_CALLBACK_UNICAST_DATA_INPUT below must be enabled
 * together, or the build fails to link. An earlier version of this tapped
 * TSCH_CALLBACK_DO_NACK instead; live Cooja testing found that hook fires
 * on TSCH's own empty keepalive frames too, which a genuinely idle child
 * sends *more* of, not less -- defeating idle detection entirely. Fixed
 * by switching to this hook (fires after packetbuf is populated, so a
 * zero-length keepalive can be told apart from real traffic). */
#define CG_CONF_IDLE_RECLAIM 1
/* #define CG_CONF_IDLE_CHECK_PERIOD (CLOCK_SECOND * 120) */
/* #define CG_CONF_IDLE_EWMA_DECAY_SHIFT 2 */
/* #define CG_CONF_IDLE_RECLAIM_THRESHOLD 64 */

/* Load bonus: DISABLED. Not part of the paper's Algorithm 1/2 -- equation
 * (3) is exactly K_S = m_S + 1, nothing else. Enabling this would grant an
 * extra shard beyond the paper's own rule based on this node's local send
 * activity, which has no counterpart in Algorithm 1. Left here only as a
 * documented, off-by-default comparison variant (see orchestra-rule-child-
 * grandchild.c's own header comment on CG_LOAD_BONUS). */
/* #define CG_CONF_LOAD_BONUS 1 */
/* #define CG_CONF_LOAD_CHECK_PERIOD (CLOCK_SECOND * 60) */
/* #define CG_CONF_LOAD_SATURATE_COUNT 4 */
/* #define CG_CONF_LOAD_EWMA_ATTACK_SHIFT 1 */
/* #define CG_CONF_LOAD_EWMA_DECAY_SHIFT 3 */
/* #define CG_CONF_LOAD_BUSY_THRESHOLD 160 */
#if defined(CG_CONF_IDLE_RECLAIM) && CG_CONF_IDLE_RECLAIM
#define TSCH_CALLBACK_UNICAST_DATA_INPUT cg_data_input
#endif

/* Rank-aware cap: DISABLED to match the paper exactly. The paper's equation
 * (3) is explicit that K_S = m_S + 1 "has no fixed upper bound other than
 * the slotframe length L" -- a rank-based (or any other) cap, such as A3's
 * own flat K_max=4 that the paper's Fig 6a explicitly contrasts itself
 * against, is NOT part of Algorithm 1/2 and must stay off for any build
 * meant to reproduce the paper's results. This was built (and is kept here,
 * off by default) as a direct mitigation for a real, separately-documented
 * shared-uplink-saturation collapse observed at 300-node extended scale in
 * a companion internal study -- a genuine finding, not a reason to enable
 * it silently for a paper-matching run; see CG_CONF_RELAY_SHARDS_MAX below
 * for how the uncapped build still avoids an actual fixed-4 ceiling. */
/* #define CG_CONF_RANK_CAP 1 */
/* #define CG_CONF_RANK_CAP_THRESHOLD 512 */
/* #define CG_CONF_RELAY_SHARDS_NEAR_ROOT 6 */
/* #define CG_CONF_RELAY_SHARDS_FAR 4 */

/* Hold-before-use, ENABLED (see orchestra-rule-child-grandchild.c's own
 * header comment on CG_HOLD_BEFORE_USE). A newly-grown shard is only used
 * once this node's own EB has actually stated the new count at least
 * once -- closes the last gap against the original design proposal, at
 * the cost of a short, bounded delay (roughly one EB period, ~12-16s
 * measured) before a genuine increase takes effect. */
#define CG_CONF_HOLD_BEFORE_USE 1

/* Root-adjacent cells' own (shorter) slotframe period, library default. */
/* ORCHESTRA_CONF_IA_ROOT_PERIOD left at its library default (7). */

/* Redirect every TSCH_CALLBACK_IA_* hook to this rule's own functions,
 * rather than orchestra-rule-implicit-ack.c's -- that file is not compiled
 * into this example, so leaving these to tsch.h's own TSCH_WITH_IMPLICIT_ACK
 * auto-defaults (which point at orchestra_ia_* names) would fail to link.
 * Only cg_get_own_children/cg_child_eb_input do real work; every other name
 * here is a harmless stub (see orchestra-rule-child-grandchild.c).
 * TSCH_CALLBACK_UNICAST_DATA_INPUT is wired above, conditionally on
 * CG_CONF_IDLE_RECLAIM -- this scheme doesn't need it to tell relayed
 * traffic apart from self-originated traffic (both use the same uplink
 * cells), but idle-reclaim does need it to tell real traffic apart from
 * empty TSCH keepalives. */
#define TSCH_CALLBACK_IMPLICIT_ACK_ACTIVE    cg_implicit_ack_active
#define TSCH_CALLBACK_IA_OVERHEAR            cg_overhear
#define TSCH_CALLBACK_IA_OWN_PARENT          cg_get_own_parent
#define TSCH_CALLBACK_IA_PARENT_EB           cg_parent_eb_input
#define TSCH_CALLBACK_IA_CONGESTION          cg_get_own_congestion
#define TSCH_CALLBACK_IA_PARENT_CONGESTION   cg_parent_congestion_input
#define TSCH_CALLBACK_IA_CONFIRMATION_CYCLES cg_confirmation_cycles
#define TSCH_CALLBACK_IA_OWN_CHILDREN        cg_get_own_children
#define TSCH_CALLBACK_IA_CHILD_EB            cg_child_eb_input
#define TSCH_CALLBACK_NEW_ASFN               cg_new_asfn

/* RPL storing mode: required for nbr_routes-based child_added/removed --
 * this is how this rule learns who its own direct RPL children are at all,
 * independent of the (now-removed) routing-table-traversal traffic
 * estimate that used to also depend on storing mode. */
#define RPL_CONF_MOP RPL_MOP_STORING_NO_MULTICAST

/* Library default (7); no implicit-ack-specific reason to change it here. */
#define TSCH_CONF_MAC_MAX_FRAME_RETRIES 7

/* QUEUEBUF_CONF_NUM: Cooja-validated value was 128 (implicit-ack-tree-
 * multicell at this same TSCH_CONF_IA_SFS_SIZE), but that ran on
 * ContikiMote's effectively unlimited RAM. 16 is a trim for the real m3
 * board's 64KB RAM budget -- a starting point, not a verified number; see
 * this file's IoT-LAB header block for the empirical plan.
 *
 * This #if TARGET guard needed to be added: the 16 trim used to apply
 * unconditionally, which also silently capped the COOJA build's TSCH
 * per-neighbor queue at 16 (TSCH_QUEUE_NUM_PER_NEIGHBOR auto-derives from
 * this, see tsch-conf.h) -- confirmed live as the actual root cause of a
 * node-count-sweep PDR shortfall: N=100 showed 7199 "can't send packet ...
 * queue 15/16 16/16" drops in one run (86.3%/83.5% PDR, both seeds under
 * the 90% bar) and N=49 showed the same pattern starting to bite on one of
 * two seeds (87.5%). Restoring the already-Cooja-validated 128 fixed the
 * N=8/25/40/60 points outright (all >=99%), but a second, live-confirmed
 * pass at N=49 (seed 2, 84.9% PDR) still showed the IDENTICAL "can't send
 * packet ... queue 127/128 127/128" pattern SUSTAINED (not just at
 * bootstrap) at one specific relay node whose child itself relays a
 * multi-level subtree -- i.e. the per-neighbor queue, not any TSCH cell,
 * was still the binding constraint for a node carrying an unusually deep
 * subtree, exactly the kind of hash-based-schedule collision + relay-
 * aggregation pressure Algorithm 1's own deliberately-shallow (direct-
 * child-only) sizing rule accepts as a cost (see this rule's own header
 * comment) in exchange for not needing whole-subtree signaling. Since
 * Cooja motes have no real RAM ceiling, the right lever here is simply more
 * queue headroom, not a schedule/algorithm change -- raised again to 512
 * (global pool) with TSCH_QUEUE_CONF_NUM_PER_NEIGHBOR explicitly pinned to
 * 512 too (the tsch-conf.h auto-derivation table tops out at 256 for any
 * QUEUEBUF_CONF_NUM > 128, so raising only QUEUEBUF_CONF_NUM further would
 * grow the global pool but not the one per-neighbor queue that was actually
 * saturating). */
#if CONTIKI_TARGET_IOTLAB
#define QUEUEBUF_CONF_NUM 16
#else
/* A larger value was tried and does NOT compile: both
 * TSCH_QUEUE_NUM_PER_NEIGHBOR (per-neighbor ring buffer) and
 * TSCH_DEQUEUED_ARRAY_SIZE feed ringbufindex_init(..., uint8_t size)
 * (os/lib/ringbufindex.h) -- 256 silently truncates to 0 in a uint8_t and
 * fails the build outright (confirmed live: "unsigned conversion from
 * 'int' to 'uint8_t' changes value from '256' to '0' [-Werror=overflow]",
 * first in tsch.c's tsch_init(), then again for TSCH_DEQUEUED_ARRAY_SIZE
 * once QUEUEBUF_CONF_NUM was raised past 128 at all, since tsch-conf.h's
 * own auto-derivation table maps any QUEUEBUF_CONF_NUM > 128 to 256 for
 * both of these). 128 is therefore the real, hard practical ceiling for
 * QUEUEBUF_CONF_NUM in this codebase without changing that parameter's
 * type -- out of scope for this evaluation. See this file's own node-count-
 * sweep notes for what a remaining, confirmed-live N=49/seed2 queue
 * saturation at this ceiling actually needed instead (it was not more
 * buffer headroom -- see the diagnosis below). */
#define QUEUEBUF_CONF_NUM 128
#endif

/* Shortened from the library default (31): the same bootstrap-time DAO/
 * default_common contention livelock found and fixed for
 * implicit-ack-tree-multicell applies identically here -- a node cannot
 * install its own uplink cell until its first DAO gets a link-layer ACK,
 * and until then must contend with every other simultaneously-bootstrapping
 * node for this one shared cell. */
#define ORCHESTRA_CONF_COMMON_SHARED_PERIOD 11

/* Radio duty cycle measurement (node.c's periodic RDC report). */
#define ENERGEST_CONF_ON 1

#define LOG_CONF_LEVEL_RPL     LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_TCPIP   LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6    LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAC     LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_FRAMER  LOG_LEVEL_WARN

/* Per-slot TSCH logging: OFF for the paper-matching evaluation runs. Not
 * needed for PDR (node.c's own "APP TX"/"APP RX" lines are the only thing
 * the offline PDR parser reads) and, measured live on an 8-node/120-min
 * Cooja run, this alone produced 369k of the run's 489k total log lines
 * (55 MB for just 8 nodes) -- for the 100-150 node configs in the
 * node-count sweep this would balloon to multi-GB per run and add real,
 * avoidable wall-clock overhead (every per-slot line is a JNI callback into
 * Cooja's Java-side log sink). Flip back to 1 if per-slot schedule debugging
 * is ever needed again. */
#define TSCH_LOG_CONF_PER_SLOT 0

#endif /* PROJECT_CONF_H_ */
