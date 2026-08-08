ROOT := $(abspath .)
BUILD_DIR := $(ROOT)/build
PPROF_FLAMEGRAPH_DIR := $(ROOT)/tools/pprof-flamegraph
PPROF_FLAMEGRAPH := $(BUILD_DIR)/pprof-flamegraph
LUA46_BUILD_DIR := $(BUILD_DIR)/lua46
LUA55_BUILD_DIR := $(BUILD_DIR)/lua55
SKYNET_BUILD_DIR := $(BUILD_DIR)/skynet
LUA_DIR := $(ROOT)/3rd/lua-5.4.8
LUA_SRC := $(LUA_DIR)/src
LUA_LIB := $(LUA_SRC)/liblua.a
LUA46_DIR := $(ROOT)/3rd/lua-5.4.6
LUA46_SRC := $(LUA46_DIR)/src
LUA46_LIB := $(LUA46_SRC)/liblua.a
LUA55_DIR := $(ROOT)/3rd/lua-5.5.0
LUA55_SRC := $(LUA55_DIR)/src
LUA55_LIB := $(LUA55_SRC)/liblua.a
SKYNET_DIR := $(ROOT)/integration/skynet
SKYNET_LUA_DIR := $(SKYNET_DIR)/3rd/lua
SKYNET_LUA_LIB := $(SKYNET_LUA_DIR)/liblua.a
INCLUDE_DIR := $(ROOT)/include
RUNTIME_SOURCE := src/runtime.c
CPU_CORE_SOURCE := src/cpu_core.c
MEMORY_CORE_SOURCE := src/memory_core.c
THREAD_TIMER_SOURCE := src/thread_timer.c
SKYNET_BACKEND_SOURCE := src/skynet_backend.c
SKYNET_HOST_SOURCE := src/skynet_host.c
LUA_MODULE_SOURCE := src/lua_module.c
LUA_BRIDGE_SOURCE := src/lua_bridge.c
LUA_SYMBOLS_SOURCE := src/lua_symbols.c
NATIVE_SYMBOL_SOURCE := src/native_symbol.c
PPROF_EXPORTER_SOURCE := src/pprof_exporter.c
RUNTIME_OBJECT := $(BUILD_DIR)/runtime.o
CPU_CORE_OBJECT := $(BUILD_DIR)/cpu_core.o
MEMORY_CORE_OBJECT := $(BUILD_DIR)/memory_core.o
THREAD_TIMER_OBJECT := $(BUILD_DIR)/thread_timer.o
THREAD_TIMER_TEST_OBJECT := $(BUILD_DIR)/thread_timer-test.o
SKYNET_BACKEND_OBJECT := $(BUILD_DIR)/skynet_backend.o
SKYNET_HOST_OBJECT := $(BUILD_DIR)/skynet_host.o
SKYNET_HOST_TEST_OBJECT := $(BUILD_DIR)/skynet_host-test.o
SKYNET_INTEGRATION_HOST_OBJECT := $(SKYNET_BUILD_DIR)/skynet_host.o
SKYNET_HOST_LIB := $(SKYNET_BUILD_DIR)/libluaprof-skynet-host.a
LUA_MODULE_OBJECT := $(BUILD_DIR)/lua_module.o
LUA_BRIDGE_OBJECT := $(BUILD_DIR)/lua_bridge.o
LUA_SYMBOLS_OBJECT := $(BUILD_DIR)/lua_symbols.o
NATIVE_SYMBOL_OBJECT := $(BUILD_DIR)/native_symbol.o
PPROF_EXPORTER_OBJECT := $(BUILD_DIR)/pprof_exporter.o
LUA_MODULE := $(BUILD_DIR)/luaprof.so
LUA46_THREAD_TIMER_OBJECT := $(LUA46_BUILD_DIR)/thread_timer.o
LUA46_LUA_MODULE_OBJECT := $(LUA46_BUILD_DIR)/lua_module.o
LUA46_LUA_BRIDGE_OBJECT := $(LUA46_BUILD_DIR)/lua_bridge.o
LUA46_LUA_SYMBOLS_OBJECT := $(LUA46_BUILD_DIR)/lua_symbols.o
LUA46_LUA_MODULE := $(LUA46_BUILD_DIR)/luaprof.so
LUA55_THREAD_TIMER_OBJECT := $(LUA55_BUILD_DIR)/thread_timer.o
LUA55_LUA_MODULE_OBJECT := $(LUA55_BUILD_DIR)/lua_module.o
LUA55_LUA_BRIDGE_OBJECT := $(LUA55_BUILD_DIR)/lua_bridge.o
LUA55_LUA_SYMBOLS_OBJECT := $(LUA55_BUILD_DIR)/lua_symbols.o
LUA55_LUA_MODULE := $(LUA55_BUILD_DIR)/luaprof.so
SKYNET_THREAD_TIMER_OBJECT := $(SKYNET_BUILD_DIR)/thread_timer.o
SKYNET_LUA_MODULE_OBJECT := $(SKYNET_BUILD_DIR)/lua_module.o
SKYNET_LUA_BRIDGE_OBJECT := $(SKYNET_BUILD_DIR)/lua_bridge.o
SKYNET_LUA_SYMBOLS_OBJECT := $(SKYNET_BUILD_DIR)/lua_symbols.o
SKYNET_LUA_MODULE := $(SKYNET_BUILD_DIR)/luaprof.so
SKYNET_VM_BRIDGE_TEST := $(SKYNET_BUILD_DIR)/vm-bridge-test
SKYNET_VM_SAFE_POINT_BENCH := $(SKYNET_BUILD_DIR)/vm-safe-point-bench
SKYNET_COMBINED_SAMPLING_BENCH := $(SKYNET_BUILD_DIR)/combined-sampling-bench
RUNTIME_TEST := $(BUILD_DIR)/runtime-test
DISABLED_BENCH := $(BUILD_DIR)/disabled-runtime-bench
MEMORY_TRACKING_BENCH := $(BUILD_DIR)/memory-tracking-bench
COMBINED_SAMPLING_BENCH := $(BUILD_DIR)/combined-sampling-bench
VM_SAFE_POINT_BENCH := $(BUILD_DIR)/vm-safe-point-bench
VM_BRIDGE_TEST := $(BUILD_DIR)/vm-bridge-test
CPU_SAMPLING_TEST := $(BUILD_DIR)/cpu-sampling-test
COMBINED_SAMPLING_TEST := $(BUILD_DIR)/combined-sampling-test
SCHEDULER_SAMPLING_TEST := $(BUILD_DIR)/scheduler-sampling-test
SKYNET_SIGNAL_MASK_TEST := $(BUILD_DIR)/skynet-signal-mask-test
CPU_CORE_TEST := $(BUILD_DIR)/cpu-core-test
MEMORY_CORE_TEST := $(BUILD_DIR)/memory-core-test
MEMORY_SAMPLING_TEST := $(BUILD_DIR)/memory-sampling-test
PPROF_EXPORTER_TEST := $(BUILD_DIR)/pprof-exporter-test
LUA_SYMBOLS_TEST := $(BUILD_DIR)/lua-symbols-test
LUA46_THREAD_VM_SMOKE := $(LUA46_BUILD_DIR)/thread-vm-smoke
LUA46_VM_BRIDGE_TEST := $(LUA46_BUILD_DIR)/vm-bridge-test
LUA46_CPU_SAMPLING_TEST := $(LUA46_BUILD_DIR)/cpu-sampling-test
LUA46_MEMORY_SAMPLING_TEST := $(LUA46_BUILD_DIR)/memory-sampling-test
LUA46_COMBINED_SAMPLING_TEST := $(LUA46_BUILD_DIR)/combined-sampling-test
LUA46_LUA_SYMBOLS_TEST := $(LUA46_BUILD_DIR)/lua-symbols-test
LUA46_VM_SAFE_POINT_BENCH := $(LUA46_BUILD_DIR)/vm-safe-point-bench
LUA46_COMBINED_SAMPLING_BENCH := $(LUA46_BUILD_DIR)/combined-sampling-bench
LUA55_THREAD_VM_SMOKE := $(LUA55_BUILD_DIR)/thread-vm-smoke
LUA55_VM_BRIDGE_TEST := $(LUA55_BUILD_DIR)/vm-bridge-test
LUA55_CPU_SAMPLING_TEST := $(LUA55_BUILD_DIR)/cpu-sampling-test
LUA55_MEMORY_SAMPLING_TEST := $(LUA55_BUILD_DIR)/memory-sampling-test
LUA55_COMBINED_SAMPLING_TEST := $(LUA55_BUILD_DIR)/combined-sampling-test
LUA55_LUA_SYMBOLS_TEST := $(LUA55_BUILD_DIR)/lua-symbols-test
LUA55_VM_SAFE_POINT_BENCH := $(LUA55_BUILD_DIR)/vm-safe-point-bench
LUA55_COMBINED_SAMPLING_BENCH := $(LUA55_BUILD_DIR)/combined-sampling-bench

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
LUA_PLATFORM ?= linux

