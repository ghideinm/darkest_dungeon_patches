# Darkest Dungeon Patches

《暗黑地牢》Linux 28212 的独立原生补丁项目，附 Windows x64 迁移测试版。统一开发入口为 `manage.py`，Python 只用标准库，C11 使用 Clang 编译。

| 功能 | 入口与范围 |
| --- | --- |
| 32 MiB 资源读取 | 默认启用；六个缓冲及读取容量同步更新 |
| 原生分辨率画布 | `--native-render`；跟随实际画布，保留逻辑布局与输入坐标 |
| 按皮肤切换骨架 | `--runtime-rig`；按当前皮肤的实际文件来源发现骨架，覆盖角色本体与详情预览 |

启动器校验游戏 SHA-256、全部改写点与运行时接口指令。补丁只修改本次进程内存，磁盘游戏程序保持原样。可一次性接入 Steam，以后直接点击“开始游戏”加载三项补丁。运行时来源模式已通过离线验证，游戏画面、换肤和战斗全流程待实机验证。

## 使用

游戏和所需原始 Mod 已正常安装、启用后，在本目录执行：

```sh
python manage.py check
python manage.py launch
```

`check` 核对游戏、补丁位置和运行时接口，并编译临时库，不启动游戏、不检查游戏内加载状态。`launch` 默认启用三项补丁，需要 Steam 已运行、游戏已退出；不读取皮肤配置、不生成路由表，也不要求原始发布包仍在本机。

底层入口可按需启用：

```sh
python launch.py --check --native-render --runtime-rig
python launch.py --native-render --runtime-rig
python launch.py --native-render
```

`manage.py --game-root <目录>` 指定游戏根目录；`launch.py --game <程序>` 指定原生可执行文件。最后一条仅启用读取与画布补丁。

## Windows x64 迁移测试版

仅适配 Steam Windows x64、SHA-256 为 `4d78fbfaa65b7d002d2208d02da809200f4a5723119521180e30100d6f1a75f2` 的 EXE。32 位版、大内存补丁改写过的 EXE 和其他版本均拒绝加载。Windows 原版有 12 个 16 MiB 读取缓冲，本补丁同步改写分配与两处读取容量为 32 MiB，总容量上限为 384 MiB。

```sh
python manage.py windows_check --game-root /path/to/windows/DarkestDungeon
python manage.py windows_build --game-root /path/to/windows/DarkestDungeon --windows-cc /path/to/x86_64-w64-mingw32-clang
```

构建输出 EXE／DLL 与启动脚本到系统临时目录。Windows 中将这些文件保持在同一目录，保持 Steam 已运行、游戏已正常退出，再双击 `windows_start.cmd`。脚本核对原 EXE，将补丁复制到当前用户的 `%LOCALAPPDATA%\DarkestDungeonPatches\windows_x64\<version>`，运行原生检查并启动；玩家无需 Python 或编译器。可向 CMD 传 `-GameRoot <目录>` 指定游戏，`-ResourcesOnly` 隔离测试读取补丁，`-NativeRender` 启用已定位的七处画布补丁。

启动器创建自己的调试子进程，经过系统加载断点后停在经核对的 EXE 入口，恢复入口原字节并校验全部目标指令；改写本次内存，加载骨架 DLL，确认 DLL 初始化成功后脱离调试并恢复主线程。不会把 DLL 注入既有进程，不修改磁盘游戏程序，也不跳过游戏断言。该入口避免在系统加载锁仍被持有时等待另一线程加载 DLL。

骨架发现与来源缓存使用 Linux 的共同代码；Windows 适配其存储设备、路径表、原生句柄、角色入口和详情预览。换肤转移原生事件容器的所有权，保留游戏创建的角色回调。支持按实际来源发现资源，不依赖打包路由表。该版本仍需在 Windows 游戏中验证换肤、战斗和画布表现；原生启动成功与 DLL 就绪不等于画面通过。当前没有写入 Windows Steam 自动启动配置。

