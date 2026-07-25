# luaprof 项目接入指南

[README](../README.md) | [采样模型](profiling-model.md) | [维护者指南](maintainer-guide.md)

本文说明如何把 `luaprof` 接入一个已有的 Linux 项目，包括固定线程运行一个 Lua VM 的
thread-per-VM 宿主，以及允许 Lua service 在 worker 之间迁移的 Skynet。Lua 层的使用
API 见项目根目录的 [README](../README.md)。

## 1. 先确认支持边界

V1 正式支持以下组合：

| 宿主 | Lua | CPU backend |
| --- | --- | --- |
| thread-per-VM | 项目固定的 PUC Lua 5.4.6 fork | `CLOCK_THREAD_CPUTIME_ID` |
| thread-per-VM | 项目固定的 PUC Lua 5.4.8 fork | `CLOCK_THREAD_CPUTIME_ID` |
| thread-per-VM | 项目固定的 PUC Lua 5.5.0 fork | `CLOCK_THREAD_CPUTIME_ID` |
| Skynet | 项目固定的 Skynet fork 及其内嵌定制 Lua 5.5 | 每个 worker 的线程 CPU timer |

原版 Lua 不包含 `luaprof` 需要的 VM bridge。仅仅编译 `luaprof.so`，而不修改宿主实际
运行的 Lua，无法工作。将 bridge 移植到其他 Lua 版本在技术上可行，但必须按该版本的
VM 数据结构重新审查，不能视为 ABI 兼容。

接入时应保证：

- `luaprof.so` 的头文件和运行时 Lua 来自同一棵源码、同一套 ABI。
- 一个进程中不要因为 profiler 再静态链接第二份 Lua。
- Linux 实时信号 `SIGRTMAX - 2` 供 thread-per-VM backend 使用，`SIGRTMAX - 3` 供
  Skynet backend 使用；安装 profiler timer 前，这些信号必须仍是默认处理方式，并且在
  对应 VM/worker 线程中未被屏蔽。luaprof 不会改写宿主的 signal mask，条件不满足时
  `profile.cpu.start()` 返回 host error。
- recorder 必须在 `lua_close` 之前停止。
- VM bridge ABI 必须为 `LUA_PROFILE_ABI_VERSION == 2`。

## 2. 接入由哪些部分组成

`luaprof` 分为三层：

1. **Lua VM bridge**：位于 Lua 内部，发布 VM 状态、safe point、allocation 事件和
   profiler-safe stack capture。
2. **Lua module**：`luaprof.so`，实现 Lua API、采样聚合、符号收集和 pprof 导出。
3. **宿主 backend**：thread-per-VM 直接使用 module 内的线程 timer；Skynet 还需要把
   `libluaprof-skynet-host.a` 链入 `skynet` 可执行文件并增加 scheduler hook。

内存事件来自 Lua 内部 `lmem.c`，而不是替换公开的 `lua_Alloc`。因此 `lua_Alloc` 的
签名和 `ud` 语义保持不变，同时 allocation callback 能拿到准确的 `lua_State *`、旧/新
指针、旧/新 requested size，以及 realloc 是否成功。

## 3. 修改或替换项目的 Lua

### 3.1 最稳妥的方式：采用固定 fork

对于普通 PUC Lua 5.4 或 5.5 项目，直接采用本仓库固定的准确 fork 是风险最低的方式：

```sh
git submodule update --init 3rd/lua-5.4.6
make lua46

git submodule update --init 3rd/lua-5.4.8
make lua

git submodule update --init 3rd/lua-5.5.0
make lua55
```

这些 fork 默认关闭 profiling bridge；父项目的 `make lua46`、`make lua`、`make lua55`
和 module target 会自动以 `LUAPROF=1` 构建。脱离父项目直接构建 fork 时必须显式启用：

```sh
make clean
make linux LUAPROF=1
```

`LUAPROF=1` 会对全部 Lua core translation unit 定义 `LUA_USE_LUAPROF`，加入
`lprofile.o`、公开 bridge API 和内部状态。普通 `make linux` 不包含这些内容，也没有 VM、
GC 或 allocator 的 profiling fast path。切换开关前必须 `make clean`，否则 Make 可能复用
使用另一套宏编译的旧 object。

宿主必须改为包含并链接这棵 Lua 的 `src/lua.h` 和 `src/liblua.a`。不要只替换头文件，
也不要让 `luaprof.so` 与宿主分别使用两份不同的 Lua。

