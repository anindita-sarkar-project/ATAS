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
 *         Smoke-test node for the autonomous implicit-ack tree scheduler
 *         (os/services/orchestra/orchestra-rule-implicit-ack.c), extended
 *         with hop-by-hop in-network aggregation of the application traffic
 *         itself.
 *
 *         Motivation: RELAY_TX shards (see orchestra-rule-implicit-ack.c)
 *         give any one parent-child relationship at most a fixed, small
 *         multiplier (ORCHESTRA_IA_RELAY_TX_SHARDS) of extra capacity. Near
 *         the root, traffic fans in from an entire subtree, and subtree size
 *         grows with network size -- so past some scale, some node's
 *         aggregate arrival rate exceeds what any fixed multiplier can
 *         drain, and packets are dropped at the TSCH queue (tsch_queue_add_
 *         packet() failing outright, "can't send packet ... queue 128/128"),
 *         not at the radio. Adding more parallel cells cannot fix this in
 *         general; the fix here instead reduces how many separate
 *         transmissions are needed to move the same amount of application
 *         data, by combining multiple nodes' tiny reports into one frame per
 *         hop instead of relaying each one individually.
 *
 *         Every non-root node still originates its own periodic report
 *         exactly as before, but instead of addressing it directly to the
 *         root (which stock 6LoWPAN/RPL would then transparently forward,
 *         hop by hop, entirely below the application), it now always
 *         addresses its own current aggregate to its own immediate RPL
 *         parent, and every node -- not just the root -- terminates and
 *         inspects the UDP payload it receives: it unpacks whatever reports
 *         a child just relayed, folds them into its own outgoing aggregate
 *         buffer alongside its own report, and periodically flushes that
 *         combined buffer as a single packet to its own parent. The root is
 *         simply the node for which "next hop" has no further hop: it
 *         unpacks and logs each item instead of forwarding it onward. This
 *         only ever changes application-layer packet counts and payload
 *         sizes -- it does not touch ROOT_ADJACENT, UPLINK, RELAY_TX,
 *         SELF_OVERHEAR, or the implicit-ack confirmation mechanism, all of
 *         which keep operating exactly as before on whatever the
 *         application now hands them.
 *
 */

#include "contiki.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "sys/energest.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/ipv6/uip-ds6-nbr.h"
#include "net/mac/tsch/tsch.h"
#include "net/mac/tsch/tsch-queue.h"
#include "net/routing/routing.h"
#include "net/ipv6/simple-udp.h"
#include "net/queuebuf.h"
#include "net/routing/rpl-classic/rpl.h"
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_PORT 8765
#if defined(APP_CONF_SEND_INTERVAL_MS)
/* Exact interval in ms (e.g. 7500 for 8 readings/min), used by the APAS-only sweeps. */
#define SEND_INTERVAL ((clock_time_t)((uint32_t)APP_CONF_SEND_INTERVAL_MS * CLOCK_SECOND / 1000))
#elif defined(IADS_CONF_SEND_INTERVAL_S)
#define SEND_INTERVAL (IADS_CONF_SEND_INTERVAL_S * CLOCK_SECOND)
#else
#define SEND_INTERVAL (15 * CLOCK_SECOND)
#endif
#define RDC_REPORT_INTERVAL (60 * CLOCK_SECOND)

/* Cooja: node 1 is the root. Real hardware (node_id comes from the factory
 * IEEE address): build the root image with `make ROOT=1`. */
#if CONTIKI_TARGET_COOJA
#define NODE_IS_ROOT (node_id == 1)
#elif defined(IADS_CONF_IS_ROOT)
#define NODE_IS_ROOT IADS_CONF_IS_ROOT
#else
#define NODE_IS_ROOT 0
#endif

/* Testbed telemetry (hardware default; Cooja keeps the original 3-byte items
 * and log lines). On hardware node_id is 16-bit, so an item carries a 16-bit
 * origin (the 8-bit one would alias boards, e.g. 0xdebc and 0x63bc), plus the
 * low 16 bits of the ASN at which the origin generated it, for end-to-end
 * latency at the root (unwrapped there against the reception ASN; valid for
 * latencies under 2^16 slots). The root logs one IADS-RX line per item and an
 * IADS-CLK (ASN, time) sample every second; every node also sends an
 * IADS-STAT report (RDC counters, rank, parent) straight to the root every
 * STATS_INTERVAL on STATS_PORT. Parse the root's log with hw/analyze.py. */
