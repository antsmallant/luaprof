ROOT := $(abspath .)
BUILD_DIR := $(ROOT)/build
LUA_DIR := $(ROOT)/3rd/lua-5.4.8
LUA_SRC := $(LUA_DIR)/src
LUA_LIB := $(LUA_SRC)/liblua.a
SKYNET_DIR := $(ROOT)/integration/skynet
INCLUDE_DIR := $(ROOT)/include
RUNTIME_SOURCE := src/runtime.c
CPU_CORE_SOURCE := src/cpu_core.c
MEMORY_CORE_SOURCE := src/memory_core.c
THREAD_TIMER_SOURCE := src/thread_timer.c
SKYNET_BACKEND_SOURCE := src/skynet_backend.c
SKYNET_HOST_SOURCE := src/skynet_host.c
LUA_MODULE_SOURCE := src/lua_module.c
LUA_BRIDGE_SOURCE := src/lua_bridge.c
PPROF_EXPORTER_SOURCE := src/pprof_exporter.c
RUNTIME_OBJECT := $(BUILD_DIR)/runtime.o
CPU_CORE_OBJECT := $(BUILD_DIR)/cpu_core.o
MEMORY_CORE_OBJECT := $(BUILD_DIR)/memory_core.o
THREAD_TIMER_OBJECT := $(BUILD_DIR)/thread_timer.o
SKYNET_BACKEND_OBJECT := $(BUILD_DIR)/skynet_backend.o
SKYNET_HOST_OBJECT := $(BUILD_DIR)/skynet_host.o
SKYNET_HOST_LIB := $(BUILD_DIR)/libluaprof-skynet-host.a
LUA_MODULE_OBJECT := $(BUILD_DIR)/lua_module.o
LUA_BRIDGE_OBJECT := $(BUILD_DIR)/lua_bridge.o
PPROF_EXPORTER_OBJECT := $(BUILD_DIR)/pprof_exporter.o
LUA_MODULE := $(BUILD_DIR)/luaprof.so
RUNTIME_TEST := $(BUILD_DIR)/runtime-test
DISABLED_BENCH := $(BUILD_DIR)/disabled-runtime-bench
MEMORY_TRACKING_BENCH := $(BUILD_DIR)/memory-tracking-bench
COMBINED_SAMPLING_BENCH := $(BUILD_DIR)/combined-sampling-bench
VM_SAFE_POINT_BENCH := $(BUILD_DIR)/vm-safe-point-bench
VM_BRIDGE_TEST := $(BUILD_DIR)/vm-bridge-test
CPU_SAMPLING_TEST := $(BUILD_DIR)/cpu-sampling-test
COMBINED_SAMPLING_TEST := $(BUILD_DIR)/combined-sampling-test
SCHEDULER_SAMPLING_TEST := $(BUILD_DIR)/scheduler-sampling-test
CPU_CORE_TEST := $(BUILD_DIR)/cpu-core-test
MEMORY_CORE_TEST := $(BUILD_DIR)/memory-core-test
MEMORY_SAMPLING_TEST := $(BUILD_DIR)/memory-sampling-test
PPROF_EXPORTER_TEST := $(BUILD_DIR)/pprof-exporter-test

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
LUA_PLATFORM ?= linux

.PHONY: all bench-combined bench-disabled bench-memory bench-vm example-skynet example-thread-vm lua module skynet submodule-lua submodule-skynet test test-api test-combined-sampling test-cpu-core test-cpu-sampling test-memory-core test-memory-sampling test-pprof-exporter test-runtime test-scheduler-sampling test-thread-vm test-vm-bridge test-skynet thread-vm

all: thread-vm module

submodule-lua:
	git submodule update --init 3rd/lua-5.4.8

submodule-skynet:
	git submodule update --init integration/skynet

lua: submodule-lua
	$(MAKE) -C $(LUA_DIR) $(LUA_PLATFORM)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/thread-vm-smoke: examples/thread_vm/main.c $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA_SRC) $(LDFLAGS) $< $(LUA_LIB) $(LDLIBS) -lm -ldl -o $@

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

$(SKYNET_BACKEND_OBJECT): $(SKYNET_BACKEND_SOURCE) src/skynet_backend.h include/luaprof/skynet_host.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -Isrc -c $< -o $@

$(SKYNET_HOST_OBJECT): $(SKYNET_HOST_SOURCE) include/luaprof/skynet_host.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -c $< -o $@

$(SKYNET_HOST_LIB): $(SKYNET_HOST_OBJECT)
	$(AR) rcs $@ $^

$(LUA_MODULE_OBJECT): $(LUA_MODULE_SOURCE) src/pprof_exporter.h include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -c $< -o $@

$(LUA_BRIDGE_OBJECT): $(LUA_BRIDGE_SOURCE) src/lua_bridge.h src/skynet_backend.h src/thread_timer.h include/luaprof/runtime.h include/luaprof/skynet_host.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(PPROF_EXPORTER_OBJECT): $(PPROF_EXPORTER_SOURCE) src/pprof_exporter.h include/luaprof/runtime.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -Isrc -c $< -o $@

