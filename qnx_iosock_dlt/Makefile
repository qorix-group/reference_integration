# Copyright (c) 2026 Qorix
# SPDX-License-Identifier: Apache-2.0
#
# Host (Linux) build of the portable core, its unit tests and the loopback check.
# The QNX io-sock module is built with the QNX make framework from qnx_module/ (see README.md).
#
#   make test               build and run the unit tests (formatter and shared-memory ring)
#   make check-freestanding verify the core has no undefined symbols and no static constructors
#   make host-e2e           formatter -> UDP loopback -> independent Python decoder
#   make host-ring-e2e      ring producer -> shared memory -> core -> UDP -> Python decoder
#   make host-forwarder-e2e the same with the sendmsg forwarder instead of the module stand-in
#   make qnx-module         build mods-score-dlt.so, synthetic input (needs a sourced QNX SDP environment)
#   make qnx-module-ring    build mods-score-dlt-ring.so, shared-memory ring input
#   make qnx-tools          build the ring producer and the ring forwarder for QNX (CPU=x86_64 or aarch64)
#   make out/ring_forwarder build the ring forwarder for the host (sendmsg twin of the module)
#   make clean

CXX ?= g++
CC ?= gcc
BUILD := out
CPU ?= x86_64

WARNINGS := -Wall -Wextra -Wpedantic -Werror
# Core flags: no exceptions/RTTI, no hosted-library assumptions, and no loop-to-memcpy rewriting,
# so the objects reference no external symbol at all.
CORE_FLAGS := -std=c++17 -O2 $(WARNINGS) -Wshadow -Wconversion -fno-exceptions -fno-rtti \
              -fno-threadsafe-statics -ffreestanding -fno-tree-loop-distribute-patterns -Iinclude
HOST_FLAGS := -std=c++17 -O2 $(WARNINGS) -Iinclude