#ifdef IADS_CONF_TELEMETRY
#define IADS_TELEMETRY IADS_CONF_TELEMETRY
#else
#define IADS_TELEMETRY (!CONTIKI_TARGET_COOJA)
#endif
#if IADS_TELEMETRY
#define STATS_PORT (UDP_PORT + 1)
#define STATS_INTERVAL (120 * CLOCK_SECOND)
#define ITEM_WIRE 6  /* origin(2) seq(2) asn16(2) */
#else
#define ITEM_WIRE 3  /* origin(1) seq(2) */
#endif

/* Aggregation parameters. Each item is (origin node id, origin's own
 * sequence number): 1 + 2 = 3 bytes on the wire. AGG_FLUSH_INTERVAL trades
 * added latency (how long an item may sit in an intermediate node's buffer
 * before being forwarded) against how many items typically get bundled into
 * one outgoing frame.
 *
 * MAX_AGG_ITEMS was originally set to 16 (49 wire bytes) purely to stay
 * comfortably clear of one 802.15.4 frame's usable payload after 6LoWPAN/
 * IPv6/UDP compression overhead. Measurement at 100/150 nodes showed that
 * value was actually far too conservative for a different reason entirely:
 * "IATRACE agg overflow-drop" (add_item()'s forced drop-oldest fallback,
 * originally meant only for the rare bootstrap case of no route yet) was
 * firing thousands of times over a single run -- 6357 times at 100 nodes --
 * driven by "IATRACE agg defer-global" (the node-wide TSCH packet pool
 * being full) firing *8071* times, meaning the underlying congestion
 * routinely outlasted a 16-item buffer's capacity to simply wait it out.
 * Raising this to 24 (73 wire bytes, confirmed fragmentation-free) let
 * PDR nearly double at 100 nodes (20.2%->39.4%) and more than double at 150
 * (10.5%->23.2%), simply by giving pending items more headroom to survive a
 * sustained congestion episode without being dropped. 32 (97 wire bytes)
 * was also tried and made things much worse (150 nodes: 23.2%->8.4%): it
 * crosses the actual single-frame payload budget once 6LoWPAN/IPv6/UDP
 * headers are added, confirmed by 6LoWPAN fragmentation appearing in the
 * log (1071 occurrences, versus zero at 24) -- a fragmented aggregate needs
 * every fragment to arrive for the whole batch to reassemble, turning one
 * lost frame into a lost aggregate instead of one lost item. 24 is the
 * validated sweet spot: as large as tested without crossing into
 * fragmentation, and already a large, measured win over the original,
 * overly-conservative value. */
#define AGG_FLUSH_INTERVAL (5 * CLOCK_SECOND)
#if IADS_TELEMETRY
/* 6-byte items: 12 keeps the same 73-byte, fragmentation-free frame */
#define MAX_AGG_ITEMS 12
#else
#define MAX_AGG_ITEMS 24
#endif
#define AGG_WIRE_MAX (1 + MAX_AGG_ITEMS * ITEM_WIRE)

/* EWMA-predicted congestion toward our own parent, in percent occupancy of
 * that neighbor's TSCH queue (0..100). A raw, instantaneous occupancy check
 * only ever tells us "full right now" -- by the time that is true, a send
 * attempt is essentially guaranteed to be wasted or, worse, contend for the
 * one slot that frees up right as some other node's packet also arrives.
 * Smoothing it into an EWMA instead answers a different, more useful
 * question -- "has this link been *trending* congested over the last
 * several flush cycles" -- which is what we actually want in order to
 * decide how much to risk bundling into a single frame *before* the queue
 * is already full, not just react once it is. EWMA_SHIFT=3 gives new
 * samples 1/8 weight (~8 flush cycles, ~40s, to mostly settle to a new
 * steady state) -- smooth enough to ignore one-off blips, responsive enough
 * to track a real, sustained shift within well under a minute. */
#define EWMA_SHIFT 3
#define CONGESTION_HIGH_PCT 50
#define CONGESTION_SMALL_CAP 4

typedef struct {
#if IADS_TELEMETRY
  uint16_t origin;
  uint16_t seq;
  uint16_t asn16;
#else
  uint8_t origin;
  uint16_t seq;
#endif
} agg_item_t;

static struct simple_udp_connection udp_conn;
#if IADS_TELEMETRY
static struct simple_udp_connection stats_conn;

