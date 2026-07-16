# 向 Lua 5.4 应用 luaprof patch

[接入指南](../integration.md) | [Lua 5.5](lua-5.5.md) | [Skynet](skynet.md)

本文只说明如何生成、应用和验证 Lua 5.4 patch。正式验证版本是 Lua 5.4.6 和
Lua 5.4.8；其他 5.4.x 或定制 fork 只能把相近版本的 patch 作为差异参考，不能视为
已经验证兼容。

## 1. 适用基线

| 版本 | 生成器参数 | Baseline | Target |
| --- | --- | --- | --- |
| Lua 5.4.6 | `lua46` | `0858f40` | `3rd/lua-5.4.6` 当前 `HEAD` |
| Lua 5.4.8 | `lua54` | `ef6de10` | `3rd/lua-5.4.8` 当前 `HEAD` |

两者都使用 `LUA_PROFILE_ABI_VERSION == 2`，module 都定义
`LUAPROF_EXPECT_LUA_VERSION == 504`。

两个 baseline 分别保持对应 Lua 版本的源码，只额外维护通用 `.gitignore`；生成的 patch
不包含 `.gitignore`。目标已经修改过 Lua VM 时，必须先执行 dry-run；失败表示需要人工
审查冲突。

## 2. 生成和应用

在 `luaprof` 根目录选择与目标版本一致的命令。Lua 5.4.6：

```sh
./scripts/generate-porting-patch.sh lua46 > /tmp/luaprof-lua46.patch
git apply --stat /tmp/luaprof-lua46.patch
git -C /path/to/your-lua apply --check /tmp/luaprof-lua46.patch
git -C /path/to/your-lua apply /tmp/luaprof-lua46.patch
```

Lua 5.4.8：

```sh
./scripts/generate-porting-patch.sh lua54 > /tmp/luaprof-lua54.patch
git apply --stat /tmp/luaprof-lua54.patch
git -C /path/to/your-lua apply --check /tmp/luaprof-lua54.patch
git -C /path/to/your-lua apply /tmp/luaprof-lua54.patch
```

生成器只固定相应 baseline，终点始终是对应 submodule 当前本地 `HEAD`。它对仓库根目录
`-- .` 生成 binary/full-index diff，因此以后 bridge commit 或修改文件变化时不需要更新
target hash 和文件清单。脚本不会 fetch 远端，并会拒绝包含未提交的 tracked 修改。不要
把 5.4.6 patch 直接当作 5.4.8 patch 使用，反之亦然。

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

`test-porting-patches` 会确认两份生成结果非空，并能在对应 Lua 5.4 `HEAD` 上 reverse
apply-check。`test-lua46` 与 `test` 分别运行 5.4.6 和 5.4.8 的完整 thread-per-VM
回归。自有 Lua 至少还应使用其 header 和 library 编译、运行
`tests/integration/vm_bridge_test.c`，再执行 CPU、memory 和组合采样测试。

## 5. patch 冲突

patch 冲突通常表示目标不是匹配的 Lua 5.4.6/5.4.8 baseline，或项目已经修改相同 VM
路径。此时应直接比较 patch 与目标源码，并重新运行完整 bridge/采样测试。本文不再维护
逐字段或逐函数的文字版修改清单，因为它不能替代实际 diff，也容易随 Lua 内部实现变化
而过期。
