# Darkest Dungeon Patches

<a href="https://space.bilibili.com/398692528"><img src="https://www.bilibili.com/favicon.ico" alt="B站主页" width="32" height="32"></a>

《暗黑地牢》Steam 版引擎补丁：**可配置的资源读取缓冲（默认 32 MiB）、原生分辨率画布、按皮肤实际 Mod 来源加载骨架**。配置后直接从 Steam 启动，无需 Python 或编译器。

**实验版本：基础补丁已在 Linux x86-64、Windows x64 实机验证；本次 DLL 加载与配置改动已通过自造程序回归，仍需实机验证。**

从 [Releases](https://github.com/ghideinm/darkest_dungeon_patches/releases) 下载并解压 `darkest_dungeon_patches.zip`。补丁不包含皮肤资源，所需 Mod 请自行安装并启用。

## 安装

### Linux

使用原生 x86-64 游戏，在 Steam 兼容性设置中关闭强制 Proton。

1. 将两个 SO 放到同一个固定目录，例如 `/home/user/.local/share/darkest_dungeon_patches`。路径不能包含空白或冒号。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，填写下面一整行，将所有 `user` 替换为实际用户名：

   ```text
   env DD_LOADBUFFER_PATCH=1 DD_NATIVE_RENDER=1 DD_RIG_RUNTIME=1 LD_PRELOAD=/home/user/.local/share/darkest_dungeon_patches/libdd_resource_loading_patch.so:/home/user/.local/share/darkest_dungeon_patches/libdd_skin_rig_patch.so %command%
   ```

3. 从 Steam 启动游戏。已有游戏参数放在 `%command%` 后；已有其他 `LD_PRELOAD` 库时，合并到同一个变量，用冒号分隔。

默认开启全部三项功能。将 `DD_NATIVE_RENDER=0` 可关闭画布扩展，将 `DD_RIG_RUNTIME=0` 可关闭动态骨架。修改参数后需重启游戏。

**更新：** 退出游戏，替换原位置的两个 SO。

### Windows

两种方式任选其一。补丁本体是 `dd_engine_patch.dll`，默认开启全部三项功能；配置文件是 `dd_engine_patch.ini`，始终与补丁 DLL 放在一起。本项目不再提供安装或启动 EXE。

#### 方式一：Darkest Dungeon DLL Mod Loader

1. 正常退出游戏，按照 [Darkest Dungeon DLL Mod Loader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader) 的说明安装加载器。
2. 在游戏根目录新建 `mods/dd_engine_patch` 文件夹，把 `dd_engine_patch.dll` 和 `dd_engine_patch.ini` 放进去。该加载器也支持扫描创意工坊中的 DLL；同一补丁只保留一份启用的副本。
3. 打开加载器的配置工具，勾选 `dd_engine_patch.dll` 并保存，再正常启动游戏。本补丁不需要在加载器配置中额外声明 DLL 路径。

该加载器使用自己的 `dbghelp.dll` 加载 Mod；使用此方式时，无需解压 `manual_winmm.zip`。手动代理收在 ZIP 中，避免被加载器扫描成另一个 Mod DLL。

#### 方式二：手动安装 WinMM 代理

1. 正常退出游戏。解压发布包中的 `manual_winmm.zip`，得到 `winmm.dll` 和 `winmm.ini`。打开游戏根目录中的 `_windows/win64`，将下面四个文件放在 `Darkest.exe` 旁。若使用 `_windowsnosteam/win64/Darkest.exe`，则安装到该目录；两个入口都使用时分别安装。

```text
winmm.dll             # 第三方 Ultimate ASI Loader，x64
winmm.ini             # 声明要加载的补丁 DLL，及代理自身的日志设置
dd_engine_patch.dll   # 包含三项功能的补丁
dd_engine_patch.ini   # 补丁的内存、皮肤遮掩和日志配置
```

2. 本包的 `winmm.ini` 已包含下面的 DLL 路径声明；路径相对 `winmm.dll` 所在目录解析，不需要填写游戏绝对路径：

```ini
[GlobalSets]
LoadExtraPlugins=dd_engine_patch.dll
DebugLog=0
DisableCrashDumps=1
```

3. 正常从 Steam 或原游戏 EXE 启动，无需 Steam 启动参数。

手动方式的代理来自 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)，许可见发布包的 `ultimate_asi_loader_license.txt`。已有其他 Mod 的 `winmm.dll` 时不要直接覆盖，可选择方式一；不要让两种方式同时加载本补丁。

