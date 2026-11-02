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
 *         APAS evaluation traffic generator -- plain, non-aggregating,
 *         one-UDP-packet-per-node-per-period client/server, matching the
 *         paper's Table II methodology directly: every non-root node
 *         generates its own periodic application packet and sends it
 *         end-to-end to the root (node 1) via stock RPL storing-mode
 *         routing; the root logs each arrival. PDR is computed externally
 *         from these logs as (packets logged received at the root during
 *         the post-warmup window) / (packets logged sent during that same
 *         window), exactly the paper's definition.
 *
 *         This replaces an earlier version of this file that implemented
 *         hop-by-hop application-layer aggregation (batching several
 *         nodes' reports into one relayed frame) -- a mechanism the paper
 *         does not describe or evaluate at all. That design is still
 *         useful elsewhere (see implicit-ack-tree-multicell/node.c, where
 *         it originated) but does not belong in the APAS paper-matching
 *         evaluation, so it has been removed here in favor of this direct,
 *         unaggregated sender.
 *
 *         Send rate is configurable at build time via
 *         APP_SEND_RATE_CONF (packets per minute per node, default 4,
 *         matching Table II's default traffic rate), so the same firmware
 *         source serves the whole traffic-rate sweep {1,2,4,8,10} pkt/min
 *         by passing -DAPP_SEND_RATE_CONF=<rate> on the Makefile command
 *         line. Slotframe length L is controlled the same way via
 *         TSCH_CONF_IA_SFS_SIZE (see project-conf.h).
 */

#include "contiki.h"
#include "sys/node-id.h"
#include "sys/log.h"
#include "sys/energest.h"
#include "net/ipv6/uip-ds6-route.h"
#include "net/mac/tsch/tsch.h"
#include "net/routing/routing.h"
#include "net/ipv6/simple-udp.h"
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_PORT 8765

/* Packets per minute per node -- Table II traffic-rate sweep values are
 * {1,2,4,8,10}; 4 is the default used whenever a build does not override
 * this (e.g. the node-count and slotframe-length sweeps, which both fix
 * the rate at 4 pkt/min per Table II). */
#ifndef APP_SEND_RATE_CONF
#define APP_SEND_RATE_CONF 4
#endif
#define SEND_INTERVAL ((60 * CLOCK_SECOND) / APP_SEND_RATE_CONF)

#define RDC_REPORT_INTERVAL (60 * CLOCK_SECOND)

/* Wire payload: 2 bytes origin node id + 2 bytes origin-local sequence
 * number. Kept intentionally tiny (this is not what the paper's congestion
 * behavior is about) and self-describing, so the root's log line alone
 * (no IPv6-address-to-node-id bookkeeping needed) is enough for the offline
 * PDR parser to attribute every arrival to the exact (origin, seq) pair the
 * corresponding send-side log line reported. */
typedef struct {
  uint16_t origin;
  uint16_t seq;
} app_payload_t;

static struct simple_udp_connection udp_conn;

/*---------------------------------------------------------------------------*/
#if CONTIKI_TARGET_IOTLAB
static void
print_banner(void)
{
  unsigned i;
  printf("\n==== APAS node up ====\n");
  printf("MAC=");
  for(i = 0; i < LINKADDR_SIZE; i++) {
    printf("%02x%s", linkaddr_node_addr.u8[i],
           (i < LINKADDR_SIZE - 1) ? ":" : "\n");
  }
  printf("logical_node_id=%u role=%s\n", node_id,
         (node_id == 1) ? "ROOT" : ((node_id == 0) ? "UNMAPPED" : "LEAF"));
  printf("=======================\n");
}
#endif
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

  last_transmit = transmit;
  last_listen = listen;
  last_total = total;
}
/*---------------------------------------------------------------------------*/
PROCESS(node_process, "APAS evaluation node");
PROCESS(rdc_process, "RDC reporting");
#if CONTIKI_TARGET_IOTLAB
PROCESS(banner_process, "Periodic identity banner");
AUTOSTART_PROCESSES(&node_process, &rdc_process, &banner_process);
#else
AUTOSTART_PROCESSES(&node_process, &rdc_process);
#endif
/*---------------------------------------------------------------------------*/
/* Only the root ever receives application data in this test (every other
 * node addresses its packet directly to the root's own IPv6 address, via
 * stock RPL storing-mode multi-hop forwarding -- no application-layer
 * relaying/aggregation of any kind). The PDR parser greps for this exact
 * "APP RX" line. */
static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr, uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr, uint16_t receiver_port,
                const uint8_t *data, uint16_t datalen)
{
  app_payload_t pkt;
  if(datalen != sizeof(pkt)) {
    return;
  }
  memcpy(&pkt, data, sizeof(pkt));
  LOG_INFO("APP RX from=%u seq=%u\n", pkt.origin, pkt.seq);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(node_process, ev, data)
{
  static struct etimer et_send;
  static uint16_t seq;
  static uip_ipaddr_t dst;
  static app_payload_t pkt;

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn, UDP_PORT, NULL, UDP_PORT, udp_rx_callback);

#if CONTIKI_TARGET_IOTLAB
  print_banner();
  /* Real AT86RF231 radio, verified via radio-rf2xx.c's set_value() power
   * table: -17dBm is this chip's actual minimum TX power level, matching
   * A3's own real IoT-LAB deployment. Must be set before the radio starts
   * transmitting (root_start()/MAC.on() below). */
  NETSTACK_RADIO.set_value(RADIO_PARAM_TXPOWER, -17);
#endif

#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_IOTLAB
  if(node_id == 1) {
    NETSTACK_ROUTING.root_start();
  }
#endif
  NETSTACK_MAC.on();

#if CONTIKI_TARGET_COOJA || CONTIKI_TARGET_IOTLAB
  if(node_id == 1) {
    /* The root only receives (and terminates) application data in this
     * test; it must not also run the periodic sender below, or it would
     * just loop traffic back to itself and never actually exercise the
     * network. */
    PROCESS_WAIT_EVENT_UNTIL(0);
  }
#endif

  etimer_set(&et_send, SEND_INTERVAL);
  while(1) {
    PROCESS_YIELD_UNTIL(etimer_expired(&et_send));
    etimer_reset(&et_send);

    if(NETSTACK_ROUTING.node_is_reachable() && NETSTACK_ROUTING.get_root_ipaddr(&dst)) {
      pkt.origin = (uint16_t)node_id;
      pkt.seq = seq;
      LOG_INFO("APP TX seq=%u\n", seq);
      simple_udp_sendto(&udp_conn, &pkt, sizeof(pkt), &dst);
      seq++;
    } else {
      LOG_INFO("not reachable yet\n");
    }
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
/*---------------------------------------------------------------------------*/
#if CONTIKI_TARGET_IOTLAB
PROCESS_THREAD(banner_process, ev, data)
{
  static struct etimer et;

  PROCESS_BEGIN();

  etimer_set(&et, 30 * CLOCK_SECOND);
  while(1) {
    PROCESS_YIELD_UNTIL(etimer_expired(&et));
    etimer_reset(&et);
    print_banner();
  }

  PROCESS_END();
}
#endif