struct iads_stat {
  uint16_t rank;
  uint16_t parent;      /* node ID of our time source (RPL parent) */
  uint32_t uptime_ms;
  uint32_t radio_tx_ms; /* cumulative ENERGEST TRANSMIT */
  uint32_t radio_rx_ms; /* cumulative ENERGEST LISTEN */
  uint16_t own_seq;     /* items generated so far */
} __attribute__((packed));

#define ENERGEST_TO_MS(t) ((uint32_t)(((uint64_t)(t) * 1000) / ENERGEST_SECOND))

/* Microseconds since boot, from the 32-bit rtimer extended to 64 bits */
static uint64_t
now_us(void)
{
  static uint32_t last;
  static uint64_t high;
  uint32_t now = (uint32_t)RTIMER_NOW();
  if(now < last) {
    high += (uint64_t)1 << 32;
  }
  last = now;
  return ((high + now) * 1000000ULL) / RTIMER_SECOND;
}
#endif /* IADS_TELEMETRY */
static agg_item_t agg_buf[MAX_AGG_ITEMS];
static uint8_t agg_count;
static unsigned own_seq;
static int32_t occ_ewma_pct;

/*---------------------------------------------------------------------------*/
static void
print_rdc(void)
{
  static uint64_t last_transmit, last_listen, last_total;
  uint64_t cpu, lpm, transmit, listen, total;
  uint64_t d_transmit, d_listen, d_total;

  energest_flush();

  cpu = energest_type_time(ENERGEST_TYPE_CPU);
  lpm = energest_type_time(ENERGEST_TYPE_LPM);
  transmit = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  listen = energest_type_time(ENERGEST_TYPE_LISTEN);
  total = cpu + lpm;

  d_transmit = transmit - last_transmit;
  d_listen = listen - last_listen;
  d_total = total - last_total;

  if(d_total > 0) {
    LOG_INFO("RDC tx %lu%% rx %lu%% total %lu%% (all-time tx %lu%% rx %lu%%)\n",
              (unsigned long)((100ULL * d_transmit) / d_total),
              (unsigned long)((100ULL * d_listen) / d_total),
              (unsigned long)((100ULL * (d_transmit + d_listen)) / d_total),
              (unsigned long)(total > 0 ? (100ULL * transmit) / total : 0),
              (unsigned long)(total > 0 ? (100ULL * listen) / total : 0));
  }

  if(d_total > 0) {
    /* Same interval, radio-on share in 1/1000 % (the line above truncates to
     * whole percent, which is coarse at a 2-3 % duty cycle). */
    LOG_INFO("RDCm %lu\n", (unsigned long)((100000ULL * (d_transmit + d_listen)) / d_total));
  }

  last_transmit = transmit;
  last_listen = listen;
  last_total = total;
}
/*---------------------------------------------------------------------------*/
/* Packs agg_buf[0..agg_count) onto the wire and sends it to our own current
 * RPL parent (uip_ds6_defrt_choose() -- the default-route next hop, which
 * for RPL is exactly the preferred parent), then clears the buffer. A no-op
 * if there is nothing queued, or if we have no route yet (items simply stay
 * buffered, bounded by MAX_AGG_ITEMS, until we do).
 *
 * Critical correctness point: simple_udp_sendto() is fire-and-forget --
 * uip_udp_packet_sendto() never reports back whether the underlying
 * tsch_queue_add_packet() (the MAC-layer per-neighbor ring buffer) actually
 * accepted the frame or rejected it outright ("can't send packet ... queue
 * 128/128", logged deep in TSCH, invisible up here). Clearing the buffer
 * unconditionally after every send call -- the first version of this file
 * did exactly that -- meant that every such rejection silently discarded
 * this entire aggregate at once, up to MAX_AGG_ITEMS reports in one shot,
 * rather than the single report an unaggregated design would have lost in
 * the same circumstance. That is a real, measured amplification: it is
 * exactly why aggregation regressed at 100/150 nodes, precisely the scales
 * where this queue is already most often full, rather than helping there
 * the way it does at 25-60. We instead check both the neighbor's own
 * current queue occupancy *and* the global packet pool (tsch_queue_add_
 * packet() can fail on either: the per-neighbor tx_ringbuf, or the single
 * node-wide packet_memb/queuebuf pool shared across every neighbor -- the
 * "queue %u/%u %u/%u" in TSCH's own "can't send packet" log is exactly
 * these two counts side by side, and checking only the first one turned out
 * to leave this fix a complete no-op: at 100 nodes the global pool, not the
 * per-neighbor one, was the one actually saturated) before committing to
 * the send, and defer (leave the buffer intact for the next tick) if either
 * is already full, turning a guaranteed silent multi-report loss into, at
 * worst, added latency. */
