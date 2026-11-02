CONTIKI_PROJECT = node
all: $(CONTIKI_PROJECT)

CONTIKI=../../..

MAKE_MAC = MAKE_MAC_TSCH
MAKE_ROUTING = MAKE_ROUTING_RPL_CLASSIC

include $(CONTIKI)/Makefile.dir-variables
include $(CONTIKI)/Makefile.identify-target

MODULES += $(CONTIKI_NG_SERVICES_DIR)/orchestra

# Deployment service: maps each node's hardware MAC to a logical id (1=root)
# for the FIT IoT-LAB m3 build -- see deployment-map.c. Gated to TARGET=iotlab
# only: BUILD_WITH_DEPLOYMENT=1 (unconditionally set by this module's own
# module-macros.h the moment it's included in MODULES at all) redirects
# node_id_init() to deployment_init()/deployment_id_from_lladdr() against
# deployment-map.c's real-hardware MAC table (see os/sys/node-id.h and
# os/services/deployment/deployment.c) -- under Cooja, no mote's simulated
# MAC (derived from simMoteID, arch/platform/cooja/platform.c's set_lladdr())
# ever matches a real M3 board's MAC, so every single node's node_id silently
# stayed 0 ("UNMAPPED") the whole run: node 1 never called root_start(), no
# RPL DAG/TSCH PAN ever formed, and every mote just scanned channels forever
# (confirmed live: a smoke-test Cooja run showed "Node ID: 0" logged by every
# mote and zero RPL/application traffic for the full run). Only link this
# module for the real IoT-LAB build, where the mapping is actually valid.
ifeq ($(TARGET),iotlab)
MODULES += $(CONTIKI_NG_SERVICES_DIR)/deployment
PROJECT_SOURCEFILES += deployment-map.c
endif

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

# Sweep hooks: pass e.g. `make ... APP_RATE=10 SFS_SIZE=43` from the
# experiment driver (Cooja .csc <commands>, or by hand) to override the
# per-node traffic rate (Table II traffic-rate sweep) and/or the unicast
# slotframe length (Table II slotframe-length sweep) without editing
# project-conf.h/node.c per run. Deliberately single-token VAR=VALUE forms
# (no spaces/quotes) rather than one combined EXTRA_CFLAGS string, since a
# Cooja .csc <commands> line is not guaranteed to preserve shell quoting
# around an embedded multi-flag string.
ifdef APP_RATE
CFLAGS += -DAPP_SEND_RATE_CONF=$(APP_RATE)
endif
ifdef SFS_SIZE
CFLAGS += -DTSCH_CONF_IA_SFS_SIZE=$(SFS_SIZE)
endif
# Still accepted for manual command-line use (not used by the .csc files).
CFLAGS += $(EXTRA_CFLAGS)

include $(CONTIKI)/Makefile.include