**更新与旧版迁移：** 退出游戏，替换所选方式下的补丁 DLL，保留自己修改过的 `dd_engine_patch.ini`。旧手动方案使用 `dd_engine_patch.asi` 时，删除该旧文件，换为新 DLL，并更新本包的 `winmm.dll`、`winmm.ini`。改用方式一时，先移除原来由本补丁安装的 `winmm.dll`、`winmm.ini`、`dd_engine_patch.asi` / `dd_engine_patch.dll`；其他 Mod 的文件应保留。更早版本使用自有 Steam 启动器时，还需恢复当时的 Steam 启动参数，之后可删除旧启动器。新版不再自动改写 Steam 配置。

## 配置

### Linux：SO + 环境变量

Linux 不读取 INI 文件，`DD_PATCH_CONFIG` 也不参与配置。合并发布包中的 INI 供 Windows 使用，Linux 只需要两个 SO。所有参数在启动时读取，修改后重启游戏。

| 环境变量 | 默认值 | 作用 |
| --- | --- | --- |
| `DD_LOADBUFFER_PATCH` | `0` | `1` 开启资源缓冲补丁 |
| `DD_NATIVE_RENDER` | `0` | `1` 开启原生画布；需要 `DD_LOADBUFFER_PATCH=1` |
| `DD_RIG_RUNTIME` | `0` | `1` 开启按 Mod 来源加载骨架及皮肤遮掩 |
| `DD_RESOURCE_BUFFER_MIB` | `32` | 每个资源读取缓冲的 MiB 数，允许 6–2047；使用系统内存，分配与读取检查同步修改 |
| `DD_CACHE_INITIAL_CAPACITY` | `16` | 皮肤方案、职业目录缓存各自的初始条目数，之后自动翻倍扩容 |
| `DD_HIDE_VANILLA_SKINS` | `1` | 有 Mod 皮肤时跳过同职业的原版及官方 DLC 皮肤；无 Mod 的职业保持原样 |
| `DD_LOG_TO_FILE` | `0` | `1` 将补丁诊断追加到文件，并及时刷新 |
| `DD_LOG_FILE` | `dd_engine_patch.log` | 日志路径；相对路径以游戏当前工作目录为准，目录需已存在 |

示例：在安装步骤的 Steam 启动选项中添加 `DD_RESOURCE_BUFFER_MIB=64 DD_CACHE_INITIAL_CAPACITY=32 DD_HIDE_VANILLA_SKINS=1 DD_LOG_TO_FILE=1 DD_LOG_FILE=/home/user/dd_patch.log`。

旧的 `DD_PATCH_LOG` 仍兼容：未设置 `DD_LOG_FILE` 时可提供日志路径；只设置旧路径会启用落盘，但显式 `DD_LOG_TO_FILE=0` 始终关闭落盘。`DD_RIG_MAP` 是旧的手工骨架映射入口，仅用于未开启 `DD_RIG_RUNTIME` 的高级模式，通常无需设置。

### Windows：DLL + INI

编辑补丁二进制同目录的 `dd_engine_patch.ini`，修改后重启游戏。缺少文件时使用默认值；也可用环境变量 `DD_PATCH_CONFIG` 指定其他 INI 路径。Linux 的上述五个新参数不会覆盖 Windows 的 INI 值。

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