启动日志在补丁版本目录的 `windows_launch.log`；部署脚本在测试期间最多十分钟将日志复制回交付目录，并写入 `windows_patch_status.json`。关闭控制台会结束日志回传，游戏可继续运行。`DD_WINDOWS_GAME=/path/to/Darkest.exe python -m unittest -v test_windows_port` 校验身份并在模拟设备上隔离执行原 Windows `IO_GetFileInfo` 指令，不执行游戏入口或存档代码。

提前退出时运行 `windows_diagnose.cmd`：加载后继续观察本次子进程二十秒，记录异常地址、寄存器、栈和退出码；异常仍交给原系统处理，不跳过断言。`windows_resources_only.cmd` 用同样的诊断只测试读取补丁。脚本还会回传游戏根目录的 `app.log` 为 `windows_game_app.log`。Windows 实机首轮曾在补丁初始化后提前退出，原因仍在定位；当前不能视为实机验收通过。

## Steam 自动加载

正常退出游戏和 Steam 后，一次性执行：

```sh
python manage.py steam_install
```

随后重新打开 Steam，照常点击《暗黑地牢》的“开始游戏”。安装器只为最近登录的 Steam 账号、AppID `262060` 接入启动包装器；已有启动选项会保留。可用 `--steam-root`、`--steam-user` 指定客户端和 userdata 数字账号 ID。安装、更新和撤销时要求 Steam 已退出，避免客户端覆盖配置。

默认固定安装到 `${XDG_DATA_HOME:-~/.local/share}/darkest_dungeon_patches`，可用 `--install-root` 指定；该路径不能包含空白或冒号。安装器预编译两个 SO 和 C11 原生启动器，用版本目录与 `current` 链接切换；日常启动不依赖 Python、Clang、源码目录或 `/tmp` 缓存，需系统提供 coreutils 的 `sha256sum`。每次启动仍校验整个游戏 SHA-256 和全部 31 处原指令，校验失败以 125 拒绝启动。

包装器保留 Steam 的原启动命令、运行时包装和参数，将游戏入口选择为已校验的 Linux x86-64 程序，再设置 `LD_PRELOAD` 与三项补丁开关。其他预加载库继续保留，旧 `DD_RIG_MAP` 清除；重复预加载本项目补丁会被拒绝。Steam 游戏入口仍需为原生 Linux 版本。直接绕过 Steam 执行原游戏文件不自动加载补丁。

```sh
python manage.py steam_status
python manage.py steam_uninstall
```

`steam_status` 检查配置及已安装原生启动器；`steam_uninstall` 在 Steam 退出后恢复原启动选项，保留安装产物供排查。安装后若手动修改了启动选项，撤销或更新会拒绝覆盖。更新源码后重新运行 `steam_install` 才会更新固定补丁；游戏版本不符时不会自动跳过校验。

启动及补丁日志默认写到 `${XDG_STATE_HOME:-~/.local/state}/darkest_dungeon_patches/steam_launch.log`，每次启动覆盖；与游戏自己的日志独立。

## 运行时骨架来源

1. 读取原生 `ActorClass` 的皮肤索引与原始路径。通过原生设备、备用根目录和 archive 的目录枚举，还原每个条目的实际来源，保持原生顺序和去重规则，并逐项核对原始路径。原版 A～D、Mod A～D 和另一个 Mod 的 E 是独立条目；目录中重复的分隔符在来源身份保留后处理。
2. 在选中条目的来源内发现贴图。缺少的文件只从本体和官方 `dlc/` 目录补齐，不借用其他用户 Mod 的同名文件；随后以原生存储层的 `>` 明确路径查询，防止再被全局 Mod 覆盖。
3. 对每张贴图，在该来源中优先找 `heroes/<hero>/<skin>/anim/*.skel`，再找同一来源的 `heroes/<hero>/anim/*.skel`，最后查本体；读取候选骨架的同名图集，只选择实际引用该贴图的骨架。不同动作可以分别来自本体、官方 DLC 或选中的 Mod，再组合为一个精灵实例。
4. 骨架缓存记录来源、皮肤、全部骨架 Key 及对应贴图目录。骨架和贴图 Key 均带 `>` 与完整来源路径；通过原生工厂和精灵构造函数加载，临时骨架句柄由原生接口释放。
5. 换肤前核对动画状态和回调；重建实例后保留动画名、时间和原生脚步事件回调。角色与预览刷新向贴图入口传递本次条目的来源上下文，再逐个原生 `PerSpineDataState` 指定贴图目录，继续由原生函数管理 Texture 句柄，避免骨架来源正确但贴图仍被全局覆盖。非英雄角色和空指针重置保留原逻辑。

