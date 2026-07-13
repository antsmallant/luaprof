ROOT := $(abspath .)
BUILD_DIR := $(ROOT)/build
LUA_DIR := $(ROOT)/3rd/lua-5.4.8
LUA_SRC := $(LUA_DIR)/src
LUA_LIB := $(LUA_SRC)/liblua.a
SKYNET_DIR := $(ROOT)/integration/skynet
INCLUDE_DIR := $(ROOT)/include
RUNTIME_SOURCE := src/runtime.c
LUA_MODULE_SOURCE := src/lua_module.c
LUA_BRIDGE_SOURCE := src/lua_bridge.c
RUNTIME_OBJECT := $(BUILD_DIR)/runtime.o
LUA_MODULE_OBJECT := $(BUILD_DIR)/lua_module.o
LUA_BRIDGE_OBJECT := $(BUILD_DIR)/lua_bridge.o
LUA_MODULE := $(BUILD_DIR)/luaprof.so
RUNTIME_TEST := $(BUILD_DIR)/runtime-test
DISABLED_BENCH := $(BUILD_DIR)/disabled-runtime-bench
VM_SAFE_POINT_BENCH := $(BUILD_DIR)/vm-safe-point-bench
VM_BRIDGE_TEST := $(BUILD_DIR)/vm-bridge-test

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=
LUA_PLATFORM ?= linux

.PHONY: all bench-disabled bench-vm lua module skynet submodule-lua submodule-skynet test test-api test-runtime test-thread-vm test-vm-bridge test-skynet thread-vm

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

$(LUA_MODULE_OBJECT): $(LUA_MODULE_SOURCE) include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -c $< -o $@

$(LUA_BRIDGE_OBJECT): $(LUA_BRIDGE_SOURCE) src/lua_bridge.h include/luaprof/runtime.h $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror -fPIC \
		-I$(INCLUDE_DIR) -I$(LUA_SRC) -Isrc -c $< -o $@

$(LUA_MODULE): $(RUNTIME_OBJECT) $(LUA_BRIDGE_OBJECT) $(LUA_MODULE_OBJECT)
	$(CC) $(LDFLAGS) -shared $^ $(LDLIBS) -o $@

$(RUNTIME_TEST): tests/unit/runtime_test.c $(RUNTIME_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(DISABLED_BENCH): tests/bench/disabled_runtime.c $(RUNTIME_OBJECT) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(INCLUDE_DIR) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(VM_SAFE_POINT_BENCH): tests/bench/vm_safe_point.c $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA_SRC) $(LDFLAGS) $< $(LUA_LIB) $(LDLIBS) -lm -ldl -o $@

$(VM_BRIDGE_TEST): tests/integration/vm_bridge_test.c $(LUA_LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Werror \
		-I$(LUA_SRC) $(LDFLAGS) $< $(LUA_LIB) $(LDLIBS) -lm -ldl -o $@

$(LUA_LIB): lua

thread-vm: $(BUILD_DIR)/thread-vm-smoke

module: $(LUA_MODULE)

test: test-thread-vm test-runtime test-api test-vm-bridge

test-thread-vm: thread-vm
	./tests/integration/thread_vm.sh

test-runtime: $(RUNTIME_TEST)
	$(RUNTIME_TEST)

test-api: module
	LUA_CPATH="$(BUILD_DIR)/?.so;;" $(LUA_SRC)/lua tests/lua/api_test.lua

test-vm-bridge: $(VM_BRIDGE_TEST)
	$(VM_BRIDGE_TEST)

bench-disabled: $(DISABLED_BENCH)
	$(DISABLED_BENCH)

bench-vm: $(VM_SAFE_POINT_BENCH)
	$(VM_SAFE_POINT_BENCH)

skynet: submodule-skynet $(LUA_LIB)
	$(MAKE) -C $(SKYNET_DIR) linux \
		LUA_INC=$(LUA_SRC) LUA_LIB=$(LUA_LIB) \
		MALLOC_STATICLIB= SKYNET_DEFINES=-DNOUSE_JEMALLOC

test-skynet: skynet
	./tests/integration/skynet.sh