- `resource_buffer_mib`：每个资源读取缓冲的大小，单位 MiB，使用系统内存，允许 16–2047；分配大小与读取检查同步修改。
- `cache_initial_capacity`：皮肤方案、职业目录缓存各自的初始条目数，之后自动翻倍扩容。
- `hide_vanilla_skins`：默认 `1`，有 Mod 皮肤时跳过同职业的原版及官方 DLC 皮肤；设为 `0` 关闭。更新时已有配置中明确填写的 `0` 会保留。
- `log_to_file`：默认 `0`；`1` 同时将补丁诊断追加到文件，并及时刷新。
- `log_file`：日志路径；相对路径以 INI 所在目录为准，也可填写 UTF-8 绝对路径。目录需已存在。

手动安装的 `winmm.ini` 中，`LoadExtraPlugins` 指定要加载的补丁，`DebugLog=0` 关闭代理日志，`DisableCrashDumps=1` 关闭代理崩溃转储。`dd_engine_patch.ini` 控制补丁自身的诊断日志；没有可读 INI 时，兼容 `DD_PATCH_LOG` 日志路径。方式一的加载器日志由该项目独立管理。

两平台的无效值均保留默认值（INI 重复字段保留前一个有效值），原因写入调试输出。日志无法打开时补丁继续运行，且不会重定向游戏自身的标准错误输出。皮肤遮掩保留游戏原始编号和资源，支持正反向循环；原版及 DLC 资源仍可为 Mod 补齐动作。游戏和 Steam 自身的日志不受补丁日志开关控制。

Windows 与 Linux 均支持新增职业跨 Mod 补齐动作：选中皮肤自己的骨骼、同 Mod 的职业公共骨骼优先；缺少的动作通过游戏的文件解析器按当前 Mod 加载优先级查找同职业资源。新皮肤缺少动作贴图时，可使用动作提供者的基础皮肤资源，不要求把 A 皮肤的共用动作重复复制到 B、C。每个动作保留实际骨骼、图集及贴图来源；已存在但损坏的覆盖文件会记录原因并拒绝该方案，不静默改用低优先级副本。此补齐不受皮肤遮掩开关影响，无需额外配置；Mod 顺序调整后需按游戏流程重新加载。

## 卸载

### Linux

1. 正常退出游戏。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，恢复安装前的内容。若只填写了上面的示例，清空即可；与其他库共用 `LD_PRELOAD` 时，保留其他库，只移除本补丁的两个 SO 路径和本补丁添加的 `DD_*` 环境变量。
3. 之后可删除两个 SO 和所配置的补丁日志。

### Windows

正常退出游戏。方式一在加载器中取消勾选本补丁并保存，或删除手动放入 `mods/dd_engine_patch` 的 DLL 与 INI；第三方加载器本身按其项目说明卸载。方式二删除实际安装入口旁的 `dd_engine_patch.dll`、`dd_engine_patch.ini`，以及只供本补丁使用的 `winmm.dll`、`winmm.ini`。代理被其他 Mod 共用时，保留代理文件，只从 `LoadExtraPlugins` 中移除本补丁条目。

## 日志与排错

| 日志 | 位置 |
| --- | --- |
| Linux | `DD_LOG_TO_FILE=1` 时写入 `DD_LOG_FILE`（兼容旧的 `DD_PATCH_LOG`） |
| Windows | INI 中 `log_to_file=1` 时写入 `log_file` |

- 游戏更新后仍默认尝试加载，不按版本号或 EXE 哈希限制；特征不兼容时记录原因并跳过补丁。
- Linux 未生效时，检查 SO 绝对路径、开关和日志目录权限，确认未使用 Proton。
- Windows 仅支持上述两个目录中的 x64 EXE。手动方式不生效时，检查 `winmm.ini` 的 `LoadExtraPlugins` 和四个文件的位置；方式一检查加载器是否勾选了补丁。仅复制普通 DLL 到 EXE 旁不会自动加载。
- 提交 Issue 时提供操作系统、补丁版本、复现步骤及相关日志即可。
