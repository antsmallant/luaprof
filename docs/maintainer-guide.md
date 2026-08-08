# luaprof 维护者指南

[README](../README.md) | [项目接入](integration.md) | [采样模型](profiling-model.md)

本文面向维护 `luaprof`、Lua fork 和 Skynet fork 的开发者。普通使用者只需要 README
和项目接入指南。

## 1. 仓库与构建边界

父仓库固定四个直接 submodule：

| Path | Remote branch | 用途 |
| --- | --- | --- |
| `3rd/lua-5.4.6` | `lua-5.4.6.git` 的 `luaprof` | thread-per-VM Lua 5.4.6 |
| `3rd/lua-5.4.8` | `lua-5.4.8.git` 的 `luaprof` | thread-per-VM Lua 5.4.8 |
| `3rd/lua-5.5.0` | `lua-5.5.0.git` 的 `luaprof` | thread-per-VM Lua 5.5.0 |
| `integration/skynet` | `skynet.git` 的 `luaprof` | Skynet host 及其定制 Lua 5.5 |

`.gitmodules` 的 fetch URL 必须保持公开 HTTPS。`branch = luaprof` 只供维护者执行
`git submodule update --remote`；普通 `make`、CI 和使用者 checkout 都按照父仓库 gitlink
固定准确 commit，不自动跟随远端分支。

四个 Lua build boundary 使用不同 module：

```text
build/lua46/luaprof.so    PUC Lua 5.4.6
build/luaprof.so          PUC Lua 5.4.8
build/lua55/luaprof.so    PUC Lua 5.5.0
build/skynet/luaprof.so   Skynet customized Lua 5.5
```

构建时 Lua 5.4.6/5.4.8 定义 `LUAPROF_EXPECT_LUA_VERSION=504`，PUC Lua 5.5 与
Skynet Lua 定义 `505`。即使版本号相同，不同源码树仍不能互换 module，也不能把另一份
`liblua.a` 静态链接进 module。

当前 ABI contract：

- Lua VM bridge：`LUA_PROFILE_ABI_VERSION == 2`
- Skynet host API：`LP_SKYNET_HOST_ABI_VERSION == 2`

`include/luaprof/runtime.h` 是 module 内部组件和仓库测试共享的源码接口，不是受支持的外部
C API，也不承诺跨版本 source/ABI 兼容。依赖它的 object 必须从同一个 checkout 整体重编译；
修改其中的结构或函数签名不单独升级上述两个 ABI version，但必须记录不兼容变化并确认没有
把它误作为外部接入点。ELF module 因默认符号可见性而出现 `lp_runtime_*`、`lp_result_*`
dynamic symbol，不构成兼容性承诺。

四个 fork 的 profiling 功能默认关闭。Lua fork 使用 `LUAPROF=1` 定义
`LUA_USE_LUAPROF`；Skynet 的同名 Make 变量还会启用 `SKYNET_LUAPROF` 并链接 host
library。父仓库所有 VM/module target 都显式启用，不依赖 fork 的默认值。修改 feature gate
时必须同时验证普通 macro-off 构建和父项目 macro-on 构建；切换模式前要 clean rebuild。

修改公开结构、callback 语义或 symbol contract 时，必须评估并更新 ABI version、三棵
PUC Lua fork、Skynet 内嵌 Lua、module 和 contract test。

## 2. 公开 checkout

使用者和 CI 应通过 HTTPS clone：

```sh
git clone https://github.com/antsmallant/luaprof.git
cd luaprof
git submodule update --init \
    3rd/lua-5.4.6 3rd/lua-5.4.8 3rd/lua-5.5.0 integration/skynet
```

`make` 默认只初始化 Lua 5.4.8 submodule。其他版本和 Skynet 只由各自显式 target
初始化，避免普通构建下载无关依赖。

变更 `.gitmodules` 后执行：

```sh
git submodule sync -- \
    3rd/lua-5.4.6 3rd/lua-5.4.8 3rd/lua-5.5.0 integration/skynet
```

提交前应在干净目录从公开 URL 重新 clone 并执行一次 direct-submodule 初始化，防止
本机 `.git/config` 或 URL rewrite 掩盖公开配置错误。

## 3. 独立 fork 开发

不要把父仓库中的 submodule checkout 当作长期开发目录。建议在父仓库旁建立独立
checkout：