Skynet 不能使用父项目的 PUC Lua fork。必须保留 Skynet 自带的 `3rd/lua`，并把同一
套 bridge 移植到该目录；本仓库固定的 `integration/skynet` 已经完成了这项工作，同时
保留带 seed 的 `lua_newstate`、code cache 和 shared Proto/table。

### 3.2 移植到项目自己的 Lua

先选择与目标 major/minor 匹配的 patch 指南：

- [Lua 5.4 bridge 移植](porting/lua-5.4.md)
- [Lua 5.5 bridge 移植](porting/lua-5.5.md)
- [Skynet embedded Lua 与 scheduler 移植](porting/skynet.md)

本仓库已经提交四份完整 patch，使用者不需要初始化 submodule 或自行生成：

| 目标 | Patch |
| --- | --- |
| Lua 5.4.6 | [`patches/lua-5.4.6.patch`](../patches/lua-5.4.6.patch) |
| Lua 5.4.8 | [`patches/lua-5.4.8.patch`](../patches/lua-5.4.8.patch) |
| Lua 5.5.0 | [`patches/lua-5.5.0.patch`](../patches/lua-5.5.0.patch) |
| Skynet | [`patches/skynet.patch`](../patches/skynet.patch) |

准确 baseline、target commit 和通用应用命令见 [`patches/README.md`](../patches/README.md)。
目标正好基于相同版本时，先用 `git apply --check` 检查再正式 apply。如果 Lua 已有内部
改造，必须审查完整 patch 并重新运行对应验证，不能只挑选部分 diff。Skynet patch 同时
包含 Lua bridge 和宿主 scheduler/host 集成。

## 4. thread-per-VM 项目接入

### 4.1 构建 module

使用仓库固定 Lua 时按 ABI 选择 target：

```sh
make module-lua46    # build/lua46/luaprof.so, PUC Lua 5.4.6
make module          # build/luaprof.so, PUC Lua 5.4.8
make module-lua55    # build/lua55/luaprof.so, PUC Lua 5.5.0
```

对于标准目录结构的自有 PUC Lua，可以让当前 Makefile 对那棵源码构建独立产物目录。
Lua 5.4.6：

```sh
make module-lua46 \
    LUA46_DIR=/absolute/path/to/lua-5.4.6 \
    LUA46_SRC=/absolute/path/to/lua-5.4.6/src \
    LUA46_LIB=/absolute/path/to/lua-5.4.6/src/liblua.a \
    LUA46_BUILD_DIR=/absolute/path/to/project-build/luaprof-lua46
```

Lua 5.4.8：

```sh
make module \
    LUA_DIR=/absolute/path/to/lua-5.4.8 \
    LUA_SRC=/absolute/path/to/lua-5.4.8/src \
    LUA_LIB=/absolute/path/to/lua-5.4.8/src/liblua.a \
    BUILD_DIR=/absolute/path/to/project-build/luaprof-lua54
```

Lua 5.5：

```sh
make module-lua55 \
    LUA55_DIR=/absolute/path/to/lua-5.5.0 \
    LUA55_SRC=/absolute/path/to/lua-5.5.0/src \
    LUA55_LIB=/absolute/path/to/lua-5.5.0/src/liblua.a \
    LUA55_BUILD_DIR=/absolute/path/to/project-build/luaprof-lua55
```

该方式会执行目标 Lua 目录的 `make linux`。自有 Lua 使用不同构建系统时，应把根
Makefile 中 `$(LUA_MODULE)` 的 source list 和编译参数移入项目构建系统。关键要求是：

- 所有 module object 使用 C11、`-fPIC` 和目标 Lua 的 include path。
- Lua core 和 luaprof module 编译时都定义 `LUA_USE_LUAPROF`；推荐通过 fork 的
  `LUAPROF=1` 统一设置 Lua core，不要只给个别 `.c` 文件加宏。
- PUC Lua 5.4.6/5.4.8 module 定义 `LUAPROF_EXPECT_LUA_VERSION=504`，5.5.0 module
  定义 `LUAPROF_EXPECT_LUA_VERSION=505`。
- shared module 链接 `-lm -lz -ldl -pthread -lrt`。
- module 保留对 `lua_*` 和 `lua_profile_*` 的未定义引用，由宿主正在运行的 Lua 解析；
  不要把另一份 `liblua.a` 静态装进 module。

如果宿主把 Lua 静态链接进可执行文件，Linux 链接时必须导出 Lua 符号，例如：

```make
my_host: $(HOST_OBJECTS) /path/to/liblua.a
	$(CC) -Wl,-E -o $@ $^ -lm -ldl -pthread
```

动态链接 Lua 时也要确认 `lua_profile_*` 符号在进程的动态符号范围内可见。

