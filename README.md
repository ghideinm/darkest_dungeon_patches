# Darkest Dungeon Patches

<a href="https://space.bilibili.com/398692528"><img src="https://www.bilibili.com/favicon.ico" alt="B站主页" width="32" height="32"></a>

《暗黑地牢》Steam 版引擎补丁：**可配置的资源读取缓冲（默认 32 MiB）、原生分辨率画布、按皮肤实际 Mod 来源加载骨架**。配置后直接从 Steam 启动，无需 Python 或编译器。

**实验版本：Linux x86-64、Windows x64 已实机验证。Windows 两个 x64 入口均已验证自动加载，Steam 入口已验证换肤、预览和画布。**

从 [Releases](https://github.com/ghideinm/darkest_dungeon_patches/releases) 下载并解压 `darkest_dungeon_patches.zip`。补丁不包含皮肤资源，所需 Mod 请自行安装并启用。

## 安装

### Linux

使用原生 x86-64 游戏，在 Steam 兼容性设置中关闭强制 Proton。

1. 将两个 SO 和 `dd_engine_patch.ini` 放到同一个固定目录，例如 `/home/user/.local/share/darkest_dungeon_patches`。路径不能包含空白或冒号。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，填写下面一整行，将所有 `user` 替换为实际用户名：

   ```text
   env DD_LOADBUFFER_PATCH=1 DD_NATIVE_RENDER=1 DD_RIG_RUNTIME=1 LD_PRELOAD=/home/user/.local/share/darkest_dungeon_patches/libdd_resource_loading_patch.so:/home/user/.local/share/darkest_dungeon_patches/libdd_skin_rig_patch.so %command%
   ```

3. 从 Steam 启动游戏。已有游戏参数放在 `%command%` 后；已有其他 `LD_PRELOAD` 库时，合并到同一个变量，用冒号分隔。

默认开启全部三项功能。将 `DD_NATIVE_RENDER=0` 可关闭画布扩展，将 `DD_RIG_RUNTIME=0` 可关闭动态骨架。修改参数后需重启游戏。

**更新：** 退出游戏，替换原位置的两个 SO。

### Windows

1. 正常退出游戏。从旧版 Steam 启动器迁移时，也请退出 Steam 客户端。
2. 双击 `dd_patch_setup.exe` 完成安装。
3. 正常启动原游戏 EXE，或从 Steam 启动。

安装器向游戏中已存在的 `_windows/win64`、`_windowsnosteam/win64` 目录各部署四个文件，与 `Darkest.exe` 同级：

```text
winmm.dll             # x64 代理加载器
dd_engine_patch.asi   # 包含三项功能的 DLL 插件
winmm.ini             # 代理加载器自身的日志和崩溃转储设置
dd_engine_patch.ini   # 补丁内存与日志配置
```

以后无需运行安装器，也无需设置 Steam 启动项。安装器不保留日志、脚本或安装状态文件；旧版安装记录存在时，会恢复原 Steam 启动参数并清理旧启动器。

也可以把安装器放在游戏根目录运行，或指定根目录：

```powershell
.\dd_patch_setup.exe --game-root "D:\SteamLibrary\steamapps\common\DarkestDungeon"
```

**更新：** 正常退出游戏，运行新版 `dd_patch_setup.exe`。已有的 `dd_engine_patch.ini` 会保留。

已有其他 Mod 的 `winmm.dll` 时，安装器会拒绝覆盖。代理使用 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)，许可见发布包的 `ultimate_asi_loader_license.txt`，或运行安装器的 `--license`。

## 配置

编辑补丁二进制同目录的 `dd_engine_patch.ini`，修改后重启游戏。缺少文件时使用默认值；也可用环境变量 `DD_PATCH_CONFIG` 指定配置文件路径。

```ini
[memory]
resource_buffer_mib=32
cache_initial_capacity=16

[skins]
hide_vanilla_skins=1

[logging]
log_to_file=0
log_file=dd_engine_patch.log
```

- `resource_buffer_mib`：每个资源读取缓冲的大小，单位 MiB，使用系统内存。允许范围为 Linux 6–2047、Windows 16–2047；分配大小与读取检查同步修改。
- `cache_initial_capacity`：皮肤方案、职业目录缓存各自的初始条目数。容量不足时继续翻倍扩容，不是条目上限。
- `hide_vanilla_skins`：默认 `1`，某职业有 Mod 皮肤时，换肤、角色创建和预览优先使用 Mod 皮肤，切换时跳过该职业全部原版及官方 DLC 皮肤。没有 Mod 皮肤的职业保留原版选择。设为 `0` 可关闭；需要启用动态骨架功能。保留游戏原始皮肤编号和资源，支持正反向循环；原版及 DLC 资源仍可为 Mod 补齐动作。更新时已有配置中明确填写的 `0` 会保留。
- `log_to_file`：`0` 只输出调试信息；`1` 同时将补丁诊断追加到文件，并及时刷新。默认关闭落盘。
- `log_file`：日志路径；相对路径以配置文件所在目录为基准，也可填写 UTF-8 绝对路径。目录需已存在。日志无法打开时补丁继续运行。

无效字段保留默认值（重复字段保留前一个有效值），原因写入调试输出；配置不会在每次分配内存时重新读取。Linux 两个 SO 使用同目录配置时共享参数与日志文件，补丁不会重定向游戏自身的标准错误输出。没有可读配置文件时，旧的 `DD_PATCH_LOG` 环境变量仍可启用文件日志。

Windows 的 `winmm.ini` 独立控制代理加载器日志和崩溃转储，`dd_engine_patch.ini` 控制补丁自身的诊断日志。游戏和 Steam 自身的日志不受此开关控制。

## 卸载

### Linux

1. 正常退出游戏。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，恢复安装前的内容。若只填写了上面的示例，清空即可；与其他库共用 `LD_PRELOAD` 时，保留其他库，只移除本补丁的两个 SO 路径和本补丁添加的 `DD_*` 环境变量（包括可选的 `DD_PATCH_CONFIG`）。
3. 之后可删除两个 SO、`dd_engine_patch.ini` 和所配置的补丁日志。

### Windows

正常退出游戏，在 `dd_patch_setup.exe` 所在目录打开 PowerShell，运行：

```powershell
.\dd_patch_setup.exe --uninstall
```

安装器会自动定位游戏，删除两个 x64 入口目录内的补丁文件。需要指定游戏根目录时：

```powershell
.\dd_patch_setup.exe --uninstall --game-root "D:\SteamLibrary\steamapps\common\DarkestDungeon"
```

也可手动删除 `_windows/win64`、`_windowsnosteam/win64` 中安装的 `winmm.dll`、`dd_engine_patch.asi`、`winmm.ini` 和 `dd_engine_patch.ini`。

## 日志与排错

| 日志 | 位置 |
| --- | --- |
| Linux / Windows | `log_to_file=1` 时写入 `log_file`；默认关闭落盘。始终保留调试输出 |

- 游戏更新后仍默认尝试加载，不按版本号或 EXE 哈希限制；特征不兼容时记录原因并跳过补丁。
- Linux 未生效时，检查 SO 绝对路径、开关和日志目录权限，确认未使用 Proton。
- Windows 仅支持上述两个目录中的 x64 EXE；旧版迁移提示 Steam 正在运行时，请从客户端菜单正常退出。
- 提交 Issue 时提供操作系统、补丁版本、复现步骤及相关日志即可。
