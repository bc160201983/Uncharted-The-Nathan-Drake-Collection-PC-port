"""Check game files (launcher -> Advanced -> FOLDERS, 29 Oct 2026): compares the player's game folder
with the list of every file of the Collection's base game (names and sizes only, no game data).
  check_game_files.py check <game folder>      prints the missing / wrong-size files, exit 1 if any
  check_game_files.py make <base uroot> [<other region's base uroot> ...]   writes the list
The list holds the files every region's base game has (the EU CUSA02344 and US CUSA02320 copies
agree); a size is checked only where all regions agree and update 1.02 does not replace the file
(players may copy the update into the game folder). sce_sys (region metadata) is left out.
Why: Uncharted 3 chapter 8 stopped for players whose dump lacked
u3data/build/main/dc1/ps4/ss-syria/ss-area2-breakable-buildings-controller-1.bin (#247, #296)."""
import gzip
import json
import os
import sys
from pathlib import Path

LIST = Path(__file__).with_name("game_files_102.json.gz")


def scan(root):
    root = Path(root)
    files = {}
    for dirpath, _, names in os.walk(root):
        for name in names:
            path = Path(dirpath, name)
            rel = path.relative_to(root).as_posix()
            if not rel.startswith("sce_sys/"):
                files[rel] = path.stat().st_size
    return files


def make(roots):
    scans = [scan(r) for r in roots]
    common = set(scans[0]).intersection(*scans[1:])
    out = {rel: (scans[0][rel] if all(s[rel] == scans[0][rel] for s in scans) else -1) for rel in sorted(common)}
    with gzip.open(LIST, "wt", encoding="utf-8") as f:
        json.dump(out, f, separators=(",", ":"))
    print(f"{len(out)} files listed ({sum(v < 0 for v in out.values())} without a size check) -> {LIST}")


def uroot(folder):
    folder = Path(folder)
    return folder / "uroot" if (folder / "uroot" / "eboot.bin").exists() else folder


def check(folder):
    with gzip.open(LIST, "rt", encoding="utf-8") as f:
        wanted = json.load(f)
    root = uroot(folder)
    missing, wrong = [], []
    for rel, size in wanted.items():
        path = root / rel
        try:
            actual = path.stat().st_size
        except OSError:
            missing.append(rel)
            continue
        if size >= 0 and actual != size:
            wrong.append(rel)
    return root, len(wanted), missing, wrong


if __name__ == "__main__":
    if sys.argv[1] == "make":
        make(sys.argv[2:])
    else:
        root, total, missing, wrong = check(sys.argv[2])
        print(f"{root}: {total} files checked, {len(missing)} missing, {len(wrong)} with a wrong size")
        for rel in missing:
            print("missing", rel)
        for rel in wrong:
            print("size   ", rel)
        sys.exit(1 if missing or wrong else 0)