CORE_SRCS := src/core/dlt_wire_formatter.cpp src/core/synthetic_record_source.cpp src/core/dlt_c_api.cpp
CORE_OBJS := $(patsubst src/core/%.cpp,$(BUILD)/core/%.o,$(CORE_SRCS))
HEADERS := $(wildcard include/dlt/*.h)

E2E_PORT ?= 39490
RING_E2E_PORT ?= 39491
RING_E2E_BATCH_PORT ?= 39492
FORWARDER_E2E_PORT ?= 39493
FORWARDER_E2E_BATCH_PORT ?= 39494

# QNX compiler driver variant for the target tools.
QNX_VARIANT_x86_64 := gcc_ntox86_64
QNX_VARIANT_aarch64 := gcc_ntoaarch64le

.PHONY: all test check-freestanding host-e2e host-ring-e2e host-forwarder-e2e qnx-module qnx-module-ring qnx-tools clean

all: test

$(BUILD)/core/%.o: src/core/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CORE_FLAGS) -c $< -o $@

$(BUILD)/dlt_wire_formatter_test: tests/dlt_wire_formatter_test.cpp $(CORE_OBJS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_FLAGS) $< $(CORE_OBJS) -o $@

$(BUILD)/host_udp_sender: tests/host_udp_sender.cpp tests/host_udp_transport.h $(CORE_OBJS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_FLAGS) $< $(CORE_OBJS) -o $@

$(BUILD)/shm_ring_test: tests/shm_ring_test.cpp $(CORE_OBJS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_FLAGS) -pthread $< $(CORE_OBJS) -o $@

$(BUILD)/host_ring_bridge: tests/host_ring_bridge.cpp tests/host_udp_transport.h $(CORE_OBJS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_FLAGS) $< $(CORE_OBJS) -o $@

$(BUILD)/ring_producer: tools/ring_producer.c include/dlt/shm_ring.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -Wall -Wextra -Werror -Iinclude $< -o $@

$(BUILD)/ring_forwarder: tools/ring_forwarder.cpp tools/posix_udp_transport.h $(CORE_OBJS) $(HEADERS)
	@mkdir -p $(BUILD)
	$(CXX) $(HOST_FLAGS) $< $(CORE_OBJS) -o $@

test: $(BUILD)/dlt_wire_formatter_test $(BUILD)/shm_ring_test
	./$(BUILD)/dlt_wire_formatter_test
	./$(BUILD)/shm_ring_test

$(BUILD)/core_combined.o: $(CORE_OBJS)
	ld -r $(CORE_OBJS) -o $@

check-freestanding: $(BUILD)/core_combined.o
	@fail=0; \
	undef=$$(nm -u $<); \
	if [ -n "$$undef" ]; then echo "undefined symbols in the combined core object:"; echo "$$undef"; fail=1; fi; \
	if readelf -S $< | grep -q -E '\.(init_array|ctors|fini_array|dtors)'; then \
	  echo "static constructor/destructor section in the combined core object"; fail=1; fi; \
	if [ $$fail -ne 0 ]; then exit 1; fi; \
	echo "core (all objects linked together): no undefined symbols, no static constructors"

host-e2e: $(BUILD)/host_udp_sender
	python3 tools/dlt_udp_check.py --bind 127.0.0.1 --port $(E2E_PORT) --count 5 --timeout 10 \
	  --exec "$(BUILD)/host_udp_sender 127.0.0.1 $(E2E_PORT) 5"

host-ring-e2e: $(BUILD)/host_ring_bridge $(BUILD)/ring_producer
	python3 tools/dlt_udp_check.py --bind 127.0.0.1 --port $(RING_E2E_PORT) --count 20 --timeout 20 \
	  --expect-text ring-dlt \
	  --exec "sh tests/host_ring_e2e.sh $(BUILD)/host_ring_bridge $(BUILD)/ring_producer $(RING_E2E_PORT) 20"
	python3 tools/dlt_udp_check.py --bind 127.0.0.1 --port $(RING_E2E_BATCH_PORT) --count 20 --timeout 20 \
	  --expect-text ring-dlt \
	  --exec "sh tests/host_ring_e2e.sh $(BUILD)/host_ring_bridge $(BUILD)/ring_producer $(RING_E2E_BATCH_PORT) 20 1472 0"

host-forwarder-e2e: $(BUILD)/ring_forwarder $(BUILD)/ring_producer
	python3 tools/dlt_udp_check.py --bind 127.0.0.1 --port $(FORWARDER_E2E_PORT) --count 20 --timeout 20 \
	  --expect-text ring-dlt \
	  --exec "sh tests/host_ring_forwarder_e2e.sh $(BUILD)/ring_forwarder $(BUILD)/ring_producer $(FORWARDER_E2E_PORT) 20"
	python3 tools/dlt_udp_check.py --bind 127.0.0.1 --port $(FORWARDER_E2E_BATCH_PORT) --count 20 --timeout 20 \
	  --expect-text ring-dlt \
	  --exec "sh tests/host_ring_forwarder_e2e.sh $(BUILD)/ring_forwarder $(BUILD)/ring_producer $(FORWARDER_E2E_BATCH_PORT) 20 1472 0"

qnx-module:
	@if [ -z "$$QNX_TARGET" ]; then \
	  echo "QNX_TARGET is not set: source the QNX SDP environment first (qnxsdp-env.sh)."; exit 2; fi
	$(MAKE) -C qnx_module clean
	$(MAKE) -C qnx_module
	cp "$$(ls -t qnx_module/libmods-score-dlt-*.so | head -1)" qnx_module/mods-score-dlt.so

# Same module with the shared-memory ring as input instead of the synthetic stream.
qnx-module-ring:
	@if [ -z "$$QNX_TARGET" ]; then \
	  echo "QNX_TARGET is not set: source the QNX SDP environment first (qnxsdp-env.sh)."; exit 2; fi
	$(MAKE) -C qnx_module clean
	$(MAKE) -C qnx_module INPUT=ring
	cp "$$(ls -t qnx_module/libmods-score-dlt-*.so | head -1)" qnx_module/mods-score-dlt-ring.so
	$(MAKE) -C qnx_module clean

qnx-tools:
	@if [ -z "$$QNX_TARGET" ]; then \
	  echo "QNX_TARGET is not set: source the QNX SDP environment first (qnxsdp-env.sh)."; exit 2; fi
	@mkdir -p $(BUILD)/qnx_$(CPU)
	qcc -V$(QNX_VARIANT_$(CPU)) -O2 -Wall -Wextra -Werror -Iinclude tools/ring_producer.c \
	  -o $(BUILD)/qnx_$(CPU)/ring_producer
	q++ -V$(QNX_VARIANT_$(CPU)) -std=gnu++17 -O2 -Wall -Wextra -Werror -Iinclude \
	  tools/ring_forwarder.cpp src/core/dlt_wire_formatter.cpp -lsocket -o $(BUILD)/qnx_$(CPU)/ring_forwarder

clean:
	rm -rf $(BUILD)
