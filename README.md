# Darkest Dungeon Patches

《暗黑地牢》Linux 28212 的独立原生补丁项目。源码、脚本和配置使用 snake_case，统一入口为 `manage.py`。Python 只用标准库，C11 使用 Clang 编译。

| 功能 | 必要文件 | 开关及验证范围 |
| --- | --- | --- |
| 32 MiB 资源读取 | `resource_loading_patch.c` | 默认启用；六个读取缓冲和读取容量一起更新，高清图集已实测加载 |
| 原生分辨率画布 | 同一份 C 源码 | `--native-render`；4K 场景和详情面板已实测，1024—5120 宽输入已做指令检查 |
| 按皮肤切换骨架 | `skin_rig_patch.c`、`skin_package.json` | 自动发现或显式配置多个职业；通用版本已做离线验证，实机换肤、城镇和战斗流程待验证 |

启动器检查游戏 SHA-256 与目标指令，只改写本次进程内存。磁盘游戏程序不改写，普通 Steam 启动不自动带补丁。

## 使用

在本目录执行：

```sh
python manage.py check
python manage.py install --source "/path/to/original_mod"
python manage.py launch
```

`check` 仅检查配置语法、游戏版本和指令，并编译临时库，不检查已安装包；`install` 构建并安装配套隔离皮肤包，需要游戏已退出；`launch` 校验安装包与配置、文件哈希一致后启用三项补丁，需要 Steam 已运行。在游戏中启用相应 Mod；默认包的名称是 `DeepSeek Highwaywoman Animation Skin`。

`build_skin_package.py` 从原始 Mod 发布包和本体资源生成可独立寻址的骨架组与内部路由索引，保留正式发布包。动作数量按实际文件发现，没有固定的职业或动作数。安装器由本项目提供，不依赖角色工程中的专用脚本；替换前检查 Mod 名称，失败回滚，成功后旧包优先移入回收站。

退出游戏后用 `python manage.py restore --source "/path/to/original_mod"` 恢复原始发布包，再使用不带 `--rig-map` 的底层入口。骨架路由补丁和配套隔离包必须一起启用或撤销。

按需启用：

```sh
python launch.py --check
python launch.py --native-render
```

资源读取默认启用。前两项共用一个库，骨架路由使用独立库；不用把三项拆成三份重复启动器。

## 自动发现与配置

默认配置为 `skin_package.json`；`manage.py` 和打包脚本都可用 `--config <文件>` 切换。通常只需填写原始 Mod 发布包和目标安装目录：

```json
{
  "source": "/path/to/original_mod",
  "destination": "mods/my_skin_package"
}
```

自动发现遍历发布包中的 `heroes/<hero>/<hero>_*` 皮肤目录，并按以下顺序选择骨架来源：

1. 当前皮肤的 `anim/*.skel`，对应贴图也从该目录读取。
2. 同一原始 Mod 中的 `heroes/<hero>/anim/*.skel`。
3. 游戏本体中该职业的骨架和默认皮肤贴图，适用于纯贴图皮肤。

发现与改名发生在打包时；运行时根据原生接口返回的当前皮肤路径查询生成的索引。它不会在每次换肤时扫描磁盘，也不会猜测其他已启用 Mod 的覆盖顺序。每个来源目录应使用统一的 `<prefix><action>.skel` 命名，配齐同名 `.atlas` 与 PNG/TGA 页面；多页图集也会逐页复制并改写路径。

非标准布局可显式配置 `heroes`，此时整个包使用这些分组。例如一个职业的原版与自定义骨架：

```json
{
  "source": "/path/to/original_mod",
  "destination": "mods/my_skin_package",
  "heroes": [{
    "id": "crusader",
    "groups": [
      {
        "id": "vanilla",
        "prefix": "crusader.sprite.rig_vanilla.",
        "skins": ["crusader_A", "crusader_B", "crusader_C", "crusader_D"],
        "assets": {
          "root": "game",
          "anim": "heroes/crusader/anim",
          "textures": "heroes/crusader/crusader_A/anim",
          "prefix": "crusader.sprite."
        }
      },
      {"id": "custom", "prefix": "crusader.sprite.", "skins": ["crusader_E"]}
    ]
  }]
}
```

