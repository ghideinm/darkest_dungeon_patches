# Darkest Dungeon Patches

《暗黑地牢》Linux 28212 的独立原生补丁项目。源码、脚本和配置使用 snake_case，统一入口为 `manage.py`。Python 只用标准库，C11 使用 Clang 编译。

| 功能 | 必要文件 | 开关及验证范围 |
| --- | --- | --- |
| 32 MiB 资源读取 | `resource_loading_patch.c` | 默认启用；六个读取缓冲和读取容量一起更新，高清图集已实测加载 |
| 原生分辨率画布 | 同一份 C 源码 | `--native-render`；4K 场景和详情面板已实测，1024—5120 宽输入已做指令检查 |
| 按皮肤切换骨架 | `skin_rig_patch.c`、`skin_rig_map.txt` | `--rig-map`；地牢中原版 A 与 DeepSeek E 往返换肤已验证，城镇、双同职业和战斗全流程待验证 |

启动器检查游戏 SHA-256 与目标指令，只改写本次进程内存。磁盘游戏程序不改写，普通 Steam 启动不自动带补丁。

## 使用

在本目录执行：

```sh
python manage.py check
python manage.py install
python manage.py launch
```

`check` 仅检查版本并编译临时库；`install` 构建并安装配套隔离皮肤包，需要游戏已退出；`launch` 检查隔离包就绪后启用三项补丁，需要 Steam 已运行。安装不自动勾选存档中的 Mod，请启用 `DeepSeek Highwaywoman Animation Skin`。

`build_skin_package.py` 是骨架路由的必要配套工具：从角色正式发布包与本体资源构建独立命名的两组动作，避免原版和自定义骨架错配。暂存目录在安装完成后自动清理，正式发布包保持原样。

退出游戏后用 `python manage.py restore` 恢复正式皮肤包，再使用不带 `--rig-map` 的底层入口。骨架路由补丁和配套隔离包必须一起启用或撤销。

按需启用：

```sh
python launch.py --check
python launch.py --native-render
python launch.py --native-render --rig-map skin_rig_map.txt
```

资源读取默认启用。前两项共用一个库，骨架路由使用独立库；不用把三项拆成三份重复启动器。

## 路径与可选集成

- `paths.py` 集中管理默认路径；游戏默认在当前用户的 Steam 目录。
- `manage.py --game-root <目录>` 指定游戏根目录；`launch.py --game <程序>` 指定原生可执行文件。
- `--project-root <目录>` 指定角色项目，默认是同级 `Darkest Dungeon`。安装／恢复皮肤包时从其中的 `游戏模组/DeepSeek强盗` 取资源与安装器。
- `--with-mcp` 可选加载观察器与调试入口，普通游戏控制关闭。默认桥接项目为角色项目的 `直播项目/darkest-dungeon-bridge`，可用 `--bridge-root <目录>` 覆写；其 Python 环境须已就绪，HTTP／MCP 服务另行连接。
- 普通补丁检查与启动不依赖角色工程或 MCP。构建产物与运行报告位于系统临时目录，不进入源码目录。

## 兼容边界

仅支持 SHA-256 为 `fe1a629f7afa74e716cb440b14db6470d572ed244e0a2420521e521bc1102156` 的 Linux x86-64 原生程序。版本或指令不符即拒绝。

读取补丁把六个缓冲的容量总计由 36 提至 192 MiB；实际驻留内存按需分配。它不会提高 GPU 纹理上限。画布补丁跟随实际分辨率，保持原逻辑布局与输入坐标；没有增加运行中热重建机制。骨架路由仍为已验证限定场景的原型，不能视为全部 Mod 组合兼容。画面清晰度和资源加载验证不代表角色美术已获认可。

## 提交前检查

先安装系统工具 Node.js、pnpm、Clang、clang-format 和 clang-tidy，再运行：

```sh
pnpm install --frozen-lockfile
pnpm check
```

Husky 的 `pre-commit` 执行 `pnpm check`，格式或静态分析失败会阻止提交；`commit-msg` 调用 commitlint，使用 `commitlint.config.js` 的 `export default` 配置，直接继承 `@commitlint/config-conventional`。安装时 `prepare` 自动配置 Husky，`.husky/_` 由它生成，不提交。

- `pnpm format`：按 `.clang-format` 的 LLVM 默认风格整理全部 C 源码。
- `pnpm check:format`：只检查格式，不改文件。
- `pnpm check:tidy`：分析全部 C 源码及已有的 `DD_PATCH_SELFTEST` 条件编译分支，使用 C11 和 `-Wall -Wextra -Werror`；分析警告也视为错误。

`.clang-tidy` 仅启用基础 `clang-analyzer-*`。其中 Annex K 替代函数建议设为官方提供的 `actionable` 模式，仅在目标 C 库提供这些接口时报告，避免要求 Linux 补丁调用不可用的 `memcpy_s` 等函数；其他分析规则保持默认。参考 [Clang 检查器文档](https://clang.llvm.org/docs/analyzer/checkers.html#security-insecureapi-deprecatedorunsafebufferhandling)。

`node_modules` 和构建产物不提交；保留 `package.json`、`pnpm-lock.yaml`、配置与两个 Hook。提交消息示例：`fix: correct skin rig routing`。
