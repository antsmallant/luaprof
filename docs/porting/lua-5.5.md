# 向 Lua 5.5 应用 luaprof patch

[预生成 patch](../../patches/README.md) | [接入指南](../integration.md) |
[Lua 5.4](lua-5.4.md) | [Skynet](skynet.md)

本文只说明如何应用和验证 Lua 5.5 patch。正式验证版本是 Lua 5.5.0；其他
5.5.x 或定制 fork 只能把 patch 作为差异参考，不能视为已经验证兼容。

## 1. 适用基线

```text
baseline: 0b13c63
target:   9270ea1
bridge:   LUA_PROFILE_ABI_VERSION == 2
module:   LUAPROF_EXPECT_LUA_VERSION == 505
```

PUC Lua 5.5 与 Skynet 定制 Lua 都报告版本 505，但不是同一 VM ABI。这里的 patch 和
`build/lua55/luaprof.so` 只用于 PUC Lua 5.5，不能用于 Skynet。

该 baseline 的 Lua 源码与官方 Lua 5.5.0 一致，只额外维护通用 `.gitignore`；已提交
patch 不包含 `.gitignore`。

## 2. 应用

直接使用仓库中已提交的 patch：

```sh
git apply --stat /path/to/luaprof/patches/lua-5.5.0.patch
git -C /path/to/your-lua apply --check \
    /path/to/luaprof/patches/lua-5.5.0.patch
git -C /path/to/your-lua apply \
    /path/to/luaprof/patches/lua-5.5.0.patch
```

该文件随 `luaprof` 仓库提交，不需要初始化 submodule。维护者更新方式见
[维护者指南](../maintainer-guide.md)。

目标源码与 baseline 一致时可以直接应用。目标已经修改过 Lua VM 时，先检查冲突，不要
使用 `--reject` 拼出部分 bridge；应在独立分支完成整套 patch 和验证。

## 3. 构建

使用仓库固定 fork：

```sh
make lua55
make module-lua55
```

patch 后的 Lua fork 默认关闭 bridge；父项目 target 会自动传入 `LUAPROF=1`。直接在目标
Lua 仓库构建时使用：

```sh
make clean
make linux LUAPROF=1
```

普通 `make linux` 不编译 `lprofile.c`，也不公开 profiling ABI。切换启用状态前必须 clean
rebuild，不能复用另一种模式生成的 object。

module 产物是 `build/lua55/luaprof.so`。自有项目必须用应用 patch 后的同一棵 Lua 5.5
源码构建 Lua 和 module。不要复用 `build/luaprof.so` 或 `build/skynet/luaprof.so`；三者
分别对应不同 Lua ABI。自定义路径和宿主链接参数见[接入指南](../integration.md)。

## 4. 验证

固定 fork 的回归入口：

```sh
make test-porting-patches
make test-lua55
make example-lua55
```

`test-porting-patches` 会确认已提交 patch 与当前固定 fork 完全一致，并分别通过
baseline forward 和 target reverse apply-check。自有 Lua 编译
`tests/integration/vm_bridge_test.c` 和需要直接调用
`lua_newstate` 的测试时必须定义 `LUAPROF_LUA_EXPLICIT_SEED`，然后执行 CPU、memory 和
组合采样测试。

## 5. patch 冲突

patch 冲突通常表示目标不是验证过的 Lua 5.5.0 baseline，或项目已经修改相同 VM 路径。
此时应直接比较 patch 与目标源码，并重新运行完整 bridge/采样测试。本文不再维护逐字段
或逐函数的文字版修改清单，因为它不能替代实际 diff，也容易随 Lua 内部实现变化而过期。