override CPPFLAGS += -DLUA_USE_LUAPROF

.PHONY: all bench-combined bench-disabled bench-lua55-combined bench-lua55-vm bench-memory bench-skynet-combined bench-skynet-vm bench-vm example-lua55 example-skynet example-thread-vm lua lua55 module module-lua55 pprof-flamegraph skynet skynet-lua skynet-module submodule-lua submodule-lua55 submodule-skynet test test-all test-api test-combined-sampling test-cpu-core test-cpu-sampling test-feature-gates test-lua-symbols test-lua55 test-lua55-api test-lua55-boundary test-lua55-combined-sampling test-lua55-cpu-sampling test-lua55-memory-sampling test-lua55-thread-vm test-lua55-vm-bridge test-memory-core test-memory-sampling test-porting-patches test-pprof-exporter test-pprof-flamegraph test-runtime test-scheduler-sampling test-thread-vm test-vm-bridge test-skynet thread-vm
.PHONY: bench-lua46-combined bench-lua46-vm example-lua46 lua46 module-lua46 submodule-lua46 test-lua46 test-lua46-api test-lua46-combined-sampling test-lua46-cpu-sampling test-lua46-lua-symbols test-lua46-memory-sampling test-lua46-thread-vm test-lua46-vm-bridge update-porting-patches

