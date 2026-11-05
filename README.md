# Darkest Dungeon Patches

为《暗黑地牢》Steam 版提供资源读取扩容、原生分辨率画布和按皮肤来源切换骨架的补丁。配置完成后，直接在 Steam 点击“开始游戏”即可加载，玩家无需安装 Python 或编译器。

| 功能 | 作用 |
| --- | --- |
| 资源读取扩容 | 将单个读取缓冲上限提高到 32 MiB，支持超过原容量限制的资源文件 |
| 原生分辨率画布 | 让游戏画布跟随实际分辨率，包括 4K，保留原有逻辑布局 |
| 动态骨架路由 | 按当前英雄皮肤的实际 Mod 来源查找骨架，区分不同来源的同名资源，无需预先生成路由表 |

**当前为实验版本。** 新版优先验证 Linux；Windows 动态定位迁移仍待后续验证。 读取扩容、动态骨架和 4K 画布已完成基础实机验证；当前自动加载分发流程和完整战斗回归仍待完成。

## 支持范围

| 平台 | 受支持的游戏入口 | 加载方式 |
| --- | --- | --- |
| Linux x86-64 | 原生 `_linux/darkest.bin.x86_64` | Steam 环境变量与 `LD_PRELOAD` |
| Windows x64 | `_windows\win64\Darkest.exe` | 运行一次安装 EXE |

补丁不检查游戏版本号，不使用游戏 EXE 哈希白名单。游戏更新后仍默认尝试加载；运行时特征和接口结构仍兼容时继续使用现有补丁，不需要仅因版本变化重新安装。

启动时在可执行代码段内扫描带通配符的特征，动态定位改写位置、调用函数和回调；从相对寻址或地址操作数解析全局数据地址，再检查目标所在代码／数据段和补丁原指令。定位要求唯一匹配，不使用旧偏移兜底。找不到特征、出现多个匹配或指令不符时，记录原因并跳过：Linux 跳过对应功能，Windows 启动未修改的游戏；Windows 注入初始化失败也会回退到未修改的游戏。代码和数据地址移动、相对位移改变时可自动重新定位；Linux 的角色贴图指针字段也从原生访问指令提取，并核对两处引用一致。若更新改变了特征本身、对象字段布局或调用约定，仍需重新适配，不能保证所有未来更新自动兼容。

补丁只修改运行中的进程内存，不修改磁盘上的游戏程序。Linux 需使用原生入口；Proton 不适用这套 Linux 配置。Windows 的 Steam 默认入口可以是 32 位，启动器会优先转到同一安装目录下的 x64 程序。

发布包不包含游戏、存档或皮肤资源。请先通过 Steam 正常安装游戏，并安装、启用所需的 Mod。

## 下载

从仓库 **Releases** 下载 `darkest_dungeon_patches.zip`。开发构建可在 **Actions → build_release → 成功完成的运行 → Artifacts** 中下载 `darkest_dungeon_patches`。

解压后的同一个目录包含两平台文件，按对应平台的说明使用即可：

```text
darkest_dungeon_patches/
  README.md
  SHA256SUMS
  libdd_resource_loading_patch.so
  libdd_skin_rig_patch.so
  dd_windows_launcher.exe
  dd_skin_rig_patch.dll
  dd_patch_setup.exe
```

`SHA256SUMS` 用于核对原始发布文件。

## Windows 安装

1. 正常保存退出游戏，并从 Steam 菜单退出客户端。
2. 双击 `dd_patch_setup.exe`，按结果提示完成安装。
3. 重新打开 Steam，在《暗黑地牢》的原游戏条目点击“开始游戏”。

安装器内置启动器、DLL 和安装逻辑，自动查找 Steam、最近登录账号及游戏库。默认安装到：

```text
%LOCALAPPDATA%\DarkestDungeonPatches\steam
```

安装阶段使用 Windows 自带的 PowerShell；日常启动只运行已安装的原生 EXE 和 DLL，无需再次运行安装器、CMD 或 PowerShell。原 Steam 启动参数会被保留，自动加载默认开启全部三项补丁。

安装完成后可以删除下载的安装包。退订用于分发补丁的 Mod 不会移除独立安装目录或清除 Steam 启动选项。**皮肤 Mod 仍需保留**：退订后被 Steam 删除的贴图和骨骼资源不会由本补丁保留。