static void
flush_aggregate(void)
{
  const uip_ipaddr_t *parent;
  const uip_lladdr_t *parent_lladdr;
  struct tsch_neighbor *nbr = NULL;
  uint8_t wire[AGG_WIRE_MAX];
  uint8_t i, n_to_send;
  int32_t occ_pct = 0;

  if(agg_count == 0) {
    return;
  }
  if(tsch_queue_global_packet_count() >= QUEUEBUF_NUM) {
    LOG_INFO("IATRACE agg defer-global count=%u\n", agg_count);
    return;
  }
  parent = uip_ds6_defrt_choose();
  if(parent == NULL) {
    LOG_INFO("IATRACE agg defer-noroute count=%u\n", agg_count);
    return;
  }
  parent_lladdr = uip_ds6_nbr_lladdr_from_ipaddr(parent);
  if(parent_lladdr != NULL) {
    nbr = tsch_queue_get_nbr((const linkaddr_t *)parent_lladdr);
  }
  if(nbr != NULL) {
    if(tsch_queue_nbr_packet_count(nbr) >= TSCH_QUEUE_NUM_PER_NEIGHBOR) {
      LOG_INFO("IATRACE agg defer-nbrfull count=%u\n", agg_count);
      return;
    }
    occ_pct = (100L * tsch_queue_nbr_packet_count(nbr)) / TSCH_QUEUE_NUM_PER_NEIGHBOR;
  }
  /* occ_pct == 0 whenever we couldn't resolve nbr (e.g. no ND entry yet):
   * treated as "no congestion signal available" and pulls the EWMA gently
   * toward 0 rather than skipping the update, so a lookup failure can never
   * get the EWMA stuck high from a stale reading. occ_ewma_pct is tracked
   * but deliberately *not* used to shrink the outgoing batch below: that was
   * tried and measured to help slightly at 100 nodes (19.1%->19.7%) but
   * regress badly at 150 (10.5%->4.4%). Root cause once traced: shrinking
   * the batch under predicted congestion only pays off if the risk being
   * managed is "losing a big batch" -- but the queue-full evidence at 150
   * nodes points to a different constraint, too few TSCH transmission
   * opportunities per second to drain the backlog at all, regardless of
   * frame size. Splitting one aggregate into several smaller ones under
   * *that* constraint means needing more opportunities, not fewer, to move
   * the same data, competing harder for the one resource that is actually
   * scarce. Left computed (and left as the obvious hook for a future,
   * opportunity-aware rather than batch-size-aware, adaptive policy) but
   * inert for now: every flush always sends its entire current buffer once
   * we've confirmed the queue has room for it. */
  occ_ewma_pct += (occ_pct - occ_ewma_pct) >> EWMA_SHIFT;

  n_to_send = agg_count;

  wire[0] = n_to_send;
  for(i = 0; i < n_to_send; i++) {
    uint8_t *w = &wire[1 + i * ITEM_WIRE];
#if IADS_TELEMETRY
    w[0] = (uint8_t)(agg_buf[i].origin & 0xff);
    w[1] = (uint8_t)(agg_buf[i].origin >> 8);
    w[2] = (uint8_t)(agg_buf[i].seq & 0xff);
    w[3] = (uint8_t)(agg_buf[i].seq >> 8);
    w[4] = (uint8_t)(agg_buf[i].asn16 & 0xff);
    w[5] = (uint8_t)(agg_buf[i].asn16 >> 8);
#else
    w[0] = agg_buf[i].origin;
    w[1] = (uint8_t)(agg_buf[i].seq & 0xff);
    w[2] = (uint8_t)((agg_buf[i].seq >> 8) & 0xff);
#endif
  }
  LOG_INFO("IATRACE agg flush n=%u\n", n_to_send);
  simple_udp_sendto(&udp_conn, wire, (uint16_t)(1 + n_to_send * ITEM_WIRE), parent);

  if(n_to_send < agg_count) {
    memmove(&agg_buf[0], &agg_buf[n_to_send], (size_t)(agg_count - n_to_send) * sizeof(agg_item_t));
    agg_count = (uint8_t)(agg_count - n_to_send);
  } else {
    agg_count = 0;
  }
}
/*---------------------------------------------------------------------------*/
static void
#if IADS_TELEMETRY
add_item(uint16_t origin, uint16_t seq, uint16_t asn16)
#else
add_item(uint8_t origin, uint16_t seq)
#endif
{
  if(agg_count >= MAX_AGG_ITEMS) {
    flush_aggregate();
    if(agg_count >= MAX_AGG_ITEMS) {
      /* Still full: no route yet. Drop the oldest queued item rather than
       * silently refusing every new one forever once the buffer first
       * fills during bootstrap. */
      LOG_INFO("IATRACE agg overflow-drop origin=%u\n", agg_buf[0].origin);
      memmove(&agg_buf[0], &agg_buf[1], (MAX_AGG_ITEMS - 1) * sizeof(agg_item_t));
      agg_count = MAX_AGG_ITEMS - 1;
    }
  }
  agg_buf[agg_count].origin = origin;
  agg_buf[agg_count].seq = seq;
#if IADS_TELEMETRY
  agg_buf[agg_count].asn16 = asn16;
#endif
  agg_count++;
}
/*---------------------------------------------------------------------------*/
PROCESS(node_process, "Implicit-ack tree test node");
PROCESS(rdc_process, "RDC reporting");
AUTOSTART_PROCESSES(&node_process, &rdc_process);
/*---------------------------------------------------------------------------*/
/* Every node (root included) registers this same callback. The root has no
 * further parent to forward to, so it terminates each item here and logs it
 * in exactly the format the existing steady-state analysis tooling already
 * parses ("received N bytes (seq S) from fd00::..."), synthesizing a
 * fd00::-style address whose final hex group is the origin's own node id --
 * the only part of that address string the analysis ever actually reads.
 * Every other node instead folds each item into its own outgoing aggregate,
 * to be flushed alongside its own report on the next AGG_FLUSH_INTERVAL
 * tick (or immediately, if that fills this node's own buffer first). */
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                const uint8_t *data, uint16_t datalen)
{
  uint8_t n, i;

  if(datalen < 1) {
    return;
  }
  n = data[0];
  if(datalen != (uint16_t)(1 + n * ITEM_WIRE)) {
    return;
  }
  for(i = 0; i < n; i++) {
    const uint8_t *w = &data[1 + i * ITEM_WIRE];
#if IADS_TELEMETRY
    uint16_t origin = (uint16_t)(w[0] | (w[1] << 8));
    uint16_t seq = (uint16_t)(w[2] | (w[3] << 8));
    uint16_t asn16 = (uint16_t)(w[4] | (w[5] << 8));
#else
    uint8_t origin = w[0];
    uint16_t seq = (uint16_t)(w[1] | (w[2] << 8));
#endif
    if(NODE_IS_ROOT) {
      LOG_INFO("received 4 bytes (seq %u) from fd00::%x:%x:%x:%x\n",
                seq, origin, origin, origin, origin);
#if IADS_TELEMETRY
      /* One line per item for hw/analyze.py (asn_tx16 is unwrapped there) */
      LOG_INFO("IADS-RX src=%u seq=%u asn_tx16=%u asn_rx=%u:%lu t_us=%llu len=%u\n",
               origin, seq, asn16, tsch_current_asn.ms1b,
               (unsigned long)tsch_current_asn.ls4b, (unsigned long long)now_us(), ITEM_WIRE);
#endif
    } else {
#if IADS_TELEMETRY
      add_item(origin, seq, asn16);
#else
      add_item(origin, seq);
#endif
    }
  }
}
#if IADS_TELEMETRY
/*---------------------------------------------------------------------------*/
static void
stats_rx_callback(struct simple_udp_connection *c,
                  const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                  const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                  const uint8_t *data, uint16_t datalen)
{
  struct iads_stat s;
  if(!NODE_IS_ROOT || datalen != sizeof(s)) {
    return;
  }
  memcpy(&s, data, sizeof(s));
  LOG_INFO("IADS-STAT src=%u rank=%u parent=%u up_ms=%lu tx_ms=%lu rx_ms=%lu own_seq=%u t_us=%llu\n",
           (unsigned)((sender_addr->u8[14] << 8) | sender_addr->u8[15]), s.rank, s.parent,
           (unsigned long)s.uptime_ms, (unsigned long)s.radio_tx_ms, (unsigned long)s.radio_rx_ms,
           s.own_seq, (unsigned long long)now_us());
}
/*---------------------------------------------------------------------------*/
static void
send_stats(void)
{
  struct iads_stat s;
  uip_ipaddr_t root;
  struct tsch_neighbor *n = tsch_queue_get_time_source();
  const linkaddr_t *ts = n != NULL ? tsch_queue_get_nbr_address(n) : NULL;
  rpl_dag_t *dag = rpl_get_any_dag();
  if(!NETSTACK_ROUTING.node_is_reachable() || !NETSTACK_ROUTING.get_root_ipaddr(&root)) {
    return;
  }
  memset(&s, 0, sizeof(s));
  s.rank = dag != NULL ? dag->rank : 0xffff;
  s.parent = ts != NULL ? (ts->u8[LINKADDR_SIZE - 2] << 8) | ts->u8[LINKADDR_SIZE - 1] : 0;
  energest_flush();
  s.uptime_ms = (uint32_t)(now_us() / 1000);
  s.radio_tx_ms = ENERGEST_TO_MS(energest_type_time(ENERGEST_TYPE_TRANSMIT));
  s.radio_rx_ms = ENERGEST_TO_MS(energest_type_time(ENERGEST_TYPE_LISTEN));
  s.own_seq = (uint16_t)own_seq;
  simple_udp_sendto(&stats_conn, &s, sizeof(s), &root);
}
#endif /* IADS_TELEMETRY */
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  static struct etimer et_send;
  static struct etimer et_flush;
