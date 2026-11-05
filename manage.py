"""Repository command entry point."""

import sys
from pathlib import Path


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parent / "src"))
    from darkest_dungeon_patches.cli import main as run

    run()


if __name__ == "__main__":
    main()
