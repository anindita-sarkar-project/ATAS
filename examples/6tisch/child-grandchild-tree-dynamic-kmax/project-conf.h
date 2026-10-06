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


#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Do not start TSCH at init, wait for NETSTACK_MAC.on() */
#define TSCH_CONF_AUTOSTART 0

#undef TSCH_CONF_DEFAULT_HOPPING_SEQUENCE
#define TSCH_CONF_DEFAULT_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4
#undef TSCH_CONF_JOIN_HOPPING_SEQUENCE
#define TSCH_CONF_JOIN_HOPPING_SEQUENCE TSCH_HOPPING_SEQUENCE_4_4


#define PARENT_SWITCH_THRESHOLD_CONF 192


#define TSCH_SCHEDULE_CONF_DEFAULT_LENGTH 3

#define TSCH_SCHEDULE_CONF_MAX_SLOTFRAMES 4

#define TSCH_SCHEDULE_CONF_MAX_LINKS 128

#define TSCH_CONF_WITH_IMPLICIT_ACK 1


#define TSCH_CONF_IA_SFS_SIZE 67


#define CG_CONF_IDLE_RECLAIM 1

#define CG_CONF_LOAD_BONUS 1

#if defined(CG_CONF_IDLE_RECLAIM) && CG_CONF_IDLE_RECLAIM
#define TSCH_CALLBACK_UNICAST_DATA_INPUT cg_data_input
#endif


#define CG_CONF_RANK_CAP 0


#define CG_CONF_DYNAMIC_KMAX 1

#define CG_CONF_HOLD_BEFORE_USE 1

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

#define RPL_CONF_MOP RPL_MOP_STORING_NO_MULTICAST


#define TSCH_CONF_MAC_MAX_FRAME_RETRIES 7

#define QUEUEBUF_CONF_NUM 128


#define ORCHESTRA_CONF_COMMON_SHARED_PERIOD 11


#define ENERGEST_CONF_ON 1

#define LOG_CONF_LEVEL_RPL     LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_TCPIP   LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_IPV6    LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_WARN
#define LOG_CONF_LEVEL_MAC     LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_FRAMER  LOG_LEVEL_WARN

#define TSCH_LOG_CONF_PER_SLOT 1

#if !CONTIKI_TARGET_COOJA && defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
#define LOG_CONF_OUTPUT(...) do { \
    uint32_t log_ipsr_; \
    __asm__ volatile ("mrs %0, ipsr" : "=r" (log_ipsr_)); \
    if(log_ipsr_ == 0) { \
      printf(__VA_ARGS__); \
    } \
  } while(0)
#endif


#ifndef IADS_TXPOWER_DBM
#define IADS_TXPOWER_DBM      -20
#endif
#define RF_CONF_TXPOWER_DBM   IADS_TXPOWER_DBM
#define TSCH_CONF_LOG_EB_RSSI 1

#ifndef IADS_RSSI_MIN
#define IADS_RSSI_MIN         -77
#endif
#define TSCH_CONF_RX_RSSI_HYST 1
#if IADS_CALIBRATE

#define TSCH_CONF_LOG_RX_RSSI 1
#define IADS_CONF_SEND_INTERVAL_S 2  /* node.c: dense samples for calibration */
#else
#define TSCH_CONF_RX_RSSI_MIN IADS_RSSI_MIN
#endif

#define NETSTACK_MAX_ROUTE_ENTRIES   40
#define NBR_TABLE_CONF_MAX_NEIGHBORS 32
#define TSCH_CONF_RX_RSSI_NBRS       40
#endif /* !CONTIKI_TARGET_COOJA */

#endif /* PROJECT_CONF_H_ */
