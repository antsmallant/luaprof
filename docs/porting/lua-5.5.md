# 向 Lua 5.5 移植 VM bridge

[接入指南](../integration.md) | [Lua 5.4](lua-5.4.md) | [Skynet](skynet.md)

本文给出 PUC Lua 5.5 的函数级修改位置。正式验证版本是 Lua 5.5.0。PUC Lua 5.5 与
Skynet 定制 Lua 都报告 `LUA_VERSION_NUM == 505`，但它们不是同一 VM ABI，module 不能
混用。

## 1. Reference diff

```text
baseline: 1097dbe
target:   3rd/lua-5.5.0 当前 HEAD
ABI:      LUA_PROFILE_ABI_VERSION == 2
```

在 `luaprof` 根目录生成、检查并应用完整 patch：

```sh
./scripts/generate-porting-patch.sh lua55 > /tmp/luaprof-lua55.patch
git -C /path/to/your-lua apply --check /tmp/luaprof-lua55.patch
git -C /path/to/your-lua apply /tmp/luaprof-lua55.patch
```

脚本只固定 baseline，终点始终是 submodule 当前 `HEAD`，并对仓库根目录 `-- .` 生成
完整 binary/full-index diff；后续 bridge commit 或修改文件变化时无需更新命令。目标正好
是 PUC Lua 5.5.0 基线时可以直接应用。其他 5.5.x 或定制 VM 应先执行 `--check`，冲突时
按函数移植。不要用 Skynet 的整棵 `3rd/lua` 替换 PUC Lua。

## 2. 新增 contract 与实现文件

### `src/lua.h`

在 `lua_Hook` 附近增加与 Lua 5.4 相同的 ABI version 2 contract：

- HOST/LUA/C/GC 状态与 Lua/C frame 常量；
- `lua_ProfileAllocEvent`、`lua_ProfileFrame`、三个 callback 类型；
- `lua_ProfileHooks`；
- `lua_setprofilehooks`、`lua_profile_request`、`lua_getprofilestate`、
  `lua_profile_capturestack`。

### `src/lprofile.h` 与 `src/lprofile.c`

从当前固定 Lua 5.5 fork 完整复制这两个新增文件。它们实现 state guard、atomic
pending、safe point、allocation event 和 profiler-safe stack capture。不要只复制
API 声明。

### `src/lstate.h`

在 `global_State` 增加：

```text
profilehooks, ud_profile, profilethread, profilepending, profileincallback
```

在 `lua_State` 增加：

```text
profilestate, profilecaptureblocked, profilecfunction
```

保持 Lua 5.5 原有 `LX mainth` 布局和 seed/state ABI，不把 profiler 数据放进
`lua_Alloc.ud`。

## 3. 各文件的函数级修改

### `src/lstate.c`

| 函数 | 修改 |
| --- | --- |
| `preinit_thread` | 对 main thread 与每个 coroutine 初始化 HOST 状态、capture guard、CFunction。 |
| `lua_newstate` | 在保留三参数 `(f, ud, seed)` 的前提下清零 global profiler 字段。 |
| `lua_closethread` | reset 后 unwind；仅当 `from != NULL && from != L` 时同步调用方。 |
| `close_state` | 在释放 `global_State` main block 前发送最终 free event。 |

Lua 5.5 的 main block 是 `global_State *g`、大小为 `sizeof(global_State)`；这里不能照搬
Lua 5.4 的 `fromstate(L)`/`sizeof(LG)`。

### `src/lvm.c`

| 位置 | 修改 |
| --- | --- |
| `vmfetch()` | instruction safe boundary 调用 `luaP_safepoint(L)`。 |
| `luaV_execute` 入口 | 创建 state guard 并发布 LUA。 |
| `CIST_FRESH` return | 返回前恢复 previous frame 状态。 |

signal handler 只增加 pending；Lua stack 只在 `vmfetch()` 消费 pending 后捕获。

### `src/ldo.c`

