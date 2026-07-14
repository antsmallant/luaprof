# Porting the VM bridge to Lua 5.5

[中文](lua-5.5.md) | [Integration](../integration-en.md) |
[Lua 5.4](lua-5.4-en.md) | [Skynet](skynet-en.md)

This guide identifies function-level changes for PUC Lua 5.5. Lua 5.5.0 is the
validated version. PUC Lua 5.5 and Skynet's customized Lua both report
`LUA_VERSION_NUM == 505`, but they do not have the same VM ABI and cannot share
a module build.

## 1. Reference diff

```text
baseline: 1097dbe
bridge:   074659c
ABI:      LUA_PROFILE_ABI_VERSION == 2
```

Inspect or generate the complete patch:

```sh
git -C 3rd/lua-5.5.0 diff 1097dbe..074659c -- src
git -C 3rd/lua-5.5.0 diff --binary 1097dbe..074659c -- src \
    > /tmp/luaprof-lua55.patch
git -C /path/to/your-lua apply --check /tmp/luaprof-lua55.patch
```

Apply it directly only to the identical PUC Lua 5.5.0 baseline. For another
5.5.x release or a customized VM, port the functions below. Do not replace PUC
Lua with Skynet's complete `3rd/lua` tree.

## 2. Contract and implementation files

### `src/lua.h`

Add the ABI version 2 contract near `lua_Hook`:

- HOST/LUA/C/GC states and Lua/C frame constants;
- `lua_ProfileAllocEvent`, `lua_ProfileFrame`, and the three callback types;
- `lua_ProfileHooks`;
- `lua_setprofilehooks`, `lua_profile_request`, `lua_getprofilestate`, and
  `lua_profile_capturestack`.

### `src/lprofile.h` and `src/lprofile.c`

Copy both new files completely from `074659c`. They implement state guards,
atomic pending requests, safe points, allocation events, and profiler-safe
stack capture. Copying only the public declarations is not sufficient.

### `src/lstate.h`

Add these fields to `global_State`:

```text
profilehooks, ud_profile, profilethread, profilepending, profileincallback
```

Add these fields to `lua_State`:

```text
profilestate, profilecaptureblocked, profilecfunction
```

Keep Lua 5.5's existing `LX mainth` layout and seeded state ABI. Profiler state
belongs to the VM and must not be stored in `lua_Alloc.ud`.

## 3. Function-level changes

### `src/lstate.c`

| Function | Change |
| --- | --- |
| `preinit_thread` | Initialize HOST state, the capture guard, and CFunction for the main thread and every coroutine. |
| `lua_newstate` | Keep `(f, ud, seed)` and clear the global profiler fields. |
| `lua_closethread` | Unwind after reset; synchronize `from` only when `from != NULL && from != L`. |
| `close_state` | Emit the final free event before releasing the `global_State` main block. |

Lua 5.5's main block is `global_State *g` with size `sizeof(global_State)`. Do
not copy Lua 5.4's `fromstate(L)` and `sizeof(LG)` expressions.

### `src/lvm.c`

| Location | Change |
| --- | --- |
| `vmfetch()` | Call `luaP_safepoint(L)` at the instruction boundary. |
| `luaV_execute` entry | Create a state guard and publish LUA state. |
| `CIST_FRESH` return | Restore the previous frame state before returning. |

The signal handler only adds pending weight. Stack capture runs after
`vmfetch()` consumes it at a safe point.

### `src/ldo.c`

| Function/path | Change |
| --- | --- |
| `luaD_throw` | Call `luaP_unwind(L)` before throwing. |
| `luaD_reallocstack` | Block capture before `relstack`; unblock after every `correctstack(L, oldstack)` recovery path. |
| `precallC` | Publish C state and the CFunction around the call. |
| `finishCcall` | Apply the same guard around a `lua_KFunction` continuation. |
| C continuation in `resume` | Publish and restore C state around the continuation. |
| `lua_resume` | Unwind the current thread after writing `nresults`, then synchronize `from`. |
| `luaD_pcall` | Resynchronize state after protected-call recovery. |

Lua 5.5 uses `TStatus`/`APIstatus`; `precallC` and stack correction also differ
from Lua 5.4. Port the guard semantics without replacing Lua 5.5 function
bodies with 5.4 implementations.

### `src/lgc.c`

Publish GC state and restore the prior state around these entry points:

- `luaC_changemode`
- `luaC_freeallobjects`
- `luaC_step`
- `luaC_fullgc`

Keep Lua 5.5's generational collector state machine unchanged.

### `src/lmem.c`

| Function | Event semantics |
| --- | --- |
| `luaM_malloc_` | Success reports the complete requested size; final failure is reported before `luaM_error`. |
| `luaM_realloc_` | Success reports complete old/new pointers and sizes; failure leaves the old block live and reports the requested new size. |
| `luaM_free_` | After free, report the old pointer and size without reading released memory. |

Keep Lua 5.5's original `GCdebt` calculation. A successful realloc ends the old
block and creates one complete new allocation in profiler semantics, whether it
moves or remains in place.

### `src/ldebug.c` and `src/ldebug.h`

Expose internal `getfuncname` as `luaG_getfuncname`, keep the debug API using
it, and reuse it for best-effort Lua call names during stack capture.

### `src/Makefile`

- Add `lprofile.o` to `CORE_O` and add its dependency rule.
- Add `lprofile.h` dependencies to `ldo.o`, `lgc.o`, `lmem.o`, `lstate.o`, and
  `lvm.o`.

Official PUC Lua 5.5.0 has no Skynet `onelua.c`. A custom unity build must also
include `lprofile.c` in its amalgamation entry.

## 4. Differences from Lua 5.4

Check these points explicitly during a port:

- `lua_newstate` receives an explicit seed as its third argument;
- the main state is embedded in `global_State`, changing the final free event;
- `luaD_reallocstack` passes `oldstack` to `correctstack`;
- `lua_resume` and internal status types differ;
- GC mode and `GCdebt` updates differ;
- PUC Lua 5.5 and Skynet Lua 5.5 require separate module builds.

The bridge contract remains ABI version 2 across Lua 5.4 and 5.5 so the
profiler core can be reused. This does not make the Lua VM ABIs compatible.

## 5. Validation

Run the pinned fork's complete path:

```sh
make test-lua55
make example-lua55
```

`test-lua55` covers thread-per-VM smoke, Lua-visible CFunction names, module
lifecycle and ABI boundary, VM bridge behavior, and CPU, memory, and combined
sampling.

When compiling `tests/integration/vm_bridge_test.c` against a custom Lua 5.5,
define `LUAPROF_LUA_EXPLICIT_SEED`:

```sh
cc -DLUAPROF_LUA_EXPLICIT_SEED -I/path/to/lua/src \
    tests/integration/vm_bridge_test.c /path/to/lua/src/liblua.a \
    -lm -ldl -o vm-bridge-test
```

Build the module with `make module-lua55` as `build/lua55/luaprof.so` and
define `LUAPROF_EXPECT_LUA_VERSION=505`. `nm -D` must show
`lua_profile_capturestack` as `U`, resolved by the actual host Lua.
