"""Repository paths and the default game location for read-only checks."""

from pathlib import Path

PATCH_ROOT = Path(__file__).resolve().parents[2]
COMMON_NATIVE = PATCH_ROOT / "src/native/common"
LINUX_NATIVE = PATCH_ROOT / "src/native/linux"
WINDOWS_NATIVE = PATCH_ROOT / "src/native/windows"
CONFIG_SCRIPTS = PATCH_ROOT / "scripts/config"
TEST_ROOT = PATCH_ROOT / "tests"
DEFAULT_GAME_ROOT = Path.home() / ".local/share/Steam/steamapps/common/DarkestDungeon"