| 函数/路径 | 修改 |
| --- | --- |
| `luaD_throw` | 抛出前 `luaP_unwind(L)`。 |
| `luaD_reallocstack` | `relstack` 前阻止 capture，所有 `correctstack(L, oldstack)` 后恢复。 |
| `precallC` | CFunction 调用外围发布 C 状态与函数指针。 |
| `finishCcall` | `lua_KFunction` continuation 外围发布/恢复 C 状态。 |
| `resume` 的 C continuation | 同上。 |
| `lua_resume` | 写入 `nresults` 后 unwind 当前 thread，再同步 `from`。 |
| `luaD_pcall` | protected recovery 后调用 `luaP_syncstate(L)`。 |

Lua 5.5 使用 `TStatus`/`APIstatus`，并且 `precallC`、stack correction 的签名与 Lua 5.4
不同。只移植 guard 语义，不要用 5.4 函数体替换 5.5 实现。

### `src/lgc.c`

以下入口外围发布 GC 状态并在返回前恢复：

- `luaC_changemode`
- `luaC_freeallobjects`
- `luaC_step`
- `luaC_fullgc`

Lua 5.5 的 generational mode 状态机与 Lua 5.4 不同；guard 包住目标原函数即可，不修改
collector 分支。

### `src/lmem.c`

| 函数 | 事件语义 |
| --- | --- |
| `luaM_malloc_` | 成功报告完整 requested size；最终失败在 `luaM_error` 前报告 failure。 |
| `luaM_realloc_` | 成功报告 old/new pointer 与完整 size；失败保留旧 block 并报告 requested new size。 |
| `luaM_free_` | free 完成后报告旧指针和旧 size，不访问已释放内存。 |

保留 Lua 5.5 原有 `GCdebt` 计算，只添加 event。成功 realloc 无论原地还是搬移，都在
profiler 语义中结束旧 block，并作为一个完整新 allocation 处理。

### `src/ldebug.c` 与 `src/ldebug.h`

把内部 `getfuncname` 改为可由 VM 内部调用的 `luaG_getfuncname`，debug API 继续使用该
函数，stack capture 用它保存 best-effort Lua 调用名。

### `src/Makefile`

- `CORE_O` 增加 `lprofile.o`；
- 增加 `lprofile.o` dependency；
- `ldo.o`、`lgc.o`、`lmem.o`、`lstate.o`、`lvm.o` dependency 增加
  `lprofile.h`。

官方 PUC Lua 5.5.0 没有 Skynet 的 `onelua.c`；如果目标 fork 使用 unity build，自行将
`lprofile.c` 加入相应入口。

## 4. 与 Lua 5.4 的关键差异

移植时特别检查：

- `lua_newstate` 是三参数并显式接收 seed；
- main state 内嵌在 `global_State`，close event 的 pointer/size 不同；
- `luaD_reallocstack` 的 `correctstack` 需要 `oldstack`；
- `lua_resume` 和内部 status 类型不同；
- GC mode 与 `GCdebt` 更新不同；
- PUC Lua 5.5 module 与 Skynet Lua 5.5 module 必须分开构建。

其余 bridge contract 与 Lua 5.4 保持 ABI version 2 一致，这使 profiler core 可以复用，
不表示 Lua VM ABI 本身兼容。

## 5. 验证

固定 fork 的完整入口：

```sh
make test-lua55
make example-lua55
```

`test-lua55` 覆盖：

- thread-per-VM smoke；
- Lua-visible CFunction symbols；
- module API lifecycle 与 ABI boundary；
- VM bridge contract；
- CPU、memory 和组合 sampling。

自有 Lua 编译 `vm_bridge_test.c` 时必须定义 `LUAPROF_LUA_EXPLICIT_SEED`：

```sh
cc -DLUAPROF_LUA_EXPLICIT_SEED -I/path/to/lua/src \
    tests/integration/vm_bridge_test.c /path/to/lua/src/liblua.a \
    -lm -ldl -o vm-bridge-test
```

module 使用 `make module-lua55` 构建为 `build/lua55/luaprof.so`，必须定义
`LUAPROF_EXPECT_LUA_VERSION=505`。使用 `nm -D` 检查
`lua_profile_capturestack` 在 module 中是 `U`，由实际宿主 Lua 解析。