#if IADS_TELEMETRY
  static struct etimer et_stats;
#endif

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);
#if IADS_TELEMETRY
  simple_udp_register(&stats_conn, STATS_PORT, NULL, STATS_PORT, stats_rx_callback);
#endif

  if(NODE_IS_ROOT) {
    NETSTACK_ROUTING.root_start();
  }
  NETSTACK_MAC.on();

  if(NODE_IS_ROOT) {
    /* The root only receives (and terminates) application data in this
     * test; it must not also run the periodic sender/aggregator below. */
#if IADS_TELEMETRY
    /* (ASN, time) samples on the root's clock, for latency in analyze.py */
    etimer_set(&et_send, CLOCK_SECOND);
    while(1) {
      PROCESS_YIELD_UNTIL(etimer_expired(&et_send));
      etimer_reset(&et_send);
      LOG_INFO("IADS-CLK asn=%u:%lu t_us=%llu\n", tsch_current_asn.ms1b,
               (unsigned long)tsch_current_asn.ls4b, (unsigned long long)now_us());
    }
#endif /* IADS_TELEMETRY */
    PROCESS_WAIT_EVENT_UNTIL(0);
  }

  etimer_set(&et_send, SEND_INTERVAL);
  etimer_set(&et_flush, AGG_FLUSH_INTERVAL);
