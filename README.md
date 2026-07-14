# luaprof

[English](READ-en.md)

`luaprof` 是面向 Lua 的 Linux 采样分析器，支持相互独立的 CPU 和内存 recorder，
并输出标准 pprof profile。当前支持固定 Lua 5.4.8、Lua 5.5.0 fork 的
thread-per-VM 宿主，以及使用定制 Lua 5.5 的固定 Skynet fork。

## 环境要求

- Linux、C11 编译器、GNU Make、POSIX threads
- zlib 开发文件
- Go `pprof`；生成 SVG 或图形化报告时还需要 Graphviz
- 能通过 HTTPS 访问 GitHub；公开 submodule 不需要 SSH key

原版 Lua 没有 `luaprof` 所需的 VM bridge。默认构建使用固定 Lua 5.4.8 fork；显式
Lua 5.5 和 Skynet target 分别使用固定 Lua 5.5.0 fork 与 Skynet 自带的定制 Lua。

## 构建与测试

```sh
git clone https://github.com/antsmallant/luaprof.git
cd luaprof
make
make test
```

Lua 5.5 使用独立构建产物和测试入口：

```sh
make test-lua55
make example-lua55
```

运行 thread-per-VM 示例：

```sh
make example-thread-vm
```

示例生成：

```text
build/thread-vm-cpu.pb.gz
build/thread-vm-heap.pb.gz
```

查看结果：

```sh
go tool pprof -top build/thread-vm-cpu.pb.gz
go tool pprof -lines -top build/thread-vm-cpu.pb.gz
go tool pprof -sample_index=inuse_space -top build/thread-vm-heap.pb.gz
```

## 基本用法

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
local cpu_result = assert(cpu:stop())

assert(memory_result:write("heap.pb.gz"))
assert(cpu_result:write("cpu.pb.gz"))
```

同一 VM 的 CPU 和 memory recorder 可以独立启动和停止。`track_free = true` 时，memory
profile 会提供停止时仍存活的 sampled allocation 估算。

## Skynet

构建、测试并运行固定 Skynet fork 的示例：

```sh
make test-skynet
make example-skynet
```

示例生成：

```text
build/skynet-cpu.pb.gz
build/skynet-heap.pb.gz
```

Skynet CPU profile 以调用 `profile.cpu.start()` 的 service 为目标，并在该 service 迁移
worker 时继续跟踪；它不是整个 Skynet 进程的合并 profile。项目接入方式见下方文档。

## 文档

- [项目接入指南](docs/integration.md)：修改自有 Lua、接入 thread-per-VM 或 Skynet。
- [采样模型与结果解读](docs/profiling-model.md)：API、CPU/内存语义、pprof、统计项与限制。
- [维护者指南](docs/maintainer-guide.md)：fork、submodule、双 Lua ABI 和发布流程。

## 许可证

本项目使用 [MIT License](LICENSE)。
