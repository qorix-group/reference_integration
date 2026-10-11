# Copyright (c) 2026 Qorix
# SPDX-License-Identifier: Apache-2.0
#
# Build description of the S-CORE DLT io-sock module (mods-score-dlt.so).
#
# Verified with SDP 8.0.0 (x86_64). The skeleton (QCONFIG, BUILD_TYPE=SO, PROJECT, INSTALLDIR, PINFO,
# "include devs/mods.mk") is taken from the QNX documentation sample "Writing an io-sock Module" and
# from the Qorix experiment's src/common.mk. The source and include search paths and the mixed C/C++
# source list are specific to this module. aarch64 has not been built yet.
#
# Build (QNX SDP environment sourced, e.g. `source ~/qnx800/qnxsdp-env.sh`):
#     make -C qnx_module                       # x86_64 (default)
#     make -C qnx_module CPU=aarch64           # aarch64
#     make -C qnx_module DST_ADDR=10.0.2.2     # unicast destination instead of the DLT multicast group
#     make -C qnx_module INPUT=ring            # shared-memory ring input instead of the synthetic stream
#     make -C qnx_module INPUT=ring RING_SLOTS=1024 RING_SLOT_PAYLOAD=1024
#     make -C qnx_module INPUT=ring BATCH_BYTES=1472   # several DLT messages per datagram
# Run `make -C qnx_module clean` when switching INPUT: the objects of the other variant are not rebuilt.
# `make qnx-module` and `make qnx-module-ring` in qnx_iosock_dlt/ do that and name the results.

CPU ?= x86_64
override BUILD_TYPE=SO
override PROJECT=score-dlt
override PROJECT_ROOT=$(PWD)

ifndef QCONFIG
QCONFIG=qconfig.mk
endif

include $(QCONFIG)

INSTALLDIR=/lib/dll

define PINFO
PINFO DESCRIPTION=S-CORE DLT io-sock module: synthetic DLT stream over the in-stack UDP path
endef

EXTRA_CLEAN+= $(PROJECT_ROOT)/mods-$(PROJECT).use

# Destination of the stream. Defaults match the DataRouter (multicast group, port 3490).
DST_ADDR ?= 239.255.42.99
DST_PORT ?= 3490
SRC_PORT ?= 0
RECORD_COUNT ?= 10
PERIOD_MS ?= 100

# Input: synthetic (finite generated stream) or ring (records from a shared-memory ring).
INPUT ?= synthetic
RING_SLOTS ?= 256
RING_SLOT_PAYLOAD ?= 512
POLL_MS ?= 1
# 0: one DLT message per datagram. 1472 packs messages up to the UDP payload of an MTU 1500 link (as the DataRouter does).
BATCH_BYTES ?= 0

DST_OCTETS := $(subst ., ,$(DST_ADDR))
CCFLAGS += -DDLT_DST_A=$(word 1,$(DST_OCTETS)) -DDLT_DST_B=$(word 2,$(DST_OCTETS)) \
           -DDLT_DST_C=$(word 3,$(DST_OCTETS)) -DDLT_DST_D=$(word 4,$(DST_OCTETS)) \
           -DDLT_DST_PORT=$(DST_PORT) -DDLT_SRC_PORT=$(SRC_PORT) \
           -DDLT_RECORD_COUNT=$(RECORD_COUNT) -DDLT_PERIOD_MS=$(PERIOD_MS)

# Plain-C module glue plus the freestanding C++ core (no STL, no exceptions, no RTTI, no heap).
EXTRA_SRCVPATH += $(PROJECT_ROOT)/../src/qnx $(PROJECT_ROOT)/../src/core
EXTRA_INCVPATH += $(PROJECT_ROOT)/../include $(PROJECT_ROOT)/../src/qnx
SRCS := iosock_module.c qnx_native_udp_transport.c qnx_module_stats.c \
        dlt_wire_formatter.cpp synthetic_record_source.cpp dlt_c_api.cpp

ifeq ($(INPUT),ring)
CCFLAGS += -DDLT_INPUT_RING -DDLT_RING_SLOTS=$(RING_SLOTS) -DDLT_RING_SLOT_PAYLOAD=$(RING_SLOT_PAYLOAD) \
           -DDLT_POLL_MS=$(POLL_MS) -DDLT_BATCH_BYTES=$(BATCH_BYTES)
SRCS += qnx_shm_map.c
else ifneq ($(INPUT),synthetic)
$(error INPUT must be synthetic or ring, got '$(INPUT)')
endif

# -fno-exceptions is valid for C and C++. -fno-rtti/-fno-threadsafe-statics only apply to C++ (gcc
# warns when it sees them for C files). The C++ compile only receives CXXFLAGS, so -fno-exceptions is
# repeated there; without it the object references __gxx_personality_v0, which io-sock does not export.
CCFLAGS += -fno-exceptions
CXXFLAGS += -fno-exceptions -fno-rtti -fno-threadsafe-statics

# The q++ link driver adds libc++ and friends; none is used, so do not record them as NEEDED.
LDFLAGS += -Wl,--as-needed

define MODULE_SPECIFIC_OPTIONS

Emits a short synthetic DLT stream (verbose log messages, ECU IOS1, APID DLTM, CTID SYN) from inside
io-sock. Load: io-sock -m score-dlt  or  mount -T io-sock <path>/mods-score-dlt.so
Health and counters: sysctl qnx.module.score_dlt

endef

include devs/mods.mk
