CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

include $(CONTIKI)/Makefile.dir-variables
include $(CONTIKI)/Makefile.identify-target

MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra

# eb_fixed_shared (not the stock eb_per_time_source): every node's EB Tx/Rx
# uses one fixed, network-wide-common slot instead of a slot re-derived as
# hash(current time source address) -- see orchestra-rule-eb-fixed.c's file
# header for the disassociation cascade this replaces (a parent switch used
# to relocate the EB-listening slot, and repeated switches could relocate it
# faster than a full re-lock ever completed, running out the desync timeout).
#
# Tried switching to eb_per_time_source (with a shortened EB period, see
# project-conf.h) to test whether removing shared-slot contention would let a
# shorter EB period close the shard-count-signaling-lag gap in child_
# grandchild_tree without the collateral damage a shared slot caused. Turned
# out to be moot either way: a shortened EB period, under BOTH eb_fixed_
# shared and eb_per_time_source, caused catastrophic per-node packet-pool
# exhaustion (own EB generation outrunning its own transmission opportunity)
# that left only root-adjacent nodes' traffic flowing at all -- a misleading
# per-node breakdown made an early n=8 result look like a 100% PDR win, when
# actually most nodes had gone silent/disassociated and simply dropped out of
# the ratio. Reverted to eb_fixed_shared with the library default EB period.
CFLAGS += -DORCHESTRA_CONF_RULES="{&eb_fixed_shared,&child_grandchild_tree,&default_common}"

# Real hardware (CC26x2 LaunchPads):
# `make ROOT=1` builds the RPL/TSCH root image;
# `make CALIBRATE=1` disables the RSSI range threshold and logs RSSI;
# `make RSSI_MIN=-70` overrides the emulated-range RSSI threshold (dBm).
ifeq ($(ROOT),1)
CFLAGS += -DIADS_CONF_IS_ROOT=1
endif
ifeq ($(CALIBRATE),1)
CFLAGS += -DIADS_CALIBRATE=1
endif
ifdef RSSI_MIN
CFLAGS += -DIADS_RSSI_MIN=$(RSSI_MIN)
endif
# `make TXPOWER=-10` overrides the radio TX power (dBm; -20 .. +5).
ifdef TXPOWER
CFLAGS += -DIADS_TXPOWER_DBM=$(TXPOWER)
endif

include $(CONTIKI)/Makefile.include
