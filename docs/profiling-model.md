# 采样模型与结果解读

[README](../README.md) | [项目接入](integration.md) | [维护者指南](maintainer-guide.md)

本文说明 `luaprof` 的 Lua API、CPU 和内存采样语义、输出格式、质量指标与固定限制。

## 1. Recorder 生命周期

CPU 和 memory recorder 相互独立，不使用共享的 `profile.start()` mode table：

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start {
    sample_hz = 100,
})
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

-- workload

local memory_result = assert(memory:stop())
-- CPU profiling 仍在运行。
local cpu_result = assert(cpu:stop())

assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz"))
```

`profile.cpu.start([options])`：

- `sample_hz`：1 到 10000 的整数，默认 100Hz。
- thread-per-VM backend 测量线程 CPU time，sleep 不产生样本。
- Skynet backend 测量目标 service 在 worker 上执行时消耗的线程 CPU time。

`profile.memory.start([options])`：

- `sample_bytes`：正整数，默认 512KiB，表示期望采样字节间隔。
- `track_free`：布尔值，默认 `false`；启用后计算停止时的 sampled in-use 指标。
- `sample_bytes = 1` 会选择每次成功的 allocation 和 realloc。

一个 Lua VM 同一时间每种 recorder 最多只能有一个处于活动状态，但 CPU 和 memory 可以
独立运行和停止。recorder 的 `__gc` 和 `__close` 会停止并丢弃仍在活动的 recording；
需要结果时应显式调用 `stop()`。

停止后的 result 持有冻结的 profile：

- `result:stats()`：返回计数器和质量元数据。
- `result:write(path[, options])`：导出 pprof 或 folded stacks。

未知 option 和错误的 option 类型会触发 Lua 参数错误。宿主或生命周期错误返回
`nil, error`。

## 2. CPU 采样模型

### 2.1 thread-per-VM

thread-per-VM backend 使用 `CLOCK_THREAD_CPUTIME_ID`。timer tick 只记录小型 VM 状态
快照，不在 signal handler 中遍历 Lua 或 native stack。recorder 在下一个 VM safe
point 消费 pending tick 并捕获 Lua stack。

因此采样权重对应实际 timer tick，但 stack 是该 tick 后第一个安全位置的 Lua stack。
这种设计避免在异步信号中访问不稳定 VM 数据。VM 必须始终在启动 recorder 的 OS
thread 上运行。

### 2.2 Skynet service

Skynet backend 是进程级基础设施，但 profile target 是单个 service：

- `profile.cpu.start()` 在当前 dispatch 中读取 service handle 并注册 target。
- 每个 worker 使用自己的线程 CPU timer。
- dispatch hook 只在当前 handle 等于活动 target 时发布样本。
- service 迁移到其他 worker 后，target 随 handle 继续跟踪。
- 其他 service、消息排队时间和 sleep 不计入该 result。

多个 service 可以分别记录独立 profile。当前所有并发 CPU recorder 必须使用相同的
`sample_hz`。Lua API 没有整个 Skynet 进程合并 profile，也不能从一个 service 指定另一
任意 handle。

### 2.3 Lua、C、GC 和 host 状态

样本按 VM 状态归入 Lua、CFunction、GC 或 host。发生在长时间 C 调用中的 tick 会保留
当前 `lua_CFunction` 指针和 Lua caller，不需要在热路径展开 native C stack。

导出时，profiler 在非热路径扫描 `_G` 与 `package.loaded` 中的 CFunction 绑定，并读取
本机 ELF mapping/symbol table。名称按以下顺序选择：

1. Lua 可见绑定名；
2. native symbol；
3. `lua_CFunction@0x...` 原始地址。

两种名称都存在且不同时显示为 `tostring [luaB_tostring]`。同一指针有多个 Lua 别名时
选择最短名称，同长度再按字典序选择。

符号扫描只发生在 `result:write()`，不会进入 signal handler、allocation callback 或
VM instruction fast path。二进制被 strip、文件已移动、不是 ELF，或扫描未覆盖时，地址
fallback 仍然保留。profile 会写入本机 mapping path；在另一台机器继续 native
symbolization 需要匹配的原二进制。

这里记录的是当前 CFunction，不还原 native C stack。定位 C/C++ 内部热点行需要另用
native profiler。GC 和 host 状态使用 synthetic frame。

### 2.4 CPU 统计项

主要统计字段：

- `samples`：完成归因的 timer-tick 权重，包括 timer overrun。
- `sample_lua`、`sample_c`、`sample_gc`、`sample_host`：按 VM 状态划分的权重。
- `safe_points`、`pending_weight`：safe-point 消费次数和请求权重。
- `state_lua`、`state_c`、`state_gc`、`state_host`：VM 状态切换次数。
- `dropped_events`：固定 event ring 已满而丢失的 tick。
- `unstable_events`：execution slot 发布竞争期间被拒绝的 tick。
- `profiler_overhead_events`：采集 profile 数据期间到达的 tick。
- `stale_events`：Skynet generation 或迁移边界拒绝的过期 tick。
- `scheduler_workers`：Skynet target 实际使用过的 worker 数。
- `stack_truncations`、`aggregate_overflows`、`symbol_overflows`：有界存储的质量计数器。

判断 CPU profile 是否健康时，应比较 `samples`、持续时间和配置频率，检查各状态占比，
并确认 drop/overflow 可以忽略。过短的 profile 没有足够样本支撑结论。

## 3. 内存采样模型

### 3.1 随机采样

allocation interval 服从期望字节间隔为 `sample_bytes` 的几何分布。一个 allocation
按照完整 requested new size 的比例被选中，最多产生一个样本，并通过逆概率加权估算
allocation bytes 和 objects。

free 和失败的 realloc 不消耗采样预算。成功 realloc 视为旧 block 结束，并按完整的
新 requested size 进行一次 allocation。事件来自 Lua 内部 allocator wrapper，因此
包含准确的 `lua_State *`、旧/新指针、旧/新 requested size 和成功状态。

内存 profile 只覆盖目标 Lua VM 通过其 Lua allocator 产生的事件。它不是进程 RSS、
物理内存、Skynet C 层 allocation，也不是遍历 VM 对象得到的 heap snapshot。

### 3.2 alloc-space 与 in-use

`track_free = false` 时，不分配或查询 live-pointer map，只生成 alloc-space 指标，
in-use 指标为零。

`track_free = true` 时，只保存被采样的 live block。free 根据指针找到相应 sampled
allocation，并从停止时的 in-use 结果中移除。不会记录未被采样的指针，也不会采集
free-site、lifetime、peak 或逐对象 timeline。

`sample_bytes = 1` 能得到精确的 allocator requested alloc-space 和 in-use 值；更大的
interval 是统计估算。增大 interval 会减少 stack-capture 工作，但会提高方差，尤其是
短 profile 或存活对象较少时。

Lua 重新分配自身 VM stack 时，call-frame 指针暂时不可用。被选中的 stack-reallocation
事件仍计入内存指标，但以空 stack 保存并增加 `stack_truncations`，不会遍历无效 VM
状态。

### 3.3 内存统计项

- `allocation_events`、`reallocation_events`、`free_events`：recording 期间精确的
  allocator event 数。
- `allocation_failures`：失败 allocation/realloc 数。
- `samples`、`sampled_alloc_bytes`：原始入选 event 数和 requested bytes。
- `alloc_space`、`alloc_objects`：概率加权后的 allocation 估算。
- `inuse_space`、`inuse_objects`：停止时仍存活的加权 sampled block。
- `live_map_overflows`：未能进入 in-use tracking 的 sampled live block。
- `stack_truncations`、`aggregate_overflows`、`symbol_overflows`：质量计数器。

查看 in-use 结论前应确认 `track_free = true`、`live_map_overflows == 0`，并判断原始
`samples` 是否足够。少量大对象或短 recording 可能有明显采样方差。

## 4. 导出与 pprof

默认格式是 gzip 压缩的 Google `profile.proto`：

```lua
assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz", {
    sample = "alloc_space",
}))
```

CPU profile 包含：

- `samples/count`
- `cpu/nanoseconds`

Memory profile 包含：

- `alloc_objects/count`
- `alloc_space/bytes`
- `inuse_objects/count`
- `inuse_space/bytes`

默认 sample 对 CPU 是 `cpu`；memory 关闭 free tracking 时是 `alloc_space`，启用时是
`inuse_space`。

常用命令：

```sh
go tool pprof -top cpu.pb.gz
go tool pprof -lines -top cpu.pb.gz
go tool pprof -list=calculate_orders cpu.pb.gz
go tool pprof -sample_index=alloc_space -top heap.pb.gz
go tool pprof -sample_index=inuse_space -top heap.pb.gz
go tool pprof -sample_index=inuse_objects -top heap.pb.gz
go tool pprof -sample_index=inuse_space -svg heap.pb.gz > heap.svg
go tool pprof -http=:0 heap.pb.gz
```

`go tool pprof -svg` 生成的是调用图。若需要从已保存 `.pb.gz` 直接生成可提交、可嵌入
GitHub 的静态 SVG 火焰图，先构建本仓库工具：

```sh
make pprof-flamegraph
build/pprof-flamegraph --output cpu-flame.svg cpu.pb.gz
build/pprof-flamegraph --sample=inuse_space --output heap-flame.svg heap.pb.gz
```

工具默认使用 profile 中标记的 default sample type；memory profile 应按结论显式选择
`alloc_space`、`inuse_space`、`alloc_objects` 或 `inuse_objects`。图的宽度表示所选指标的
inclusive value，横向位置不表示时间顺序。输出不包含脚本，可由浏览器或 GitHub 直接渲染。
这条路径只读取既有 profile，不要求采集时额外调用 `result:write(..., { format = "folded" })`。
该可选工具使用 Go 1.24+ 和 `github.com/google/pprof/profile` 解析标准 profile.proto，不需要
Graphviz、Perl 或额外的 flame graph 工具。

默认 `-top` 按函数聚合，`-lines -top` 按实际执行行拆分，`-list` 把采样权重标到源码
行。Lua frame 的函数名、定义行和当前执行行是独立数据；调用名是 VM 在采样点能推断的
best-effort 名称，匿名调用无法推断时回退到 source/definition line。

Skynet 从 `integration/skynet` 加载仓库外层示例，从仓库根目录运行源码列表时需要：

```sh
go tool pprof -source_path=examples/skynet \
    -list=calculate_orders build/skynet-cpu.pb.gz