all: thread-vm module

submodule-lua:
	git submodule update --init 3rd/lua-5.4.8

submodule-lua46:
	git submodule update --init 3rd/lua-5.4.6

submodule-lua55:
	git submodule update --init 3rd/lua-5.5.0

submodule-skynet:
	git submodule update --init integration/skynet

lua: submodule-lua
	$(MAKE) -C $(LUA_DIR) $(LUA_PLATFORM) LUAPROF=1

lua46: submodule-lua46
	$(MAKE) -C $(LUA46_DIR) $(LUA_PLATFORM) LUAPROF=1

lua55: submodule-lua55
	$(MAKE) -C $(LUA55_DIR) $(LUA_PLATFORM) LUAPROF=1

skynet-lua: submodule-skynet
	$(MAKE) -C $(SKYNET_LUA_DIR) $(LUA_PLATFORM) LUAPROF=1

$(SKYNET_LUA_LIB): skynet-lua

$(LUA46_LIB): lua46

$(LUA55_LIB): lua55

$(BUILD_DIR):
	mkdir -p $@

$(LUA46_BUILD_DIR):
	mkdir -p $@

$(LUA55_BUILD_DIR):
	mkdir -p $@

$(SKYNET_BUILD_DIR):
	mkdir -p $@

$(PPROF_FLAMEGRAPH): $(PPROF_FLAMEGRAPH_DIR)/go.mod $(PPROF_FLAMEGRAPH_DIR)/go.sum $(PPROF_FLAMEGRAPH_DIR)/main.go | $(BUILD_DIR)
	cd $(PPROF_FLAMEGRAPH_DIR) && go build -o $@ .

$(BUILD_DIR)/thread-vm-smoke: examples/thread_vm/main.c $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA_SRC) $(LDFLAGS) $< $(LUA_LIB) $(LDLIBS) -lm -ldl -o $@

$(LUA46_THREAD_VM_SMOKE): examples/thread_vm/main.c $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA46_SRC) $(LDFLAGS) $< $(LUA46_LIB) $(LDLIBS) -lm -ldl -o $@

$(LUA55_THREAD_VM_SMOKE): examples/thread_vm/main.c $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA55_SRC) $(LDFLAGS) $< $(LUA55_LIB) $(LDLIBS) -lm -ldl -o $@

$(RUNTIME_OBJECT): $(RUNTIME_SOURCE) include/luaprof/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -c $< -o $@

$(CPU_CORE_OBJECT): $(CPU_CORE_SOURCE) src/cpu_core.h include/luaprof/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -Isrc -c $< -o $@

$(MEMORY_CORE_OBJECT): $(MEMORY_CORE_SOURCE) src/memory_core.h include/luaprof/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -Isrc -c $< -o $@

$(THREAD_TIMER_OBJECT): $(THREAD_TIMER_SOURCE) src/thread_timer.h include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(THREAD_TIMER_TEST_OBJECT): $(THREAD_TIMER_SOURCE) src/thread_timer.h src/thread_timer_test.h include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-DLUAPROF_TESTING -I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(SKYNET_BACKEND_OBJECT): $(SKYNET_BACKEND_SOURCE) src/skynet_backend.h include/luaprof/skynet_host.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -Isrc -c $< -o $@

