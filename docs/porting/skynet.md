# 向 Skynet 应用 luaprof patch

[接入指南](../integration.md) | [Lua 5.4](lua-5.4.md) | [Lua 5.5](lua-5.5.md)

本文只说明如何生成、应用和验证固定 Skynet fork 的 patch。该 patch 同时包含 Skynet
内嵌 Lua bridge 和 scheduler/host 集成，不能拆成互不相关的两部分使用。

## 1. 适用基线

```text
baseline: f19d160
target:   integration/skynet 当前 HEAD
bridge:   LUA_PROFILE_ABI_VERSION == 2
module:   customized Lua 5.5, LUAPROF_EXPECT_LUA_VERSION == 505
```

目标源码与 baseline 内容一致时可以直接应用。旧版或深度定制的 Skynet 只能把 patch
作为差异参考，不能视为已经验证兼容。不要使用父项目的 PUC Lua 替换 Skynet 自带的
`3rd/lua`。

## 2. 生成和应用

在 `luaprof` 根目录执行：

```sh
./scripts/generate-porting-patch.sh skynet > /tmp/luaprof-skynet.patch
git apply --stat /tmp/luaprof-skynet.patch
git -C /path/to/your-skynet apply --check /tmp/luaprof-skynet.patch
git -C /path/to/your-skynet apply /tmp/luaprof-skynet.patch
```

生成器只固定 baseline，终点始终是 `integration/skynet` 当前本地 `HEAD`。它对仓库根
目录 `-- .` 生成 binary/full-index diff，所以以后新增 embedded Lua、host 或 scheduler
修改文件时不需要维护 target hash 和文件清单。脚本不会 fetch 远端，并会拒绝包含未提交
的 tracked 修改。

目标 fork 已经修改相同路径时，先检查冲突，不要部分应用。Skynet service 会跨 worker
迁移，缺少 host 或 scheduler 部分即使能够编译，也不能得到正确的 service CPU profile。

## 3. 构建与部署边界

使用仓库固定 fork：

```sh
make skynet
```

参考 module 是 `build/skynet/luaprof.so`，必须使用同一份 `skynet/3rd/lua` header 构建。
它不能与 `build/lua55/luaprof.so` 互换。host library 必须链接进 `skynet` 主程序，并导出
host API；具体编译、链接和 `lua_cpath` 设置见[接入指南](../integration.md)。

CPU recorder 必须在目标 service 的 dispatch 内启动。profile 会跟随该 service 迁移
worker，但不是整个 Skynet 进程的合并 profile。

## 4. 验证

固定 fork 的回归入口：

```sh
make test-porting-patches
make test-skynet
make example-skynet
```

`test-porting-patches` 会确认生成结果非空，并能在当前 Skynet `HEAD` 上 reverse
apply-check。真实项目至少使用两个 worker，让目标 service 主动 yield/迁移，并确认
profile metadata 的 `scheduler_workers > 0`。同时检查 CPU/heap profile 能被标准
`go tool pprof` 读取。

## 5. patch 冲突

patch 冲突表示目标 Skynet/Lua 与验证基线存在结构差异。应直接审查完整 patch，并在目标
fork 上重新验证 embedded Lua、host API、worker 生命周期、service dispatch 和 module
ABI。本文不再维护逐文件或逐函数文字清单，因为它不能替代实际 diff，也容易随 Skynet
和内嵌 Lua 的实现变化而过期。
