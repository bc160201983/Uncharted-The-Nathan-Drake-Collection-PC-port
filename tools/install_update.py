"""Installs a game update from its PKG file (launcher -> Advanced -> Install game update; #32).

The update the player has (for example update 1.02's .pkg) is unpacked next to the game folder as
`<game folder>-patch`, where tools/probe_uncharted.py finds it. The port ships no game files: this
only unpacks a file the player supplies, with PkgTool (LibOrbisPkg, LGPL-3.0, tools/pkgtool).

    python tools/install_update.py <update.pkg> <game folder>
"""
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "pkgtool" / "PkgTool.exe"
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
# The update's metadata outside its file system (the PKG's entries), as sce_sys files.
METADATA = {"PARAM_SFO": "param.sfo", "PLAYGO_CHUNK_DAT": "playgo-chunk.dat",
            "PLAYGO_CHUNK_SHA": "playgo-chunk.sha", "PLAYGO_MANIFEST_XML": "playgo-manifest.xml",
            "SHAREPARAM_JSON": "shareparam.json", "CHANGEINFO__CHANGEINFO_XML": "changeinfo/changeinfo.xml"}
# What the port needs from update 1.02: the three games' executables and their new data.
REQUIRED = ("eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf", "sce_sys/param.sfo",
            "u1data/data.psarc", "u2data/data.psarc", "u3data/data.psarc",
            "u1data/build/main/bin.psarc", "u2data/build/main/bin.psarc", "u3data/build/main/bin.psarc")
UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


class UpdateError(Exception):
    """A reason the update can't be installed, worded for the player."""


def sfo_values(path):
    """The entries of a param.sfo (strings and integers)."""
    try:
        return _sfo_values(path)
    except UpdateError:
        raise
    except (struct.error, ValueError, IndexError, OSError) as error:
        # Truncated or corrupt file: tell the player instead of failing silently.
        raise UpdateError(f"The update's param.sfo is damaged ({error}).") from error


def _sfo_values(path):
    data = Path(path).read_bytes()
    magic, _, keys, values, count = struct.unpack_from("<IIIII", data)
    if magic != 0x46535000:
        raise UpdateError("The update's param.sfo is damaged.")
    found = {}
    for i in range(count):
        key_at, kind, length, _, value_at = struct.unpack_from("<HHIII", data, 20 + i * 16)
        key = data[keys + key_at:data.index(bytes(1), keys + key_at)].decode("ascii", "replace")
        raw = data[values + value_at:values + value_at + length]
        found[key] = struct.unpack("<I", raw[:4])[0] if kind == 0x0404 else \
            raw.rstrip(bytes(1)).decode("utf-8", "replace")
    return found