$(SKYNET_HOST_OBJECT): $(SKYNET_HOST_SOURCE) include/luaprof/skynet_host.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -c $< -o $@

$(SKYNET_HOST_TEST_OBJECT): $(SKYNET_HOST_SOURCE) src/skynet_host_test.h include/luaprof/skynet_host.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-DLUAPROF_TESTING -I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(LUA_MODULE_OBJECT): $(LUA_MODULE_SOURCE) src/lua_symbols.h src/pprof_exporter.h include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-DLUAPROF_EXPECT_LUA_VERSION=504 \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -c $< -o $@

$(LUA_BRIDGE_OBJECT): $(LUA_BRIDGE_SOURCE) src/lua_bridge.h src/skynet_backend.h src/thread_timer.h include/luaprof/runtime.h include/luaprof/skynet_host.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(LUA_SYMBOLS_OBJECT): $(LUA_SYMBOLS_SOURCE) src/lua_symbols.h include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(NATIVE_SYMBOL_OBJECT): $(NATIVE_SYMBOL_SOURCE) src/native_symbol.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-Isrc -c $< -o $@

$(PPROF_EXPORTER_OBJECT): $(PPROF_EXPORTER_SOURCE) src/pprof_exporter.h src/native_symbol.h include/luaprof/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -Isrc -c $< -o $@

$(LUA_MODULE): $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_SYMBOLS_OBJECT) $(NATIVE_SYMBOL_OBJECT) $(PPROF_EXPORTER_OBJECT) $(LUA_MODULE_OBJECT)
	$(CC) $(LDFLAGS) -shared $^ $(LDLIBS) -lm -lz -ldl -pthread -lrt -o $@

$(LUA46_THREAD_TIMER_OBJECT): $(THREAD_TIMER_SOURCE) src/thread_timer.h include/luaprof/runtime.h $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc -c $< -o $@

$(LUA46_LUA_MODULE_OBJECT): $(LUA_MODULE_SOURCE) src/lua_symbols.h src/pprof_exporter.h include/luaprof/runtime.h $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-DLUAPROF_EXPECT_LUA_VERSION=504 \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -c $< -o $@

$(LUA46_LUA_BRIDGE_OBJECT): $(LUA_BRIDGE_SOURCE) src/lua_bridge.h src/skynet_backend.h src/thread_timer.h include/luaprof/runtime.h include/luaprof/skynet_host.h $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc -c $< -o $@

$(LUA46_LUA_SYMBOLS_OBJECT): $(LUA_SYMBOLS_SOURCE) src/lua_symbols.h include/luaprof/runtime.h $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc -c $< -o $@

$(LUA46_LUA_MODULE): $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA46_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA46_LUA_BRIDGE_OBJECT) $(LUA46_LUA_SYMBOLS_OBJECT) $(NATIVE_SYMBOL_OBJECT) $(PPROF_EXPORTER_OBJECT) $(LUA46_LUA_MODULE_OBJECT) | $(LUA46_BUILD_DIR)
	$(CC) $(LDFLAGS) -shared $^ $(LDLIBS) -lm -lz -ldl -pthread -lrt -o $@

module-lua46: $(LUA46_LUA_MODULE)

$(LUA55_THREAD_TIMER_OBJECT): $(THREAD_TIMER_SOURCE) src/thread_timer.h include/luaprof/runtime.h $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc -c $< -o $@

$(LUA55_LUA_MODULE_OBJECT): $(LUA_MODULE_SOURCE) src/lua_symbols.h src/pprof_exporter.h include/luaprof/runtime.h $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-DLUAPROF_EXPECT_LUA_VERSION=505 \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -c $< -o $@

$(LUA55_LUA_BRIDGE_OBJECT): $(LUA_BRIDGE_SOURCE) src/lua_bridge.h src/skynet_backend.h src/thread_timer.h include/luaprof/runtime.h include/luaprof/skynet_host.h $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc -c $< -o $@

$(LUA55_LUA_SYMBOLS_OBJECT): $(LUA_SYMBOLS_SOURCE) src/lua_symbols.h include/luaprof/runtime.h $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc -c $< -o $@

$(LUA55_LUA_MODULE): $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA55_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA55_LUA_BRIDGE_OBJECT) $(LUA55_LUA_SYMBOLS_OBJECT) $(NATIVE_SYMBOL_OBJECT) $(PPROF_EXPORTER_OBJECT) $(LUA55_LUA_MODULE_OBJECT) | $(LUA55_BUILD_DIR)
	$(CC) $(LDFLAGS) -shared $^ $(LDLIBS) -lm -lz -ldl -pthread -lrt -o $@