这里的索引是进程内的发现缓存，不依赖打包产生的文件。来源缓存签名包括原生皮肤表与备用根目录；无法逐项对应原生顺序时拒绝路由。每次选择仍检查所选来源的实际资源；不支持在运行中修改骨架内容的热重载。

本版本只发现磁盘上的 `.skel` 与同名 `.atlas`，支持标准 PNG/TGA 皮肤目录。损坏的本地骨架不会静默回退；同名骨架被图集的不同页面同时选中却指向不同来源、资源缓存命中其他来源、目标缺少当前动画或角色正忙时，会拒绝换肤并保留旧状态。未被候选图集引用的贴图不参与选组。首次创建角色时若无法确定或加载骨架，会以 125 退出并输出 `DD_RIG_SOURCE_REFUSED` 等诊断。

内部最多缓存 128 个皮肤资源组合，每组最多 256 个骨架；原生资源键需短于 256 字节。实例匹配会核对全部骨架来源。旧打包器使用的 `.rig_vanilla.` 与 `.sprite.rig_` 名称保留给隔离副本，运行时发现会忽略它们。非标准目录、存档间 Mod 热切换及全部 Mod 组合尚未验证。

## 原生接口与离线验证

以下入口来自当前受支持游戏文件的符号与反汇编：

| 接口 | 地址 | 用途 |
| --- | --- | --- |
| `FileMatch(StorageMount, ...)` | `0xc27c90` | 枚举皮肤贴图 |
| `FileMatch(StorageDevice*, ...)` | `0xc27c30` | 在指定来源的设备枚举目录 |
| `StorageDevice::GetAlternateRootDirectoryCount` / `GetAlternateRootDirectory` | `0xa40930` / `0xa40950` | 获取已挂载来源及顺序 |
| `StorageDeviceArchive::FindFiles` | `0xa41700` | 使用原生 archive 目录索引 |
| `StorageManager::GetFullFilePath` | `0xa5a7d0` | 查询命中的 Mod／本体路径 |
| `StorageManager::IO_GetFileInfo` | `0xa5c2d0` | 按覆盖顺序查询；`>` 跳过覆盖搜索 |
| `SpineDataFactory::CreateSpineData` | `0xc3a260` | 通过来源路径创建原生骨架句柄 |
| `ResourceManager::GetResource` | `0xc3f3f0` | 等待并取得加载结果 |
| `delete_SpineData` | `0xa63a60` | 释放临时骨架句柄 |
| `SpineSprite::OverrideAtlasTextures` | `0xa70e10` | 规范英雄贴图目录后执行原生换肤 |

`test_runtime_rig.py` 覆盖来源搜索、冲突拒绝、完整资源键、句柄回收和无需配置的启动入口，包括原版四张、同名 Mod 四张及额外一张的九条独立来源回归、错误顺序拒绝和受限回退；另从已校验的游戏文件中提取 `IO_GetFileInfo` 与 `OverrideAtlasTextures` 指令，在模拟存储设备和资源句柄上隔离执行，验证原生覆盖顺序、`>` 分支、重复分隔符和逐动作来源目录。该测试不执行游戏入口、初始化、图形或存档代码，不能替代实际游戏验证。不提交商业资源或提取的完整函数指令。

