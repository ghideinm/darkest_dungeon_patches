# Darkest Dungeon Patches

[![B站主页](https://img.shields.io/badge/Bilibili-00A1D6?style=for-the-badge&logo=bilibili&logoColor=white)](https://space.bilibili.com/398692528)

《暗黑地牢》Steam 版引擎补丁：**32 MiB 资源读取缓冲、原生分辨率画布、按皮肤实际 Mod 来源加载骨架**。配置后直接从 Steam 启动，无需 Python 或编译器。

**实验版本：Linux x86-64 已实机验证；Windows x64 新版动态定位仍待实机验证。**

从 [Releases](https://github.com/ghideinm/darkest_dungeon_patches/releases) 下载并解压 `darkest_dungeon_patches.zip`。补丁不包含皮肤资源，所需 Mod 请自行安装并启用。

## Linux

使用原生 x86-64 游戏，在 Steam 兼容性设置中关闭强制 Proton。

1. 将两个 SO 放到固定目录，例如 `/home/user/.local/share/darkest_dungeon_patches`。路径不能包含空白或冒号。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，填写下面一整行，将所有 `user` 替换为实际用户名：

   ```text
   env DD_LOADBUFFER_PATCH=1 DD_NATIVE_RENDER=1 DD_RIG_RUNTIME=1 DD_PATCH_LOG=/home/user/.local/share/darkest_dungeon_patches/steam_launch.log LD_PRELOAD=/home/user/.local/share/darkest_dungeon_patches/libdd_resource_loading_patch.so:/home/user/.local/share/darkest_dungeon_patches/libdd_skin_rig_patch.so %command%
   ```

3. 从 Steam 启动游戏。已有游戏参数放在 `%command%` 后；已有其他 `LD_PRELOAD` 库时，合并到同一个变量，用冒号分隔。

默认开启全部三项功能。将 `DD_NATIVE_RENDER=0` 可关闭画布扩展，将 `DD_RIG_RUNTIME=0` 可关闭动态骨架。修改参数后需重启游戏。

**更新：** 退出游戏，替换原位置的两个 SO。

**撤销：** 从 Steam 启动选项中移除本补丁的环境变量及 SO 路径，恢复原参数。

## Windows

1. 正常退出游戏，并从 Steam 菜单退出客户端。
2. 双击 `dd_patch_setup.exe` 完成安装。
3. 重新打开 Steam，从原游戏条目启动。

安装器自动定位 Steam、账号和游戏，将补丁安装到 `%LOCALAPPDATA%\DarkestDungeonPatches\steam`，保留原启动参数。以后无需再次运行安装器，下载的安装包可以删除。

自动定位游戏失败时，可指定游戏根目录：

```powershell
.\dd_patch_setup.exe --game-root "D:\SteamLibrary\steamapps\common\DarkestDungeon"
```

**更新：** 正常退出游戏和 Steam，运行新版 `dd_patch_setup.exe`。

**撤销：** 正常退出游戏和 Steam，运行以下命令恢复原启动选项；已安装的补丁文件会保留：

```powershell
.\dd_patch_setup.exe --uninstall
```

## 日志与排错

| 日志 | 位置 |
| --- | --- |
| Linux | `DD_PATCH_LOG` 指定的文件；上述示例为补丁目录的 `steam_launch.log` |
| Windows 安装／卸载 | `%LOCALAPPDATA%\DarkestDungeonPatches\setup.log` |
| Windows 游戏启动 | `%LOCALAPPDATA%\DarkestDungeonPatches\steam\windows_launch.log` |

- 游戏更新后仍默认尝试加载，不按版本号或 EXE 哈希限制；特征不兼容时记录原因并跳过补丁。
- Linux 未生效时，检查 SO 绝对路径、开关和日志目录权限，确认未使用 Proton。
- Windows 安装时提示 Steam 正在运行，请退出客户端后重试；关闭窗口不一定代表退出。
- 提交 Issue 时提供操作系统、补丁版本、复现步骤及相关日志即可。
