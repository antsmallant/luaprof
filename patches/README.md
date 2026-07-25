# luaprof porting patches

本目录中的 patch 是 `luaprof` 的正式发布产物。使用者只需要 clone 本仓库，不必初始化
submodule，也可以把 VM bridge 和宿主集成应用到对应源码。

| Patch | 验证基线 | luaprof target |
| --- | --- | --- |
| `lua-5.4.6.patch` | `0858f40a3a6e171445a90d60cead721a0007a33d` | `0119ebc30b7827930c4a96ede96c111b52d20183` |
| `lua-5.4.8.patch` | `ef6de10bbcfe86f6bc113b8ca2d241eef3cb049b` | `43225b31438d0d003fdc32f326d3a95993f7ad07` |
| `lua-5.5.0.patch` | `0b13c63981247b5753070caddaad785c0ec840f3` | `9270ea15f5f37873c9394d5ea33b65f88249f704` |
| `skynet.patch` | `f19d1605b4b313c27d9931582c3153313a571492` | `5606145d4cd6e111ee258cf893e395c9ce586923` |

Lua patch 只包含 VM bridge，不包含 fork 的 `.gitignore`。`skynet.patch` 是一个整体，
同时包含 Skynet 内嵌 Lua bridge、worker/dispatch hook 和构建集成，不能只应用其中一部分。

## 应用

先选择与目标源码版本一致的文件，并在独立分支执行检查。例如 Lua 5.4.6：

```sh
git -C /path/to/lua apply --check \
    /path/to/luaprof/patches/lua-5.4.6.patch
git -C /path/to/lua apply \
    /path/to/luaprof/patches/lua-5.4.6.patch
```

Skynet：

```sh
git -C /path/to/skynet apply --check \
    /path/to/luaprof/patches/skynet.patch
git -C /path/to/skynet apply \
    /path/to/luaprof/patches/skynet.patch
```

目标源码与验证基线不同或已经修改过相同 VM/scheduler 路径时，应把 patch 作为完整差异
参考，审查冲突并重新运行对应验证。不要使用 `--reject` 拼出部分 bridge。具体构建和测试
要求见 [Lua 5.4](../docs/porting/lua-5.4.md)、[Lua 5.5](../docs/porting/lua-5.5.md)和
[Skynet](../docs/porting/skynet.md)。

## 维护

这些文件由父仓库固定 submodule gitlink 生成，不应手工编辑。维护者更新 fork 和 gitlink
后执行：

```sh
make update-porting-patches
make test-porting-patches
```

测试会逐份重生成 patch，并要求结果与已提交文件字节一致，同时在 baseline 上执行
forward apply-check、在 target 上执行 reverse apply-check。生成器固定 baseline，
target 始终是对应 submodule 当前检出的 `HEAD`。