```

还可以为其他 flame graph 工具导出 root-to-leaf folded stack：

```lua
assert(memory_result:write("heap.folded", {
    format = "folded",
    sample = "inuse_space",
}))
```

## 5. 固定上限

热路径存储预先分配，recording 期间不会增长：

| 资源 | 每个 recorder 或 host 的上限 |
| --- | ---: |
| 捕获的 stack 深度 | 64 frames |
| CPU symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| Memory symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| 每个 sampled Lua function 的调用名 | 255 bytes |
| 使用 `track_free` 时的 sampled live blocks | 16384 |
| Thread 或 Skynet timer event ring | 4096 entries |
| Thread timers / Skynet targets / Skynet workers | 64 / 128 / 64 |

超过 1024 bytes 的 source name 和超过 255 bytes 的 Lua 调用名会被截断，并增加
`symbol_overflows`。达到 recording 上限后仍保持有界，对应 drop、truncation 或 overflow
计数器会增加。

导出在 stop 后执行，期间可以分配内存。导出期 Lua-visible CFunction 扫描另有 4096
function、4096 table、6 层和 255-byte 名称上限；未覆盖函数继续尝试 native symbol，
最终保留地址 fallback。

## 6. 已知限制

V1 不提供：

- native C stack unwinding；
- call/return tracing；
- allocation timeline、free-site、lifetime 或 peak profile；
- VM object graph 或 heap snapshot；
- RSS、物理内存或非 Lua allocator 的进程内存；
- Windows/macOS backend；
- 未列出 Lua 版本的 ABI 兼容保证。

将 profiler 接入已有项目时，继续阅读[项目接入指南](integration.md)。修改 fork、
submodule 或发布构建时，阅读[维护者指南](maintainer-guide.md)。
