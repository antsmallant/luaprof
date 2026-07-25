# 向 Lua 5.4 应用 luaprof patch

[预生成 patch](../../patches/README.md) | [接入指南](../integration.md) |
[Lua 5.5](lua-5.5.md) | [Skynet](skynet.md)

本文只说明如何应用和验证 Lua 5.4 patch。正式验证版本是 Lua 5.4.6 和
Lua 5.4.8；其他 5.4.x 或定制 fork 只能把相近版本的 patch 作为差异参考，不能视为
已经验证兼容。

## 1. 适用基线

| 版本 | Patch | Baseline | Target |
| --- | --- | --- | --- |
| Lua 5.4.6 | `lua-5.4.6.patch` | `0858f40` | `0119ebc` |
| Lua 5.4.8 | `lua-5.4.8.patch` | `ef6de10` | `43225b3` |

两者都使用 `LUA_PROFILE_ABI_VERSION == 2`，module 都定义
`LUAPROF_EXPECT_LUA_VERSION == 504`。

两个 baseline 分别保持对应 Lua 版本的源码，只额外维护通用 `.gitignore`；已提交 patch
不包含 `.gitignore`。目标已经修改过 Lua VM 时，必须先执行 dry-run；失败表示需要人工
审查冲突。

## 2. 应用

选择与目标版本一致的已提交文件。Lua 5.4.6：

```sh
git apply --stat /path/to/luaprof/patches/lua-5.4.6.patch
git -C /path/to/your-lua apply --check \
    /path/to/luaprof/patches/lua-5.4.6.patch
git -C /path/to/your-lua apply \
    /path/to/luaprof/patches/lua-5.4.6.patch
```

Lua 5.4.8：

```sh
git apply --stat /path/to/luaprof/patches/lua-5.4.8.patch
git -C /path/to/your-lua apply --check \
    /path/to/luaprof/patches/lua-5.4.8.patch
git -C /path/to/your-lua apply \
    /path/to/luaprof/patches/lua-5.4.8.patch
```

两份 patch 都随 `luaprof` 仓库提交，不需要初始化 submodule。不要把 5.4.6 patch 直接
当作 5.4.8 patch 使用，反之亦然。维护者更新方式见[维护者指南](../maintainer-guide.md)。

正式 apply 前应在目标项目创建独立分支，并保留 `--check` 结果。不要跳过检查后直接使用
`--reject` 部分应用；一套不完整的 VM bridge 可能正常编译，但在采样时产生错误状态或
崩溃。

## 3. 构建

使用仓库固定 Lua 5.4.6 fork：

```sh
make lua46
make module-lua46
```

使用仓库固定 Lua 5.4.8 fork：

```sh
make lua
make module
```

patch 后的 Lua fork 默认关闭 bridge；父项目 target 会自动传入 `LUAPROF=1`。直接在目标
Lua 仓库构建时使用：

```sh
make clean
make linux LUAPROF=1
```

普通 `make linux` 不编译 `lprofile.c`，也不公开 profiling ABI。切换启用状态前必须 clean
rebuild，不能复用另一种模式生成的 object。

Lua 5.4.6 module 产物是 `build/lua46/luaprof.so`，Lua 5.4.8 module 产物是
`build/luaprof.so`。自有项目必须用应用 patch 后的同一棵 Lua 源码构建 Lua 和 module；
不能只替换 header，也不能让 module 静态包含第二份 Lua。自定义路径和宿主链接参数见
[接入指南](../integration.md)。

## 4. 验证

固定 fork 的回归入口：

```sh
make test-porting-patches
make test-lua46
make example-lua46
make test
make example-thread-vm
```

`test-porting-patches` 会确认两份已提交 patch 与当前固定 fork 完全一致，并分别通过
baseline forward 和 target reverse apply-check。`test-lua46` 与 `test` 分别运行
5.4.6 和 5.4.8 的完整 thread-per-VM 回归。自有 Lua 至少还应使用其 header 和 library
编译、运行
`tests/integration/vm_bridge_test.c`，再执行 CPU、memory 和组合采样测试。

## 5. patch 冲突

patch 冲突通常表示目标不是匹配的 Lua 5.4.6/5.4.8 baseline，或项目已经修改相同 VM
路径。此时应直接比较 patch 与目标源码，并重新运行完整 bridge/采样测试。本文不再维护
逐字段或逐函数的文字版修改清单，因为它不能替代实际 diff，也容易随 Lua 内部实现变化
而过期。
