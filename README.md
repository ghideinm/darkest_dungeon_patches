# Darkest Dungeon Patches

<a href="https://space.bilibili.com/398692528"><img src="https://www.bilibili.com/favicon.ico" alt="B站主页" width="32" height="32"></a>

《暗黑地牢》Steam 版引擎补丁：**32 MiB 资源读取缓冲、原生分辨率画布、按皮肤实际 Mod 来源加载骨架**。配置后直接从 Steam 启动，无需 Python 或编译器。

**实验版本：Linux x86-64、Windows x64 已实机验证。Windows 两个 x64 入口均已验证自动加载，Steam 入口已验证换肤、预览和画布。**

从 [Releases](https://github.com/ghideinm/darkest_dungeon_patches/releases) 下载并解压 `darkest_dungeon_patches.zip`。补丁不包含皮肤资源，所需 Mod 请自行安装并启用。

## 安装

### Linux

使用原生 x86-64 游戏，在 Steam 兼容性设置中关闭强制 Proton。

1. 将两个 SO 放到固定目录，例如 `/home/user/.local/share/darkest_dungeon_patches`。路径不能包含空白或冒号。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，填写下面一整行，将所有 `user` 替换为实际用户名：

   ```text
   env DD_LOADBUFFER_PATCH=1 DD_NATIVE_RENDER=1 DD_RIG_RUNTIME=1 DD_PATCH_LOG=/home/user/.local/share/darkest_dungeon_patches/steam_launch.log LD_PRELOAD=/home/user/.local/share/darkest_dungeon_patches/libdd_resource_loading_patch.so:/home/user/.local/share/darkest_dungeon_patches/libdd_skin_rig_patch.so %command%
   ```

3. 从 Steam 启动游戏。已有游戏参数放在 `%command%` 后；已有其他 `LD_PRELOAD` 库时，合并到同一个变量，用冒号分隔。

默认开启全部三项功能。将 `DD_NATIVE_RENDER=0` 可关闭画布扩展，将 `DD_RIG_RUNTIME=0` 可关闭动态骨架。修改参数后需重启游戏。

**更新：** 退出游戏，替换原位置的两个 SO。

### Windows

1. 正常退出游戏。从旧版 Steam 启动器迁移时，也请退出 Steam 客户端。
2. 双击 `dd_patch_setup.exe` 完成安装。
3. 正常启动原游戏 EXE，或从 Steam 启动。

安装器向游戏中已存在的 `_windows/win64`、`_windowsnosteam/win64` 目录各部署三个文件，与 `Darkest.exe` 同级：

```text
winmm.dll             # x64 代理加载器
dd_engine_patch.asi   # 包含三项功能的 DLL 插件
winmm.ini             # 关闭代理日志和崩溃转储
```

以后无需运行安装器，也无需设置 Steam 启动项。安装器不保留日志、脚本或安装状态文件；旧版安装记录存在时，会恢复原 Steam 启动参数并清理旧启动器。

也可以把安装器放在游戏根目录运行，或指定根目录：

```powershell
.\dd_patch_setup.exe --game-root "D:\SteamLibrary\steamapps\common\DarkestDungeon"
```

**更新：** 正常退出游戏，运行新版 `dd_patch_setup.exe`。

已有其他 Mod 的 `winmm.dll` 时，安装器会拒绝覆盖。代理使用 [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)，许可见发布包的 `ultimate_asi_loader_license.txt`，或运行安装器的 `--license`。

## 卸载

### Linux

1. 正常退出游戏。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，恢复安装前的内容。若只填写了上面的示例，清空即可；与其他库共用 `LD_PRELOAD` 时，保留其他库，只移除本补丁的两个 SO 路径和本补丁添加的 `DD_*` 环境变量。
3. 之后可删除 `libdd_resource_loading_patch.so`、`libdd_skin_rig_patch.so`，以及 `DD_PATCH_LOG` 指定的补丁日志。

### Windows

正常退出游戏，在 `dd_patch_setup.exe` 所在目录打开 PowerShell，运行：

```powershell
.\dd_patch_setup.exe --uninstall
```

安装器会自动定位游戏，删除两个 x64 入口目录内的补丁文件。需要指定游戏根目录时：

```powershell
.\dd_patch_setup.exe --uninstall --game-root "D:\SteamLibrary\steamapps\common\DarkestDungeon"
```

也可手动删除 `_windows/win64`、`_windowsnosteam/win64` 中安装的 `winmm.dll`、`dd_engine_patch.asi` 和 `winmm.ini`。

## 日志与排错

| 日志 | 位置 |
| --- | --- |
| Linux | `DD_PATCH_LOG` 指定的文件；上述示例为补丁目录的 `steam_launch.log` |
| Windows | 不写磁盘日志；诊断通过调试输出和进程内状态提供 |

- 游戏更新后仍默认尝试加载，不按版本号或 EXE 哈希限制；特征不兼容时记录原因并跳过补丁。
- Linux 未生效时，检查 SO 绝对路径、开关和日志目录权限，确认未使用 Proton。
- Windows 仅支持上述两个目录中的 x64 EXE；旧版迁移提示 Steam 正在运行时，请从客户端菜单正常退出。
- 提交 Issue 时提供操作系统、补丁版本、复现步骤及相关日志即可。
