# 向 Skynet 移植 luaprof

[接入指南](../integration.md) | [Lua 5.4](lua-5.4.md) | [Lua 5.5](lua-5.5.md)

本文给出 Skynet 内嵌 Lua bridge 和 scheduler hook 的函数级修改位置。正式验证版本是
项目固定的 Skynet fork。profile 的目标是调用 `profile.cpu.start()` 的 service；它在
service 迁移 worker 后继续跟踪，但不会把整个 Skynet 进程合并成一份 profile。

## 1. Reference diff

```text
baseline: f19d160
bridge:   0af0699
Lua ABI:  customized Lua 5.5, LUA_PROFILE_ABI_VERSION == 2
```

查看完整修改：

```sh
git -C integration/skynet diff f19d160..0af0699 -- \
    3rd/lua Makefile skynet-src/skynet_server.c skynet-src/skynet_start.c
```

该 diff 包含两部分：内嵌 Lua 的 VM bridge，以及 Skynet host/scheduler 集成。目标 fork
不是相同基线时必须分别按函数移植，不能用父项目的 PUC Lua 5.5 替换 `3rd/lua`。

## 2. 内嵌 Lua bridge

在 `3rd/lua` 中移植 [Lua 5.5 bridge](lua-5.5.md) 的 contract 与 guard 语义，同时保留
Skynet 已有差异：

- 带显式 seed 的 `lua_newstate`；
- `lua_sharetable`、code cache、coroutine 和 GC 改动；
- `onelua.c` amalgamated build；
- Skynet 自己的 makefile 和 dependency 布局。

准确修改文件是：

```text
ldebug.c/h, ldo.c, lgc.c, lmem.c, lstate.c/h, lua.h, lvm.c,
新增 lprofile.c/h, makefile, onelua.c
```

`3rd/lua/makefile` 把 `lprofile.o` 加入 `CORE_O` 并补全依赖；`onelua.c` 在 VM core
区域包含 `lprofile.c`。不要把 profiler 数据塞进 `lua_Alloc.ud`。

## 3. 链入 host library

使用 Skynet 内嵌 Lua 的 header 编译 `src/skynet_host.c`，得到
`libluaprof-skynet-host.a`。在 Skynet 根 `Makefile`：

1. 增加可覆盖变量 `LUAPROF_HOST_LIB ?=`；
2. 把该 archive 加入 `skynet` 可执行文件的 prerequisites 和 link inputs；
3. 编译 scheduler hook 时定义 `SKYNET_LUAPROF` 并加入 luaprof include 路径；
4. 主程序使用 `-Wl,-E`/`--export-dynamic`，使 module 能通过 `dlsym` 找到
   `lp_skynet_host_get_api`。

host library 必须进入主程序，不是 Lua service module。`luaprof.so` 通过
`dlsym(RTLD_DEFAULT, "lp_skynet_host_get_api")` 自动选择 Skynet backend。

参考 diff 还把 `$(LUA_STATICLIB)` 加入各 C/Lua module 的 build prerequisites，并在
link 命令中过滤为 `%.c`。这样初次并行构建会先生成内嵌 Lua library，但不会把 archive
错误链进每个 `.so`。

## 4. Worker 生命周期 hook

### `skynet-src/skynet_start.c`：`thread_worker`

在 `skynet_initthread(THREAD_WORKER)` 和 `skynet_handle_register_thread()` 完成后、进入
dispatch loop 前调用：

```c
lp_skynet_host_worker_start((unsigned int)id);
```

在 worker 离开 loop 后、函数返回前调用：

```c
lp_skynet_host_worker_stop();
```

两个调用都由 `#ifdef SKYNET_LUAPROF` 保护。每个 worker 只注册一次；不要按 service
创建 timer。

## 5. Service dispatch hook

### `skynet-src/skynet_server.c`：`dispatch_message`

在增加 `message_count` 后、调用任何 `ctx->cb` 分支前进入当前 service：

```c
lp_skynet_host_dispatch_enter(ctx->handle);
```

所有 callback 分支完成后立即离开：

```c
lp_skynet_host_dispatch_leave();
```

enter/leave 同样由 `SKYNET_LUAPROF` 保护。`leave` 必须覆盖 `ctx->profile` 为真和为假的
两个分支，并在释放 message 或开始下一个 dispatch 前完成。handle 是 CPU 样本归属
service 的依据，不能以 `lua_State *` 替代这个调度边界。

## 6. 构建 ABI 对应的 module

`luaprof.so` 必须使用同一份 `skynet/3rd/lua` header 编译，并定义：

```text
LUAPROF_EXPECT_LUA_VERSION=505
LUAPROF_LUA_EXPLICIT_SEED
```

参考产物是 `build/skynet/luaprof.so`。它与 `build/lua55/luaprof.so` 都报告 Lua 5.5，
但 ABI 不同，不能互换。启动 CPU recorder 时必须位于目标 service 的 dispatch 内，
否则当前 handle 为零，Skynet backend 会拒绝启动。

## 7. 验证

```sh
make test-skynet
make example-skynet
```

至少检查：

- `lua_profile_capturestack` 由内嵌 Lua 和最终 `skynet` 定义；
- `lp_skynet_host_get_api` 由主程序动态导出；
- module 中 `lua_profile_capturestack` 是未定义符号 `U`；
- 使用两个以上 worker，让目标 service 主动 yield/迁移；
- profile metadata 的 `scheduler_workers > 0`，且只含目标 service 的 dispatch CPU。

具体命令、部署目录和故障诊断见[接入指南](../integration.md)。
