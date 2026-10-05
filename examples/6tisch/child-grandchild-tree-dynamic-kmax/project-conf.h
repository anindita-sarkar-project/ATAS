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

/* Do not start TSCH at init, wait for NETSTACK_MAC.on() */
#define TSCH_CONF_AUTOSTART 0

/* 4-channel hopping over IEEE 802.15.4 channels {15, 25, 26, 20} -- the same
 * 4 channels as the paper's FIT IoT-LAB evaluation (Table II). Set
 * explicitly rather than relying on the library default, and used for
 * joining too so a scanning node only listens where EBs can be. */
#undef TSCH_CONF_DEFAULT_HOPPING_SEQUENCE
#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4
#undef TSCH_CONF_JOIN_HOPPING_SEQUENCE
#define TSCH_CONF_JOIN_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4

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

/* CG_CONF_MAX_CHILDREN deliberately NOT set: under CG_CONF_DYNAMIC_KMAX the
 * children table defaults to NBR_TABLE_MAX_NEIGHBORS, so the number of
 * direct children a node can track is bounded only by how many neighbors
 * it can have at all, not by a separate fixed child limit (previously 10). */

/* Every cell is a TSCH link, and with no K_max a relay's cell count grows
 * with its children: each node holds sum over its children c of K_c Rx
 * cells, plus its own K_S Tx cells, plus the EB/common rules' links. The
 * library default of 32 links would be the real limit on K otherwise. */
#define TSCH_SCHEDULE_CONF_MAX_LINKS 128

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
 * 101. */
#define TSCH_CONF_IA_SFS_SIZE 67

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

/* Load bonus X_S, queue-based (the default under CG_CONF_DYNAMIC_KMAX): no
 * traffic-rate threshold, so it works at any send rate. Once per slotframe
 * a node checks whether the packet at the head of its queue to its parent
 * is still the same one -- i.e. a full round of its K_S cells passed
 * without clearing it. If so its cells are not enough and it takes one
 * extra cell at once; it gives it back after CG_CONF_LOAD_RELEASE_WINDOWS
 * windows of CG_CONF_LOAD_CHECK_PERIOD with no such shortage. */
#define CG_CONF_LOAD_BONUS 1
/* #define CG_CONF_LOAD_CHECK_PERIOD (CLOCK_SECOND * 60) */
/* #define CG_CONF_LOAD_RELEASE_WINDOWS 5 */
#if defined(CG_CONF_IDLE_RECLAIM) && CG_CONF_IDLE_RECLAIM
#define TSCH_CALLBACK_UNICAST_DATA_INPUT cg_data_input
#endif

/* Rank-aware cap, ENABLED (see orchestra-rule-child-grandchild.c's own
 * header comment on CG_RANK_CAP). Direct mitigation for the 300-node
 * shared-uplink-saturation collapse documented in the companion paper's
 * extended-scale check (Table~extendedscale): nodes within a couple of
 * hops of the root get a larger shard cap than everyone else, using RPL
 * rank they already compute for routing -- no new signaling of any kind.
 * CG_CONF_RANK_CAP_THRESHOLD assumes the library default
 * RPL_MIN_HOPRANKINC (256); retune if your deployment overrides it. */
#define CG_CONF_RANK_CAP 0

/* Dynamic K: no K_max. A node with k direct children uses K_S = k + 1
 * (+1 load bonus X_S) cells toward its parent, and every parent -- the BR
 * included -- installs exactly the K_c = k_c + 1 Rx cells each of its
 * children advertises. No policy cap and no compile-time per-link limit
 * (CG_CONF_DYNAMIC_KMAX_LIMIT no longer exists) -- see orchestra-rule-
 * child-grandchild.c's own header comment on CG_DYNAMIC_KMAX. */
#define CG_CONF_DYNAMIC_KMAX 1

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

/* QUEUEBUF_CONF_NUM: kept at the same value validated for
 * implicit-ack-tree-multicell at this same TSCH_CONF_IA_SFS_SIZE. */
#define QUEUEBUF_CONF_NUM 128

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

#define TSCH_LOG_CONF_PER_SLOT 1

/* Real hardware: TSCH/Orchestra paths LOG from inside the TSCH slot-operation
 * rtimer ISR (same issue found and fixed for IADS in implicit-ack-tree). On the CC26x2 the UART
 * driver pends on a semaphore inside printf, which from ISR context puts the
 * CPU into WFI with nothing able to wake it: the node hung (or was reset by
 * the watchdog) right after joining. Drop log output produced in exception
 * context (IPSR != 0) on Cortex-M; Cooja's printf never blocks, so it keeps
 * the full traces. ISR-side events still reach the console via TSCH_LOG_ADD. */
#if !CONTIKI_TARGET_COOJA && defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
#define LOG_CONF_OUTPUT(...) do { \
    uint32_t log_ipsr_; \
    __asm__ volatile ("mrs %0, ipsr" : "=r" (log_ipsr_)); \
    if(log_ipsr_ == 0) { \
      printf(__VA_ARGS__); \
    } \
  } while(0)
#endif

/* Real-hardware testbed range (~3 m per hop): low TX power plus an RSSI
 * threshold, so a node only hears the level next to it and the 3-level tree
 * forms from physical placement alone. Build with IADS_CALIBRATE=1 (make
 * CALIBRATE=1) to disable the threshold and just log EB RSSI for measuring.
 * IADS_RSSI_MIN can be overridden at build time (make RSSI_MIN=-70). */
#if !CONTIKI_TARGET_COOJA
/* TX power (dBm): -20 for the 1 m-per-hop testbed (2026-10-04), -10 for the
 * earlier 3 m one. Override at build time with `make TXPOWER=-15`. */
#ifndef IADS_TXPOWER_DBM
#define IADS_TXPOWER_DBM      -20
#endif
#define RF_CONF_TXPOWER_DBM   IADS_TXPOWER_DBM
#define TSCH_CONF_LOG_EB_RSSI 1
/* Measured at -10 dBm (2026-09-29): 3 m -69, 6 m -73, 10 m -83 dBm mean.
 * -71 +/- 1 dB: 3 m neighbours accepted (avg >= -70), 6 m dropped (< -72). */
/* 1 m testbed, measured at -20 dBm (2026-10-04): 1 m -75.3, 3 m -78.4 dBm mean.
 * -77 +/- 1 dB: neighbours averaging >= -76 accepted, < -78 dropped. */
#ifndef IADS_RSSI_MIN
#define IADS_RSSI_MIN         -77
#endif
#define TSCH_CONF_RX_RSSI_HYST 1
#if IADS_CALIBRATE
/* Dense RSSI samples: every received data frame logged, a packet every 2 s */
#define TSCH_CONF_LOG_RX_RSSI 1
#define IADS_CONF_SEND_INTERVAL_S 2  /* node.c: dense samples for calibration */
#else
#define TSCH_CONF_RX_RSSI_MIN IADS_RSSI_MIN
#endif
/* 37-board testbed (root + 36 nodes, 2026-10-04): the storing-mode root needs
 * a route per node, and in a dense 1 m layout a node can hear many neighbours.
 * Contiki-NG's defaults (16 / 16) are too small. Hardware only, so Cooja runs
 * keep their original configuration. */
#define NETSTACK_MAX_ROUTE_ENTRIES   40
#define NBR_TABLE_CONF_MAX_NEIGHBORS 32
#define TSCH_CONF_RX_RSSI_NBRS       40
#endif /* !CONTIKI_TARGET_COOJA */

#endif /* PROJECT_CONF_H_ */
