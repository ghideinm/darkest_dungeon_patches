"""Repository command entry point."""

import sys
from pathlib import Path


def main():
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
    from darkest_dungeon_patches.build_skin_package import main as run

    run()


if __name__ == "__main__":
    main()
