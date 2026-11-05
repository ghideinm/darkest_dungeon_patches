"""Defaults for optional sibling projects; no game or asset discovery side effects."""

from pathlib import Path

PATCH_ROOT = Path(__file__).resolve().parents[2]
COMMON_NATIVE = PATCH_ROOT / "src/native/common"
LINUX_NATIVE = PATCH_ROOT / "src/native/linux"
WINDOWS_NATIVE = PATCH_ROOT / "src/native/windows"
WINDOWS_SCRIPTS = PATCH_ROOT / "scripts/windows"
CONFIG_ROOT = PATCH_ROOT / "config"
TEST_ROOT = PATCH_ROOT / "tests"
DEFAULT_PROJECT_ROOT = PATCH_ROOT.parent / "Darkest Dungeon"
DEFAULT_GAME_ROOT = Path.home() / ".local/share/Steam/steamapps/common/DarkestDungeon"