module-lua55: $(LUA55_LUA_MODULE)

$(SKYNET_THREAD_TIMER_OBJECT): $(THREAD_TIMER_SOURCE) src/thread_timer.h include/luaprof/runtime.h $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(SKYNET_LUA_DIR) -Isrc -c $< -o $@

$(SKYNET_INTEGRATION_HOST_OBJECT): $(SKYNET_HOST_SOURCE) include/luaprof/skynet_host.h $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(SKYNET_LUA_DIR) -c $< -o $@

$(SKYNET_HOST_LIB): $(SKYNET_INTEGRATION_HOST_OBJECT) | $(SKYNET_BUILD_DIR)
	$(AR) rcs $@ $^

$(SKYNET_LUA_MODULE_OBJECT): $(LUA_MODULE_SOURCE) src/lua_symbols.h src/pprof_exporter.h include/luaprof/runtime.h $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-DLUAPROF_EXPECT_LUA_VERSION=505 \
		-I$(INCLUDE_DIR) -I$(SKYNET_LUA_DIR) -c $< -o $@

$(SKYNET_LUA_BRIDGE_OBJECT): $(LUA_BRIDGE_SOURCE) src/lua_bridge.h src/skynet_backend.h src/thread_timer.h include/luaprof/runtime.h include/luaprof/skynet_host.h $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(SKYNET_LUA_DIR) -Isrc -c $< -o $@

$(SKYNET_LUA_SYMBOLS_OBJECT): $(LUA_SYMBOLS_SOURCE) src/lua_symbols.h include/luaprof/runtime.h $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(SKYNET_LUA_DIR) -Isrc -c $< -o $@

$(SKYNET_LUA_MODULE): $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(SKYNET_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(SKYNET_LUA_BRIDGE_OBJECT) $(SKYNET_LUA_SYMBOLS_OBJECT) $(NATIVE_SYMBOL_OBJECT) $(PPROF_EXPORTER_OBJECT) $(SKYNET_LUA_MODULE_OBJECT) | $(SKYNET_BUILD_DIR)
	$(CC) $(LDFLAGS) -shared $^ $(LDLIBS) -lm -lz -ldl -pthread -lrt -o $@

skynet-module: $(SKYNET_LUA_MODULE)

$(RUNTIME_TEST): tests/unit/runtime_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

$(CPU_CORE_TEST): tests/unit/cpu_core_test.c $(CPU_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -Isrc $(LDFLAGS) $^ $(LDLIBS) -o $@

$(MEMORY_CORE_TEST): tests/unit/memory_core_test.c $(MEMORY_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -Isrc $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

$(PPROF_EXPORTER_TEST): tests/unit/pprof_exporter_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(NATIVE_SYMBOL_OBJECT) $(PPROF_EXPORTER_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -Isrc $(LDFLAGS) $^ $(LDLIBS) -lm -lz -ldl -o $@

$(LUA_SYMBOLS_TEST): tests/unit/lua_symbols_test.c $(LUA_SYMBOLS_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -o $@

$(LUA46_LUA_SYMBOLS_TEST): tests/unit/lua_symbols_test.c $(LUA46_LUA_SYMBOLS_OBJECT) $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -o $@

$(LUA55_LUA_SYMBOLS_TEST): tests/unit/lua_symbols_test.c $(LUA55_LUA_SYMBOLS_OBJECT) $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -o $@

$(DISABLED_BENCH): tests/bench/disabled_runtime.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

$(MEMORY_TRACKING_BENCH): tests/bench/memory_tracking.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

$(COMBINED_SAMPLING_BENCH): tests/bench/combined_sampling.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(VM_SAFE_POINT_BENCH): tests/bench/vm_safe_point.c $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA_SRC) $(LDFLAGS) $< $(LUA_LIB) $(LDLIBS) -lm -ldl -o $@

$(VM_BRIDGE_TEST): tests/integration/vm_bridge_test.c $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA_SRC) $(LDFLAGS) $< $(LUA_LIB) $(LDLIBS) -lm -ldl -o $@

$(LUA46_VM_BRIDGE_TEST): tests/integration/vm_bridge_test.c $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA46_SRC) $(LDFLAGS) $< $(LUA46_LIB) $(LDLIBS) \
		-lm -ldl -o $@

$(LUA55_VM_BRIDGE_TEST): tests/integration/vm_bridge_test.c $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-DLUAPROF_LUA_EXPLICIT_SEED \
		-I$(LUA55_SRC) $(LDFLAGS) $< $(LUA55_LIB) $(LDLIBS) \
		-lm -ldl -o $@

$(SKYNET_VM_BRIDGE_TEST): tests/integration/vm_bridge_test.c $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-DLUAPROF_LUA_EXPLICIT_SEED \
		-I$(SKYNET_LUA_DIR) $(LDFLAGS) $< $(SKYNET_LUA_LIB) $(LDLIBS) \
		-lm -ldl -o $@

$(SKYNET_VM_SAFE_POINT_BENCH): tests/bench/vm_safe_point.c $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(SKYNET_LUA_DIR) $(LDFLAGS) $< $(SKYNET_LUA_LIB) $(LDLIBS) \
		-lm -ldl -o $@

$(LUA46_VM_SAFE_POINT_BENCH): tests/bench/vm_safe_point.c $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA46_SRC) $(LDFLAGS) $< $(LUA46_LIB) $(LDLIBS) \
		-lm -ldl -o $@

$(LUA55_VM_SAFE_POINT_BENCH): tests/bench/vm_safe_point.c $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA55_SRC) $(LDFLAGS) $< $(LUA55_LIB) $(LDLIBS) \
		-lm -ldl -o $@

