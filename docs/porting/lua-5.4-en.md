# Porting the VM bridge to Lua 5.4

[中文](lua-5.4.md) | [Integration](../integration-en.md) |
[Lua 5.5](lua-5.5-en.md) | [Skynet](skynet-en.md)

This guide identifies function-level changes for PUC Lua 5.4. Lua 5.4.8 is the
validated version. Treat it as a porting reference for another 5.4.x release
and rerun the complete contract tests.

## 1. Reference diff

Exact baseline and validated commit:

```text
baseline: 46f8c3d
bridge:   02c8f57
ABI:      LUA_PROFILE_ABI_VERSION == 2
```

Inspect or generate the complete patch:

```sh
git -C 3rd/lua-5.4.8 diff 46f8c3d..02c8f57 -- src
git -C 3rd/lua-5.4.8 diff --binary 46f8c3d..02c8f57 -- src \
    > /tmp/luaprof-lua54.patch
git -C /path/to/your-lua apply --check /tmp/luaprof-lua54.patch
```

Apply it directly only to the identical base. For another 5.4.x release or a
customized fork, port each function below; copying only the new files is not
sufficient.

## 2. Public bridge contract

### `src/lua.h`

Near the `lua_Hook` type, add:

- `LUA_PROFILE_ABI_VERSION`;
- HOST/LUA/C/GC state constants;
- Lua/C frame constants;
- `lua_ProfileAllocEvent` and `lua_ProfileFrame`;
- safe-point, state-change, and allocation callback types;
- `lua_ProfileHooks`.

In the state API declarations, add:

```c
lua_setprofilehooks
lua_profile_request
lua_getprofilestate
lua_profile_capturestack
```

Callbacks must not call Lua, allocate through Lua, or raise errors. Safe-point
and allocation callbacks may only use `lua_profile_capturestack`.

## 3. State storage and bridge implementation

### `src/lstate.h`

Add these fields to `global_State`:

```text
profilehooks, ud_profile, profilethread, profilepending, profileincallback
```

Add these fields to `lua_State`:

```text
profilestate, profilecaptureblocked, profilecfunction
```

They belong to the VM, not the public allocator `ud`. Do not change the
`lua_Alloc` ABI.

### `src/lprofile.h` and `src/lprofile.c`

Port these files completely from the reference commit. They implement:

- conditional state guards;
- signal-safe pending-weight add/exchange;
- safe-point callbacks;
- allocation event packaging;
- Lua/C frame capture, source, definition/current lines, and call names;
- state synchronization after errors and yields.

For compilers other than GCC/Clang, the reference atomic fallback is suitable
only where requests are not asynchronous. The supported Linux path requires
the corresponding atomics to be lock-free.

## 4. Function-level changes

### `src/lstate.c`

| Function | Change |
| --- | --- |
| `preinit_thread` | Initialize HOST state, capture guard, and current CFunction for every coroutine. |
| `lua_newstate` | Clear hooks, hook userdata, pending state, and callback guard in `global_State`. |
| `lua_closethread` | Publish HOST after reset and synchronize `from` when present. |
| `close_state` | Emit the final main-block free before the `lua_State` becomes invalid. |

Lua 5.4.8 stores the main block at `fromstate(L)` with size `sizeof(LG)`. Do not
copy Lua 5.5's `global_State` main-block expression.

### `src/lvm.c`

| Location | Change |
| --- | --- |
| `vmfetch()` | Call `luaP_safepoint(L)` at the instruction boundary; its fast path first checks hook and pending state. |
| `luaV_execute` entry | Create `luaP_StateGuard` and publish LUA state. |
| `CIST_FRESH` return in `luaV_execute` | Restore the previous frame state before returning. |

Never walk the stack from the timer signal handler. It calls only
`lua_profile_request`; capture happens at the `vmfetch()` safe point.

### `src/ldo.c`

| Function/path | Change |
| --- | --- |
| `luaD_throw` | Call `luaP_unwind(L)` before longjmp. |
| `luaD_reallocstack` | Set `profilecaptureblocked` before `relstack`; clear it after `correctstack` on every success/failure recovery path. |
| `precallC` | Publish C state and function pointer around the CFunction call, then restore the previous frame. |
| `finishCcall` | Guard the continuation with the same C state. |
| C continuation in `resume` | Publish and restore C state around the continuation. |
| `lua_resume` | Unwind the resumed thread before returning to the host, then synchronize `from`. |
| `luaD_pcall` | Resynchronize from the current `CallInfo` after protected-call recovery. |

Lua 5.4 temporarily turns frame pointers into offsets while relocating the VM
stack. The allocation callback can run in that window, so the capture guard is
mandatory. The allocation is still counted with an empty stack and truncation.

### `src/lgc.c`

Wrap these public GC entry points in a GC `luaP_StateGuard`, restoring state on
normal return:

- `luaC_changemode`
- `luaC_freeallobjects`
- `luaC_step`
- `luaC_fullgc`

Do not repeat transitions in every internal mark/sweep helper.

### `src/lmem.c`

| Function | Event semantics |
| --- | --- |
| `luaM_malloc_` | On success emit `(NULL, new, 0, size, success=1)`; after both attempts fail, emit failure before raising. |
| `luaM_realloc_` | Failure reports the old pointer and requested new size while the old block stays live; success reports complete old/new pointers and sizes. |
| `luaM_free_` | After allocator free, emit `(old, NULL, old_size, 0, success=1)` without dereferencing the pointer. |

Place events in Lua's internal wrappers, not public `lua_Alloc`, so they have
both `lua_State *` and exact requested sizes. Lua 5.4 and 5.5 update `GCdebt` in
different directions. Keep the target version's debt code and add events after
it.

### `src/ldebug.c` and `src/ldebug.h`

Expose internal `getfuncname` as `luaG_getfuncname`, keep the debug API using
it, and reuse it from stack capture. Do not scan `_G` in the sampling path;
Lua-visible CFunction binding discovery happens during result export.

### `src/Makefile`

- Add `lprofile.o` to `CORE_O`.
- Add its dependency rule.
- Add `lprofile.h` dependencies for `ldo.o`, `lgc.o`, `lmem.o`, `lstate.o`, and
  `lvm.o`.

An amalgamated build must also include `lprofile.c` in its unity source.

## 5. Validation

Build the pinned fork and module:

```sh
make lua
make module
make test-vm-bridge
make test-api
make test-cpu-sampling
make test-memory-sampling
make test-combined-sampling
```

At minimum, compile `tests/integration/vm_bridge_test.c` with the customized
Lua headers and `liblua.a`. It covers pending safe points, Lua/C/GC/HOST,
coroutines, yield/error, allocation/realloc/free, stack relocation, and names.

The final module must use the target Lua 5.4 headers, define
`LUAPROF_EXPECT_LUA_VERSION=504`, and leave `lua_profile_*` undefined for the
host to resolve. See the [integration guide](../integration-en.md) for the host
build and link steps.