#if IADS_TELEMETRY
  etimer_set(&et_stats, STATS_INTERVAL);
#endif
  while(1) {
    PROCESS_YIELD();
    if(etimer_expired(&et_send)) {
      etimer_reset(&et_send);
      if(NETSTACK_ROUTING.node_is_reachable()
#ifdef APP_CONF_SILENT_FROM_ID
         /* Idle-reclaim experiment only: nodes with id >= APP_CONF_SILENT_FROM_ID
          * send nothing between 600 s and 1500 s. */
         && !(node_id >= APP_CONF_SILENT_FROM_ID
              && clock_seconds() >= 600 && clock_seconds() < 1500)
#endif
         ) {
        LOG_INFO("sending %u to root\n", own_seq);
#if IADS_TELEMETRY
        add_item((uint16_t)node_id, (uint16_t)own_seq, (uint16_t)tsch_current_asn.ls4b);
#else
        add_item((uint8_t)node_id, (uint16_t)own_seq);
#endif
        own_seq++;
      } else {
        LOG_INFO("not reachable yet\n");
      }
    }
    if(etimer_expired(&et_flush)) {
      etimer_reset(&et_flush);
      flush_aggregate();
    }
#if IADS_TELEMETRY
    if(etimer_expired(&et_stats)) {
      etimer_reset(&et_stats);
      send_stats();
    }
#endif
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(rdc_process, ev, data)
{
  static struct etimer et;

  PROCESS_BEGIN();

  etimer_set(&et, RDC_REPORT_INTERVAL);
  while(1) {
    PROCESS_YIELD_UNTIL(etimer_expired(&et));
    etimer_reset(&et);
    print_rdc();
  }

  PROCESS_END();
}