### 更新与卸载

更新时，正常退出游戏和 Steam，再运行新版 `dd_patch_setup.exe`。

卸载自动加载时，正常退出游戏和 Steam，在安装器所在目录运行：

```powershell
.\dd_patch_setup.exe --uninstall
```

安装器会恢复原 Steam 启动选项，补丁文件仍保留在安装目录中。安装器已删除时，可重新下载发布包执行撤销。

### 手动指定路径

自动定位失败时，例如游戏安装在其他磁盘，可运行：

```powershell
.\dd_patch_setup.exe --game-root "D:\SteamLibrary\steamapps\common\DarkestDungeon"
```

| 参数 | 用途 |
| --- | --- |
| `--steam-root <目录>` | 指定 Steam 客户端目录 |
| `--steam-user <数字 ID>` | 指定 Steam `userdata` 下的账号目录名 |
| `--game-root <目录>` | 指定游戏根目录 |
| `--install-root <目录>` | 指定补丁的独立安装目录 |
| `--preview` | 查看拟写入的配置，不执行安装 |
| `--status` | 查询自动加载状态 |
| `--uninstall` | 恢复原 Steam 启动选项 |

状态查询和预览结果写入安装日志 `setup.log`，位置见下方“排错与反馈”。

## Linux 安装

使用原生 Linux x86-64 游戏，在 Steam 的兼容性设置中关闭强制 Proton。无需启动脚本、额外启动器或配置文件。

1. 将解压后的目录放到固定位置，例如 `/home/user/.local/share/darkest_dungeon_patches`，保留两个 SO。将示例中的 `user` 替换为实际用户名；SO 的绝对路径不能包含空白或冒号。
2. Steam →《暗黑地牢》→ 属性 → 通用 → 启动选项，填写以下**一行**，替换其中的实际路径：

   ```text
   env DD_LOADBUFFER_PATCH=1 DD_NATIVE_RENDER=1 DD_RIG_RUNTIME=1 LD_PRELOAD=/home/user/.local/share/darkest_dungeon_patches/libdd_resource_loading_patch.so:/home/user/.local/share/darkest_dungeon_patches/libdd_skin_rig_patch.so %command%
   ```

以后直接在 Steam 点击“开始游戏”。动态链接器加载 SO，SO 在修改内存前自行扫描特征、解析地址并核对原指令。三个开关分别控制读取扩容、原生分辨率画布和动态骨架路由。

如需写入独立日志，在同一行的 `env` 后加入 `DD_PATCH_LOG=/home/user/.local/share/darkest_dungeon_patches/steam_launch.log`。日志按追加方式写入，该目录需存在且可写；未设置时日志输出到标准错误。

已有普通游戏参数放在 `%command%` 后。已有 `LD_PRELOAD` 时将两份 SO 路径追加到同一个变量中，以冒号分隔；同一补丁不要重复加载。原生 64 位入口必须由 Steam 正常启动。

更新时退出游戏，替换原位置的 SO。撤销时，从 Steam 启动选项中删除本补丁的环境变量与两份 SO 路径，恢复原参数。

## 排错与反馈

自动加载的生效范围是 Steam 的游戏条目及通过该条目启动的快捷方式。未设置这些环境变量或启动选项时，直接运行原游戏程序不会加载补丁。

| 日志 | 位置 |
| --- | --- |
| Windows 安装、更新、撤销及状态查询 | `%LOCALAPPDATA%\DarkestDungeonPatches\setup.log` |
| Windows 日常启动 | 默认 `%LOCALAPPDATA%\DarkestDungeonPatches\steam\windows_launch.log` |
| Linux 日常启动 | `DD_PATCH_LOG` 指定的位置；未设置则输出到标准错误 |

常见问题：

