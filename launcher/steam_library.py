"""Adds the port to the Steam library as a non-Steam game (launcher: Advanced -> STEAM).

Steam lists non-Steam games in <Steam>/userdata/<account>/config/shortcuts.vdf, a binary VDF
file, and shows their library pictures from config/grid/<app id>*.png next to it. Steam reads
the file when it starts and writes its own copy back when it exits, so changes made while Steam
runs are lost: the launcher asks the player to close Steam first.

The file is read and written losslessly: every entry this launcher did not make keeps its exact
bytes, including field types Steam rarely uses. Each write keeps a timestamped backup.

Pictures come only from the player's own game files (sce_sys/pic1.png, pic0.png, icon0.png) and
the launcher's icon; nothing is downloaded. Pictures the player already set are not replaced.

No Tkinter here: the launcher shows the questions and messages, tools/test_steam_library.py
tests the rest with temporary folders.
"""
import binascii
import os
from pathlib import Path
import struct
import subprocess
import time

COLLECTION_NAME = "Uncharted: The Nathan Drake Collection"
BACKUP_SUFFIX = ".uncharted-backup"
KEEP_BACKUPS = 5

# Binary VDF node types. Steam's shortcuts.vdf uses maps, strings and 32-bit numbers; the
# others are read and written back unchanged so an unusual file survives too.
MAP, STRING, INT32, FLOAT32, POINTER, WSTRING, COLOR, UINT64, END, INT64 = 0, 1, 2, 3, 4, 5, 6, 7, 8, 10
FIXED_SIZES = {INT32: 4, FLOAT32: 4, POINTER: 4, COLOR: 4, UINT64: 8, INT64: 8}

# Library picture sizes Steam asks for.
TALL = (600, 900)     # <id>p.png: the library's portrait capsule
WIDE = (920, 430)     # <id>.png: the wide capsule (recent games, Big Picture)
HERO = (1920, 620)    # <id>_hero.png: the banner behind the game page
LOGO_WIDTH = 1280     # <id>_logo.png: the title over the banner (transparent)


class SteamError(Exception):
    """kind: 'no_steam' (no Steam account found) or 'bad_file' (a shortcuts.vdf it cannot read)."""

    def __init__(self, kind, detail=""):
        super().__init__(f"{kind}: {detail}" if detail else kind)
        self.kind, self.detail = kind, detail


# ------------------------------------------------------------------------------------------------
# Binary VDF, kept as nested lists of [type, key, value] so nothing is lost or reordered.
# Keys and strings are decoded as UTF-8 with surrogateescape: any byte sequence comes back as is.

def _cstring(data, pos):
    end = data.index(b"\0", pos)
    return data[pos:end].decode("utf-8", "surrogateescape"), end + 1


def _wstring(data, pos):
    end = pos
    while data[end:end + 2] != b"\0\0":
        end += 2
        if end >= len(data):
            raise ValueError("unterminated wide string")
    return data[pos:end], end + 2


def parse_vdf(data):
    """The nodes of a binary VDF file: a list of [type, key, value]; a map's value is a list."""
    pos = 0

    def nodes():
        nonlocal pos
        result = []
        while True:
            if pos >= len(data):
                raise ValueError("the file ends inside a map")
            kind = data[pos]
            pos += 1
            if kind == END:
                return result
            key, pos = _cstring(data, pos)
            if kind == MAP:
                value = nodes()
            elif kind == STRING:
                value, pos = _cstring(data, pos)
            elif kind == WSTRING:
                value, pos = _wstring(data, pos)
            elif kind in FIXED_SIZES:
                size = FIXED_SIZES[kind]
                if pos + size > len(data):
                    raise ValueError("the file ends inside a number")
                value = struct.unpack_from("<I", data, pos)[0] if kind == INT32 else data[pos:pos + size]
                pos += size
            else:
                raise ValueError(f"unknown VDF type {kind} at byte {pos - 1}")
            result.append([kind, key, value])

    tree = nodes()
    if pos != len(data) and data[pos:].strip(b"\0"):
        raise ValueError(f"unexpected data after byte {pos}")
    return tree