### 4.2 部署和生命周期

把 module 放入应用的 Lua C module 搜索路径：

```sh
export LUA_CPATH=/absolute/path/to/modules/?.so\;\;
```

也可以由宿主在创建 VM 后设置 `package.cpath`。每个需要 profile 的 VM 直接加载：

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start { sample_hz = 100 })
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

-- workload

local memory_result = assert(memory:stop())
local cpu_result = assert(cpu:stop())
assert(memory_result:write("heap.pb.gz"))
assert(cpu_result:write("cpu.pb.gz"))
```

CPU recorder 的 `start()`、VM 执行和 `stop()` 必须位于该 VM 所属的同一个 OS thread。
recorder 活动期间不能把 VM 迁移到其他线程。多个 thread-per-VM 可以分别 profile，当前
进程最多同时注册 64 个 thread timer；同一线程不能同时运行两个 VM 的 CPU recorder。
同一 VM 内的 coroutine 共享 bridge，可以正常采集。

## 5. Skynet 项目接入

Skynet service 会在 worker 之间迁移，不能使用 thread-per-VM timer 来代表 service 的
CPU time。完整接入必须同时完成内嵌 Lua bridge、host library、worker/dispatch hook 和
Skynet ABI module 四部分。

### 5.1 修改 Skynet 内嵌 Lua

把第 3 节 bridge 移植到项目的 `skynet/3rd/lua`，不要用父项目 Lua 替换整个目录。
修改时保留项目原有的：

- `lua_newstate` 参数和 seed 传播；
- `lua_sharefunction`、shared Proto/table 和 code cache；
- Skynet 对 coroutine、GC 和 state 创建流程的其他修改；
- `onelua.c` 或项目使用的 amalgamated build 入口。

构建完成后，bridge 和 Skynet 扩展符号必须同时存在于同一份 `liblua.a` 和最终的
`skynet` 可执行文件中。

### 5.2 构建并链接 host library

`src/skynet_host.c` 应使用项目内嵌 Lua 的头文件编译，归档为
`libluaprof-skynet-host.a`。参考命令为：

```sh
cc -std=c11 -O2 -fPIC \
    -I/path/to/luaprof/include \
    -I/path/to/skynet/3rd/lua \
    -c /path/to/luaprof/src/skynet_host.c -o skynet_host.o
ar rcs libluaprof-skynet-host.a skynet_host.o
```

把静态库链接进 `skynet` 可执行文件，而不是 Lua service module，并保证主程序使用
`-Wl,-E`（或等价的 `--export-dynamic`）。还需要 C11、pthread、realtime timer 和
`dlopen` 对应的链接环境。应用当前 Skynet patch 后，使用统一构建入口：

```sh
make clean
make -C 3rd/lua clean
make linux LUAPROF=1 \
    LUAPROF_HOST_LIB=/path/to/libluaprof-skynet-host.a \
    LUAPROF_INC=/path/to/luaprof/include