```sh
git clone --branch luaprof \
    https://github.com/antsmallant/lua-5.4.6.git ../lua-5.4.6
git clone --branch luaprof \
    https://github.com/antsmallant/lua-5.4.8.git ../lua-5.4.8
git clone --branch luaprof \
    https://github.com/antsmallant/lua-5.5.0.git ../lua-5.5.0
git clone --branch luaprof \
    https://github.com/antsmallant/skynet.git ../skynet
```

需要通过 SSH 推送时，只修改独立 checkout 的 push URL，不改变公开 fetch URL：

```sh
git -C ../lua-5.4.6 remote set-url --push origin \
    git@github.com:antsmallant/lua-5.4.6.git
git -C ../lua-5.4.8 remote set-url --push origin \
    git@github.com:antsmallant/lua-5.4.8.git
git -C ../lua-5.5.0 remote set-url --push origin \
    git@github.com:antsmallant/lua-5.5.0.git
git -C ../skynet remote set-url --push origin \
    git@github.com:antsmallant/skynet.git
```

四个公开 `luaprof` 分支都保持同一历史形态：对应 `origin/master` 加一个 luaprof
提交。Lua fork 的提交 message 为 `feat: luaprof VM bridge`，Skynet 为
`feat: luaprof integration`。本地开发可以使用临时提交，但发布前必须压回单个提交，不能
把实现过程中的修复提交逐个留在公开 fork 历史中。

fork 的标准流程：

1. fetch 远端，确认 `origin/master` 仍是生成 patch 使用的固定 baseline。
2. 在独立 checkout 的 `luaprof` 分支修改并运行对应 build/test。
3. 确保最终源码已提交且工作区干净，然后执行 `git reset --soft origin/master`。
4. 使用约定 message 创建新的单个提交，并确认
   `git rev-list --count origin/master..luaprof` 输出 `1`。
5. 执行 `git push --force-with-lease origin luaprof`，确认远端能 fetch 新 commit。
6. 立即回到父仓库更新 gitlink 并完成回归，避免父仓库继续引用已替换的旧 commit。

重写公开分支是有意的发布流程，因此必须使用 `--force-with-lease`，不能使用无保护的
`--force`。先推送 fork、再更新父仓库非常重要；否则父仓库会引用外部无法获取的 commit。

## 4. 更新父仓库 gitlink

Lua 5.4.6 fork：

```sh
git submodule update --remote 3rd/lua-5.4.6
git diff --submodule=log -- 3rd/lua-5.4.6
git add 3rd/lua-5.4.6
make test-lua46
git commit -m "build: update Lua 5.4.6 submodule"
```

默认 Lua 5.4.8 fork：

```sh
git submodule update --remote 3rd/lua-5.4.8
git diff --submodule=log -- 3rd/lua-5.4.8
git add 3rd/lua-5.4.8
make test
git commit -m "build: update Lua submodule"
```

Lua 5.5 fork 使用相同流程：

```sh
git submodule update --remote 3rd/lua-5.5.0
git diff --submodule=log -- 3rd/lua-5.5.0
git add 3rd/lua-5.5.0
make test-lua55
git commit -m "build: update Lua 5.5 submodule"
```

Skynet fork：

```sh
git submodule update --remote integration/skynet
git diff --submodule=log -- integration/skynet
git add integration/skynet
make test-skynet
git commit -m "build: update Skynet submodule"
```

`git submodule update --remote` 应只改变 gitlink。若 submodule 内出现未提交文件，先判断
来源，不要用 reset/checkout 删除其他人的修改。

更新后检查：

```sh
git submodule status
git status --short
```

父仓库提交并 push 后，再确认 `HEAD` 与 `origin/master` 一致。

## 5. 更新 Lua bridge 与 patch

生成和应用入口见 [Lua 5.4](porting/lua-5.4.md)、[Lua 5.5](porting/lua-5.5.md) 和
[Skynet](porting/skynet.md)。公开交付物位于 [`patches/`](../patches/README.md)，
实现差异以已提交 patch 和固定 fork 源码为准，不另外维护逐文件或逐函数文字清单。

不要只修改其中一个 PUC Lua fork。若 contract 也适用于 Skynet，必须将等效改动移植到
Skynet 自带的 `3rd/lua`，并分别运行对应 bridge test。

### 生成移植 patch

提交 fork 并更新父仓库 gitlink 后，一次性更新四份对外 patch：