def dump_vdf(tree):
    out = bytearray()

    def write(items):
        for kind, key, value in items:
            out.append(kind)
            out.extend(key.encode("utf-8", "surrogateescape") + b"\0")
            if kind == MAP:
                write(value)
            elif kind == STRING:
                out.extend(value.encode("utf-8", "surrogateescape") + b"\0")
            elif kind == WSTRING:
                out.extend(bytes(value) + b"\0\0")
            elif kind == INT32:
                out.extend(struct.pack("<I", value & 0xFFFFFFFF))
            else:
                out.extend(bytes(value))
        out.append(END)

    write(tree)
    return bytes(out)


def field(entry, name, default=None):
    """A shortcut's value by key; Steam has written both AppName/Exe and appname/exe."""
    for kind, key, value in entry:
        if key.lower() == name.lower():
            return value
    return default


def shortcut_list(tree):
    """The 'shortcuts' map of the file's nodes (made when the file was empty)."""
    for node in tree:
        if node[0] == MAP and node[1].lower() == "shortcuts":
            return node[2]
    node = [MAP, "shortcuts", []]
    tree.append(node)
    return node[2]


# ------------------------------------------------------------------------------------------------
# Shortcuts.

def app_id(exe, name):
    """The id Steam gives a non-Steam game (and names its pictures by): CRC-32 of the quoted
    executable and the name, with the top bit set."""
    return (binascii.crc32((exe + name).encode("utf-8")) & 0xFFFFFFFF) | 0x80000000


def quoted(path):
    return f'"{path}"'


def launch_targets(root, starter, python_exe, script, games):
    """What each Steam entry starts: [(name, exe, start folder, options)].

    The packaged build's starter ("Uncharted Launcher.exe") when it is there, otherwise the
    source tree's launcher script under pythonw. games is None for one Collection entry, or a list
    of (key, name) for one entry per game (the launcher's --play u1/u2/u3)."""
    if Path(starter).is_file():
        exe, base = quoted(starter), ""
    else:
        pythonw = Path(python_exe).with_name("pythonw.exe")
        exe, base = quoted(pythonw if pythonw.is_file() else python_exe), quoted(script)
    start = quoted(root)
    if games is None:
        return [(COLLECTION_NAME, exe, start, base)]
    return [(name, exe, start, f"{base} --play {key}".strip()) for key, name in games]


LAUNCHER_FILES = ("uncharted launcher.exe", "uncharted_launcher.pyw")


def owned(entry, exe, base_options):
    """Whether a shortcut is one of this launcher's: same executable and, from source, the same
    script (so other Python programs' shortcuts are left alone)."""
    entry_exe = str(field(entry, "Exe", "")).strip().strip('"').lower()
    options = str(field(entry, "LaunchOptions", "")).strip().lower()
    base = base_options.lower()
    if entry_exe == exe.strip('"').lower():
        return options.startswith(base) if base else (options == "" or options.startswith("--play"))
    return False


def stale(entry, names):
    """One of this launcher's entries from another folder (the port was moved or switched
    between the package and the source tree): one of our names, started by a launcher file."""
    if str(field(entry, "AppName", "")) not in names:
        return False
    command = (str(field(entry, "Exe", "")) + " " + str(field(entry, "LaunchOptions", ""))).lower()
    return any(name in command for name in LAUNCHER_FILES)


def new_entry(appid, name, exe, start, options, icon):
    """A shortcut with the fields Steam itself writes, in its order."""
    return [[INT32, "appid", appid], [STRING, "AppName", name], [STRING, "Exe", exe],
            [STRING, "StartDir", start], [STRING, "icon", str(icon)], [STRING, "ShortcutPath", ""],
            [STRING, "LaunchOptions", options], [INT32, "IsHidden", 0], [INT32, "AllowDesktopConfig", 1],
            [INT32, "AllowOverlay", 1], [INT32, "OpenVR", 0], [INT32, "Devkit", 0],
            [STRING, "DevkitGameID", ""], [INT32, "DevkitOverrideAppID", 0], [INT32, "LastPlayTime", 0],
            [STRING, "FlatpakAppID", ""], [MAP, "tags", []]]