$(LUA_MODULE): $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(PPROF_EXPORTER_OBJECT) $(LUA_MODULE_OBJECT)
	$(CC) $(LDFLAGS) -shared $^ $(LDLIBS) -lm -lz -ldl -pthread -lrt -o $@

$(RUNTIME_TEST): tests/unit/runtime_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

$(CPU_CORE_TEST): tests/unit/cpu_core_test.c $(CPU_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -Isrc $(LDFLAGS) $^ $(LDLIBS) -o $@

$(MEMORY_CORE_TEST): tests/unit/memory_core_test.c $(MEMORY_CORE_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -Isrc $(LDFLAGS) $^ $(LDLIBS) -lm -o $@

$(PPROF_EXPORTER_TEST): tests/unit/pprof_exporter_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(PPROF_EXPORTER_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -Isrc $(LDFLAGS) $^ $(LDLIBS) -lm -lz -o $@

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

$(CPU_SAMPLING_TEST): tests/integration/cpu_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(COMBINED_SAMPLING_TEST): tests/integration/combined_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(MEMORY_SAMPLING_TEST): tests/integration/memory_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) $^ $(LDLIBS) \
		-lm -ldl -pthread -lrt -o $@

$(SCHEDULER_SAMPLING_TEST): tests/integration/scheduler_sampling_test.c $(RUNTIME_OBJECT) $(CPU_CORE_OBJECT) $(MEMORY_CORE_OBJECT) $(THREAD_TIMER_OBJECT) $(SKYNET_BACKEND_OBJECT) $(LUA_BRIDGE_OBJECT) $(SKYNET_HOST_OBJECT) $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc $(LDFLAGS) -Wl,-E $^ \
		$(LDLIBS) -lm -ldl -pthread -lrt -o $@

$(LUA_LIB): lua

thread-vm: $(BUILD_DIR)/thread-vm-smoke

module: $(LUA_MODULE)

example-thread-vm: module
	LUA_CPATH="$(BUILD_DIR)/?.so;;" $(LUA_SRC)/lua \
		examples/thread_vm/profile.lua \
		$(BUILD_DIR)/thread-vm-cpu.pb.gz \
		$(BUILD_DIR)/thread-vm-heap.pb.gz

test: test-thread-vm test-runtime test-cpu-core test-memory-core test-pprof-exporter test-api test-vm-bridge test-cpu-sampling test-memory-sampling test-combined-sampling test-scheduler-sampling

test-thread-vm: thread-vm
	./tests/integration/thread_vm.sh

test-runtime: $(RUNTIME_TEST)
	$(RUNTIME_TEST)

test-cpu-core: $(CPU_CORE_TEST)
	$(CPU_CORE_TEST)

test-memory-core: $(MEMORY_CORE_TEST)
	$(MEMORY_CORE_TEST)

test-pprof-exporter: $(PPROF_EXPORTER_TEST)
	$(PPROF_EXPORTER_TEST)

test-api: module
	LUA_CPATH="$(BUILD_DIR)/?.so;;" $(LUA_SRC)/lua tests/lua/api_test.lua

test-vm-bridge: $(VM_BRIDGE_TEST)
	$(VM_BRIDGE_TEST)

test-cpu-sampling: $(CPU_SAMPLING_TEST)
	$(CPU_SAMPLING_TEST)

test-combined-sampling: $(COMBINED_SAMPLING_TEST)
	$(COMBINED_SAMPLING_TEST)

test-memory-sampling: $(MEMORY_SAMPLING_TEST)
	$(MEMORY_SAMPLING_TEST)

test-scheduler-sampling: $(SCHEDULER_SAMPLING_TEST)
	$(SCHEDULER_SAMPLING_TEST)

bench-disabled: $(DISABLED_BENCH)
	$(DISABLED_BENCH)

bench-combined: $(COMBINED_SAMPLING_BENCH)
	$(COMBINED_SAMPLING_BENCH)

bench-memory: $(MEMORY_TRACKING_BENCH)
	$(MEMORY_TRACKING_BENCH)

bench-vm: $(VM_SAFE_POINT_BENCH)
	$(VM_SAFE_POINT_BENCH)

skynet: submodule-skynet module $(SKYNET_HOST_LIB)
	$(MAKE) -C $(SKYNET_DIR) linux \
		LUA_INC=$(LUA_SRC) LUA_LIB=$(LUA_LIB) \
		LUAPROF_HOST_LIB=$(SKYNET_HOST_LIB) MALLOC_STATICLIB= \
		SKYNET_DEFINES="-DNOUSE_JEMALLOC -DSKYNET_LUAPROF -I$(INCLUDE_DIR)"

test-skynet: skynet
	./tests/integration/skynet.sh

example-skynet: test-skynet