def run_tool(*arguments):
    if not TOOL.is_file():
        raise UpdateError(f"PkgTool is missing ({TOOL}). Reinstall the port.")
    try:
        result = subprocess.run([str(TOOL), *map(str, arguments)], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", creationflags=NO_WINDOW)
    except OSError as error:
        raise UpdateError(f"PkgTool could not start: {error}. It needs the .NET Framework of Windows "
                          f"10/11.") from error
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()
        raise UpdateError("PkgTool could not read this file" + (f": {detail[-1]}" if detail else "."))
    return result.stdout


def entries(pkg):
    """name -> (index, size, encrypted) of the PKG's entries (PkgTool pkg_listentries)."""
    rows = {}
    for line in run_tool("pkg_listentries", pkg).splitlines():
        row = re.match(r"^(0x[\da-fA-F]+)\s+(0x[\da-fA-F]+)\s+([\da-fA-F]{8})\s+(\d+)\s+(?:\d+\s+)?(\S+)$",
                       line.strip())
        if row:
            rows[row[5]] = (row[4], int(row[2], 16), bool(int(row[3], 16) & 0x80000000))
    return rows


def title_folder(game):
    game = Path(game)
    return game.parent if game.name.lower() == "uroot" else game


def existing_update(game):
    """The update folder already next to the game, if any (found as the start script finds it)."""
    title = title_folder(game)
    names = {title.name.lower() + suffix for suffix in UPDATE_SUFFIXES}
    try:
        return next((entry for entry in title.parent.iterdir()
                     if entry.name.lower() in names and entry.is_dir()), None)
    except OSError:
        return None


def psarc_names(path):
    """The file names a PSARC archive lists (its manifest, the first entry), or None when the file
    isn't a readable PSARC."""
    import zlib
    try:
        with open(path, "rb") as f:
            magic, _, _, compression, toc_length, entry_size, count, block_size, _ = struct.unpack(
                ">4sHH4sIIIII", f.read(32))
            if magic != b"PSAR" or count < 1 or entry_size < 30 or toc_length < 32 + count * entry_size:
                return None
            toc = f.read(toc_length - 32)
            block, size, offset = (struct.unpack(">I", toc[16:20])[0], int.from_bytes(toc[20:25], "big"),
                                   int.from_bytes(toc[25:30], "big"))
            width = 2 if block_size <= 0x10000 else 3 if block_size <= 0x1000000 else 4
            sizes = toc[count * entry_size:]
            # The archive is its table of contents followed by every stored block: a file cut short
            # (or grown) is damaged even when its first entries still read.
            stored = sum(int.from_bytes(sizes[i:i + width], "big") or block_size
                         for i in range(0, len(sizes) - width + 1, width))
            if Path(path).stat().st_size != toc_length + stored:
                return None
            f.seek(offset)
            manifest = bytearray()
            while len(manifest) < size:
                stored = int.from_bytes(sizes[block * width:(block + 1) * width], "big") or block_size
                chunk = f.read(stored)
                if not chunk:
                    return None
                if compression == b"zlib" and len(chunk) < block_size and chunk[:1] == b"\x78":
                    chunk = zlib.decompress(chunk)
                manifest += chunk
                block += 1
            return set(bytes(manifest[:size]).decode("utf-8", "replace").split("\n"))
    except (OSError, struct.error, zlib.error, IndexError):
        return None


def check_update(folder):
    """What is wrong with `folder` (an update's uroot) for the port: missing files, and archives the
    games can't read. #43: a bin.psarc the game couldn't use made all three games stop at start
    ("files.json -> not found", ASSERTION: root in ingame-shaders.cpp)."""
    folder = Path(folder)
    problems = [name for name in REQUIRED if not (folder / name).is_file()]
    for game in ("u1data", "u2data", "u3data"):
        archive = folder / game / "build" / "main" / "bin.psarc"
        if not archive.is_file():
            continue
        names = psarc_names(archive)
        if names is None:
            problems.append(f"{game}/build/main/bin.psarc (damaged: not a readable archive)")
        elif "bin/shaders/src/files.json" not in names:
            problems.append(f"{game}/build/main/bin.psarc (incomplete: no shader list)")
    return problems


def inspect(pkg, game):
    """(update values, game values) after checking that `pkg` is an update for the game in `game`."""
    listing = entries(pkg)
    if "PARAM_SFO" not in listing:
        raise UpdateError("This file is not a game update (it has no param.sfo).")
    index, _, encrypted = listing["PARAM_SFO"]
    if encrypted:
        raise UpdateError("This PKG is encrypted (a store PKG). Use a decrypted (fake) PKG of the update.")
    with tempfile.TemporaryDirectory() as temporary:
        sfo = Path(temporary) / "param.sfo"
        run_tool("pkg_extractentry", pkg, index, sfo)
        update = sfo_values(sfo)
    game_sfo = Path(game) / "sce_sys" / "param.sfo"
    if not game_sfo.is_file():
        raise UpdateError("Choose the game folder first (the one with eboot.bin and sce_sys).")
    base = sfo_values(game_sfo)
    if update.get("CATEGORY") != "gp":
        raise UpdateError(f"This PKG is not an update but '{update.get('TITLE', '?')}' "
                          f"(category {update.get('CATEGORY', '?')}).")
    if update.get("TITLE_ID") != base.get("TITLE_ID"):
        raise UpdateError(f"This update is for {update.get('TITLE_ID', '?')}, your game is "
                          f"{base.get('TITLE_ID', '?')}. Use the update of the same region.")
    return update, base


def check_folder(folder, game):
    """An update the player already extracted, anywhere (the launcher remembers it and passes it as
    UNCHARTED_UPDATE): its uroot, after the same checks as a PKG's. The folder chosen may be the
    update's root or its uroot."""
    folder = Path(folder)
    root = next((candidate for candidate in (folder / "uroot", folder)
                 if (candidate / "eboot.bin").is_file() and (candidate / "sce_sys" / "param.sfo").is_file()),
                None)
    if root is None:
        raise UpdateError("This folder is not an extracted game update: it has no eboot.bin and "
                          "sce_sys\\param.sfo (in it or in its uroot folder).")
    game_sfo = Path(game) / "sce_sys" / "param.sfo"
    if not game_sfo.is_file():
        raise UpdateError("Choose the game folder first (the one with eboot.bin and sce_sys).")
    if root.resolve() == Path(game).resolve():
        raise UpdateError("That is the game folder itself. Choose the update's folder.")
    update, base = sfo_values(root / "sce_sys" / "param.sfo"), sfo_values(game_sfo)
    if update.get("CATEGORY") != "gp":
        raise UpdateError(f"This folder holds a game ('{update.get('TITLE', '?')}', version "
                          f"{update.get('APP_VER', '?')}), not an update.")
    if update.get("TITLE_ID") != base.get("TITLE_ID"):
        raise UpdateError(f"This update is for {update.get('TITLE_ID', '?')}, your game is "
                          f"{base.get('TITLE_ID', '?')}. Use the update of the same region.")
    missing = check_update(root)
    if missing:
        raise UpdateError("The update folder has missing or damaged files (extract the update "
                          "again): " + ", ".join(missing))
    return root


def install(pkg, game, log=print):
    """Unpacks the update `pkg` next to the game folder `game` as <game folder>-patch (an earlier
    update folder there is replaced); returns its uroot."""
    pkg, game = Path(pkg), Path(game)
    update, _ = inspect(pkg, game)
    title = title_folder(game)
    target = existing_update(game) or title.parent / f"{title.name}-patch"
    need = int(pkg.stat().st_size * 2.3) + (1 << 28)
    free = shutil.disk_usage(title.parent).free
    if free < need:
        raise UpdateError(f"Not enough free space on that drive: {need / 2**30:.1f} GB needed, "
                          f"{free / 2**30:.1f} GB free.")
    partial = title.parent / f"{title.name}-patch.partial"
    shutil.rmtree(partial, ignore_errors=True)
    try:
        log(f"Unpacking update {update.get('APP_VER', '?')}...")
        run_tool("pkg_extract", pkg, partial)
        uroot = partial / "uroot"
        if not uroot.is_dir():
            raise UpdateError("The PKG held no game files (no uroot folder).")
        log("Adding the update's metadata...")
        listing = entries(pkg)
        for name, relative in METADATA.items():
            if name in listing and not listing[name][2]:
                destination = uroot / "sce_sys" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                run_tool("pkg_extractentry", pkg, listing[name][0], destination)
        missing = check_update(uroot)
        if missing:
            raise UpdateError("The update has missing or damaged files: " + ", ".join(missing))
        log("Putting it in place...")
        old = None
        if target.exists():
            old = target.with_name(target.name + ".old")
            shutil.rmtree(old, ignore_errors=True)
            target.rename(old)
        try:
            partial.rename(target)
        except OSError:
            if old:
                old.rename(target)  # the earlier update stays as it was
            raise
        if old:
            shutil.rmtree(old, ignore_errors=True)
    except BaseException:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    return target / "uroot"


# The base game (version 1.00) from its PKG: the three executables and their data.
BASE_REQUIRED = ("eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf", "sce_sys/param.sfo",
                 "u1data", "u2data", "u3data")


def install_game(pkg, parent, log=print):
    """Unpacks the player's own base game PKG (for example the US copy, CUSA02320) into
    <parent>/<title id> and returns its uroot, the folder the launcher uses as game folder. PkgTool
    leaves out the PKG's metadata entries (param.sfo, playgo-chunk.dat, ...), which the launcher and
    the update installer need, so they are added to sce_sys as for an update (20 Oct 2026: a base
    unpacked by hand had none, and Install from .pkg refused it)."""
    pkg, parent = Path(pkg), Path(parent)
    listing = entries(pkg)
    if "PARAM_SFO" not in listing:
        raise UpdateError("This file is not a PS4 game package (it has no param.sfo).")
    index, _, encrypted = listing["PARAM_SFO"]
    if encrypted:
        raise UpdateError("This PKG is encrypted (a store PKG). Use a decrypted (fake) PKG of the game.")
    with tempfile.TemporaryDirectory() as temporary:
        sfo = Path(temporary) / "param.sfo"
        run_tool("pkg_extractentry", pkg, index, sfo)
        values = sfo_values(sfo)
    if str(values.get("CATEGORY", "")).startswith("gp"):
        raise UpdateError("This is a game update, not the game. Install it under GAME UPDATE -> "
                          "Install from .pkg once the game is installed.")
    title = str(values.get("TITLE_ID") or pkg.stem).strip()
    target = parent / title
    if (target / "uroot" / "eboot.bin").is_file() or (target / "eboot.bin").is_file():
        raise UpdateError(f"The game is there already:\n{target}\n\nChoose it with Change game folder.")
    need = int(pkg.stat().st_size * 1.45) + (1 << 30)
    free = shutil.disk_usage(parent).free
    if free < need:
        raise UpdateError(f"Not enough free space on that drive: {need / 2**30:.0f} GB needed, "
                          f"{free / 2**30:.0f} GB free.")
    partial = parent / f"{title}.partial"
    shutil.rmtree(partial, ignore_errors=True)
    try:
        log(f"Unpacking {values.get('TITLE', 'the game')} ({title}), a few minutes...")
        run_tool("pkg_extract", pkg, partial)
        uroot = partial / "uroot"
        if not uroot.is_dir():
            raise UpdateError("The PKG held no game files (no uroot folder).")
        log("Adding the game's metadata...")
        for name, relative in METADATA.items():
            if name in listing and not listing[name][2]:
                destination = uroot / "sce_sys" / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                run_tool("pkg_extractentry", pkg, listing[name][0], destination)
        missing = [name for name in BASE_REQUIRED if not (uroot / name).exists()]
        if missing:
            raise UpdateError("This PKG is not The Nathan Drake Collection, or it is damaged: "
                              "missing " + ", ".join(missing))
        log("Putting it in place...")
        partial.rename(target)
    except BaseException:
        shutil.rmtree(partial, ignore_errors=True)
        raise
    return target / "uroot"


if __name__ == "__main__":
    # python tools/install_update.py --game <game.pkg> <destination folder>: the base game.
    if len(sys.argv) == 4 and sys.argv[1] == "--game":
        try:
            print(f"Installed: {install_game(sys.argv[2], sys.argv[3])}")
        except UpdateError as error:
            sys.exit(str(error))
        sys.exit(0)
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    try:
        print(f"Installed: {install(sys.argv[1], sys.argv[2])}")
    except UpdateError as error:
        sys.exit(str(error))