def set_field(entry, key, kind, value):
    """Sets a shortcut's value, in place when the key is there (any letter case)."""
    for node in entry:
        if node[1].lower() == key.lower():
            node[0], node[2] = kind, value
            return
    entry.append([kind, key, value])


def merge_shortcuts(tree, targets, icon, all_names=()):
    """Puts this launcher's entries (targets from launch_targets) into the file's nodes.

    An entry it made before (same executable and launch options, or one of all_names started by
    a launcher file in another folder) is updated in place: only its id, name, paths, icon and
    options change, so the player's play time, categories, hidden state and any other field
    stay. Its entries of the other kind (per game versus the Collection) and duplicates are
    removed. Every other shortcut is untouched; the entries are numbered 0, 1, 2... again as
    Steam does. Returns (app ids, added, updated, removed)."""
    shortcuts = shortcut_list(tree)
    exe, start = targets[0][1], targets[0][2]
    base = targets[0][3].partition("--play")[0].strip()
    wanted = {options.lower(): (name, options) for name, _, _, options in targets}
    by_name = {name: options.lower() for name, _, _, options in targets}
    names = set(all_names) | set(by_name)
    kept, done = [], set()
    added = updated = removed = 0
    for node in shortcuts:
        entry = node[2] if node[0] == MAP else None
        if entry is not None and owned(entry, exe, base):
            options = str(field(entry, "LaunchOptions", "")).strip().lower()
        elif entry is not None and stale(entry, names):
            options = by_name.get(str(field(entry, "AppName", "")))  # None: the other kind
        else:
            kept.append(node)
            continue
        if options not in wanted or options in done:
            removed += 1
            continue
        name, real_options = wanted[options]
        for key, kind, value in (("appid", INT32, app_id(exe, name)), ("AppName", STRING, name),
                                 ("Exe", STRING, exe), ("StartDir", STRING, start), ("icon", STRING, str(icon)),
                                 ("LaunchOptions", STRING, real_options)):
            set_field(entry, key, kind, value)
        kept.append(node)
        done.add(options)
        updated += 1
    for name, _, _, options in targets:
        if options.lower() not in done:
            kept.append([MAP, "", new_entry(app_id(exe, name), name, exe, start, options, icon)])
            added += 1
    ids = [app_id(exe, name) for name, _, _, _ in targets]
    for number, node in enumerate(kept):
        node[1] = str(number)
    shortcuts[:] = kept
    return ids, added, updated, removed


# ------------------------------------------------------------------------------------------------
# Steam's folders.

def steam_running():
    """Whether steam.exe runs (Windows); False when that cannot be told."""
    try:
        result = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"], capture_output=True,
                                text=True, errors="replace",
                                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0), timeout=15)
    except (OSError, subprocess.SubprocessError):
        return False
    return "steam.exe" in result.stdout.lower()


