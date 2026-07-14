# 向 Lua 5.4 移植 VM bridge

[接入指南](../integration.md) | [Lua 5.5](lua-5.5.md) | [Skynet](skynet.md)

本文给出 PUC Lua 5.4 的函数级修改位置。正式验证版本是 Lua 5.4.8；其他 5.4.x 应以
本文作为移植参考，并重新运行完整 contract test。

## 1. Reference diff

准确基线和已验证 commit：

```text
baseline: 46f8c3d
target:   3rd/lua-5.4.8 当前 HEAD
ABI:      LUA_PROFILE_ABI_VERSION == 2
```

在 `luaprof` 根目录生成完整 patch，并先对目标 Lua 做 dry-run：

```sh
./scripts/generate-porting-patch.sh lua54 > /tmp/luaprof-lua54.patch
git -C /path/to/your-lua apply --check /tmp/luaprof-lua54.patch
git -C /path/to/your-lua apply /tmp/luaprof-lua54.patch
```

脚本固定 baseline，但始终 diff 到 submodule 当前 `HEAD`，并使用仓库根目录 `-- .`，所以
以后 bridge 修改或增加文件时不需要维护 target commit 和文件清单。它忽略 untracked
文件，并在存在未提交的 tracked 修改时拒绝生成。目标正好是同一基线时可以直接应用；
其他 5.4.x 或已有内部修改的 fork 应先执行 `--check`，冲突时结合下列函数说明逐项移植。

## 2. 公开 bridge contract

### `src/lua.h`

在 `lua_Hook` 类型附近增加：

- `LUA_PROFILE_ABI_VERSION`；
- HOST/LUA/C/GC 状态常量；
- Lua/C frame 类型常量；
- `lua_ProfileAllocEvent` 与 `lua_ProfileFrame`；
- safe-point、state-change、allocation callback 类型；
- `lua_ProfileHooks`。

在 state API 声明区增加：

```c
lua_setprofilehooks
lua_profile_request
lua_getprofilestate
lua_profile_capturestack
```

callback 不能调用 Lua、通过 Lua allocator 分配或抛错。safe-point 和 allocation
callback 只允许调用 `lua_profile_capturestack`。

## 3. 状态存储与 bridge 实现

### `src/lstate.h`

在 `global_State` 增加：

```text
profilehooks, ud_profile, profilethread, profilepending, profileincallback
```

在 `lua_State` 增加：

```text
profilestate, profilecaptureblocked, profilecfunction
```

这些字段属于 VM，不属于公开 allocator `ud`。不要改变 `lua_Alloc` ABI。

### `src/lprofile.h` 与 `src/lprofile.c`

这两个文件应从 reference commit 完整移植，不建议重新实现。它们负责：

- 有条件的 state guard；
- signal-safe pending weight 增加/交换；
- safe-point callback；
- allocation event 封装；
- Lua/C frame capture、source、definition/current line 和调用名；
- error/yield 后的状态同步。

如果目标编译器不是 GCC/Clang，reference 中的 atomic fallback 只适用于没有异步并发
request 的环境；Linux 正式支持路径要求相应原子操作无锁。

## 4. 各文件的函数级修改

### `src/lstate.c`

| 函数 | 修改 |
| --- | --- |
| `preinit_thread` | 初始化 `profilestate = HOST`、capture guard 和当前 CFunction。每个 coroutine 都要执行。 |
| `lua_newstate` | 清零 `global_State` 中的 hooks、userdata、pending 和 callback guard。 |
| `lua_closethread` | reset 后发布 HOST；存在 `from` 时同步调用方状态。 |
| `close_state` | 在释放 main `LG` block 前发送最后一个 free event；callback 必须发生在 `lua_State` 仍有效时。 |

Lua 5.4.8 的 main block 是 `fromstate(L)`、大小为 `sizeof(LG)`。不要照搬 Lua 5.5 的
`global_State` main block 写法。

### `src/lvm.c`

| 位置 | 修改 |
| --- | --- |
| `vmfetch()` | 在取/执行每条 instruction 的安全边界调用 `luaP_safepoint(L)`。宏的 fast path 必须先检查 hook 和 pending。 |
| `luaV_execute` 入口 | 创建 `luaP_StateGuard`，发布 LUA 状态。 |
| `luaV_execute` 的 `CIST_FRESH` return | 恢复 previous frame 状态后才能返回。 |

