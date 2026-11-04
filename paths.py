"""Defaults for optional sibling projects; no game or asset discovery side effects."""

from pathlib import Path

PATCH_ROOT = Path(__file__).resolve().parent
DEFAULT_PROJECT_ROOT = PATCH_ROOT.parent / "Darkest Dungeon"
DEFAULT_GAME_ROOT = (
    Path.home() / ".local/share/Steam/steamapps/common/DarkestDungeon"
)


def bridge_root(project_root: Path) -> Path:
    return project_root.expanduser().resolve().parent / "darkest_dungeon_mcp"