def steam_roots():
    """Steam installation folders: the registry's first, then the usual places."""
    roots = []
    try:
        import winreg
        for hive, path, name in ((winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                                 (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
                                 (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath")):
            try:
                with winreg.OpenKey(hive, path) as key:
                    roots.append(Path(winreg.QueryValueEx(key, name)[0]))
            except OSError:
                pass
    except ImportError:
        pass
    for folder in ("ProgramFiles(x86)", "ProgramFiles"):
        if os.environ.get(folder):
            roots.append(Path(os.environ[folder]) / "Steam")
    unique, seen = [], set()
    for root in roots:
        key = os.path.normcase(os.path.abspath(root))
        if key not in seen:
            seen.add(key)
            unique.append(root)
    return unique


def account_names(root):
    """{account folder id: persona name} from config/loginusers.vdf (text VDF); {} if unreadable."""
    import re
    try:
        text = (Path(root) / "config" / "loginusers.vdf").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return {}
    names = {}
    for steam_id, body in re.findall(r'"(\d{17})"\s*\{([^{}]*)\}', text):
        persona = re.search(r'"PersonaName"\s*"([^"]*)"', body)
        names[str(int(steam_id) & 0xFFFFFFFF)] = persona.group(1) if persona else ""
    return names


def find_accounts(roots=None):
    """[(userdata/<id> folder, persona name)] of every Steam account that used this PC."""
    found, seen = [], set()
    for root in roots if roots is not None else steam_roots():
        userdata = Path(root) / "userdata"
        if not userdata.is_dir():
            continue
        names = account_names(root)
        for folder in sorted(userdata.iterdir()):
            if not (folder.is_dir() and folder.name.isdigit() and folder.name != "0"):
                continue
            real = os.path.normcase(str(folder.resolve()))
            if real not in seen:
                seen.add(real)
                found.append((folder, names.get(folder.name, "")))
    return found


# ------------------------------------------------------------------------------------------------
# Library pictures from the player's game files.

def _fill(image, size, focus=(0.5, 0.5)):
    """image scaled to cover size and cropped around focus (0..1 on each axis)."""
    from PIL import Image
    scale = max(size[0] / image.width, size[1] / image.height)
    image = image.resize((max(size[0], round(image.width * scale)), max(size[1], round(image.height * scale))),
                         Image.LANCZOS)
    left = round((image.width - size[0]) * focus[0])
    top = round((image.height - size[1]) * focus[1])
    return image.crop((left, top, left + size[0], top + size[1]))


def _soft_paste(base, picture, at, fade):
    """picture pasted on base at `at`, its edges blending into base over `fade` pixels."""
    from PIL import Image, ImageDraw
    mask = Image.new("L", picture.size, 255)
    draw = ImageDraw.Draw(mask)
    for i in range(fade):
        draw.rectangle((i, i, picture.width - 1 - i, picture.height - 1 - i), outline=round(255 * i / fade))
    base.paste(picture, at, mask)


def title_logo(cover):
    """The Collection's title lettering cut out of the PS4 cover (icon0.png): dark ink on the
    parchment becomes opaque, the parchment transparent."""
    from PIL import Image
    w, h = cover.size
    area = cover.crop((round(w * 0.06), round(h * 0.47), round(w * 0.94), round(h * 0.74)))
    alpha = area.convert("L").point(lambda v: 255 if v <= 60 else 0 if v >= 110 else round(255 * (110 - v) / 50))
    logo = area.convert("RGBA")
    logo.putalpha(alpha)
    box = alpha.getbbox()
    logo = logo.crop(box) if box else logo
    return logo.resize((LOGO_WIDTH, round(logo.height * LOGO_WIDTH / logo.width)), Image.LANCZOS)


def make_pictures(game_dir):
    """{suffix: image} for the library from the game's sce_sys pictures; {} without them.

    Suffixes: 'p' portrait capsule, '' wide capsule, '_hero', '_logo'."""
    from PIL import Image
    sce = Path(game_dir) / "sce_sys"
    background = next((sce / n for n in ("pic1.png", "pic0.png") if (sce / n).is_file()), None)
    cover = sce / "icon0.png"
    pictures = {}
    art = Image.open(background).convert("RGB") if background else None
    box = Image.open(cover).convert("RGB") if cover.is_file() else None
    if art is not None:
        pictures["_hero"] = _fill(art, HERO, (0.5, 0.25))
    if box is not None:
        # The cover on plain parchment (the empty left of the background picture) so Drake shows once.
        paper = art.crop((0, 0, round(art.width * 0.3), art.height)) if art is not None else box.crop((0, 0, 8, 8))
        tall = _fill(paper, TALL)
        _soft_paste(tall, box.resize((TALL[0], TALL[0]), Image.LANCZOS), (0, 210), 130)
        pictures["p"] = tall
        wide = _fill(paper, WIDE)
        _soft_paste(wide, box.resize((WIDE[1], WIDE[1]), Image.LANCZOS), ((WIDE[0] - WIDE[1]) // 2, 0), 100)
        pictures[""] = wide
        pictures["_logo"] = title_logo(box)
    return pictures


def save_pictures(grid, ids, pictures):
    """Writes the pictures for each app id into grid/, keeping any picture already there (the
    player's own choice, in any format). Returns the number of files written."""
    grid = Path(grid)
    grid.mkdir(parents=True, exist_ok=True)
    written = 0
    for appid in ids:
        for suffix, image in pictures.items():
            stem = f"{appid}{suffix}"
            if any(grid.glob(stem + ".*")):
                continue
            image.save(grid / f"{stem}.png")
            written += 1
    return written


# ------------------------------------------------------------------------------------------------
# Writing.

def backup(file):
    """Copies shortcuts.vdf to shortcuts.vdf.<time>.uncharted-backup; keeps the newest few."""
    file = Path(file)
    if not file.is_file():
        return None
    copy = file.with_name(f"{file.name}.{time.strftime('%Y%m%d-%H%M%S')}{BACKUP_SUFFIX}")
    number = 1
    while copy.exists():
        number += 1
        copy = file.with_name(f"{file.name}.{time.strftime('%Y%m%d-%H%M%S')}-{number}{BACKUP_SUFFIX}")
    copy.write_bytes(file.read_bytes())
    old = sorted(file.parent.glob(f"{file.name}.*{BACKUP_SUFFIX}"), key=lambda p: p.stat().st_mtime)
    for stale in old[:-KEEP_BACKUPS]:
        try:
            stale.unlink()
        except OSError:
            pass
    return copy


def update_account(account, targets, icon, pictures, all_names=()):
    """Adds or updates this launcher's entries in one account's shortcuts.vdf (backed up first)
    and writes the pictures. Returns a dict for the launcher's message."""
    config = Path(account) / "config"
    file = config / "shortcuts.vdf"
    data = file.read_bytes() if file.is_file() else b""
    try:
        tree = parse_vdf(data) if data.strip(b"\0") else []
    except (ValueError, IndexError) as error:
        raise SteamError("bad_file", f"{file}: {error}")
    ids, added, updated, removed = merge_shortcuts(tree, targets, icon, all_names)
    config.mkdir(parents=True, exist_ok=True)
    saved = backup(file)
    temporary = file.with_name(file.name + ".writing")
    temporary.write_bytes(dump_vdf(tree))
    os.replace(temporary, file)
    pictures_written = save_pictures(config / "grid", ids, pictures) if pictures else 0
    return {"file": file, "backup": saved, "ids": ids, "added": added, "updated": updated,
            "removed": removed, "pictures": pictures_written}


def add_to_steam(targets, icon, game_dir, accounts=None, all_names=()):
    """Writes the entries to every Steam account found (accounts: [(folder, name)] to use instead).
    Returns [(folder, name, result)]; raises SteamError('no_steam') when there is none.
    Every file is read and checked before the first one is written."""
    accounts = find_accounts() if accounts is None else accounts
    if not accounts:
        raise SteamError("no_steam")
    for folder, _ in accounts:
        file = Path(folder) / "config" / "shortcuts.vdf"
        if file.is_file() and file.read_bytes().strip(b"\0"):
            try:
                parse_vdf(file.read_bytes())
            except (ValueError, IndexError) as error:
                raise SteamError("bad_file", f"{file}: {error}")
    try:
        pictures = make_pictures(game_dir) if game_dir else {}
    except OSError:
        pictures = {}
    return [(folder, name, update_account(folder, targets, icon, pictures, all_names))
            for folder, name in accounts]