不要从 timer signal handler 直接遍历 stack。signal handler 只调用
`lua_profile_request`；stack capture 在 `vmfetch()` safe point 完成。

### `src/ldo.c`

| 函数/路径 | 修改 |
| --- | --- |
| `luaD_throw` | longjmp 前调用 `luaP_unwind(L)`，避免保留错误的 C/Lua 状态。 |
| `luaD_reallocstack` | `relstack` 前置 `profilecaptureblocked = 1`；所有成功/失败恢复路径在 `correctstack` 后清零。 |
| `precallC` | 调用 CFunction 前发布 C 状态和函数指针，返回后恢复 previous frame。 |
| `finishCcall` | continuation 外围使用同样的 C state guard。 |
| `resume` 的 C continuation | 调用 continuation 前后发布/恢复 C 状态。 |
| `lua_resume` | 返回宿主前 unwind 当前 thread，并同步 `from`。 |
| `luaD_pcall` | protected call 完成错误恢复后，根据当前 `CallInfo` 重新同步状态。 |

Lua 5.4 的 stack relocation 暂时把 frame pointer 转为 offset。allocation hook 可能恰好
在此窗口命中，因此 capture guard 不能省略；命中的 allocation 仍记录，但 stack 为空并
增加 truncation。

### `src/lgc.c`

在下列公开 GC 入口外围使用 `luaP_StateGuard` 发布 GC 状态，并在所有正常返回前恢复：

- `luaC_changemode`
- `luaC_freeallobjects`
- `luaC_step`
- `luaC_fullgc`

不要在每个内部 mark/sweep helper 上重复切换；入口 guard 已覆盖嵌套 GC 工作。

### `src/lmem.c`

| 函数 | 事件语义 |
| --- | --- |
| `luaM_malloc_` | 成功后发送 `(NULL, new, 0, size, success=1)`；两次尝试都失败时在抛错前发送 failure。 |
| `luaM_realloc_` | 失败发送旧指针和 requested new size，旧 block 保持存活；成功发送完整 old/new pointer 与 size。 |
| `luaM_free_` | allocator free 完成后发送 `(old, NULL, old_size, 0, success=1)`，不得解引用旧指针。 |

事件应放在 Lua 内部 wrapper，不放进公开 `lua_Alloc`。这样 callback 才能同时得到
`lua_State *` 和准确的旧/新 requested size。Lua 5.4 的 `GCdebt` 更新方向与 Lua 5.5
不同，移植时保留目标版本原有 debt 代码，只在其后增加事件。

### `src/ldebug.c` 与 `src/ldebug.h`

将内部 `getfuncname` 暴露为 `luaG_getfuncname`，保留原 debug API 调用，并让
`lua_profile_capturestack` 复用它。不要在采样热路径扫描 `_G`；Lua-visible CFunction
绑定扫描在 result export 阶段完成。

### `src/Makefile`

- 将 `lprofile.o` 加入 `CORE_O`；
- 添加 `lprofile.o` dependency；
- 为引用 `lprofile.h` 的 `ldo.o`、`lgc.o`、`lmem.o`、`lstate.o`、`lvm.o` 更新 dependency。

若项目使用 amalgamated build，还必须在相应 unity source 中包含 `lprofile.c`。

## 5. 验证

构建固定 fork 和 module：

```sh
make lua
make module
make test-vm-bridge
make test-api
make test-cpu-sampling
make test-memory-sampling
make test-combined-sampling
```

自有 Lua 至少应使用其头文件和 `liblua.a` 编译
`tests/integration/vm_bridge_test.c`。该测试覆盖 pending safe point、Lua/C/GC/HOST、
coroutine、yield/error、allocation/realloc/free、stack relocation 和调用名。

最终 module 必须使用目标 Lua 5.4 头文件、定义
`LUAPROF_EXPECT_LUA_VERSION=504`，并保持 `lua_profile_*` 为由宿主解析的 undefined
symbol。完整宿主构建与链接步骤见[项目接入指南](../integration.md)。