$(LUA55_COMBINED_SAMPLING_BENCH): tests/bench/combined_sampling.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA55_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA55_LUA_BRIDGE_OBJECT) $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc $(LDFLAGS) $^ \
		$(LDLIBS) -lm -ldl -pthread -lrt -o $@

$(LUA46_COMBINED_SAMPLING_BENCH): tests/bench/combined_sampling.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA46_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA46_LUA_BRIDGE_OBJECT) $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc $(LDFLAGS) $^ \
		$(LDLIBS) -lm -ldl -pthread -lrt -o $@

$(SKYNET_COMBINED_SAMPLING_BENCH): tests/bench/combined_sampling.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(SKYNET_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(SKYNET_LUA_BRIDGE_OBJECT) $(SKYNET_LUA_LIB) | $(SKYNET_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(SKYNET_LUA_DIR) -Isrc $(LDFLAGS) $^ \
		$(LDLIBS) -lm -ldl -pthread -lrt -o $@

$(CPU_SAMPLING_TEST): tests/integration/cpu_sampling_test.c src/thread_timer_test.h $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_TEST_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-DLUAPROF_TESTING -I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) \
		tests/integration/cpu_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) \
		$(MEMORY_CORE_OBJECT) $(THREAD_TIMER_TEST_OBJECT) $(SKYNET_BACKEND_OBJECT) \
		$(LUA_BRIDGE_OBJECT) $(LUA_LIB) $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(LUA46_CPU_SAMPLING_TEST): tests/integration/cpu_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA46_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA46_LUA_BRIDGE_OBJECT) $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(LUA55_CPU_SAMPLING_TEST): tests/integration/cpu_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA55_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA55_LUA_BRIDGE_OBJECT) $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(COMBINED_SAMPLING_TEST): tests/integration/combined_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(LUA46_COMBINED_SAMPLING_TEST): tests/integration/combined_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA46_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA46_LUA_BRIDGE_OBJECT) $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(LUA55_COMBINED_SAMPLING_TEST): tests/integration/combined_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA55_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA55_LUA_BRIDGE_OBJECT) $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(MEMORY_SAMPLING_TEST): tests/integration/memory_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(LUA46_MEMORY_SAMPLING_TEST): tests/integration/memory_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA46_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA46_LUA_BRIDGE_OBJECT) $(LUA46_LIB) | $(LUA46_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA46_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(LUA55_MEMORY_SAMPLING_TEST): tests/integration/memory_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(LUA55_THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA55_LUA_BRIDGE_OBJECT) $(LUA55_LIB) | $(LUA55_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-DLUAPROF_LUA_EXPLICIT_SEED \
		-I$(INCLUDE_DIR) -I$(LUA55_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(SCHEDULER_SAMPLING_TEST): tests/integration/scheduler_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(SKYNET_HOST_TEST_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) -Wl,-E $^ \
		$(LDLIBS) -lm -ldl -pthread -lrt -o $@

$(SKYNET_SIGNAL_MASK_TEST): tests/integration/skynet_signal_mask_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(SKYNET_HOST_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) -Wl,-E $^ \
		$(LDLIBS) -lm -ldl -pthread -lrt -o $@

$(LUA_LIB): lua

thread-vm: $(BUILD_DIR)/thread-vm-smoke

module: $(LUA_MODULE)

example-thread-vm: module tests/integration/thread_vm_example.sh
	./tests/integration/thread_vm_example.sh \
		"$(LUA_SRC)/lua" "$(BUILD_DIR)" "$(BUILD_DIR)"

example-lua46: module-lua46 tests/integration/thread_vm_example.sh
	./tests/integration/thread_vm_example.sh \
		"$(LUA46_SRC)/lua" "$(LUA46_BUILD_DIR)" "$(LUA46_BUILD_DIR)"

example-lua55: module-lua55 tests/integration/thread_vm_example.sh
	./tests/integration/thread_vm_example.sh \
		"$(LUA55_SRC)/lua" "$(LUA55_BUILD_DIR)" "$(LUA55_BUILD_DIR)"

test: test-thread-vm test-runtime test-cpu-core test-memory-core test-lua-symbols test-pprof-exporter test-api test-vm-bridge test-cpu-sampling test-memory-sampling test-combined-sampling test-scheduler-sampling

test-all: test test-lua46 test-lua55 test-skynet test-porting-patches test-feature-gates test-pprof-flamegraph

test-feature-gates: submodule-lua submodule-lua46 submodule-lua55 submodule-skynet
	./tests/integration/feature_gates.sh

test-porting-patches: submodule-lua submodule-lua46 submodule-lua55 submodule-skynet
	./tests/integration/porting_patches.sh

update-porting-patches: submodule-lua submodule-lua46 submodule-lua55 submodule-skynet
	./scripts/update-porting-patches.sh

test-lua46: test-lua46-thread-vm test-lua46-lua-symbols test-lua46-api test-lua46-vm-bridge test-lua46-cpu-sampling test-lua46-memory-sampling test-lua46-combined-sampling

test-lua55: test-lua55-thread-vm test-lua55-lua-symbols test-lua55-api test-lua55-boundary test-lua55-vm-bridge test-lua55-cpu-sampling test-lua55-memory-sampling test-lua55-combined-sampling

test-thread-vm: thread-vm
	./tests/integration/thread_vm.sh

test-lua46-thread-vm: $(LUA46_THREAD_VM_SMOKE)
	$(LUA46_THREAD_VM_SMOKE)

test-lua55-thread-vm: $(LUA55_THREAD_VM_SMOKE)
	$(LUA55_THREAD_VM_SMOKE)

test-runtime: $(RUNTIME_TEST)
	$(RUNTIME_TEST)

test-cpu-core: $(CPU_CORE_TEST)
	$(CPU_CORE_TEST)

test-memory-core: $(MEMORY_CORE_TEST)
	$(MEMORY_CORE_TEST)

test-lua-symbols: $(LUA_SYMBOLS_TEST)
	$(LUA_SYMBOLS_TEST)

test-lua46-lua-symbols: $(LUA46_LUA_SYMBOLS_TEST)
	$(LUA46_LUA_SYMBOLS_TEST)

test-lua55-lua-symbols: $(LUA55_LUA_SYMBOLS_TEST)
	$(LUA55_LUA_SYMBOLS_TEST)

test-pprof-exporter: $(PPROF_EXPORTER_TEST)
	$(PPROF_EXPORTER_TEST)

pprof-flamegraph: $(PPROF_FLAMEGRAPH)

test-pprof-flamegraph: $(PPROF_FLAMEGRAPH) example-thread-vm example-skynet
	cd $(PPROF_FLAMEGRAPH_DIR) && go test ./...
	./tests/integration/pprof_flamegraph.sh $(PPROF_FLAMEGRAPH)

test-api: module
	LUA_CPATH="$(BUILD_DIR)/?.so;;" $(LUA_SRC)/lua tests/lua/api_test.lua

test-lua46-api: module-lua46
	LUA_CPATH="$(LUA46_BUILD_DIR)/?.so;;" $(LUA46_SRC)/lua \
		tests/lua/api_test.lua

test-lua55-api: module-lua55
	LUA_CPATH="$(LUA55_BUILD_DIR)/?.so;;" $(LUA55_SRC)/lua \
		tests/lua/api_test.lua

test-lua55-boundary: module-lua55
	./tests/integration/lua55_boundary.sh

test-vm-bridge: $(VM_BRIDGE_TEST)
	$(VM_BRIDGE_TEST)

test-lua46-vm-bridge: $(LUA46_VM_BRIDGE_TEST)
	$(LUA46_VM_BRIDGE_TEST)

test-lua55-vm-bridge: $(LUA55_VM_BRIDGE_TEST)
	$(LUA55_VM_BRIDGE_TEST)

test-cpu-sampling: $(CPU_SAMPLING_TEST)
	$(CPU_SAMPLING_TEST)

test-lua46-cpu-sampling: $(LUA46_CPU_SAMPLING_TEST)
	$(LUA46_CPU_SAMPLING_TEST)

test-lua55-cpu-sampling: $(LUA55_CPU_SAMPLING_TEST)
	$(LUA55_CPU_SAMPLING_TEST)

test-combined-sampling: $(COMBINED_SAMPLING_TEST)
	$(COMBINED_SAMPLING_TEST)

test-lua46-combined-sampling: $(LUA46_COMBINED_SAMPLING_TEST)
	$(LUA46_COMBINED_SAMPLING_TEST)

test-lua55-combined-sampling: $(LUA55_COMBINED_SAMPLING_TEST)
	$(LUA55_COMBINED_SAMPLING_TEST)

test-memory-sampling: $(MEMORY_SAMPLING_TEST)
	$(MEMORY_SAMPLING_TEST)

test-lua46-memory-sampling: $(LUA46_MEMORY_SAMPLING_TEST)
	$(LUA46_MEMORY_SAMPLING_TEST)

test-lua55-memory-sampling: $(LUA55_MEMORY_SAMPLING_TEST)
	$(LUA55_MEMORY_SAMPLING_TEST)

test-scheduler-sampling: $(SCHEDULER_SAMPLING_TEST) $(SKYNET_SIGNAL_MASK_TEST)
	$(SCHEDULER_SAMPLING_TEST)
	$(SKYNET_SIGNAL_MASK_TEST)

bench-disabled: $(DISABLED_BENCH)
	$(DISABLED_BENCH)

bench-combined: $(COMBINED_SAMPLING_BENCH) tests/integration/combined_benchmark.sh
	./tests/integration/combined_benchmark.sh $(COMBINED_SAMPLING_BENCH)

bench-memory: $(MEMORY_TRACKING_BENCH)
	$(MEMORY_TRACKING_BENCH)

bench-vm: $(VM_SAFE_POINT_BENCH)
	$(VM_SAFE_POINT_BENCH)

bench-lua46-vm: $(LUA46_VM_SAFE_POINT_BENCH)
	$(LUA46_VM_SAFE_POINT_BENCH)

bench-lua46-combined: $(LUA46_COMBINED_SAMPLING_BENCH) tests/integration/combined_benchmark.sh
	./tests/integration/combined_benchmark.sh $(LUA46_COMBINED_SAMPLING_BENCH)

bench-lua55-vm: $(LUA55_VM_SAFE_POINT_BENCH)
	$(LUA55_VM_SAFE_POINT_BENCH)

bench-lua55-combined: $(LUA55_COMBINED_SAMPLING_BENCH) tests/integration/combined_benchmark.sh
	./tests/integration/combined_benchmark.sh $(LUA55_COMBINED_SAMPLING_BENCH)

bench-skynet-vm: $(SKYNET_VM_SAFE_POINT_BENCH)
	$(SKYNET_VM_SAFE_POINT_BENCH)

bench-skynet-combined: $(SKYNET_COMBINED_SAMPLING_BENCH) tests/integration/combined_benchmark.sh
	./tests/integration/combined_benchmark.sh $(SKYNET_COMBINED_SAMPLING_BENCH)

skynet: submodule-skynet skynet-module $(SKYNET_HOST_LIB)
	$(MAKE) -C $(SKYNET_DIR) linux \
		LUAPROF=1 LUAPROF_HOST_LIB=$(SKYNET_HOST_LIB) \
		LUAPROF_INC=$(INCLUDE_DIR) MALLOC_STATICLIB= \
		SKYNET_DEFINES="-DNOUSE_JEMALLOC"

test-skynet: skynet $(SKYNET_VM_BRIDGE_TEST)
	./tests/integration/skynet_lua_boundary.sh
	$(SKYNET_VM_BRIDGE_TEST)
	LUA_CPATH="$(SKYNET_BUILD_DIR)/?.so;;" $(SKYNET_LUA_DIR)/lua \
		tests/lua/api_test.lua
	./tests/integration/skynet.sh

example-skynet: skynet
	LUAPROF_SKYNET_SERVICE=luaprof_demo \
	LUAPROF_EXPECT_OUTPUT="luaprof skynet demo: ok" \
	LUAPROF_OUTPUT_DIR="$(BUILD_DIR)" ./tests/integration/skynet.sh