```sh
make update-porting-patches
make test-porting-patches
make test-feature-gates
```

`update-porting-patches.sh` 先在临时目录生成全部文件，四份都成功后才替换 `patches/`
中的发布产物。不要手工编辑 patch。gitlink 变化时还必须同步更新
`patches/README.md` 的完整 baseline/target commit；测试会检查 target 元数据和已提交
patch 是否与当前 fork 字节一致，并分别执行 baseline forward 和 target reverse
apply-check。

需要调试单一目标时，可以把底层生成器输出到临时文件；不要直接覆盖发布目录：

```sh
./scripts/generate-porting-patch.sh lua46 > /tmp/luaprof-lua46.patch
```

脚本中的 baseline 对应固定 master。Lua fork 的 master 可以包含 `.gitignore` 这类
非 profiler 仓库维护，但 Lua 源码必须保持对应官方版本；只有明确更换或 rebase 基线时
才修改。target 始终是相应 submodule 的 `HEAD`，不写死 commit。diff 范围是仓库根目录
`-- .`，后续新增修改文件也会自动进入 patch。脚本检测到未提交的 tracked 修改会失败，
应先在独立 fork 完成 commit/push，再更新父仓库 gitlink。脚本不执行 fetch；这里的
`HEAD` 就是 submodule 当前检出的 commit，不等同于远端分支自动最新值。

### Skynet Lua 的约束

Skynet profiling 修改属于 Skynet fork 本身。不要用父项目中的任何 PUC Lua fork 替换
`integration/skynet/3rd/lua`，也不要向 Skynet build 传入父项目 `LUA_INC`/`LUA_LIB`。

移植时必须保留：

- 带 seed 的 `lua_newstate`；
- `lua_sharefunction`、shared Proto/table 和 code cache；
- Skynet 自己的 VM ABI 和 coroutine/GC 行为；
- `onelua.c` 中的相应 source inclusion。

## 6. Skynet host 集成

Skynet fork 除 embedded Lua bridge 外，还维护：

- `skynet-src/skynet_start.c` 的 worker start/stop hook；
- `skynet-src/skynet_server.c` 的 dispatch enter/leave hook；
- 根 Makefile 对 `libluaprof-skynet-host.a` 的依赖和链接；
- 主程序 `-Wl,-E` dynamic symbol export。

修改 scheduler 边界后至少验证 service migration、并发 target、stale tick、worker
shutdown 和 active-runtime destruction。host 与 module 的 symbol 边界检查位于
`tests/integration/skynet_lua_boundary.sh`。

## 7. 测试矩阵

普通 Lua：

```sh
make test-lua46
make example-lua46
make bench-lua46-vm
make bench-lua46-combined

make test
make example-thread-vm
make bench-vm
make bench-combined

make test-lua55
make example-lua55
make bench-lua55-vm
make bench-lua55-combined
```

Skynet：

```sh
make test-skynet
make example-skynet
make bench-skynet-vm
make bench-skynet-combined

make test-porting-patches
```

只修改文档或 submodule URL 时，至少执行 `git diff --check`、公开 HTTPS clone 和 direct
submodule 初始化。修改 bridge、host、采样或 exporter 时，应运行完整对应矩阵。

示例不是只检查文件生成。还应使用 pprof 查看函数、行号、CFunction 名称和 memory
sample index，并检查 drop、truncation 和 overflow 计数。

## 8. 发布检查

发布或对外更新前确认：

- README 保持面向首次使用，不放入本机路径和内部工作流。
- `.gitmodules` 只使用公开 HTTPS fetch URL。
- 四个 gitlink commit 已经推送到各自 `luaprof` 分支。
- `patches/` 的四份发布文件与当前 gitlink 一致，baseline/target 元数据准确。
- README 和 docs 入口有效。
- 四个 module 使用正确 Lua ABI，且没有静态包含另一份 Lua。
- `make test`、`make test-lua46`、`make test-lua55` 和 `make test-skynet` 通过。
- thread-per-VM 与 Skynet 示例能生成可由标准 `go tool pprof` 读取的结果。
- 父仓库和四个 fork 没有维护者遗留的未提交修改。

本机 workspace 路径、独立 checkout 位置和 agent 协作约定属于内部开发信息，应记录在
开发文档或 `AGENTS.md`，不写入公开 README。