省略 `assets` 时直接使用原始发布包中该职业公共 `anim` 的资源。配置 `assets` 时，`root` 可选 `game`、`source`、`project`、`config`，分别相对游戏、原始发布包、`--project-root`、配置文件目录；`anim` 和 `textures` 为相对路径，`prefix` 是来源文件名前缀。不同职业可复用分组名与前缀，同一职业内必须唯一；嵌套前缀按最长匹配。原生索引上限为 128 个皮肤，超限或配置歧义会拒绝。

仅打包、不安装或启动游戏：

```sh
python build_skin_package.py --config /path/to/skin_package.json --output /tmp/my_skin_build
```

输出包含 `skin_package/` 与生成的 `skin_rig_map.txt`。底层入口可用 `--rig-map /tmp/my_skin_build/skin_rig_map.txt` 加载三列索引；配套包必须已安装。修改配置后重新安装，避免骨架和路由不一致。

## 路径与可选集成

- `paths.py` 集中管理默认路径；游戏默认在当前用户的 Steam 目录。
- `manage.py --game-root <目录>` 指定游戏根目录；`launch.py --game <程序>` 指定原生可执行文件。
- `--project-root <目录>` 指定资源项目，默认是同级 `Darkest Dungeon`；配置的相对 `source` 路径以它为基准。`--source <目录>` 可直接覆盖原始 Mod 发布包路径。默认配置保留 DeepSeek 发布包的历史位置，目录迁移后须指定新路径。
- `--with-mcp` 可选加载观察器与调试入口，普通游戏控制关闭。默认桥接项目为角色项目同级的 `darkest_dungeon_mcp`，可用 `--bridge-root <目录>` 覆写；其 Python 环境须已就绪，HTTP／MCP 服务另行连接。
- 普通补丁检查不依赖角色工程或 MCP；安装后，启动所需的发现结果保存在安装包清单中，不需要原始发布包仍在原处。构建产物与运行报告位于系统临时目录，安装切换目录位于目标 Mod 目录旁，不进入源码目录。

## 兼容边界

仅支持 SHA-256 为 `fe1a629f7afa74e716cb440b14db6470d572ed244e0a2420521e521bc1102156` 的 Linux x86-64 原生程序。版本或指令不符即拒绝。

读取补丁把六个缓冲的容量总计由 36 提至 192 MiB；实际驻留内存按需分配。它不会提高 GPU 纹理上限。画布补丁跟随实际分辨率，保持原逻辑布局与输入坐标；没有增加运行中热重建机制。骨架路由仍为原型：通用打包与职业隔离的离线验证不能视为所有职业、Mod 组合和战斗流程已兼容。画面清晰度和资源加载验证不代表角色美术已获认可。

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

离线回归使用 `python -m unittest -v test_skin_package`；两个 C 源码均可用 `clang -std=c11 -O2 -Wall -Wextra -Werror -DDD_PATCH_SELFTEST <源码> -ldl -lm -o /tmp/<自测程序>` 编译后运行。Python 测试只操作系统临时目录，不接触实际游戏或存档。

`.clang-tidy` 仅启用基础 `clang-analyzer-*`。其中 Annex K 替代函数建议设为官方提供的 `actionable` 模式，仅在目标 C 库提供这些接口时报告，避免要求 Linux 补丁调用不可用的 `memcpy_s` 等函数；其他分析规则保持默认。参考 [Clang 检查器文档](https://clang.llvm.org/docs/analyzer/checkers.html#security-insecureapi-deprecatedorunsafebufferhandling)。

`node_modules` 和构建产物不提交；保留 `package.json`、`pnpm-lock.yaml`、配置与两个 Hook。提交消息示例：`fix: correct skin rig routing`。