## 安装与可选旧式打包

默认运行时模式可直接使用已安装的原始 Mod。需要通过本项目安装或恢复原始发布包时，在游戏退出后执行：

```sh
python manage.py install --source "/path/to/original_mod"
python manage.py restore --source "/path/to/original_mod"
```

两条命令均安装原始包，目标由 `skin_package.json` 的 `destination` 或 `--destination` 指定；修改前核对 Mod 名称，替换失败回滚，旧包优先移入回收站。`source` 相对 `--project-root`，也可用绝对路径。默认配置保留 DeepSeek 发布包的历史位置，资源目录迁移后须指定新路径。

旧式隔离打包保留为显式兼容选项：

```sh
python manage.py install --packaged-rig --config /path/to/skin_package.json
python manage.py launch --packaged-rig --config /path/to/skin_package.json
python build_skin_package.py --config /path/to/skin_package.json --output /tmp/my_skin_build
```

配置通常只需 `source` 与 `destination`，打包器自动发现多个职业和皮肤；非标准布局可用 `heroes[{id, groups[{id, prefix, skins, assets?}]}]` 显式描述。分组的 `assets` 包含 `root`（game/source/project/config）、`anim`、`textures` 和来源 `prefix`，路径相对所选根。省略 `assets` 时使用发布包公共 `anim` 的资源；同职业内按最长前缀匹配。

旧式输出包含 `skin_package/` 与 `skin_rig_map.txt`；底层入口用 `--rig-map <文件>` 加载，和 `--runtime-rig` 互斥。该模式仍要求隔离包、配置与路由配套，恢复原始包后撤销 `--rig-map`。

## 可选集成与兼容边界

`--with-mcp` 可加载独立观察器与调试入口，普通游戏控制关闭。默认桥接项目为同级 `darkest_dungeon_mcp`，可用 `--bridge-root` 覆写；其 Python 环境须已就绪，HTTP／MCP 服务另行连接。普通补丁检查和运行时来源模式不依赖角色工程或 MCP。

仅支持 SHA-256 为 `fe1a629f7afa74e716cb440b14db6470d572ed244e0a2420521e521bc1102156` 的 Linux x86-64 原生程序。版本或指令不符即拒绝，不跳过游戏断言。

读取补丁把六个缓冲的容量总计由 36 提至 192 MiB，实际驻留内存按需分配，不提高 GPU 纹理上限。画布补丁没有运行中热重建机制。开发构建和临时日志放系统临时目录；Steam 安装产物与日志位于上述用户数据和状态目录；Mod 安装切换目录位于目标 Mod 目录旁，均不进入源码目录。离线检查、原生日志、实际画面和用户美术认可分别验证。

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

离线回归使用 `python -m unittest -v test_runtime_rig test_skin_package test_steam_integration`；两个补丁 C 源码均可用 `clang -std=c11 -O2 -Wall -Wextra -Werror -DDD_PATCH_SELFTEST <源码> -ldl -lm -o /tmp/<自测程序>` 编译后运行。Steam 测试用临时 ELF 和假库执行实际原生启动器，验证无 Python 的启动、原参数与环境传递、64 位入口选择、版本／指令拒绝、配置恢复与失败回滚；不会启动 Steam 或游戏。

`.clang-tidy` 仅启用基础 `clang-analyzer-*`。其中 Annex K 替代函数建议设为官方提供的 `actionable` 模式，仅在目标 C 库提供这些接口时报告，避免要求 Linux 补丁调用不可用的 `memcpy_s` 等函数；其他分析规则保持默认。参考 [Clang 检查器文档](https://clang.llvm.org/docs/analyzer/checkers.html#security-insecureapi-deprecatedorunsafebufferhandling)。

`node_modules` 和构建产物不提交；保留 `package.json`、`pnpm-lock.yaml`、配置与两个 Hook。提交消息示例：`fix: correct skin rig routing`。
