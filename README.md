# luaprof

[English](READ-en.md)

`luaprof` 是面向 PUC Lua 的采样分析器。CPU 和内存 recorder 相互独立；同一套 API
既适用于使用固定 Lua 5.4.8 fork 的 Linux thread-per-VM 宿主，也适用于受支持的
Skynet service（使用 Skynet 固定的定制 Lua 5.5）。V1 有意不包含 call/return tracing。

## 环境要求

- Linux，支持 POSIX 线程 CPU timer，以及无锁的指针、整数和 64 位原子操作
- C11 编译器、GNU Make、POSIX threads 和 zlib 开发文件
- 默认构建需要 `3rd/` 中固定版本的 `lua-5.4.8` fork；原版 Lua 没有暴露所需的
  profiling bridge
- 能通过 GitHub SSH 初始化已配置的 submodule URL
- 使用 Go 的 `pprof` 命令读取默认输出；生成 SVG 和基于图的 Web 报告还需要 Graphviz

profiler core 不依赖 Skynet。Skynet 支持是通过 `integration/skynet` 中固定版本 fork
提供的显式宿主集成；该 target 保留 Skynet 自带 Lua，包括带 seed 的 state 创建、
code cache 以及 shared Proto/table 支持。

## 快速开始

```sh
git clone git@github.com:antsmallant/luaprof.git
cd luaprof
make
make test
make example-thread-vm
```

示例会同时运行 CPU 和 in-use memory recorder，先停止 memory recorder，再继续 CPU
profiling，并写出：

```text
build/thread-vm-cpu.pb.gz
build/thread-vm-heap.pb.gz
```

使用标准 pprof 工具查看：

```sh
go tool pprof -top build/thread-vm-cpu.pb.gz
go tool pprof -lines -top build/thread-vm-cpu.pb.gz
go tool pprof -list=calculate_orders build/thread-vm-cpu.pb.gz
go tool pprof -sample_index=alloc_space -top build/thread-vm-heap.pb.gz
go tool pprof -sample_index=inuse_space -top build/thread-vm-heap.pb.gz
```

默认 `-top` 按函数聚合，示例应能看到 `calculate_orders`、
`calculate_discounts` 和 `tostring [luaB_tostring]` 等热点。`-lines -top`
按实际执行行拆分，`-list` 则把采样权重标到对应源码行。主 chunk 稳定显示为
`main chunk`，不再显示成 `profile.lua:0`。

`make` 只初始化必需的 Lua submodule。`make test` 运行 core、thread-per-VM 和 exporter
测试；只有显式的 Skynet target 才会初始化 Skynet 及其直接 submodule。

将 `luaprof` 接入已有的 thread-per-VM 或 Skynet 项目时，请阅读
[项目接入指南](docs/integration.md)。该文档包含 Lua VM bridge 的源码修改清单、构建与
链接要求、Skynet scheduler hook，以及接入后的验证步骤。

## Lua API

直接启动各个 recorder，不使用共享的 `profile.start()` mode table：

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start {
    sample_hz = 100,
})
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

-- 在这里运行 workload。

local memory_result = assert(memory:stop())
-- 此时 CPU profiling 仍在运行。
local cpu_result = assert(cpu:stop())

assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz"))
```

`profile.cpu.start([options])` 接受 `sample_hz`，取值为 1 到 10000 的整数，默认值为
100Hz。它测量线程 CPU time，因此 sleep 不会产生样本。

`profile.memory.start([options])` 接受正整数 `sample_bytes` 和布尔值 `track_free`，
默认值分别为 512KiB 和 `false`。`sample_bytes = 1` 会记录每次成功的 allocation 和
realloc。

一个 Lua VM 同一时间每种 recorder 最多只能有一个处于活动状态，但 CPU 和 memory
可以独立运行和停止。recorder 的 `__gc` 和 `__close` 方法会停止并丢弃仍在活动的
recording；需要结果时应显式调用 `stop()`。停止后的 result 持有冻结的 profile，并支持：

- `result:stats()`：返回计数器和质量元数据
- `result:write(path[, options])`：导出 pprof 或 folded stacks

未知 option 和错误的 option 类型会触发 Lua 参数错误。宿主或生命周期错误返回
`nil, error`。

## CPU 采样

thread-per-VM backend 使用 `CLOCK_THREAD_CPUTIME_ID`。timer tick 只记录很小的 VM
状态快照，不会在 signal handler 中遍历 Lua 或 native stack。recorder 在下一个 VM
safe point 消费 pending tick 并捕获 Lua stack。

发生在长时间 C 调用中的 tick 会保留 `lua_CFunction` 指针和 Lua caller。导出时，
profiler 在非热路径扫描 `_G` 与 `package.loaded` 中的 CFunction 绑定，并读取本机 ELF
mapping/symbol table。名称按以下顺序选择：Lua 可见绑定名、native symbol、原始
`lua_CFunction@0x...` 地址；两种名称都存在且不同时会显示为
`tostring [luaB_tostring]`。同一指针有多个 Lua 别名时选择最短名称，同长度再按字典序
选择。

这些步骤只发生在 `result:write()`，不会进入 signal handler、allocation callback 或
VM instruction fast path。二进制被 strip、文件已移动、不是 ELF，或绑定扫描未覆盖时，
地址 fallback 仍然保留。profile 会写入本机 mapping path；在另一台机器继续做 native
symbolization 时需要匹配的原二进制。这里仍然只表示当前 `lua_CFunction`，不还原 native
C stack；定位 native hot line 需要另用 native profiler。GC 和 host 状态以 synthetic
frame 输出。

重要的 CPU 统计项包括：

- `samples`：完成归因的 timer-tick 权重，包括 timer overrun
- `sample_lua`、`sample_c`、`sample_gc`、`sample_host`：按 VM 状态划分的 tick 权重
- `safe_points`、`pending_weight`：safe-point 消费次数和请求权重
- `state_lua`、`state_c`、`state_gc`、`state_host`：VM 状态切换次数
- `dropped_events`：固定 event ring 已满而丢失的 tick
- `unstable_events`：execution-slot 发布竞争期间被拒绝的 tick
- `profiler_overhead_events`：采集 profile 数据期间到达的 tick
- `stale_events`、`scheduler_workers`：Skynet generation 拒绝次数和目标使用过的 worker 数
- `stack_truncations`、`aggregate_overflows`、`symbol_overflows`：有界存储的质量计数器

判断 profile 是否健康时，应比较 `samples`、持续时间和配置频率，检查各状态占比，
并确认 drop/overflow 计数可以忽略。过短的 profile 可能没有足够样本支撑结论。

## 内存采样

allocation interval 服从期望字节间隔为 `sample_bytes` 的几何分布。一个 allocation
按照完整 requested new size 的比例被选中，最多产生一个样本，并通过逆概率加权估算
allocation bytes 和 objects。free 和失败的 realloc 不消耗采样预算。成功的 realloc
视为旧 block 结束，并按完整的新 requested size 进行一次 allocation。

内存统计项将观测值和估算值分开：

- `allocation_events`、`reallocation_events`、`free_events` 和
  `allocation_failures`：recording 期间精确的 allocator event 计数
- `samples`、`sampled_alloc_bytes`：原始的入选 event 数和 requested bytes
- `alloc_space`、`alloc_objects`：概率加权后的 allocation 估算
- `inuse_space`、`inuse_objects`：停止时仍存活的加权 sampled block
- `live_map_overflows`：未能进入 in-use tracking 的 sampled live block
- `stack_truncations`、`aggregate_overflows`、`symbol_overflows`：有界存储的质量计数器

当 `track_free = false` 时，不会分配或查询 live-pointer map，in-use 指标保持为零。
当 `track_free = true` 时，只保存被采样的 live block，free 会归因回其 allocation stack。
不会采集 free-site、lifetime、peak 或逐对象 timeline profile。

Lua 在重新分配自身 VM stack 时，call-frame 指针暂时不可用。被选中的 stack-reallocation
event 仍会计入内存指标，但会以空 stack 保存并增加 `stack_truncations`，而不会遍历
无效的 VM 状态。

这些指标是 allocator requested size，不是 RSS、物理内存或 VM heap snapshot。
`sample_bytes = 1` 能得到精确的 requested alloc-space 和 in-use 值；更大的 interval
是统计估算。增大 interval 会减少 stack-capture 工作，但会提高方差，尤其是在短 profile
和 in-use object 数较少时。

## 导出格式

默认格式是经过 gzip 压缩的 Google `profile.proto`：

```lua
assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz", {
    sample = "alloc_space",
}))
```

CPU profile 包含 `samples/count` 和 `cpu/nanoseconds`。Memory profile 包含
`alloc_objects/count`、`alloc_space/bytes`、`inuse_objects/count` 和
`inuse_space/bytes`。默认 sample 对 CPU 是 `cpu`；关闭 free tracking 时是
`alloc_space`；启用时是 `inuse_space`。

Lua frame 的函数名、定义行和当前执行行是相互独立的数据。pprof 默认函数视图使用
函数名；`-lines` 和 `-list` 使用当前执行行。Lua 的调用名是 VM 在采样点能够推断出的
best-effort 名称；匿名调用无法推断时回退到 source/definition line。

其他报告示例：

```sh
go tool pprof -text cpu.pb.gz
go tool pprof -sample_index=inuse_space -svg heap.pb.gz > heap.svg
go tool pprof -http=:0 heap.pb.gz
```

还可以为 flame graph 工具导出从 root 到 leaf 的 folded stack：

```lua
assert(memory_result:write("heap.folded", {
    format = "folded",
    sample = "inuse_space",
}))
```

## Skynet 集成

显式构建并运行双 worker 示例：

```sh
make example-skynet
```

`make example-skynet` 运行 `examples/skynet/luaprof_demo.lua` 中较长的诊断 workload，
并生成：

```text
build/skynet-cpu.pb.gz
build/skynet-heap.pb.gz
```

查看函数、执行行和源码列表：

```sh
go tool pprof -top build/skynet-cpu.pb.gz
go tool pprof -lines -top build/skynet-cpu.pb.gz
go tool pprof -source_path=examples/skynet -list=calculate_orders build/skynet-cpu.pb.gz
go tool pprof -sample_index=inuse_space -top build/skynet-heap.pb.gz
```

Skynet 从 `integration/skynet` 加载 `../../examples/...`，因此从仓库根目录运行 `-list`
时需要上述 `-source_path`。`tests/integration/skynet.sh` 默认运行更短的
`luaprof_smoke.lua`，只承担 CI lifecycle/shared-table 回归，不用于展示热点结论。
Skynet fork 将一个小型 host library 链接到可执行文件中，在 service dispatch 边界发布
目标 VM，并为每个 worker 维护一个 CPU timer。

两个宿主有意使用不同 Lua ABI 的构建产物：

```text
build/luaprof.so          Lua 5.4.8 thread-per-VM module
build/skynet/luaprof.so   Skynet customized Lua 5.5 module
```

`make test-skynet` 使用 `integration/skynet/3rd/lua` 构建 Skynet；不会传入父项目的
`LUA_INC` 或 `LUA_LIB`。该 target 会检查链接符号，使用 Skynet Lua 运行 VM bridge 和
Lua API 测试，然后运行覆盖 shared table 的真实双 worker service。

多个 Skynet service 可以并发记录 CPU，但目前所有活动的 CPU recorder 必须使用相同的
`sample_hz`。scheduler 测试覆盖 service migration、stale tick、worker shutdown 和
concurrent stop。

使用 Skynet Lua 运行相同的 VM 和组合 profiler benchmark：

```sh
make bench-skynet-vm
make bench-skynet-combined
```

## 固定上限

热路径存储会预先分配，recording 期间不会增长：

| 资源 | 每个 recorder 或 host 的上限 |
| --- | ---: |
| 捕获的 stack 深度 | 64 frames |
| CPU symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| Memory symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| 每个采样 Lua 函数的调用名 | 255 bytes |
| 使用 `track_free` 时的 sampled live blocks | 16384 |
| Thread 或 Skynet timer event ring | 4096 entries |
| Thread timers / Skynet targets / Skynet workers | 64 / 128 / 64 |

超过 1024 bytes 的 source name 和超过 255 bytes 的 Lua 调用名会被截断，并增加
`symbol_overflows`。达到 recording 上限后仍保持有界，对应的 drop、truncation 或
overflow 计数器会增加。导出在 stop 后执行，期间可以分配内存；导出期 Lua-visible
CFunction 扫描另有 4096 function、4096 table、6 层和 255-byte 名称上限，未覆盖的
函数继续尝试 native symbol，最终保留地址 fallback。

## 支持范围

- thread-per-VM 宿主使用父仓库准确 gitlink 固定的 PUC Lua 5.4.8
- Linux thread-per-VM 宿主，且 VM 始终在所属 OS thread 上运行
- 固定版本的 Skynet fork 及其定制 Lua 5.5；VM 可以在 worker 之间串行迁移
- 不超过文档所述固定深度的 Lua 和 coroutine stack

原版 Lua、未列出的 Lua 版本、Windows/macOS、native C stack unwinding、tracing、
allocation timeline 和 VM object snapshot 不在 V1 范围内。

## Submodule 开发

父仓库固定准确的 Lua 和 Skynet commit。`branch = luaprof` 只用于标识协作分支；
普通构建不会浮动到远端最新 commit。

修改 fork 时，先提交并推送 fork，再更新父仓库 gitlink：

```sh
git -C 3rd/lua-5.4.8 switch luaprof
git -C 3rd/lua-5.4.8 add src/lprofile.c
git -C 3rd/lua-5.4.8 commit -m "describe the Lua change"
git -C 3rd/lua-5.4.8 push origin luaprof
git add 3rd/lua-5.4.8
git commit -m "build: update Lua submodule"
```

`integration/skynet` 使用相同的步骤。全新 checkout 应执行
`git submodule update --init 3rd/lua-5.4.8 integration/skynet`，以恢复准确固定的
commit，且不会下载未使用的嵌套依赖。

Skynet Lua 的 profiling 修改应放在 `integration/skynet/3rd/lua` 中，并作为 Skynet
fork 的一部分提交。不要用父项目 Lua fork 替换该目录，也不要把父项目的 `LUA_INC`/
`LUA_LIB` 传入 Skynet 构建。两棵 Lua 源码树都暴露 profiler bridge ABI version 2，
运行相同的 bridge contract test，同时保留各自的 VM ABI 和宿主特定行为。