- **游戏更新后补丁未生效**：查看日志中的 `SIGNATURE_MISSING`、`SKIPPED` 或 `MISMATCH`，提供对应的 `feature` 和 `matches`；`matches=0` 表示未找到，`matches=2` 表示至少两处匹配。补丁不会仅因版本变化阻止启动；原指令不匹配时会跳过，提交日志以便适配。
- **Linux 没有加载补丁**：检查两份 SO 的绝对路径和三个开关，确认 Steam 启动的是原生 x86-64 版本，且未使用 Proton。日志路径需可写。
- **Windows 安装器提示 Steam 仍在运行**：正常退出客户端后重试。关闭 Steam 窗口不一定代表退出客户端。
- **Windows 提示启动选项被修改**：当前配置与安装器记录不同，安装器会拒绝覆盖。请保留现有配置和安装日志后反馈。
- **安全软件拦截 EXE 或 DLL**：自动构建目前未做数字签名，安装器不会关闭防护或添加排除项。反馈时请附检测名称和对应文件名。

提交 Issue 时请提供操作系统、补丁版本、复现步骤和相关日志；显示异常可附截图。无需上传游戏文件或存档。

## 兼容性限制

- 资源读取上限为每个缓冲 32 MiB；Linux 有 6 个缓冲，Windows 有 12 个。扩容不改变 GPU 的纹理尺寸上限。
- 骨架发现面向标准英雄皮肤目录，读取磁盘上的 `.skel`、同名 `.atlas` 及 PNG/TGA 贴图。非标准资源布局、全部 Mod 组合和存档间热切换尚未全面验证。
- 来源冲突、损坏资源、缺少当前动画或角色忙碌时，补丁可能跳过换肤，并记录原因。
- 画布补丁需要在启动时加载；运行中替换补丁文件或修改骨架内容不支持热重载。

## 源码构建与贡献

源码与发布文件分开组织，根目录的 `manage.py` 仅作为统一命令入口：

```text
src/
  darkest_dungeon_patches/  # Python 构建、配置与部署工具
  native/
    common/                # 特征扫描、地址解析和共用骨架路由
    linux/                 # Linux 原生补丁与加载逻辑
    windows/               # Windows 适配、注入器与安装器
scripts/                   # 开发入口和安装器内置脚本
config/                    # 配置文件
tests/                     # 自包含单元测试与集成测试
.github/workflows/         # GitHub Actions
```

完整发布包从 Linux x86-64 构建，需要 Python 3.10+、uv、Clang 和 LLVM-MinGW x64 Clang，不需要游戏文件：

```sh
uv sync --locked
uv run --locked manage.py bundle --windows-cc /path/to/x86_64-w64-mingw32-clang
```

默认输出到系统临时目录；可用 `--output <空目录>` 指定输出位置。构建生成 Linux SO、Windows DLL、注入器、内置载荷的安装 EXE，以及包含校验和的统一 ZIP。

需要从源码只读核对已安装的游戏时：

```sh
uv run --locked manage.py check --game-root "/path/to/DarkestDungeon"
uv run --locked manage.py windows_check --game-root "/path/to/windows/DarkestDungeon"
```

开发检查依赖 Node.js、pnpm、Clang、clang-format 和 clang-tidy：

```sh
uv sync --locked
pnpm install --frozen-lockfile
pnpm format
pnpm check
uv run --locked manage.py test
```

Python 环境与开发依赖由 `pyproject.toml`、`uv.lock` 管理；`pnpm check` 同时执行 Ruff 风格检查、格式检查及 C 静态检查。

`tests/` 只包含针对项目代码的单元测试和集成测试，数据、Steam 配置和可执行程序均为临时生成的测试替身，不读取已安装的游戏、存档或商业资源，也不需要手动观察画面。回归覆盖代码／数据地址独立移动、相对调用与数据引用、特征缺失／歧义，以及注入失败后的正常启动。Windows 原生启动链的自有程序测试可按需提供 LLVM-MinGW 与 Wine；安装器集成检查在 Windows runner 上自动执行。

[GitHub Actions 工作流](.github/workflows/build_release.yml) 在 push、pull request 或手动运行时构建，执行离线检查和 Windows 安装器检查；`v*` 标签在检查通过后发布统一 ZIP。CI 通过不等于实际游戏场景验证。

C11 使用 Clang，保留 `-Wall -Wextra -Werror`；格式和静态分析检查失败会阻止提交。提交消息遵循 Conventional Commits。修改平台适配时，必须核对目标原指令与调用约定，不增加游戏版本或整文件哈希白名单，不修改游戏磁盘程序或跳过游戏断言。
