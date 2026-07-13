ROOT := $(abspath .)
BUILD_DIR := $(ROOT)/build
LUA_DIR := $(ROOT)/3rd/lua-5.4.8
LUA_SRC := $(LUA_DIR)/src
LUA_LIB := $(LUA_SRC)/liblua.a
SKYNET_DIR := $(ROOT)/integration/skynet

CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LUA_PLATFORM ?= linux

.PHONY: all lua skynet submodule-lua submodule-skynet test test-thread-vm test-skynet thread-vm

all: thread-vm

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
		-I$(LUA_SRC) $< $(LUA_LIB) -lm -ldl -o $@

$(LUA_LIB): lua

thread-vm: $(BUILD_DIR)/thread-vm-smoke

test: test-thread-vm

test-thread-vm: thread-vm
	./tests/integration/thread_vm.sh

skynet: submodule-skynet $(LUA_LIB)
	$(MAKE) -C $(SKYNET_DIR) linux \
		LUA_INC=$(LUA_SRC) LUA_LIB=$(LUA_LIB) \
		MALLOC_STATICLIB= SKYNET_DEFINES=-DNOUSE_JEMALLOC

test-skynet: skynet
	./tests/integration/skynet.sh