```

`LUAPROF=1` 同时让内嵌 Lua 定义 `LUA_USE_LUAPROF`，并让 Skynet 主程序定义
`SKYNET_LUAPROF`、链接 host library。普通 `make linux` 两层都关闭，不要求 luaprof header
或 library。Skynet 的普通 `make clean` 不清理内嵌 Lua object，因此切换模式时上面的第二条
命令不能省略。父仓库的 `make skynet` 已自动传入启用参数。

`luaprof.so` 通过 `dlsym(RTLD_DEFAULT, "lp_skynet_host_get_api")` 自动发现这个 host
backend。缺少 host library 时，module 会退回 thread-per-VM backend；这对会迁移的
Skynet service 是无效配置，不能投入使用。

### 5.3 增加 scheduler hook

定义 `SKYNET_LUAPROF` 时，在 Skynet worker 生命周期增加：

```c
lp_skynet_host_worker_start(worker_id);  /* worker 初始化后、dispatch loop 前 */
lp_skynet_host_worker_stop();            /* 离开 dispatch loop 后 */
```

在每次 service message callback 外围增加：

```c
lp_skynet_host_dispatch_enter(ctx->handle);
reserve_msg = ctx->cb(/* existing arguments */);
lp_skynet_host_dispatch_leave();
```

当前 fork 的准确位置见 `skynet-src/skynet_start.c` 和
`skynet-src/skynet_server.c`。所有 callback 分支都必须经过 enter/leave，且 leave 必须
在下一个 service dispatch 之前完成。handle 用于把当前 worker timer 样本归到目标
service；不要用 `lua_State *` 代替调度边界。

### 5.4 构建 Skynet ABI module

module 必须使用 `skynet/3rd/lua` 的头文件编译，并为当前定制 Lua 5.5 定义：

```text
LUAPROF_EXPECT_LUA_VERSION=505
```

产物应与普通 Lua module 分开，例如：

```text
modules/lua54/luaprof.so
modules/skynet-lua55/luaprof.so
```

本仓库的参考产物是 `build/skynet/luaprof.so`。不要把
`build/luaprof.so` 复制到 Skynet 中使用。Skynet 配置需要把正确目录加入 `lua_cpath`：

```lua
lua_cpath = "/absolute/path/to/skynet-lua55/?.so;" .. root .. "luaclib/?.so"
```

CPU recorder 应在目标 service 的 dispatch 内启动，例如 `skynet.start` callback。启动时
必须存在当前 service handle，否则 scheduler backend 会返回 host error。多个 service
可以并发 profile，但当前所有活动 CPU recorder 必须使用相同的 `sample_hz`。

## 6. 接入验证

不要只以“生成了 `.pb.gz` 文件”作为接入成功的依据。至少执行以下检查。

### 6.1 VM bridge contract

仓库固定 Lua：

```sh
make test-vm-bridge
```

自有 Lua 应使用它的头文件和库编译
`tests/integration/vm_bridge_test.c`。Skynet 的带 seed `lua_newstate` 版本需要额外定义
`LUAPROF_LUA_EXPLICIT_SEED`。该测试覆盖 Lua/C/GC/host 状态、pending safe point、
allocation/reallocation/free、coroutine 和 stack capture。

### 6.2 符号和 ABI 边界

普通或 Skynet Lua 库必须定义 bridge：

```sh
nm -g --defined-only /path/to/liblua.a | grep lua_profile_capturestack
```

module 应引用宿主 bridge，而不是自带另一份定义：

```sh
nm -D /path/to/luaprof.so | grep lua_profile_capturestack
```

预期类型是 `U`。Skynet 主程序还必须导出 host API 和内嵌 Lua bridge：

```sh
nm -D /path/to/skynet | grep lp_skynet_host_get_api
nm -g --defined-only /path/to/skynet | grep lua_profile_capturestack
```

### 6.3 运行时 smoke test

- thread-per-VM：执行较长的 Lua 计算和 allocation workload，分别停止 CPU/memory，
  确认 CPU `samples > 0`，memory `samples > 0`。
- Skynet：至少使用两个 worker，让目标 service 主动 yield/迁移，并确认
  `scheduler_workers > 0`；若为零，说明 module 没有使用 Skynet backend。
- `track_free = true` 时确认 `inuse_space > 0` 且 `live_map_overflows == 0`。
- 检查 `dropped_events`、`unstable_events`、`stack_truncations` 和各种 overflow 计数；
  它们不应在正常 workload 中占显著比例。

最后用 pprof 检查函数、行号和源码：

```sh
go tool pprof -top cpu.pb.gz
go tool pprof -lines -top cpu.pb.gz
go tool pprof -list=your_hot_function cpu.pb.gz
go tool pprof -sample_index=inuse_space -top heap.pb.gz
```

## 7. 常见接入错误

| 现象 | 优先检查 |
| --- | --- |
| `undefined symbol: lua_profile_*` | 宿主 Lua 没有 bridge，或者静态 Lua 符号未用 `-Wl,-E` 导出 |
| 编译时 `unexpected Lua ABI` | module 使用了错误版本的 Lua 头文件或错误的 `LUAPROF_EXPECT_LUA_VERSION` |
| Skynet CPU profile 的 `scheduler_workers == 0` | host library/hook 未生效，module 错误退回 thread backend |
| Skynet `cpu.start` 返回 host error | 不在 service dispatch 内、当前 handle 为零、worker 未注册、`SIGRTMAX - 3` 被屏蔽，或并发 recorder 的 `sample_hz` 不一致 |
| profile 只有地址形式的 CFunction | 可执行文件被 strip、符号未导出，或 Lua 可见绑定扫描不到该函数；不影响 Lua caller stack |
| memory `inuse_*` 始终为零 | 没有设置 `track_free = true` |
| 启动 timer 返回 host error | 实时信号已被占用、当前线程已有 recorder，或超过固定 timer/worker 上限 |

仓库中的 `examples/thread_vm`、`examples/skynet` 和 `tests/integration` 是接入后的最小
参考实现；生产项目应再增加覆盖自身 VM 创建、线程退出、service migration 和 shutdown
流程的回归测试。
