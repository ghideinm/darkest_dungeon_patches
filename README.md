# Darkest Dungeon Patches

《暗黑地牢》Linux 28212 的独立原生补丁项目。统一入口为 `manage.py`，Python 只用标准库，C11 使用 Clang 编译。

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

## Steam 自动加载

正常退出游戏和 Steam 后，一次性执行：

```sh
python manage.py steam_install
```

随后重新打开 Steam，照常点击《暗黑地牢》的“开始游戏”。安装器只为最近登录的 Steam 账号、AppID `262060` 接入启动包装器；已有启动选项会保留。可用 `--steam-root`、`--steam-user` 指定客户端和 userdata 数字账号 ID。安装、更新和撤销时要求 Steam 已退出，避免客户端覆盖配置。

默认固定安装到 `${XDG_DATA_HOME:-~/.local/share}/darkest_dungeon_patches`，可用 `--install-root` 指定；该路径不能包含空白或冒号。安装器预编译两个 SO 和 C11 原生启动器，用版本目录与 `current` 链接切换；日常启动不依赖 Python、Clang、源码目录或 `/tmp` 缓存，需系统提供 coreutils 的 `sha256sum`。每次启动仍校验整个游戏 SHA-256 和全部 25 处原指令，校验失败以 125 拒绝启动。

包装器保留 Steam 的原启动命令、运行时包装和参数，将游戏入口选择为已校验的 Linux x86-64 程序，再设置 `LD_PRELOAD` 与三项补丁开关。其他预加载库继续保留，旧 `DD_RIG_MAP` 清除；重复预加载本项目补丁会被拒绝。Steam 游戏入口仍需为原生 Linux 版本。直接绕过 Steam 执行原游戏文件不自动加载补丁。

```sh
python manage.py steam_status
python manage.py steam_uninstall
```

`steam_status` 检查配置及已安装原生启动器；`steam_uninstall` 在 Steam 退出后恢复原启动选项，保留安装产物供排查。安装后若手动修改了启动选项，撤销或更新会拒绝覆盖。更新源码后重新运行 `steam_install` 才会更新固定补丁；游戏版本不符时不会自动跳过校验。

启动及补丁日志默认写到 `${XDG_STATE_HOME:-~/.local/state}/darkest_dungeon_patches/steam_launch.log`，每次启动覆盖；与游戏自己的日志独立。

## 运行时骨架来源

1. 用原生 `ActorClass::GetColourVariationPath` 获取选中皮肤的逻辑路径。
2. 用原生 `FileMatch` 枚举皮肤贴图，再用 `StorageManager::GetFullFilePath` 查询每张贴图实际命中的来源，遵循游戏自己的 Mod 覆盖顺序。
3. 对每张贴图，在该来源中优先找 `heroes/<hero>/<skin>/anim/*.skel`，再找同一来源的 `heroes/<hero>/anim/*.skel`，最后查本体；读取候选骨架的同名图集，只选择实际引用该贴图的骨架。不同动作可以分别来自本体、官方 DLC 或 Mod，再组合为一个精灵实例。
4. 用原生存储层的 `>` 前缀明确指定来源，再通过原生骨架工厂、资源管理器和精灵构造函数加载，避免被其他 Mod 再次覆盖；临时资源句柄由原生释放接口回收。
5. 换肤前核对动画状态和回调；重建实例后保留动画名、时间和原生脚步事件回调。非英雄角色保留原逻辑。
6. 在原生 `SpineSprite::OverrideAtlasTextures` 入口合并英雄逻辑目录中的重复斜杠，保留末尾分隔符，再交给原生贴图加载逻辑。这样 `heroes/<hero>/<skin>//anim/` 不会绕过按精确路径索引的 Mod 文件，退到本体贴图或生成占位纹理；空目录指针的重置行为不变。

这里的索引是进程内的发现缓存，不依赖打包产生的文件。每次选择皮肤都会重新查询当前存储覆盖结果；不支持在运行中修改骨架内容的热重载。

本版本只发现磁盘上的 `.skel` 与同名 `.atlas`，支持标准 PNG/TGA 皮肤目录。损坏的本地骨架不会静默回退；同名骨架被图集的不同页面同时选中却指向不同来源、资源缓存命中其他来源、目标缺少当前动画或角色正忙时，会拒绝换肤并保留旧状态。未被候选图集引用的贴图不参与选组。首次创建角色时若无法确定或加载骨架，会以 125 退出并输出 `DD_RIG_SOURCE_REFUSED` 等诊断。

内部最多缓存 128 个皮肤资源组合，每组最多 256 个骨架；原生资源键需短于 256 字节。实例匹配会核对全部骨架来源。旧打包器使用的 `.rig_vanilla.` 与 `.sprite.rig_` 名称保留给隔离副本，运行时发现会忽略它们。非标准目录、存档间 Mod 热切换及全部 Mod 组合尚未验证。

## 原生接口与离线验证

以下入口来自当前受支持游戏文件的符号与反汇编：

| 接口 | 地址 | 用途 |
| --- | --- | --- |
| `FileMatch(StorageMount, ...)` | `0xc27c90` | 枚举皮肤贴图 |
| `StorageManager::GetFullFilePath` | `0xa5a7d0` | 查询命中的 Mod／本体路径 |
| `StorageManager::IO_GetFileInfo` | `0xa5c2d0` | 按覆盖顺序查询；`>` 跳过覆盖搜索 |
| `SpineDataFactory::CreateSpineData` | `0xc3a260` | 通过来源路径创建原生骨架句柄 |
| `ResourceManager::GetResource` | `0xc3f3f0` | 等待并取得加载结果 |
| `delete_SpineData` | `0xa63a60` | 释放临时骨架句柄 |
| `SpineSprite::OverrideAtlasTextures` | `0xa70e10` | 规范英雄贴图目录后执行原生换肤 |

`test_runtime_rig.py` 覆盖来源搜索、冲突拒绝、资源键、句柄回收和无需配置的启动入口；另从已校验的游戏文件中提取 `IO_GetFileInfo` 与 `OverrideAtlasTextures` 指令，在模拟存储设备和资源句柄上隔离执行，验证原生覆盖顺序、`>` 分支与重复分隔符导致的贴图缺失。该测试不执行游戏入口、初始化、图形或存档代码，不能替代实际游戏验证。不提交商业资源或提取的完整函数指令。

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
