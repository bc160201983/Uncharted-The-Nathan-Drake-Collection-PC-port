#!/usr/bin/env python3
"""Uncharted: The Nathan Drake Collection - launcher (Windows, Tkinter + Pillow).

Pick a game, set display and performance options (presets for low-end and high-end PCs) and play.
Runtime options are kept in analysis/uncharted-user/probe.ini, the same file the in-game menu
(Insert / R3+L2) reads and writes; launcher-only options in %APPDATA%/Uncharted Launcher (under
Wine/Proton in ~/.local/share/uncharted-launcher, the same for every prefix).
The game is started through tools/probe_uncharted.py (preparation, code patches, Collection game
switches), with the options below passed as environment variables.
"""
import ctypes
import json
import os
from pathlib import Path
import queue
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import tkinter as tk
import tkinter.font as tkfont
import webbrowser
from tkinter import messagebox

from PIL import Image, ImageDraw, ImageTk

try:  # launcher/net_lan.py: Uncharted 2 games hosted on this network (Multiplayer page). Optional:
    import net_lan  # without it (0.6.1's package missed it, #208) the launcher must still open
except ImportError:
    net_lan = None
import updater  # launcher/updater.py: updates from the GitHub releases

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "launcher" / "assets"
DEFAULT_GAME_DIR = ROOT / "game" / "CUSA02344" / "uroot"
STARTER_EXE = ROOT / "Uncharted Launcher.exe"  # the packaged build's starter (tools/package_windows.py)
USER_DIR = ROOT / "analysis" / "uncharted-user"
INI_FILE = USER_DIR / "probe.ini"
RUN_SCRIPT = ROOT / "tools" / "probe_uncharted.py"
PREFIX_CONFIG_FILE = Path(os.environ.get("APPDATA", Path.home())) / "Uncharted Launcher" / "launcher.json"
# Uncharted 2's net games for the Multiplayer page (the co-op research's *net-games* table).
NET_GAMES_FILE = ROOT / "launcher" / "net_games.json"


def wine_kernel32():
    """kernel32 when the launcher runs under Wine (Proton, Lutris, CrossOver), else None."""
    try:
        if hasattr(ctypes.windll.ntdll, "wine_get_version"):
            return ctypes.windll.kernel32
    except (AttributeError, OSError):
        pass
    return None


WINE = wine_kernel32()


def slow_drive_reason(path):
    """Why the drive holding `path` makes the games freeze, or None (20 Oct 2026). On a
    laptop the port and game on an external USB drive (an NVMe SSD in a USB case, exFAT) froze
    for 0.2-0.8 s dozens of times a minute while the port wrote its shader cache, logs and saves
    (Windows writes to such drives without a cache); on the internal NVMe drive: no freeze over
    100 ms. Reports USB, SD/MMC and removable drives and FAT/exFAT volumes; None elsewhere, under
    Wine and for network paths."""
    if os.name != "nt" or WINE:
        return None
    drive = os.path.splitdrive(str(Path(path).resolve()))[0]
    if len(drive) != 2 or drive[1] != ":":
        return None
    kernel32 = ctypes.windll.kernel32
    reasons = []
    fs_name = ctypes.create_unicode_buffer(32)
    if kernel32.GetVolumeInformationW(drive + "\\", None, 0, None, None, None, fs_name, len(fs_name)):
        if fs_name.value.upper() in ("EXFAT", "FAT32", "FAT"):
            reasons.append(f"{fs_name.value}-formatted")
    if kernel32.GetDriveTypeW(drive + "\\") == 2:  # DRIVE_REMOVABLE
        reasons.append("removable")
    # The bus of the disk (IOCTL_STORAGE_QUERY_PROPERTY, StorageDeviceProperty): USB enclosures
    # report themselves as fixed drives.
    kernel32.CreateFileW.restype = ctypes.c_void_p
    handle = kernel32.CreateFileW("\\\\.\\" + drive, 0, 3, None, 3, 0, None)  # share read|write, OPEN_EXISTING
    if handle and handle != ctypes.c_void_p(-1).value:
        try:
            query = struct.pack("<II4x", 0, 0)  # StorageDeviceProperty, PropertyStandardQuery
            out = ctypes.create_string_buffer(1024)
            returned = ctypes.c_ulong(0)
            if kernel32.DeviceIoControl(ctypes.c_void_p(handle), 0x2D1400, query, len(query), out,
                                        len(out), ctypes.byref(returned), None) and returned.value >= 32:
                bus = struct.unpack_from("<I", out.raw, 28)[0]
                names = {7: "USB", 12: "SD card", 13: "MMC card", 4: "FireWire"}
                if bus in names:
                    reasons.insert(0, names[bus])
        finally:
            kernel32.CloseHandle(ctypes.c_void_p(handle))
    if not reasons:
        return None
    return f"{drive} is a {', '.join(dict.fromkeys(reasons))} drive"


def _wine_string(pointer, wide):
    """A string Wine returned in memory of the process heap, which is freed."""
    if not pointer:
        return None
    try:
        return ctypes.wstring_at(pointer) if wide else ctypes.string_at(pointer).decode("utf-8", "replace")
    finally:
        WINE.GetProcessHeap.restype = ctypes.c_void_p
        WINE.HeapFree.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
        WINE.HeapFree(WINE.GetProcessHeap(), 0, pointer)


def unix_path(path):
    """The Linux path of a Windows path under Wine, else None."""
    if not WINE:
        return None
    function = WINE.wine_get_unix_file_name
    function.argtypes, function.restype = [ctypes.c_wchar_p], ctypes.c_void_p
    return _wine_string(function(str(path)), wide=False)


def windows_path(path):
    """The Windows path of a Linux path under Wine (in this prefix's drive letters), else None."""
    if not WINE or not path:
        return None
    function = WINE.wine_get_dos_file_name
    function.argtypes, function.restype = [ctypes.c_char_p], ctypes.c_void_p
    return _wine_string(function(path.encode("utf-8")), wide=True)


def shared_config_file():
    """Under Wine: launcher.json in the Linux user's data folder. Each Steam shortcut has a prefix
    (and an %APPDATA%) of its own, so a game started from its shortcut (--play) would not see the
    launcher's options; this folder is the same for all of them, and outside the port's folder."""
    home = os.environ.get("HOME", "")
    data = os.environ.get("XDG_DATA_HOME") or (home + "/.local/share" if home.startswith("/") else "")
    folder = windows_path(data) if data.startswith("/") else None
    if not folder and os.environ.get("WINEHOMEDIR", "").startswith("\\??\\"):
        folder = str(Path(os.environ["WINEHOMEDIR"][4:]) / ".local" / "share")  # set by Wine itself
    return Path(folder) / "uncharted-launcher" / "launcher.json" if folder else None


CONFIG_FILE = (WINE and shared_config_file()) or PREFIX_CONFIG_FILE
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
APP_TITLE = "Uncharted: The Nathan Drake Collection"
# Credits shown on the Advanced page: the anti-aliasing technologies.
CREDITS = ("Credits: DLSS and DLAA use NVIDIA DLSS technology. NVIDIA, RTX and DLSS are trademarks of "
           "NVIDIA Corporation. AMD FidelityFX Super Resolution (FSR 3.1, FSR 4) by AMD. Uncharted camera "
           "motion for anti-aliasing after SuperMedo's Uncharted port.")
# The author's links in the launcher header: label, address, color, hover color.
LINKS = (("Patreon", "https://www.patreon.com/c/bilalreloaded", "#f96854", "#ff7f6e"),
         ("YouTube", "https://www.youtube.com/@BilalReloaded/videos", "#d6302a", "#ef4038"),
         ("GitHub", "https://github.com/bc160201983", "#24292f", "#3a4048"))

def multiplayer_preview():
    """The Multiplayer page (Uncharted 2 over the network, 25 Oct 2026) is hidden until it
    works: UNCHARTED_MULTIPLAYER=1 or a file named multiplayer-preview in analysis\\uncharted-user
    shows it."""
    return os.environ.get("UNCHARTED_MULTIPLAYER") == "1" or (USER_DIR / "multiplayer-preview").exists()


# The characters a player can pick on the Multiplayer page (actor, label): Uncharted 2's
# multiplayer skins that ship in the Collection, and Drake's own models. Only those whose actor is
# in the player's game files are offered (net_characters); "" = the net game's default model.
NET_CHARACTERS = [
    ("", "Default (Drake)"), ("hero-default", "Drake"), ("hero-parka", "Drake (winter)"),
    ("mp-hero-chloe-variation", "Chloe"), ("mp-hero-elena", "Elena"), ("mp-hero-elena-divesuit", "Elena (dive suit)"),
    ("mp-hero-divesuit", "Drake (dive suit)"), ("mp-hero-sullivan-blue", "Sully (blue)"),
    ("mp-hero-sullivan-red", "Sully (red)"), ("mp-hero-sullivan-winter", "Sully (winter)"),
    ("mp-hero-eddie", "Eddy Raja"), ("mp-hero-navarro", "Navarro"), ("mp-hero-roman", "Roman"),
    ("mp-hero-red", "Harry Flynn"), ("mp-hero-pirate", "Pirate"), ("mp-hero-merc", "Mercenary"),
    ("mp-hero-udf", "UDF soldier"), ("mp-hero-skeleton-1-glow", "Skeleton (glow)"),
    ("mp-hero-skeleton-1-platinum", "Skeleton (platinum)"),
]


def net_characters(game_dir):
    """NET_CHARACTERS whose actor package is in the game folder (all when it is not known)."""
    actors = Path(game_dir) / "u2data" / "build" / "main" / "actor27" / "ps4" if game_dir else None
    if not actors or not actors.is_dir():
        return list(NET_CHARACTERS)
    return [(a, label) for a, label in NET_CHARACTERS if not a or (actors / (a + ".pak")).exists()]


def net_games():
    """The net games the Multiplayer page offers (index = the game's *net-games* entry, label):
    the complete entries of launcher/net_games.json, else The Village co-op."""
    games = []
    try:
        data = json.loads(NET_GAMES_FILE.read_text(encoding="utf-8"))
        for entry in data.get("games", []) if isinstance(data, dict) else data:
            if entry.get("complete") and isinstance(entry.get("index"), int):
                mode = {"coop": "Co-op"}.get(entry.get("kind"), (entry.get("mode") or "").title())
                label = " - ".join(part for part in (entry.get("map") or entry.get("name"), mode) if part)
                games.append({"index": entry["index"], "label": label or f"Net game {entry['index']}"})
    except (OSError, ValueError, AttributeError, TypeError):
        pass
    return games or [{"index": 114, "label": "The Village - Co-op"}]


GAMES = [
    {"key": "u1", "number": "1", "title": "DRAKE'S FORTUNE", "name": "Uncharted: Drake's Fortune", "subtitle": "Uncharted 1  •  double-click to play",
     "exe": "eboot.bin", "log": "uncharted"},
    {"key": "u2", "number": "2", "title": "AMONG THIEVES", "name": "Uncharted 2: Among Thieves", "subtitle": "Uncharted 2  •  double-click to play",
     "exe": "big2-ps4_Shipping.elf", "log": "big2-ps4_Shipping"},
    {"key": "u3", "number": "3", "title": "DRAKE'S DECEPTION", "name": "Uncharted 3: Drake's Deception", "subtitle": "Uncharted 3  •  double-click to play",
     "exe": "big3-ps4_Shipping.elf", "log": "big3-ps4_Shipping"},
]

# Palette: the Collection's parchment and gold over a dark leather brown.
BG = "#120e0a"
PANEL = "#1b1510"
PANEL_HI = "#251d16"
LINE = "#3a2e22"
TEXT = "#f1e6cf"
MUTED = "#a8977a"
DIM = "#6f604b"
GOLD = "#d4a24c"
GOLD_HI = "#e9bc66"
INK = "#2a1c0e"
STOP_RED = "#b5533c"

# At most 120: faster, the game's movement timing breaks (Drake's Fortune at 140-160 FPS: running
# slows, Nate fell through the world). The runtime caps older, faster settings at 120 too.
MAX_FPS = 120
FPS_LIMITS = [(30, "30"), (40, "40"), (45, "45"), (50, "50"), (55, "55"), (60, "60"), (75, "75"),
              (90, "90"), (100, "100"), (MAX_FPS, str(MAX_FPS))]

# Runtime options in probe.ini (key -> default) and launcher-only ones.
INI_DEFAULTS = {
    "display_mode": "windowed", "vsync": "1", "fps_limit": "60", "frames_queued": "1",
    "async_shaders": "1", "fsr1": "0", "rcas": "1", "rcas_strength": "75", "show_fps": "1",
    "upscaler": "off",
    # The performance counter (Display tab): the items on its line, its corner, layout, text size
    # and background opacity. The runtime's defaults; the in-game Overlay tab writes them too.
    # Items as the runtime starts (DefaultItems: FPS, frame time, upscaler) until the game has
    # written its ini once.
    "overlay_fps": "1", "overlay_frametime": "1", "overlay_gpu": "0", "overlay_cpu": "0",
    "overlay_ram": "0", "overlay_vram": "0", "overlay_upscaler": "1", "overlay_corner": "1",
    "overlay_layout": "0", "overlay_scale": "100", "overlay_opacity": "50",
}
# Cheats (Advanced page): name, title, hint. tools/probe_uncharted.py turns on the games' own
# switches for them (CHEATS there).
CHEATS = (("invincible", "Invincible", "Nate takes no damage. All three games."),
          ("ammo", "Infinite ammo", "Ammo never runs out. All three games."),
          ("noreload", "No reload", "The magazine never empties. Uncharted 2 and 3."),
          ("grenades", "Infinite grenades", "Grenades never run out. Uncharted 3."),
          ("onehit", "One-hit kills", "Enemies go down with one hit. Uncharted 3."))
LAUNCHER_DEFAULTS = {"game": "u1", "diagnostics": True, "game_dir": "", "check_updates": True, "language": "1",
                     "unlock": False}

# The PS4's system language (its id, as sceSystemServiceParamGetInt reports it) for the 22 languages
# the Collection has text for (all three games; text1/ps4/<n>.common in core.psarc). Each game's main
# menu then offers it next to English (Square). Spoken languages depend on the copy of the game:
# its sound1 speech packs (SPEECH_PACKS).
LANGUAGES = [("1", "English (US)"), ("18", "English (UK)"), ("2", "Français"), ("4", "Deutsch"),
             ("5", "Italiano"), ("3", "Español (España)"), ("20", "Español (Latinoamérica)"),
             ("7", "Português (Portugal)"), ("17", "Português (Brasil)"), ("6", "Nederlands"),
             ("14", "Dansk"), ("15", "Norsk"), ("13", "Svenska"), ("12", "Suomi"), ("16", "Polski"),
             ("23", "Čeština"), ("25", "Ελληνικά"), ("8", "Русский"), ("19", "Türkçe"),
             ("0", "日本語"), ("9", "한국어"), ("10", "中文 (繁體)")]
SPEECH_PACKS = {"english": ("1", "18"), "french": ("2",), "german": ("4",), "italian": ("5",),
                "spanish": ("3", "20"), "portuguese": ("7", "17"), "dutch": ("6",), "danish": ("14",),
                "norwegian": ("15",), "swedish": ("13",), "finnish": ("12",), "polish": ("16",),
                "czech": ("23",), "greek": ("25",), "russian": ("8",), "turkish": ("19",),
                "japanese": ("0",), "korean": ("9",), "chinese": ("10",)}


def voice_languages(game_dir):
    """Language ids with speech in this copy of the game (its *_ps4.psarc speech packs)."""
    ids = set()
    for pattern in ("u*data/build/main/sound1/speech/*_ps4.psarc",
                    "u*data/build/main/sound1/streams/speech/*_ps4.psarc"):
        for pack in Path(game_dir).glob(pattern) if game_dir else ():
            ids.update(SPEECH_PACKS.get(pack.name.removesuffix("_ps4.psarc").lower(), ()))
    return ids
GAME_KEYS = ("u1", "u2", "u3")
GAME_SETTING_DEFAULTS = {"preset": "medium", "resolution": "1920x1080", "aspect": "16:9", "multithreaded": False, "save_profile": "1",
                         "async_readback": False, "controls": None, "mouse": True, "mouse_sens": "1.0",
                         "mouse_invert": False, "prompts": "auto", "mouse_aim_sens": "1.0",
                         "mouse_vertical": "1.0", "mouse_invert_x": False, "vibration": True}

# Keyboard and mouse controls (runtime_pad.c): action -> up to two bindings, "key:<SDL key name>"
# or "mouse:<left|middle|right|x1|x2|wheel_up|wheel_down>". Stick directions take keys only.
CONTROL_GROUPS = (
    ("MOVEMENT", (("ls_up", "Move forward"), ("ls_down", "Move back"), ("ls_left", "Move left"),
                  ("ls_right", "Move right"), ("rs_up", "Look up"), ("rs_down", "Look down"),
                  ("rs_left", "Look left"), ("rs_right", "Look right"))),
    ("WEAPONS (D-PAD)", (("left", "Left"), ("right", "Right"), ("up", "Up"), ("down", "Down"))),
    ("ACTIONS", (("cross", "Jump, climb"), ("circle", "Cover, roll, drop"),
                 ("square", "Melee"), ("triangle", "Reload, pick up"),
                 ("l2", "Aim"), ("r2", "Fire"), ("l1", "Grenade aim"),
                 ("r1", "Grenade throw"), ("l3", "Left stick press"), ("r3", "Right stick press"),
                 ("options", "Pause menu"), ("touchpad_left", "Touch left"),
                 ("touchpad_right", "Touch right"))),
)
# Controllers. The controller button of each action, as SDL names it (runtime_pad.c's default
# controller map; GOW3_PAD_MAP takes these names). Buttons can be rebound; the sticks, triggers
# and the touchpad's left click (Back/View with LB held) cannot.
PAD_DEFAULTS = {"cross": "a", "circle": "b", "square": "x", "triangle": "y", "l1": "leftshoulder",
                "r1": "rightshoulder", "l3": "leftstick", "r3": "rightstick", "options": "start",
                "touchpad_right": "back", "up": "dpup", "down": "dpdown", "left": "dpleft", "right": "dpright"}
PAD_FIXED = {"l2": "lefttrigger", "r2": "righttrigger", "touchpad_left": "leftshoulder+back",
             "ls_up": "lstick", "ls_down": "lstick", "ls_left": "lstick", "ls_right": "lstick",
             "rs_up": "rstick", "rs_down": "rstick", "rs_left": "rstick", "rs_right": "rstick"}
# SDL_GamepadButton order (SDL3).
SDL_BUTTONS = ("a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
               "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2",
               "paddle3", "paddle4", "touchpad")
PAD_NAMES = {  # (Xbox, PlayStation): the slot's text where there is no icon, and hints
    "a": ("A", "Cross"), "b": ("B", "Circle"), "x": ("X", "Square"), "y": ("Y", "Triangle"),
    "back": ("View", "Share"), "guide": ("Guide", "PS"), "start": ("Menu", "Options"),
    "leftstick": ("LS", "L3"), "rightstick": ("RS", "R3"), "leftshoulder": ("LB", "L1"),
    "rightshoulder": ("RB", "R1"), "lefttrigger": ("LT", "L2"), "righttrigger": ("RT", "R2"),
    "dpup": ("D-pad up", "D-pad up"), "dpdown": ("D-pad down", "D-pad down"),
    "dpleft": ("D-pad left", "D-pad left"), "dpright": ("D-pad right", "D-pad right"),
    "misc1": ("Share", "Mute"), "paddle1": ("P1", "P1"), "paddle2": ("P2", "P2"), "paddle3": ("P3", "P3"),
    "paddle4": ("P4", "P4"), "touchpad": ("Touchpad", "Touchpad"), "lstick": ("L stick", "L stick"),
    "rstick": ("R stick", "R stick"),
}


def merged_pad_controls(pad_controls):
    merged = dict(PAD_DEFAULTS)
    for action, button in (pad_controls or {}).items():
        if action in merged:
            merged[action] = button or None
    return merged


def pad_map(pad_controls):
    """GOW3_PAD_MAP for a game's controller bindings ("" when they are the defaults)."""
    merged = merged_pad_controls(pad_controls)
    if merged == PAD_DEFAULTS:
        return ""
    return ",".join(f"{action}={button or ''}" for action, button in merged.items())


def _pad_font(size):
    from PIL import ImageFont
    for name in ("segoeuib.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def pad_icon(button, style, height):
    """A controller button's icon (PIL RGBA image, `height` pixels tall): Xbox or PlayStation."""
    s = 4  # drawn 4x, then scaled down (smooth edges)
    H = height * s
    ps = style == "playstation"
    body, rim, ink = (32, 32, 34, 255), (150, 150, 150, 255), (240, 240, 240, 255)
    if "+" in button:  # a combination: the icons side by side with a plus
        parts = [pad_icon(b, style, height) for b in button.split("+")]
        plus_w = height // 2
        img = Image.new("RGBA", (sum(i.width for i in parts) + plus_w * (len(parts) - 1), height))
        x = 0
        for i, part in enumerate(parts):
            img.alpha_composite(part, (x, 0))
            x += part.width
            if i + 1 < len(parts):
                d = ImageDraw.Draw(img)
                cx, cy, r = x + plus_w // 2, height // 2, max(2, height // 6)
                d.line((cx - r, cy, cx + r, cy), fill=ink, width=max(1, height // 12))
                d.line((cx, cy - r, cx, cy + r), fill=ink, width=max(1, height // 12))
                x += plus_w
        return img

    def canvas(width_factor):
        W = int(H * width_factor)
        return Image.new("RGBA", (W, H)), W

    def finish(img):
        return img.resize((max(1, img.width // s), height), Image.LANCZOS)

    def text(d, W, label, color, size_factor=0.55, dy=0):
        font = _pad_font(max(8, int(H * size_factor)))
        box = d.textbbox((0, 0), label, font=font)
        d.text(((W - (box[2] - box[0])) / 2 - box[0], (H - (box[3] - box[1])) / 2 - box[1] + dy), label,
               font=font, fill=color)

    line = max(2, H // 12)
    if button in ("a", "b", "x", "y"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((line, line, W - line, H - line), fill=body, outline=rim, width=line)
        if ps:
            color = {"a": (124, 156, 255, 255), "b": (255, 102, 102, 255), "x": (240, 140, 210, 255),
                     "y": (64, 214, 160, 255)}[button]
            c, r, w = W / 2, H * 0.22, int(H * 0.08)
            if button == "a":
                d.line((c - r, c - r, c + r, c + r), fill=color, width=w)
                d.line((c - r, c + r, c + r, c - r), fill=color, width=w)
            elif button == "b":
                d.ellipse((c - r, c - r, c + r, c + r), outline=color, width=w)
            elif button == "x":
                d.rectangle((c - r * 0.85, c - r * 0.85, c + r * 0.85, c + r * 0.85), outline=color, width=w)
            else:
                d.polygon([(c, c - r * 1.05), (c - r * 1.1, c + r * 0.75), (c + r * 1.1, c + r * 0.75)],
                          outline=color, width=w)
        else:
            color = {"a": (108, 194, 74, 255), "b": (229, 71, 59, 255), "x": (63, 142, 230, 255),
                     "y": (244, 196, 48, 255)}[button]
            text(d, W, button.upper(), color, 0.6)
        return finish(img)
    if button in ("leftshoulder", "rightshoulder"):
        img, W = canvas(1.55)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((line, H * 0.14, W - line, H * 0.86), radius=H * 0.3, fill=body, outline=rim, width=line)
        text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.46)
        return finish(img)
    if button in ("lefttrigger", "righttrigger"):
        img, W = canvas(1.25)
        d = ImageDraw.Draw(img)
        d.rounded_rectangle((line, line, W - line, H - line), radius=H * 0.32, fill=body, outline=rim, width=line)
        d.rectangle((line * 2, H * 0.55, W - line * 2, H - line * 2), fill=body)
        text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.44)
        return finish(img)
    if button in ("leftstick", "rightstick", "lstick", "rstick"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((line, line, W - line, H - line), fill=body, outline=rim, width=line)
        if button in ("lstick", "rstick"):
            d.ellipse((H * 0.24, H * 0.24, H * 0.76, H * 0.76), outline=rim, width=line)
            text(d, W, "L" if button == "lstick" else "R", ink, 0.38)
        else:
            text(d, W, PAD_NAMES[button][1 if ps else 0], ink, 0.4)
        return finish(img)
    if button.startswith("dp"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        a, b = H * 0.31, H * 0.69
        arms = {"dpup": (a, line, b, a + line), "dpdown": (a, b - line, b, H - line), "dpleft": (line, a, a + line, b),
                "dpright": (b - line, a, H - line, b)}
        d.rectangle((a, line, b, H - line), fill=body, outline=rim, width=line)
        d.rectangle((line, a, H - line, b), fill=body, outline=rim, width=line)
        d.rectangle((a + line, a, b - line, b), fill=body)
        d.rectangle((a, a + line, b, b - line), fill=body)
        x0, y0, x1, y1 = arms[button]
        d.rectangle((x0 + line, y0 + line, x1 - line, y1 - line), fill=(250, 200, 90, 255))
        return finish(img)
    if button in ("start", "back"):
        img, W = canvas(1.0)
        d = ImageDraw.Draw(img)
        d.ellipse((H * 0.1, H * 0.1, H * 0.9, H * 0.9), fill=body, outline=rim, width=line)
        c = H / 2
        if button == "start":  # three lines: Xbox Menu, PlayStation Options
            for k in (-1, 0, 1):
                y = c + k * H * 0.13
                d.line((c - H * 0.17, y, c + H * 0.17, y), fill=ink, width=line)
        elif ps:  # Share/Create: a small arrow up
            d.line((c, c - H * 0.18, c, c + H * 0.16), fill=ink, width=line)
            d.line((c - H * 0.12, c - H * 0.06, c, c - H * 0.18, c + H * 0.12, c - H * 0.06), fill=ink, width=line)
        else:  # View: two windows
            d.rectangle((c - H * 0.17, c - H * 0.15, c + H * 0.05, c + H * 0.05), outline=ink, width=line)
            d.rectangle((c - H * 0.05, c - H * 0.03, c + H * 0.17, c + H * 0.17), fill=ink)
        return finish(img)
    # Anything else (touchpad, paddles, misc): its name on a rounded key.
    label = PAD_NAMES.get(button, (button, button))[1 if ps else 0]
    img, W = canvas(max(1.0, 0.36 * len(label)))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((line, H * 0.14, W - line, H * 0.86), radius=H * 0.25, fill=body, outline=rim, width=line)
    text(d, W, label, ink, 0.4)
    return finish(img)


class Gamepads:
    """Controllers through SDL3, the game's own library (gow3_pc/out in a release, MSYS2 here):
    which buttons are held, and whether the first one is a PlayStation controller."""
    PLAYSTATION_TYPES = (4, 5, 6)  # SDL_GAMEPAD_TYPE_PS3, PS4, PS5

    def __init__(self):
        self.sdl = None
        self.pads = {}
        msys = Path(os.environ.get("MSYS2_ROOT", "C:/msys64"))
        for path in (ROOT / "gow3_pc/out/SDL3.dll", msys / "clang64/bin/SDL3.dll", ROOT.parent / "msys64/clang64/bin/SDL3.dll"):
            if not path.is_file():
                continue
            try:
                sdl = ctypes.CDLL(str(path))
            except OSError:
                continue
            sdl.SDL_SetHint.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
            sdl.SDL_SetHint.restype = ctypes.c_bool
            sdl.SDL_Init.argtypes = [ctypes.c_uint32]
            sdl.SDL_Init.restype = ctypes.c_bool
            sdl.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            sdl.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_uint32)
            sdl.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
            sdl.SDL_OpenGamepad.restype = ctypes.c_void_p
            sdl.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            sdl.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            sdl.SDL_GetGamepadButton.restype = ctypes.c_bool
            sdl.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            sdl.SDL_GetGamepadAxis.restype = ctypes.c_int16
            sdl.SDL_GetGamepadType.argtypes = [ctypes.c_void_p]
            sdl.SDL_GetGamepadType.restype = ctypes.c_int
            sdl.SDL_free.argtypes = [ctypes.c_void_p]
            # The launcher has no SDL window: controller input arrives anyway.
            sdl.SDL_SetHint(b"SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", b"1")
            if sdl.SDL_Init(0x2000):  # SDL_INIT_GAMEPAD
                self.sdl = sdl
            break

    def refresh(self):
        if not self.sdl:
            return []
        self.sdl.SDL_PumpEvents()
        count = ctypes.c_int(0)
        ids = self.sdl.SDL_GetGamepads(ctypes.byref(count))
        current = [ids[i] for i in range(count.value)] if ids else []
        if ids:
            self.sdl.SDL_free(ctypes.cast(ids, ctypes.c_void_p))
        for jid in current:
            if jid not in self.pads:
                pad = self.sdl.SDL_OpenGamepad(jid)
                if pad:
                    self.pads[jid] = pad
        for jid in [j for j in self.pads if j not in current]:
            self.sdl.SDL_CloseGamepad(self.pads.pop(jid))
        self.sdl.SDL_UpdateGamepads()
        return list(self.pads.values())

    def held(self):
        down = set()
        for pad in self.refresh():
            for index, name in enumerate(SDL_BUTTONS):
                if self.sdl.SDL_GetGamepadButton(pad, index):
                    down.add(name)
            for axis, name in ((4, "lefttrigger"), (5, "righttrigger")):
                if self.sdl.SDL_GetGamepadAxis(pad, axis) > 16000:
                    down.add(name)
        return down

    def style(self):
        for pad in self.refresh():
            return "playstation" if self.sdl.SDL_GetGamepadType(pad) in self.PLAYSTATION_TYPES else "xbox"
        return None


STICK_ACTIONS = {"ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"}
DEFAULT_CONTROLS = {
    "ls_up": ["key:W"], "ls_down": ["key:S"], "ls_left": ["key:A"], "ls_right": ["key:D"],
    "rs_up": ["key:Up"], "rs_down": ["key:Down"], "rs_left": ["key:Left"], "rs_right": ["key:Right"],
    "left": ["key:1", "mouse:wheel_down"], "right": ["key:2", "mouse:wheel_up"], "up": ["key:3"],
    "down": ["key:4"],
    "cross": ["key:Space"], "circle": ["key:Left Ctrl"], "square": ["key:F"], "triangle": ["key:R"],
    "l2": ["mouse:right"], "r2": ["mouse:left"], "l1": ["key:Q", "mouse:x1"], "r1": ["key:E", "mouse:x2"],
    "l3": ["key:Left Shift"], "r3": ["key:V", "mouse:middle"], "options": ["key:Escape", "key:Return"],
    "touchpad_left": ["key:Tab"], "touchpad_right": ["key:Backspace"],
}
# Button icons in the games' text and menus (runtime GOW3_PROMPTS): the device used last, or fixed.
PROMPT_MODES = [("auto", "Auto"), ("keyboard", "Keyboard and mouse"), ("playstation", "PlayStation"),
                ("xbox", "Xbox")]
MOUSE_SENSITIVITIES = [("0.5", "0.5x"), ("0.75", "0.75x"), ("1.0", "1x"), ("1.5", "1.5x"), ("2.0", "2x"),
                       ("3.0", "3x"), ("4.0", "4x"), ("5.0", "5x")]
# While aiming (times the sensitivity) and up/down compared with left/right (the in-game menu's
# Controls tab has the same as sliders).
MOUSE_FACTORS = [("0.5", "0.5x"), ("0.75", "0.75x"), ("1.0", "1x"), ("1.25", "1.25x"), ("1.5", "1.5x"),
                 ("2.0", "2x")]
MOUSE_AIM_FACTORS = [("0.5", "0.5x"), ("1.0", "1x"), ("2.0", "2x"), ("3.0", "3x"), ("5.0", "5x"),
                     ("10.0", "10x")]
MOUSE_LABELS = {"left": "Left mouse", "middle": "Middle mouse", "right": "Right mouse", "x1": "Mouse back",
                "x2": "Mouse forward", "wheel_up": "Wheel up", "wheel_down": "Wheel down"}
# Tk key names -> SDL key names (letters and digits map to themselves, upper case).
TK_TO_SDL = {
    "space": "Space", "Return": "Return", "Escape": "Escape", "Tab": "Tab", "BackSpace": "Backspace",
    "Shift_L": "Left Shift", "Shift_R": "Right Shift", "Control_L": "Left Ctrl", "Control_R": "Right Ctrl",
    "Alt_L": "Left Alt", "Alt_R": "Right Alt", "Caps_Lock": "CapsLock", "Up": "Up", "Down": "Down",
    "Left": "Left", "Right": "Right", "Delete": "Delete", "Home": "Home", "End": "End", "Prior": "PageUp",
    "Next": "PageDown", "minus": "-", "equal": "=", "bracketleft": "[", "bracketright": "]",
    "backslash": "\\", "semicolon": ";", "apostrophe": "'", "grave": "`", "period": ".", "slash": "/",
    "KP_Divide": "Keypad /", "KP_Multiply": "Keypad *", "KP_Subtract": "Keypad -", "KP_Add": "Keypad +",
    "KP_Enter": "Keypad Enter", "KP_Decimal": "Keypad .", "Pause": "Pause", "Scroll_Lock": "ScrollLock",
}
TK_TO_SDL.update({f"F{n}": f"F{n}" for n in range(1, 13)})
TK_TO_SDL.update({f"KP_{n}": f"Keypad {n}" for n in range(10)})
# Keys the runtime keeps for itself: Insert opens the in-game menu, F9 records the pad.
RESERVED_KEYS = {"Insert", "F9", "comma"}


def merged_controls(controls):
    merged = {action: list(bindings) for action, bindings in DEFAULT_CONTROLS.items()}
    for action, bindings in (controls or {}).items():
        if action in merged:
            merged[action] = list(bindings)[:2]
    return merged


def control_maps(controls):
    """GOW3_KEY_MAP and GOW3_MOUSE_MAP for a game's controls (every action listed, so unbound ones
    lose the runtime's defaults)."""
    keys, mouse = [], []
    for action, bindings in merged_controls(controls).items():
        names = [b[4:] for b in bindings if b.startswith("key:")]
        keys += [f"{action}={name}" for name in names] or [f"{action}="]
        if action not in STICK_ACTIONS:
            buttons = [b[6:] for b in bindings if b.startswith("mouse:")]
            mouse += [f"{action}={name}" for name in buttons] or [f"{action}="]
    return ",".join(keys), ",".join(mouse)


def parse_map(spec):
    """runtime_pad.c's map syntax: (action, source name) per entry ("" = none)."""
    entries = []
    for entry in (spec or "").split(","):
        action, eq, name = entry.partition("=")
        if eq:
            entries.append((action.strip(), name.strip()))
    return entries


def apply_map(bindings, spec, prefix):
    """A map on top of an action -> ["key:..", "mouse:.."] dict, as the runtime applies it: an
    action's first entry replaces its sources of this kind, further ones add, "action=" none."""
    replaced = set()
    for action, name in parse_map(spec):
        if action not in bindings:
            continue
        if action not in replaced:
            replaced.add(action)
            bindings[action] = [b for b in bindings[action] if not b.startswith(prefix)]
        if name:
            bindings[action].append(prefix + name)


def sensitivity_value(text):
    """A mouse sensitivity as the Sensitivity choice names it (the in-game menu sets any)."""
    try:
        value = float(text)
    except (TypeError, ValueError):
        return "1.0"
    for option, _ in MOUSE_SENSITIVITIES:
        if abs(float(option) - value) < 0.005:
            return option
    return f"{value:g}"


def factor_value(text, options):
    """A mouse factor from a settings file as one of the options when it is one."""
    try:
        value = float(text)
    except ValueError:
        return "1.0"
    for option, _ in options:
        if abs(float(option) - value) < 0.005:
            return option
    return f"{value:g}"


# The controls in each game's settings file: the game reads them there, and its in-game menu
# (Insert / R3+L2, Controls tab) changes them too.
def controls_to_ini(settings):
    keys, mouse = control_maps(settings.get("controls"))
    return {"key_map": keys, "mouse_map": mouse, "pad_map": pad_map(settings.get("pad_controls")),
            "mouse": "1" if settings.get("mouse", True) else "0",
            "mouse_sens": settings.get("mouse_sens", "1.0"),
            "mouse_invert": "1" if settings.get("mouse_invert") else "0",
            "prompts": settings.get("prompts", "auto"),
            "mouse_aim_sens": settings.get("mouse_aim_sens", "1.0"),
            "mouse_vertical": settings.get("mouse_vertical", "1.0"),
            "mouse_invert_x": "1" if settings.get("mouse_invert_x") else "0",
            "vibration": "1" if settings.get("vibration", True) else "0"}


def controls_from_ini(values, settings):
    """A game's controls from its settings file into the launcher's settings; False when the file
    has none yet."""
    if not any(key in values for key in ("key_map", "mouse_map", "pad_map")):
        return False
    controls = {action: list(bindings) for action, bindings in DEFAULT_CONTROLS.items()}
    apply_map(controls, values.get("key_map"), "key:")
    apply_map(controls, values.get("mouse_map"), "mouse:")
    settings["controls"] = {action: bindings[:2] for action, bindings in controls.items()}
    pad = dict(PAD_DEFAULTS)
    replaced = set()
    for action, name in parse_map(values.get("pad_map")):
        if action in pad and action not in replaced:
            replaced.add(action)
            pad[action] = name or None
    settings["pad_controls"] = {action: button for action, button in pad.items() if button != PAD_DEFAULTS.get(action)}
    if "mouse" in values:
        settings["mouse"] = values["mouse"] != "0"
    if "mouse_sens" in values:
        settings["mouse_sens"] = sensitivity_value(values["mouse_sens"])
    if "mouse_invert" in values:
        settings["mouse_invert"] = values["mouse_invert"] == "1"
    for key, options in (("mouse_aim_sens", MOUSE_AIM_FACTORS), ("mouse_vertical", MOUSE_FACTORS)):
        if key in values:
            settings[key] = factor_value(values[key], options)
    if "mouse_invert_x" in values:
        settings["mouse_invert_x"] = values["mouse_invert_x"] == "1"
    if "vibration" in values:
        settings["vibration"] = values["vibration"] == "1"
    if "prompts" in values:
        settings["prompts"] = values["prompts"] if values["prompts"] in dict(PROMPT_MODES) else "auto"
    return True


def binding_label(binding):
    if not binding:
        return "—"
    kind, _, name = binding.partition(":")
    return MOUSE_LABELS.get(name, name) if kind == "mouse" else name


def is_game_dir(folder):
    """The Collection's game folder (CUSA02344): eboot.bin and sce_sys/param.sfo."""
    return bool(folder) and (Path(folder) / "eboot.bin").is_file() and \
        (Path(folder) / "sce_sys" / "param.sfo").is_file()


UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


def game_update_dir(folder):
    """The game's update (e.g. 1.02) next to its folder, found as tools/probe_uncharted.py does:
    named like the game folder plus -patch or -update, with or without uroot."""
    folder = Path(folder)
    title = folder.parent if folder.name.lower() == "uroot" else folder
    names = {title.name.lower() + suffix for suffix in UPDATE_SUFFIXES}
    try:
        for entry in title.parent.iterdir():
            if entry.name.lower() in names:
                for candidate in (entry / "uroot", entry):
                    if is_game_dir(candidate):
                        return candidate
    except OSError:
        pass
    return None


def app_version(folder):
    """APP_VER of a game folder (sce_sys/param.sfo); 01.02 when the executables are 1.02 although
    param.sfo says otherwise (dumps with the update merged in that kept the 1.00 param.sfo)."""
    try:
        data = (Path(folder) / "sce_sys" / "param.sfo").read_bytes()
        _, _, keys, values, count = struct.unpack_from("<IIIII", data)
        for i in range(count):
            key_at, _, length, _, value_at = struct.unpack_from("<HHIII", data, 20 + i * 16)
            key = data[keys + key_at:data.index(bytes(1), keys + key_at)].decode()
            if key == "APP_VER":
                version = data[values + value_at:values + value_at + length].rstrip(bytes(1)).decode()
                return "01.02" if version < "01.02" and has_update_102_code(folder) else version
    except (OSError, ValueError, struct.error):
        pass
    return None


# Uncharted 2's torch code as update 1.02 compiled it (tools/probe_uncharted.py
# TORCH_FORWARD_PATCHES; 1.00 lays this code out differently).
UPDATE_102_CODE = bytes.fromhex("488d05cc9fd500 8b4850 84c9 7440 8a50".replace(" ", ""))
_update_102_cache = {}


def has_update_102_code(folder):
    path = Path(folder) / "big2-ps4_Shipping.elf"
    try:
        key = (str(path), path.stat().st_mtime_ns)
    except OSError:
        return False
    if key not in _update_102_cache:
        try:
            _update_102_cache[key] = UPDATE_102_CODE in path.read_bytes()
        except OSError:
            _update_102_cache[key] = False
    return _update_102_cache[key]


def desktop_folder():
    """The user's Desktop folder. Windows can move it (OneDrive backup, a redirected or renamed
    Desktop), so it is asked for (SHGetKnownFolderPath) instead of assumed at %USERPROFILE%\\Desktop:
    shortcuts failed there with "DirectoryNotFound" (#39)."""
    try:
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD), ("Data3", wintypes.WORD),
                        ("Data4", ctypes.c_ubyte * 8)]

        desktop_id = GUID(0xB4BFCC3A, 0xDB2C, 0x424C,  # FOLDERID_Desktop
                          (ctypes.c_ubyte * 8)(0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41))
        found = ctypes.c_void_p()
        shell32 = ctypes.windll.shell32
        shell32.SHGetKnownFolderPath.argtypes = [ctypes.POINTER(GUID), wintypes.DWORD, wintypes.HANDLE,
                                                 ctypes.POINTER(ctypes.c_void_p)]
        if shell32.SHGetKnownFolderPath(ctypes.byref(desktop_id), 0, None, ctypes.byref(found)) == 0:
            try:
                folder = Path(ctypes.wstring_at(found.value))
            finally:
                ctypes.windll.ole32.CoTaskMemFree(found)
            if folder.is_dir():
                return folder
    except (AttributeError, OSError, ValueError):
        pass
    return Path(os.environ.get("USERPROFILE", Path.home())) / "Desktop"


def monitor_refresh_hz():
    try:
        user32, gdi32 = ctypes.windll.user32, ctypes.windll.gdi32
        dc = user32.GetDC(0)
        hz = gdi32.GetDeviceCaps(dc, 116)  # VREFRESH
        user32.ReleaseDC(0, dc)
        return hz if hz > 1 else 60
    except Exception:
        return 60


def fps_limit_value(value):
    """A listed limit; unlimited (0) and faster ones (earlier versions offered 144-240) are 120."""
    listed = {str(v) for v, _ in FPS_LIMITS}
    if value in listed:
        return value
    try:
        number = int(value)
    except (TypeError, ValueError):
        return "60"
    return str(MAX_FPS) if number == 0 or number > MAX_FPS else "60"


RESOLUTIONS = [("960x540", "540p"), ("1280x720", "720p"), ("1600x900", "900p"), ("1920x1080", "1080p"), ("2560x1440", "1440p"),
               ("2880x1620", "1620p"), ("3840x2160", "4K")]
# Ultrawide: the render resolution's height at a wider aspect ratio. The games widen their view
# (more to the sides, nothing stretched); menus and the HUD keep their layout. 16:10 (Steam Deck,
# 1280x800): the render resolution's width, taller; the games keep the 16:9 width of view and
# show more at the top and bottom.
ASPECTS = [("16:10", "16:10"), ("16:9", "16:9"), ("21:9", "21:9"), ("32:9", "32:9")]
# Uncharted 2 at 21:9 / 32:9 (tools/probe_uncharted.py hud_layout): its HUD and menus in the
# centred 16:9 area, in it with the side elements at the screen edges, or the game's own stretch.
HUD_WIDE_AREAS = [("screen", "Screen edges"), ("21:9", "21:9"), ("32:9", "32:9")]
HUD_MODES = [("16:9", "16:9"), ("wide", "Wide"), ("stretch", "Stretched")]
ASPECT_WIDTHS = {"16:10": 16 / 10, "16:9": 16 / 9, "21:9": 64 / 27, "32:9": 32 / 9}
# Upscaling: the resolution chosen is the output; the game draws at it divided by the quality's
# scale and the upscaler brings each frame up to it before the HUD is drawn. FSR 1 is spatial (the
# frame alone; the "fsr1" setting); FSR 3.1, FSR 4 and DLSS are temporal (the runtime's "upscaler"
# setting, also the in-game menu's): they smooth edges themselves, at the resolution too (Native
# AA, DLAA). TAA, the other "upscaler" value, is the anti-aliasing with Off and FSR 1.
UPSCALE_PRESETS = [("off", "Native AA"), ("quality", "Quality"), ("balanced", "Balanced"),
                   ("performance", "Performance"), ("ultra", "Ultra Performance")]
UPSCALE_SCALES = {"quality": 1.5, "balanced": 1.7, "performance": 2.0, "ultra": 3.0}
TEMPORAL_UPSCALERS = ("fsr3", "fsr4", "fsr411", "dlss")


def upscaling_choice(ini):
    """The upscaler a game's settings file holds: a temporal one, else FSR 1, else "off"."""
    upscaler = ini.get("upscaler", INI_DEFAULTS["upscaler"])
    if upscaler in TEMPORAL_UPSCALERS:
        return upscaler
    return "fsr1" if ini.get("fsr1", "0") == "1" else "off"


def anti_aliasing_choice(ini, settings):
    """TAA or off: the runtime's setting with Off and FSR 1; kept in the game's launcher settings
    while a temporal upscaler (which smooths edges itself) takes the runtime's setting."""
    upscaler = ini.get("upscaler", INI_DEFAULTS["upscaler"])
    if upscaler in ("off", "taa"):
        return upscaler
    return "taa" if settings.get("aa") == "taa" else "off"


def game_sizes(settings, upscaling):
    """The size a game draws at and, with upscaling, the size it is upscaled to (else None).
    The engines need widths in steps of 16 (Uncharted 3 asserts it in its post processing) and
    heights in steps of 4 (the tile grid)."""
    output = aspect_resolution(settings["resolution"], settings.get("aspect", "16:9"))
    scale = UPSCALE_SCALES.get(settings.get("upscale", "off"))
    if not scale or (upscaling not in TEMPORAL_UPSCALERS and upscaling != "fsr1"):
        return output, None
    width, height = (int(v) for v in output.split("x"))
    render = f"{max(16, round(width / scale / 16) * 16)}x{max(4, round(height / scale / 4) * 4)}"
    return render, output


def aspect_resolution(resolution, aspect):
    """The render resolution at the aspect ratio: 1440p at 21:9 is 3440x1440 (widths in steps of 16),
    720p at 16:10 is 1280x800 (heights in steps of 4)."""
    width, height = (int(v) for v in resolution.split("x"))
    if aspect not in ASPECT_WIDTHS or aspect == "16:9":
        return resolution
    if ASPECT_WIDTHS[aspect] < 16 / 9:
        height = int(round(width / ASPECT_WIDTHS[aspect] / 4)) * 4
    elif aspect == "21:9" and height in (1080, 1440, 1600):
        width = {1080: 2560, 1440: 3440, 1600: 3840}[height]  # the common monitor sizes
    else:
        width = int(round(height * ASPECT_WIDTHS[aspect] / 16)) * 16
    return f"{width}x{height}"


def resolution_options(aspect):
    """RESOLUTIONS labelled by the height the game draws at this aspect ratio (720p is 800p at
    16:10; wider ones keep the height); 2160 lines are 4K, ultrawide included."""
    options = []
    for value, _ in RESOLUTIONS:
        height = int(aspect_resolution(value, aspect).split("x")[1])
        options.append((value, "4K" if height == 2160 else f"{height}p"))
    return options

# Quality presets: the game has one detail level (the PS4's); quality here is the resolution the
# game draws at, the upscaling, the sharpening and the frame rate. Above 1080p the
# render resolution is new (October 2026): confirm in play. All three games draw at 4K
# natively except Drake's Fortune, which stops at 1620p (its memory layout) and upscales.
PRESETS = {
    "low": {
        "title": "LOW", "summary": "60 FPS  •  1080p, FSR 1",
        "detail": "For integrated and older graphics cards: the game draws at half of 1080p and FSR 1 "
                  "upscales it; the HUD stays sharp.",
        "resolution": "1920x1080", "upscale": "performance",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "1", "rcas": "1", "rcas_strength": "75"},
    },
    "medium": {
        "title": "MEDIUM", "summary": "60 FPS  •  as on PS4",
        "detail": "Like the PS4: 1080p at 60 FPS. Right for most PCs.",
        "resolution": "1920x1080", "upscale": "off",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "0", "rcas": "1", "rcas_strength": "75"},
    },
    "high": {
        "title": "HIGH", "summary": "60 FPS  •  1440p",
        "detail": "Sharper: the game draws at 1440p.",
        "resolution": "2560x1440", "upscale": "off",
        "values": {"fps_limit": "60", "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "0", "rcas": "1", "rcas_strength": "50"},
    },
    "ultra": {
        "title": "ULTRA", "summary": "4K  •  up to 120 FPS",
        "detail": "For fast graphics cards: native 4K and up to 120 FPS (experimental). Drake's "
                  "Fortune draws at 1620p and scales up.",
        "resolution": "3840x2160", "upscale": "off",
        "values": {"fps_limit": str(MAX_FPS), "frames_queued": "1", "vsync": "1", "async_shaders": "1",
                   "fsr1": "0", "rcas": "1", "rcas_strength": "50"},
    },
}
PRESET_ORDER = ("low", "medium", "high", "ultra")


def upscaling_options():
    """FSR 1 and FSR 3.1 always; FSR 4 when its model files are installed, DLSS when its files are
    beside Uncharted.exe."""
    options = [("off", "Off"), ("fsr1", "FSR 1"), ("fsr3", "FSR 3.1")]
    if (ROOT / "gow3_pc/fsr4_shaders/fsr4_model_v07_i8_native_1080_pre.spv").is_file():
        options.append(("fsr4", "FSR 4"))
    out = ROOT / "gow3_pc/out"
    if (out / "gow3_dlss.dll").is_file() and (out / "nvngx_dlss.dll").is_file():
        options.append(("dlss", "DLSS"))
    return options


def preset_values(name):
    return dict(PRESETS[name]["values"])


def gpu_names():
    """The graphics cards: the one the game used last (its log names it), else Windows' list."""
    logs = [ROOT / "analysis" / g["log"] / "boot.log" for g in GAMES]
    for log in sorted((log for log in logs if log.exists()), key=lambda p: p.stat().st_mtime, reverse=True):
        try:
            with open(log, encoding="utf-8", errors="replace") as lines:
                for number, line in enumerate(lines):
                    if line.startswith("GPU: using "):
                        return [line[len("GPU: using "):].strip()]
                    if number > 400:
                        break
        except OSError:
            pass
    try:
        result = subprocess.run(["powershell", "-NoProfile", "-Command", "(Get-CimInstance Win32_VideoController).Name"],
                                capture_output=True, text=True, timeout=20, creationflags=NO_WINDOW)
        return [line.strip() for line in result.stdout.splitlines() if line.strip()]
    except (OSError, subprocess.SubprocessError):
        return []


def gpu_tier(name):
    """The Quick Setup preset for a graphics card, from its model name; None when not known.
    Rough classes: Low = integrated and older entry cards, Medium = 1080p cards (GTX 1060-1660,
    RTX x060, RX 580-7600), High = 1440p cards, Ultra = 4K cards (RTX x080/x090,
    RTX 4070 Ti and up, RX 6800 XT/7900/9070). High and Ultra laptop chips count one class lower."""
    n = (name or "").lower().replace("(tm)", "").replace("(r)", "")
    nvidia = re.search(r"\b(rtx|gtx|gt|mx)\s*(\d{3,4})\s*(ti)?\s*(super)?", n)
    amd = re.search(r"\brx\s*(\d{3,4})\s*(xtx|xt|gre)?", n)
    arc = re.search(r"\barc\s*([ab])(\d{3})", n)
    if nvidia:
        kind, number = nvidia.group(1), int(nvidia.group(2))
        ti, super_ = bool(nvidia.group(3)), bool(nvidia.group(4))
        if kind in ("gt", "mx"):
            tier = "low"
        elif kind == "gtx":
            if number == 1080 and ti:
                tier = "high"
            elif 1060 <= number < 1100 or 1660 <= number < 1700 or (number == 1650 and super_):
                tier = "medium"
            else:
                tier = "low"
        else:
            series, model = number // 100, number % 100
            if model >= 80:
                tier = "ultra" if series >= 30 else "high"
            elif model == 70:
                tier = "ultra" if series >= 40 and (ti or super_) else "high"
            elif model == 60:
                tier = "high" if ti or series >= 50 else "medium"
            else:
                tier = "medium"
    elif amd:
        number, suffix = int(amd.group(1)), amd.group(2) or ""
        if number >= 9000:
            tier = {7: "ultra", 6: "high"}.get((number % 100) // 10, "medium")
        elif number >= 1000:
            series, model = number // 1000, (number % 1000) // 100
            if model >= 9:
                tier = "ultra"
            elif model == 8:
                tier = "ultra" if suffix.startswith("xt") else "high"
            elif model == 7:
                tier = "medium" if series == 5 else "high"
            elif model == 6 or number == 5500:
                tier = "medium"
            else:
                tier = "low"
        else:
            tier = "medium" if number in (470, 480, 570, 580, 590) else "low"
    elif "vega 56" in n or "vega 64" in n:
        tier = "medium"
    elif "radeon vii" in n:
        tier = "high"
    elif arc:
        tier = "medium" if int(arc.group(2)) >= 500 else "low"
    elif "intel" in n or ("radeon" in n and "graphics" in n):
        tier = "low"  # integrated graphics
    else:
        return None
    if "laptop" in n or "max-q" in n or "mobile" in n or re.search(r"\d{3,4}m\b", n):
        tier = {"ultra": "high", "high": "medium"}.get(tier, tier)
    return tier


# ---------------------------------------------------------------------------------------------
# Settings files

def ini_file(game):
    """Each game keeps its own runtime settings (tools/probe_uncharted.py passes the same file)."""
    return USER_DIR / f"probe-{game}.ini"


def ini_lines(game):
    """The game's settings file, or the shared one of earlier versions as its starting point."""
    for path in (ini_file(game), INI_FILE):
        try:
            return path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
    return ["# Uncharted settings (launcher and in-game menu: Insert / R3+L2)"]


def read_ini(game):
    values = dict(INI_DEFAULTS)
    for line in ini_lines(game):
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    values["fps_limit"] = fps_limit_value(values["fps_limit"])
    return values


def ini_mtime(game):
    try:
        return ini_file(game).stat().st_mtime_ns
    except OSError:
        return None


def write_ini(game, updates):
    """Updates keys in place and keeps every other line (the in-game menu writes more keys)."""
    lines = ini_lines(game)
    done = set()
    for i, line in enumerate(lines):
        if "=" in line and not line.lstrip().startswith("#"):
            key = line.split("=", 1)[0].strip()
            if key in updates:
                lines[i] = f"{key}={updates[key]}"
                done.add(key)
    lines += [f"{key}={value}" for key, value in updates.items() if key not in done]
    path = ini_file(game)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def load_config_file(path):
    """A launcher.json as a dict; {} when missing. One that can't be read (cut off while written)
    is kept beside it as .bad, and the other copy or the defaults apply."""
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if isinstance(data, dict) else {}
    except OSError:
        return {}
    except ValueError:
        try:
            shutil.copy2(path, path.with_name(path.name + ".bad"))
        except OSError:
            pass
        return {}


def read_config():
    config = dict(LAUNCHER_DEFAULTS)
    stored = load_config_file(CONFIG_FILE)
    if CONFIG_FILE != PREFIX_CONFIG_FILE:
        # Under Wine the shared file wins; what it lacks comes from this prefix's own file (the place
        # earlier versions used), game by game. That file is left as it is.
        older = load_config_file(PREFIX_CONFIG_FILE)
        if older:
            games = {}
            for source in (older, stored):
                for game, entry in (source.get("games") if isinstance(source.get("games"), dict) else {}).items():
                    if isinstance(entry, dict):
                        games.setdefault(game, {}).update(entry)
            stored = {**older, **stored, "games": games}
    config.update(stored)
    # Version 2: render resolutions above 1080p crash in gameplay; earlier presets chose them.
    if config.get("version", 1) < 2:
        config["resolution"] = "1920x1080"
    # Version 3: preset, resolution and multithreading per game; the shared values start all three.
    if not isinstance(config.get("games"), dict):
        config["games"] = {}
    games = config["games"]
    resolutions = [r for r, _ in RESOLUTIONS]
    for game in GAME_KEYS:
        if not isinstance(games.get(game), dict):
            games[game] = {}
        entry = games[game]
        for name, default in GAME_SETTING_DEFAULTS.items():
            entry.setdefault(name, config.get(name, default))
        entry["preset"] = {"balanced": "medium"}.get(entry["preset"], entry["preset"])
        # A value that isn't valid (another version, a hand edit) falls back alone.
        if entry["resolution"] not in resolutions:
            entry["resolution"] = "1920x1080"
        if entry.get("aspect") not in ASPECT_WIDTHS:
            entry["aspect"] = "16:9"
    # Version 4: the presets draw at 720p / 1080p / 1440p / 4K (all were 1080p before).
    if config.get("version", 1) < 4:
        for game, entry in games.items():
            if entry["preset"] in PRESETS:
                entry["resolution"] = PRESETS[entry["preset"]]["resolution"]
                write_ini(game, preset_values(entry["preset"]))
    # Version 5: every preset queues 2 frames (with 1 the CPU waits for the GPU each frame: heavy
    # Uncharted 2 scenes ran at 45 instead of 57 FPS). Not with multithreaded rendering, where
    # Uncharted 2 needs 1.
    if config.get("version", 1) < 5:
        for game, entry in games.items():
            if entry["preset"] in PRESETS and not entry.get("multithreaded"):
                write_ini(game, {"frames_queued": "2"})
    # Version 6: every preset queues 1 frame again: with 2, GPU results the game reads came a frame
    # later and Uncharted 2's Nate and NPCs stood above the floor (16 Oct). Custom settings stay.
    if config.get("version", 1) < 6:
        for game, entry in games.items():
            if entry["preset"] in PRESETS:
                write_ini(game, {"frames_queued": "1"})
    # Version 7: FSR 1 is an upscaler like FSR 3.1 (the game draws below the resolution); before,
    # it scaled the finished frame up to a larger screen. Low draws at half of 1080p with it; other
    # settings that had it on keep their resolution without it.
    if config.get("version", 1) < 7:
        for game, entry in games.items():
            if entry["preset"] == "low":
                entry["resolution"] = PRESETS["low"]["resolution"]
                entry["upscale"] = PRESETS["low"]["upscale"]
                write_ini(game, {"fsr1": "1"})
            elif read_ini(game).get("fsr1") == "1" and entry.get("upscale", "off") == "off":
                write_ini(game, {"fsr1": "0"})
    # Version 8: Drake's Deception with multithreaded rendering (the port no longer turns it off
    # there): on a laptop's heavy spots 60 FPS instead of 53.7 with 11 stalls (21 Oct). Once;
    # a player who turns it off again keeps it off.
    if config.get("version", 1) < 8:
        games["u3"]["multithreaded"] = True
    for name in GAME_SETTING_DEFAULTS:
        config.pop(name, None)
    config["version"] = 8
    # Under Wine the game folder is also kept as a Linux path: another prefix can show the same
    # folder under another drive letter (Launcher.game_dir).
    folder = config.get("game_dir")
    if WINE and folder and not config.get("game_dir_unix") and Path(folder).exists():
        config["game_dir_unix"] = unix_path(folder)
    return config


def write_config(config):
    CONFIG_FILE.parent.mkdir(parents=True, exist_ok=True)
    # Through a temporary file: a launcher and a game started from a shortcut may write at once, and
    # a write cut short must not leave half a file.
    text = json.dumps(config, indent=2)
    temporary = CONFIG_FILE.with_name(f"{CONFIG_FILE.name}.{os.getpid()}.tmp")
    try:
        temporary.write_text(text, encoding="utf-8")
        os.replace(temporary, CONFIG_FILE)
    except OSError:
        # Windows refuses the swap while another process has the file open (a virus scanner, a
        # second launcher): written in place, as before.
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass
        CONFIG_FILE.write_text(text, encoding="utf-8")


# ---------------------------------------------------------------------------------------------
# Drawing helpers and widgets

def round_rect(canvas, x1, y1, x2, y2, r, **kw):
    points = [x1 + r, y1, x2 - r, y1, x2, y1, x2, y1 + r, x2, y2 - r, x2, y2, x2 - r, y2,
              x1 + r, y2, x1, y2, x1, y2 - r, x1, y1 + r, x1, y1]
    return canvas.create_polygon(points, smooth=True, **kw)


class Toggle(tk.Canvas):
    def __init__(self, parent, scale, value, command):
        self.s = scale
        w, h = int(46 * scale), int(24 * scale)
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.w, self.h, self.value, self.command = w, h, bool(value), command
        self.bind("<Button-1>", lambda e: self.set(not self.value, notify=True))
        self.draw()

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def draw(self):
        self.delete("all")
        pad = max(1, int(2 * self.s))
        track = GOLD if self.value else LINE
        round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=track, outline=track)
        d = self.h - 2 * pad - 2
        x = self.w - pad - d - 1 if self.value else pad + 1
        self.create_oval(x, pad + 1, x + d, pad + 1 + d, fill=INK if self.value else MUTED, outline="")


class Chip(tk.Canvas):
    """A small on/off pill, for the performance counter's items (Display tab)."""

    def __init__(self, parent, scale, font, label, value, command):
        self.s, self.label, self.command = scale, label, command
        self.value = bool(value)
        self.font = tkfont.Font(font=font)
        pad_x, pad_y = int(10 * scale), int(5 * scale)
        w = self.font.measure(label) + 2 * pad_x
        h = self.font.metrics("linespace") + 2 * pad_y
        super().__init__(parent, width=w, height=h, bg=parent["bg"], highlightthickness=0, cursor="hand2")
        self.w, self.h = w, h
        self.bind("<Button-1>", lambda e: self.set(not self.value, notify=True))
        self.draw()

    def set(self, value, notify=False):
        self.value = bool(value)
        self.draw()
        if notify:
            self.command(self.value)

    def draw(self):
        self.delete("all")
        fill = GOLD if self.value else PANEL
        round_rect(self, 1, 1, self.w - 1, self.h - 1, self.h // 2, fill=fill,
                   outline=GOLD if self.value else LINE)
        self.create_text(self.w / 2, self.h / 2, text=self.label, font=self.font,
                         fill=INK if self.value else MUTED)


class Segmented(tk.Frame):
    """A row of choices; the selected one is gold."""

    def __init__(self, parent, options, value, command, font, recommended=None):
        super().__init__(parent, bg=LINE, padx=1, pady=1)
        self.command, self.labels, self.value, self.recommended = command, {}, value, recommended
        self.enabled = True
        for option, label in options:
            item = tk.Label(self, text=self.text(option, label), font=font, padx=12, pady=5, cursor="hand2", bd=0)
            item.bind("<Button-1>", lambda e, v=option: self.enabled and self.set(v, notify=True))
            item.bind("<Enter>", lambda e, w=item, v=option: self.enabled and v != self.value and w.config(bg=PANEL_HI))
            item.bind("<Leave>", lambda e: self.restyle())
            self.labels[option] = item
        self.show(list(self.labels))
        self.restyle()

    def show(self, options):
        """Only these choices, in their order (FSR 1 has no Native AA)."""
        for item in self.labels.values():
            item.grid_forget()
        for i, option in enumerate(options):
            self.labels[option].grid(row=0, column=i, padx=(0 if i == 0 else 1, 0))

    def set_enabled(self, enabled):
        """A disabled row shows its choice greyed and takes no clicks."""
        self.enabled = enabled
        for item in self.labels.values():
            item.config(cursor="hand2" if enabled else "arrow")
        self.restyle()

    def text(self, option, label):
        # The recommended choice carries a star.
        return f"{label}  ★" if option == self.recommended else label

    def relabel(self, options):
        """New labels for the same choices (the resolutions at another aspect ratio)."""
        for option, label in options:
            if option in self.labels:
                self.labels[option].config(text=self.text(option, label))

    def set(self, value, notify=False):
        self.value = value
        self.restyle()
        if notify:
            self.command(value)

    def restyle(self):
        for option, item in self.labels.items():
            selected = option == self.value
            if self.enabled:
                item.config(bg=GOLD if selected else PANEL, fg=INK if selected else TEXT)
            else:
                item.config(bg=PANEL_HI if selected else PANEL, fg=MUTED)


class Dropdown(tk.Label):
    """A choice from a longer list: the current one, and a menu of all on click."""

    def __init__(self, parent, options, value, command, font):
        super().__init__(parent, font=font, bg=PANEL, fg=TEXT, padx=12, pady=5, cursor="hand2", anchor="w",
                         highlightthickness=1, highlightbackground=LINE)
        self.options, self.command, self.font = options, command, font
        self.set(value)
        self.bind("<Button-1>", lambda e: self.open())
        self.bind("<Enter>", lambda e: self.config(bg=PANEL_HI))
        self.bind("<Leave>", lambda e: self.config(bg=PANEL))

    def set(self, value, notify=False):
        self.value = value
        label = next((label for option, label in self.options if option == value), value)
        self.config(text=f"{label}   \u25be")
        if notify:
            self.command(value)

    def open(self):
        menu = tk.Menu(self, tearoff=0, bg=PANEL, fg=TEXT, activebackground=GOLD, activeforeground=INK,
                       font=self.font, bd=0)
        for option, label in self.options:
            menu.add_command(label=("\u2713  " if option == self.value else "     ") + label,
                             command=lambda v=option: self.set(v, notify=True))
        menu.tk_popup(self.winfo_rootx(), self.winfo_rooty() + self.winfo_height())


class ScrollBar(tk.Canvas):
    """A thin scrollbar in the launcher's colors for a scrolling canvas (its yscrollcommand)."""

    def __init__(self, parent, scale, target):
        super().__init__(parent, width=max(6, int(10 * scale)), bg=parent["bg"], highlightthickness=0)
        self.target, self.first, self.last, self.hover, self.grab = target, 0.0, 1.0, False, None
        self.bind("<Configure>", lambda e: self.draw())
        self.bind("<Enter>", lambda e: self.highlight(True))
        self.bind("<Leave>", lambda e: self.highlight(False))
        self.bind("<Button-1>", self.press)
        self.bind("<B1-Motion>", self.drag)
        self.bind("<ButtonRelease-1>", self.release)

    def set(self, first, last):
        self.first, self.last = float(first), float(last)
        self.draw()

    def highlight(self, hover):
        self.hover = hover
        self.draw()

    def draw(self):
        self.delete("all")
        w, h = self.winfo_width(), self.winfo_height()
        if h <= 1 or self.last - self.first >= 1:
            return
        x1, x2, r = 2, w - 2, max(1, (w - 4) // 2)
        round_rect(self, x1, 1, x2, h - 1, r, fill=PANEL, outline=PANEL)
        top = min(int(self.first * h), h - 24)
        bottom = min(h, max(int(self.last * h), top + 24))
        color = MUTED if self.hover or self.grab is not None else DIM
        round_rect(self, x1, top + 1, x2, bottom - 1, r, fill=color, outline=color)

    def press(self, event):
        position = event.y / max(1, self.winfo_height())
        if self.first <= position <= self.last:
            self.grab = position - self.first
        else:  # on the track: jump there, then drag from the thumb's middle
            self.grab = (self.last - self.first) / 2
            self.target.yview_moveto(position - self.grab)

    def drag(self, event):
        if self.grab is not None:
            self.target.yview_moveto(event.y / max(1, self.winfo_height()) - self.grab)

    def release(self, event):
        self.grab = None
        self.draw()


class Launcher:
    def __init__(self, root, direct=None):
        self.root = root
        # Direct start (--play u1|u2|u3, GitHub #25): the game starts at once with the saved
        # settings, without the launcher window, which closes with the game.
        self.direct = direct
        self.s = max(1.0, root.winfo_fpixels("1i") / 96.0)
        self.config = read_config()
        if self.config.get("game") not in GAME_KEYS:
            self.config["game"] = "u1"
        for key in GAME_KEYS:
            if not controls_from_ini(read_ini(key), self.config["games"][key]):
                write_ini(key, controls_to_ini(self.config["games"][key]))
        self.reload_ini()
        self.process = None
        self.output = queue.Queue()
        self.last_output = 0.0
        self.game_started = False
        self.widgets = {}
        self.images = []
        # The graphics card (as last detected; detect_gpu checks again) for the Quick Setup advice.
        self.gpu_name = self.config.get("gpu")
        self.recommended = gpu_tier(self.gpu_name)
        self.fonts()
        root.title(f"{APP_TITLE} - Launcher by BilalReloaded")
        root.configure(bg=BG)
        root.resizable(True, True)
        self.width = self.px(1180)
        try:
            root.iconbitmap(default=str(ASSETS / "uncharted.ico"))
        except tk.TclError:
            pass
        self.build()
        root.update_idletasks()
        root.minsize(root.winfo_reqwidth(), root.winfo_reqheight())
        self.fit_to_screen()
        self.restore_window_position()
        write_config(self.config)  # keeps one-time updates (read_config) even without a change
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(150, self.poll)
        # Updates (launcher/updater.py): what an installed one left behind goes; a newer release
        # shows the UPDATE button.
        self.update_release = None
        self.update_busy = False
        self.update_result = None
        root.after(3000, lambda: updater.cleanup(ROOT))
        if self.config.get("check_updates", True) and not direct:
            root.after(1500, self.check_updates)
        if not direct:
            self.detect_gpu()
        if direct:
            self.select_game(direct, save=False)
            root.after(100, self.play_direct)

    def restore_window_position(self):
        """The window opens where it was last closed, if that spot is still on a screen."""
        position = self.config.get("window_xy")
        if not (isinstance(position, list) and len(position) == 2):
            return
        x, y = int(position[0]), int(position[1])
        self.root.update_idletasks()
        screen_w, screen_h = self.root.winfo_vrootwidth(), self.root.winfo_vrootheight()
        if -50 <= x <= max(0, screen_w - 200) and 0 <= y <= max(0, screen_h - 200):
            # All of the window on the screen (fit_to_screen may have made it taller than it was).
            y = min(y, max(0, self.root.winfo_screenheight() - self.root.winfo_reqheight() - self.px(70)))
            self.root.geometry(f"+{x}+{y}")

    def fit_to_screen(self):
        """More room for the settings when the screen is tall enough (fewer pages need to scroll)."""
        spare = self.root.winfo_screenheight() - self.px(110) - self.root.winfo_reqheight()
        if spare > 0:
            self.body.config(height=self.body.winfo_reqheight() + min(spare, self.px(240)))
            self.root.update_idletasks()

    def detect_gpu(self):
        """Finds the graphics card in the background (Windows' list takes a moment) and marks the
        Quick Setup preset that suits it."""
        found = []
        threading.Thread(target=lambda: found.append(gpu_names()), daemon=True).start()

        def done():
            if not found:
                self.root.after(200, done)
                return
            names = found[0]
            if not names:
                return
            known = [name for name in names if gpu_tier(name)]
            name = max(known, key=lambda n: PRESET_ORDER.index(gpu_tier(n))) if known else names[0]
            self.gpu_name, self.recommended = name, gpu_tier(name)
            if self.config.get("gpu") != name:
                self.config["gpu"] = name
                write_config(self.config)
            if hasattr(self, "preset_cards"):
                self.redraw_presets()
        self.root.after(200, done)

    def px(self, value):
        return int(value * self.s)

    def fonts(self):
        self.f_title = ("Georgia", 40, "bold")
        self.f_subtitle = ("Segoe UI Semibold", 11)
        self.f_card_number = ("Georgia", 30, "bold")
        self.f_card_title = ("Georgia", 14, "bold")
        self.f_body = ("Segoe UI", 10)
        self.f_body_bold = ("Segoe UI Semibold", 10)
        self.f_small = ("Segoe UI", 9)
        self.f_tab = ("Segoe UI Semibold", 10)
        self.f_caps = ("Segoe UI Semibold", 8)
        self.f_play = ("Georgia", 17, "bold")

    # -- layout -----------------------------------------------------------------------------

    def build(self):
        self.build_hero()
        self.build_cards()
        self.build_footer()
        self.build_tabs()
        self.select_game(self.config.get("game", "u1"), save=False)
        self.show_tab(self.config.get("tab", "setup") if self.config.get("tab") in self.pages else "setup",
                      save=False)

    def build_hero(self):
        w, h = self.width, self.px(170)
        art = None
        for name in ("pic1.png", "pic0.png"):
            if (self.game_dir() / "sce_sys" / name).exists():
                art = Image.open(self.game_dir() / "sce_sys" / name).convert("RGB")
                break
        if art is None:
            art = Image.new("RGB", (1920, 1080), "#d9c9a6")
        # Fill the width; keep Drake (right) and the top of the parchment in view.
        scale = w / art.width
        art = art.resize((w, int(art.height * scale)), Image.LANCZOS)
        top = int(art.height * 0.02)
        art = art.crop((0, top, w, top + h))
        # Fade into the dark panel below.
        fade = Image.new("L", (w, h), 0)
        draw = ImageDraw.Draw(fade)
        start = int(h * 0.62)
        for y in range(start, h):
            draw.line([(0, y), (w, y)], fill=int(255 * ((y - start) / (h - start)) ** 1.6))
        dark = Image.new("RGB", (w, h), BG)
        art = Image.composite(dark, art, fade)
        photo = ImageTk.PhotoImage(art)
        self.images.append(photo)
        canvas = tk.Canvas(self.root, width=w, height=h, bg=BG, highlightthickness=0)
        canvas.pack(side="top")
        canvas.create_image(0, 0, image=photo, anchor="nw")
        x, y = self.px(48), self.px(52)
        canvas.create_text(x, y, text="UNCHARTED", font=self.f_title, fill="#3b2914", anchor="w")
        canvas.create_text(x + self.px(3), y + self.px(44), text="T H E   N A T H A N   D R A K E   C O L L E C T I O N",
                           font=self.f_subtitle, fill="#8a6326", anchor="w")
        canvas.create_line(x + self.px(3), y + self.px(66), x + self.px(330), y + self.px(66), fill="#b98d45", width=2)
        self.draw_links(canvas, w)

    def draw_links(self, canvas, width):
        """The author's Patreon, YouTube channel and GitHub as badges in the top right of the hero."""
        right, top, height = width - self.px(22), self.px(18), self.px(30)
        for label, url, color, hover in reversed(LINKS):
            tag = f"link-{label}"
            text = canvas.create_text(0, 0, text=label, font=self.f_body_bold, fill="white", anchor="w", tags=tag)
            x1, _, x2, _ = canvas.bbox(text)
            badge_w = (x2 - x1) + self.px(46)
            left = right - badge_w
            mid = top + height // 2
            canvas.coords(text, left + self.px(34), mid)
            badge = round_rect(canvas, left, top, right, top + height, height // 2, fill=color, outline=color, tags=tag)
            canvas.tag_lower(badge, text)
            # Icon: YouTube's play triangle on white, a heart for Patreon, GitHub's code brackets.
            icon_x = left + self.px(12)
            if label == "Patreon":
                canvas.create_text(icon_x + self.px(8), mid, text="♥", font=self.f_body_bold, fill="white",
                                   tags=tag)
            elif label == "YouTube":
                round_rect(canvas, icon_x, mid - self.px(7), icon_x + self.px(16), mid + self.px(7), self.px(4),
                           fill="white", outline="white", tags=tag)
                canvas.create_polygon(icon_x + self.px(6), mid - self.px(4), icon_x + self.px(6), mid + self.px(4),
                                      icon_x + self.px(12), mid, fill=color, outline=color, tags=tag)
            else:
                canvas.create_text(icon_x + self.px(8), mid, text="</>", font=self.f_small, fill="white", tags=tag)
            canvas.tag_bind(tag, "<Button-1>", lambda e, u=url: webbrowser.open(u))
            canvas.tag_bind(tag, "<Enter>", lambda e, b=badge, c=hover: (canvas.itemconfig(b, fill=c, outline=c),
                                                                       canvas.config(cursor="hand2")))
            canvas.tag_bind(tag, "<Leave>", lambda e, b=badge, c=color: (canvas.itemconfig(b, fill=c, outline=c),
                                                                       canvas.config(cursor="")))
            right = left - self.px(10)

    def build_cards(self):
        frame = tk.Frame(self.root, bg=BG)
        frame.pack(side="top", fill="x", padx=self.px(40), pady=(0, self.px(12)))
        self.cards = {}
        gap = self.px(16)
        card_w = (self.width - 2 * self.px(40) - 2 * gap) // 3
        card_h = self.px(72)
        for i, game in enumerate(GAMES):
            canvas = tk.Canvas(frame, width=card_w, height=card_h, bg=BG, highlightthickness=0, cursor="hand2")
            canvas.grid(row=0, column=i, padx=(0 if i == 0 else gap, 0))
            canvas.bind("<Button-1>", lambda e, k=game["key"]: self.select_game(k))
            canvas.bind("<Double-Button-1>", lambda e, k=game["key"]: self.play_game(k))
            canvas.bind("<Enter>", lambda e, k=game["key"]: self.draw_card(k, hover=True))
            canvas.bind("<Leave>", lambda e, k=game["key"]: self.draw_card(k))
            self.cards[game["key"]] = (canvas, game, card_w, card_h)

    def draw_card(self, key, hover=False):
        canvas, game, w, h = self.cards[key]
        selected = key == self.config.get("game")
        canvas.delete("all")
        fill = PANEL_HI if selected or hover else PANEL
        outline = GOLD if selected else ("#5a4733" if hover else LINE)
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(10), fill=fill, outline=outline, width=2 if selected else 1)
        canvas.create_text(self.px(22), h // 2, text=game["number"], font=self.f_card_number,
                           fill=GOLD if selected else DIM, anchor="w")
        canvas.create_text(self.px(66), h // 2 - self.px(10), text=game["title"], font=self.f_card_title,
                           fill=TEXT, anchor="w")
        canvas.create_text(self.px(67), h // 2 + self.px(13), text=game["subtitle"], font=self.f_small,
                           fill=MUTED, anchor="w")
        if selected:
            canvas.create_text(w - self.px(18), self.px(16), text="SELECTED", font=("Segoe UI Semibold", 7),
                               fill=GOLD, anchor="e")

    def select_game(self, key, save=True):
        if key not in self.cards:
            key = "u1"
        changed = key != self.config.get("game")
        self.config["game"] = key
        for k in self.cards:
            self.draw_card(k)
        if save:
            write_config(self.config)
        if changed:
            # Each game keeps its own settings: show the selected game's.
            self.reload_ini()
            self.refresh_controls()
        if hasattr(self, "status") and not self.process:
            self.set_status(f"Ready to play {self.current_game()['name']}.")
        self.update_multiplayer_tab()

    def update_multiplayer_tab(self):
        """The Multiplayer tab shows only while Uncharted 2 is chosen (its net games)."""
        cell = getattr(self, "tab_cells", {}).get("multiplayer")
        if cell is None:
            return
        if self.config.get("game") == "u2":
            if not cell.winfo_manager():
                cell.pack(side="left", padx=(0, self.px(26)))
        else:
            cell.pack_forget()
            if getattr(self, "current_tab", None) == "multiplayer":
                self.show_tab("setup", save=False)

    def game_settings(self):
        """Preset, render resolution and multithreading of the selected game."""
        return self.config["games"][self.config["game"]]

    def save_ini(self, updates):
        write_ini(self.config["game"], updates)
        self.ini_seen = ini_mtime(self.config["game"])

    def reload_ini(self):
        """The selected game's settings from its file (the in-game menu writes it too)."""
        self.ini = read_ini(self.config["game"])
        self.ini_seen = ini_mtime(self.config["game"])
        if controls_from_ini(self.ini, self.game_settings()):
            write_config(self.config)

    def save_controls(self):
        """The selected game's controls, also into its settings file (the game reads them there)."""
        write_config(self.config)
        self.save_ini(controls_to_ini(self.game_settings()))

    def follow_ini(self):
        """Shows changes the in-game menu saved (Insert, then closing the menu)."""
        if ini_mtime(self.config["game"]) != getattr(self, "ini_seen", None):
            self.reload_ini()
            self.refresh_controls()

    def refresh_controls(self):
        """Every control shows the selected game's settings."""
        for key in INI_DEFAULTS:
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]) == "1")
            elif isinstance(widget, Segmented):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]))
        # The performance counter's items are Chip pills (overlay_panel): the loop above skips them.
        for key, widget in getattr(self, "overlay_chips", {}).items():
            if isinstance(widget, Chip):
                widget.set(self.ini.get(key, INI_DEFAULTS[key]) == "1")
        settings = self.game_settings()
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].set(settings["resolution"])
        self.refresh_render_size()
        if isinstance(self.widgets.get("aspect"), Segmented):
            self.widgets["aspect"].set(settings.get("aspect", "16:9"))
        if isinstance(self.widgets.get("upscale"), Segmented):
            self.widgets["upscale"].set(settings.get("upscale", "off"))
        self.refresh_render_size()
        if isinstance(self.widgets.get("multithreaded"), Toggle):
            self.widgets["multithreaded"].set(settings["multithreaded"])
        if isinstance(self.widgets.get("save_profile"), Segmented):
            self.widgets["save_profile"].set(str(settings.get("save_profile", "1")))
        if isinstance(self.widgets.get("mouse"), Toggle):
            self.widgets["mouse"].set(settings.get("mouse", True))
            self.widgets["mouse_sens"].set(settings.get("mouse_sens", "1.0"))
            self.widgets["mouse_invert"].set(settings.get("mouse_invert", False))
            self.widgets["mouse_aim_sens"].set(settings.get("mouse_aim_sens", "1.0"))
            self.widgets["mouse_vertical"].set(settings.get("mouse_vertical", "1.0"))
            self.widgets["mouse_invert_x"].set(settings.get("mouse_invert_x", False))
            self.widgets["vibration"].set(settings.get("vibration", True))
            self.widgets["prompts"].set(settings.get("prompts", "auto"))
            self.refresh_pad_slots()
        if hasattr(self, "binding_slots"):
            self.refresh_bindings()
        self.sync_sharpening()
        if hasattr(self, "preset_cards"):
            self.redraw_presets()
        self.update_fps_note()
        if hasattr(self, "ini_label"):
            self.ini_label.config(text=f"Settings: {ini_file(self.config['game'])}")

    def game_dir(self):
        folder = self.config.get("game_dir") or ""
        if not folder:
            return DEFAULT_GAME_DIR
        if WINE and not Path(folder).exists():
            # Chosen in another prefix (another Steam shortcut): the same folder by its Linux path.
            here = windows_path(self.config.get("game_dir_unix"))
            if here and Path(here).exists():
                return Path(here)
        return Path(folder)

    def choose_game_dir(self):
        from tkinter import filedialog
        folder = filedialog.askdirectory(
            title="Select the Uncharted game folder (the one with eboot.bin and sce_sys)",
            initialdir=str(self.game_dir() if self.game_dir().exists() else ROOT))
        if not folder:
            return False
        if not is_game_dir(folder):
            messagebox.showerror(APP_TITLE, "That folder has no eboot.bin and sce_sys/param.sfo.\n\n"
                                            "Choose the game's root folder (CUSA02344).")
            return False
        self.config["game_dir"] = folder
        if WINE:
            self.config["game_dir_unix"] = unix_path(folder)
        write_config(self.config)
        if hasattr(self, "game_dir_label"):
            self.game_dir_label.config(text=f"Game files: {folder}")
            self.update_label.config(text=self.update_text())
        return True

    def chosen_update(self):
        """The update folder chosen in Advanced (Choose update folder), if it is still there."""
        folder = self.config.get("update_dir") or ""
        if folder:
            for candidate in (Path(folder), Path(folder) / "uroot"):
                if is_game_dir(candidate):
                    return candidate
        return None

    def update_dir(self):
        """The update the games use: the chosen folder, else one next to the game folder."""
        return self.chosen_update() or game_update_dir(self.game_dir())

    def update_text(self):
        update = self.update_dir()
        if update:
            return f"Game update {app_version(update) or '?'} is used: {update}"
        folder = self.game_dir()
        version = app_version(folder)
        if version and version >= "01.02":
            return f"Game version {version} (the update is in the game folder)."
        title = folder.parent if folder.name.lower() == "uroot" else folder
        return (f"Game version {version or '?'}, no update found. For the 1.02 fixes, choose your update 1.02 "
                f"folder (or its .pkg) under GAME UPDATE.")

    def current_game(self):
        return next(g for g in GAMES if g["key"] == self.config.get("game"))

    def build_tabs(self):
        outer = tk.Frame(self.root, bg=BG)
        outer.pack(side="top", fill="both", expand=True, padx=self.px(40))
        strip = tk.Frame(outer, bg=BG)
        strip.pack(side="top", fill="x")
        self.tab_buttons = {}
        self.tab_cells = {}
        tabs = [("setup", "QUICK SETUP"), ("display", "DISPLAY"), ("performance", "PERFORMANCE"),
                ("controls", "CONTROLS"), ("cheats", "CHEATS"), ("advanced", "ADVANCED")]
        if multiplayer_preview():
            tabs.append(("multiplayer", "MULTIPLAYER"))
        for name, label in tabs:
            cell = tk.Frame(strip, bg=BG)
            cell.pack(side="left", padx=(0, self.px(26)))
            text = tk.Label(cell, text=label, font=self.f_tab, bg=BG, fg=MUTED, cursor="hand2")
            text.pack(side="top", pady=(0, self.px(6)))
            bar = tk.Frame(cell, bg=BG, height=max(2, self.px(2)))
            bar.pack(side="top", fill="x")
            for widget in (cell, text):
                widget.bind("<Button-1>", lambda e, n=name: self.show_tab(n))
            self.tab_buttons[name] = (text, bar)
            self.tab_cells[name] = cell
        tk.Frame(outer, bg=LINE, height=1).pack(side="top", fill="x")
        self.body = tk.Frame(outer, bg=BG, height=self.px(330))
        self.body.pack(side="top", fill="both", expand=True, pady=(self.px(14), 0))
        self.body.pack_propagate(False)
        # Pages taller than the space under the tabs scroll (Display, Performance, Advanced, Controls).
        self.scroll_canvases = set()
        self.root.bind_all("<MouseWheel>", self.wheel)
        self.pages = {
            "setup": self.page_setup(),
            "display": self.page_display(),
            "performance": self.page_performance(),
            "controls": self.page_controls(),
            "cheats": self.page_cheats(),
            "advanced": self.page_advanced(),
        }
        if multiplayer_preview():
            self.pages["multiplayer"] = self.page_multiplayer()

    def show_tab(self, name, save=True):
        if name == "multiplayer" and self.config.get("game") != "u2":
            name = "setup"
        self.current_tab = name
        if name == "multiplayer" and not getattr(self, "net_searched", False):
            self.net_searched = True
            self.root.after(300, self.search_net_games)
        if save and self.config.get("tab") != name:
            self.config["tab"] = name
            write_config(self.config)
        for tab, (text, bar) in self.tab_buttons.items():
            text.config(fg=TEXT if tab == name else MUTED)
            bar.config(bg=GOLD if tab == name else BG)
        for tab, page in self.pages.items():
            holder = getattr(page, "holder", page)
            if tab == name:
                holder.pack(fill="both", expand=True)
            else:
                holder.pack_forget()

    def wheel(self, event):
        """The mouse wheel scrolls the page under the pointer, when it is taller than the window."""
        if getattr(self, "capturing", None) is not None:  # a Controls slot is waiting for an input
            return
        try:
            widget = self.root.winfo_containing(event.x_root, event.y_root)
        except (KeyError, tk.TclError):  # over a menu
            return
        while widget is not None:
            if str(widget) in self.scroll_canvases:
                if tuple(widget.yview()) != (0.0, 1.0):
                    step = int(-event.delta / 120) or (-1 if event.delta > 0 else 1)
                    widget.yview_scroll(step, "units")
                return
            widget = widget.master

    # -- pages ------------------------------------------------------------------------------

    def page(self, scroll=True):
        """A tab's page: the frame its rows go in. With `scroll`, it scrolls (with a scrollbar)
        when the rows are taller than the space under the tabs; show_tab packs its holder."""
        if not scroll:
            return tk.Frame(self.body, bg=BG)
        holder = tk.Frame(self.body, bg=BG)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = ScrollBar(holder, self.s, canvas)
        inner = tk.Frame(canvas, bg=BG)
        window = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)

        def layout(event=None):
            width, height, need = canvas.winfo_width(), canvas.winfo_height(), inner.winfo_reqheight()
            if height <= 1:  # not shown yet
                return
            canvas.itemconfigure(window, width=width)
            canvas.configure(scrollregion=(0, 0, width, max(need, height)))
            if need > height and not bar.winfo_manager():
                bar.pack(side="right", fill="y", padx=(self.px(8), 0), before=canvas)
            elif need <= height and bar.winfo_manager():
                bar.pack_forget()
                canvas.yview_moveto(0)
        inner.bind("<Configure>", layout)
        canvas.bind("<Configure>", layout)
        self.scroll_canvases.add(str(canvas))
        inner.holder = holder
        return inner

    def row(self, page, title, hint, control_factory):
        row = tk.Frame(page, bg=BG)
        row.pack(side="top", fill="x", pady=(0, self.px(10)))
        text = tk.Frame(row, bg=BG)
        text.pack(side="left", fill="x", expand=True)
        tk.Label(text, text=title, font=self.f_body_bold, bg=BG, fg=TEXT, anchor="w").pack(side="top", fill="x")
        if hint:
            tk.Label(text, text=hint, font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
                     wraplength=self.px(560)).pack(side="top", fill="x")
        control = control_factory(row)
        control.pack(side="right", padx=(self.px(20), 0))
        return control

    def ini_toggle(self, page, key, title, hint):
        def factory(parent):
            return Toggle(parent, self.s, self.ini.get(key, "0") == "1",
                          lambda v, k=key: self.change(k, "1" if v else "0"))
        self.widgets[key] = self.row(page, title, hint, factory)

    def ini_choice(self, page, key, title, hint, options, recommended=None):
        def factory(parent):
            return Segmented(parent, options, self.ini.get(key, INI_DEFAULTS.get(key)), lambda v, k=key: self.change(k, v),
                             self.f_small, recommended)
        self.widgets[key] = self.row(page, title, hint, factory)

    def page_setup(self):
        page = self.page()
        self.setup_intro = tk.Label(page, text="", font=self.f_body, bg=BG, fg=MUTED, anchor="w")
        self.setup_intro.pack(side="top", fill="x", pady=(0, self.px(12)))
        grid = tk.Frame(page, bg=BG)
        grid.pack(side="top", fill="x")
        self.preset_cards = {}
        gap = self.px(16)
        card_w = (self.width - 2 * self.px(40) - 3 * gap) // 4
        card_h = self.px(150)
        for i, name in enumerate(PRESET_ORDER):
            canvas = tk.Canvas(grid, width=card_w, height=card_h, bg=BG, highlightthickness=0, cursor="hand2")
            canvas.grid(row=0, column=i, padx=(0 if i == 0 else gap, 0))
            canvas.bind("<Button-1>", lambda e, n=name: self.apply_preset(n))
            canvas.bind("<Enter>", lambda e, n=name: self.draw_preset(n, hover=True))
            canvas.bind("<Leave>", lambda e, n=name: self.draw_preset(n))
            self.preset_cards[name] = (canvas, card_w, card_h)
        self.custom_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=MUTED, anchor="w")
        self.custom_note.pack(side="top", fill="x", pady=(self.px(10), 0))
        self.root.after_idle(self.redraw_presets)
        return page

    def draw_preset(self, name, hover=False):
        canvas, w, h = self.preset_cards[name]
        preset = PRESETS[name]
        selected = self.game_settings().get("preset") == name
        canvas.delete("all")
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(10), fill=PANEL_HI if selected or hover else PANEL,
                   outline=GOLD if selected else ("#5a4733" if hover else LINE), width=2 if selected else 1)
        canvas.create_text(self.px(20), self.px(26), text=preset["title"], font=self.f_card_title,
                           fill=GOLD if selected else TEXT, anchor="w")
        canvas.create_text(self.px(20), self.px(50), text=preset["summary"], font=self.f_body_bold,
                           fill=TEXT, anchor="w")
        canvas.create_text(self.px(20), self.px(70), text=preset["detail"], font=self.f_small, fill=MUTED,
                           anchor="nw", width=w - self.px(40))
        if name == self.recommended:
            badge = canvas.create_text(w - self.px(20), self.px(26), text="RECOMMENDED", font=("Segoe UI Semibold", 7),
                                       fill=INK, anchor="e")
            x1, y1, x2, y2 = canvas.bbox(badge)
            pill = round_rect(canvas, x1 - self.px(8), y1 - self.px(3), x2 + self.px(8), y2 + self.px(3), self.px(8),
                              fill=GOLD, outline=GOLD)
            canvas.tag_lower(pill, badge)

    def redraw_presets(self):
        for name in self.preset_cards:
            self.draw_preset(name)
        if self.recommended:
            self.setup_intro.config(text=f"Recommended for your graphics card ({self.gpu_name}): "
                                         f"{PRESETS[self.recommended]['title'].title()}. Every setting can still be "
                                         "changed in Display and Performance.")
        else:
            self.setup_intro.config(text="Pick a quality level. Every setting can still be changed in Display "
                                         "and Performance.")
        custom = self.game_settings().get("preset") not in PRESETS
        name = self.current_game()["name"]
        self.custom_note.config(text=f"{name}: custom settings (changed by hand)." if custom
                                else f"Settings are kept per game: these are {name}'s.")

    def apply_preset(self, name):
        settings = self.game_settings()
        settings["resolution"] = PRESETS[name]["resolution"]
        settings["upscale"] = PRESETS[name]["upscale"]
        if "resolution" in self.widgets:
            self.widgets["resolution"].set(settings["resolution"])
        for key, value in preset_values(name).items():
            self.ini[key] = value
            widget = self.widgets.get(key)
            if isinstance(widget, Toggle):
                widget.set(value == "1")
            elif isinstance(widget, Segmented):
                widget.set(value)
        self.sync_sharpening()
        settings["preset"] = name
        write_config(self.config)
        self.save_ini({k: self.ini[k] for k in INI_DEFAULTS})
        self.refresh_render_size()
        self.redraw_presets()
        self.update_fps_note()

    def page_display(self):
        page = self.page()

        # The screen's shape first, then the resolution at that shape.
        def aspect(parent):
            return Segmented(parent, ASPECTS, self.game_settings().get("aspect", "16:9"),
                             self.change_aspect, self.f_small)
        self.widgets["aspect"] = self.row(
            page, "Aspect ratio",
            "For ultrawide monitors (21:9, 32:9), with Borderless or Fullscreen: the games show more at "
            "the sides and the same view top to bottom as on a 16:9 screen. 16:10 (Steam Deck: 800p is "
            "1280x800) fills the screen and shows more at the top and bottom.", aspect)

        def hud(parent):
            return Segmented(parent, HUD_MODES, self.config.get("hud", "16:9"),
                             lambda value: self.change_config("hud", value), self.f_small)
        self.widgets["hud"] = self.row(
            page, "HUD (Uncharted 2, ultrawide)",
            "At 21:9 and 32:9: 16:9 keeps the HUD and menus in the middle 16:9 of the screen; Wide "
            "moves the corner elements (ammo, prompts at the sides) out to the screen edges, the rest "
            "stays in the middle (experimental); Stretched is the game's own, stretched to the sides.",
            hud)

        def hud_wide(parent):
            return Segmented(parent, HUD_WIDE_AREAS, self.config.get("hud_wide_aspect", "screen"),
                             lambda value: self.change_config("hud_wide_aspect", value),
                             self.f_small)
        self.widgets["hud_wide_aspect"] = self.row(
            page, "Wide HUD edges",
            "With HUD Wide: where the side elements go \u2014 the screen's edges, or the edges of a "
            "21:9 area (closer in on a 32:9 screen).",
            hud_wide)

        def resolution(parent):
            return Segmented(parent, resolution_options(self.game_settings().get("aspect", "16:9")),
                             self.game_settings()["resolution"], self.change_resolution, self.f_small)
        self.widgets["resolution"] = self.row(
            page, "Resolution",
            "How sharp the picture is; higher needs a faster graphics card (the PS4 drew 1080p). With "
            "upscaling below, the game draws less and is upscaled to it. If a game crashes above 1080p, "
            "go back to 1080p. Drake's Fortune draws at most 1620p and scales up.", resolution)
        self.render_size_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="w")
        self.render_size_note.pack(side="top", fill="x", pady=(0, self.px(10)))
        self.refresh_render_size()
        def upscaling(parent):
            return Segmented(parent, upscaling_options(), upscaling_choice(self.ini), self.change_upscaling,
                             self.f_small)
        self.widgets["upscaling"] = self.row(
            page, "Upscaling",
            "More FPS: the game draws below the resolution and each frame is upscaled to it; the HUD and "
            "menus stay sharp at the resolution. FSR 1 is the lightest and works on any card; FSR 3.1, "
            "FSR 4 and DLSS (NVIDIA RTX cards) also use the previous frames: more detail, and they smooth "
            "edges themselves. Also in the in-game menu.", upscaling)

        # Shown with an upscaler only (refresh_upscale), right under it.
        def upscale(parent):
            box = tk.Frame(parent, bg=BG)
            self.widgets["upscale"] = Segmented(
                box, UPSCALE_PRESETS, self.game_settings().get("upscale", "off"),
                lambda v: (self.change_game_setting("upscale", v), self.refresh_render_size()), self.f_small)
            self.widgets["upscale"].pack(side="top", anchor="e")
            self.upscale_note = tk.Label(box, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="e")
            self.upscale_note.pack(side="top", anchor="e", pady=(self.px(4), 0))
            return box
        self.upscale_row = self.row(
            page, "Upscaling quality",
            "How much below the resolution the game draws: Quality 1/1.5, Balanced 1/1.7, Performance 1/2, "
            "Ultra Performance 1/3 of its width and height. Native AA (DLAA with DLSS) draws at the "
            "resolution and only smooths edges.", upscale).master

        def anti_aliasing(parent):
            return Segmented(parent, [("off", "Off"), ("taa", "TAA")],
                             anti_aliasing_choice(self.ini, self.game_settings()), self.change_anti_aliasing,
                             self.f_small)
        self.widgets["aa"] = self.row(
            page, "Anti-aliasing",
            "Smooths jagged, shimmering edges, with upscaling Off or FSR 1. FSR 3.1, FSR 4 and DLSS do "
            "it themselves.", anti_aliasing)
        self.refresh_render_size()
        self.ini_choice(page, "display_mode", "Display mode",
                        "A normal window, a borderless window (quick Alt+Tab) or fullscreen.",
                        [("windowed", "Windowed"), ("borderless", "Borderless"), ("fullscreen", "Fullscreen")])
        self.ini_toggle(page, "vsync", "VSync", "Stops tearing (a line across the picture when the camera turns). "
                                               "Recommended on.")

        def sharpening(parent):
            value = self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off"
            return Segmented(parent, [("off", "Off"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")],
                             value, self.change_sharpening, self.f_small)
        self.widgets["sharpening"] = self.row(page, "Sharpening", "Makes the final picture crisper (AMD FidelityFX RCAS).",
                                              sharpening)
        self.ini_toggle(page, "show_fps", "Show performance counter",
                        "FPS, temperatures and memory in a corner of the screen. Insert, then the Overlay "
                        "tab, changes it in the game too.")
        self.overlay_panel(page)
        return page

    # The performance counter's items: the ini key and the label on its pill.
    OVERLAY_ITEMS = (("overlay_fps", "FPS"), ("overlay_frametime", "Frame time"), ("overlay_gpu", "GPU"),
                     ("overlay_cpu", "CPU"), ("overlay_ram", "RAM"), ("overlay_vram", "VRAM"),
                     ("overlay_upscaler", "Upscaler"))

    def overlay_panel(self, page):
        """What the performance counter shows, where and how big (Display tab; probe-u*.ini's
        show_fps and overlay_* keys, the ones the in-game menu's Overlay tab writes). It is built
        on its own: an error here leaves the rest of the page and the launcher working."""
        try:
            self._overlay_panel(page)
        except Exception:
            log_launcher_error(*sys.exc_info())
            self.overlay_chips = {}

    def _overlay_panel(self, page):
        def items(parent):
            box = tk.Frame(parent, bg=BG)
            self.overlay_chips = {}
            for key, label in self.OVERLAY_ITEMS:
                chip = Chip(box, self.s, self.f_small, label,
                            self.ini.get(key, INI_DEFAULTS[key]) == "1",
                            lambda v, k=key: self.change(k, "1" if v else "0"))
                chip.pack(side="left", padx=(self.px(6), 0))
                self.overlay_chips[key] = chip
            return box
        self.row(page, "What it shows",
                 "The frame rate, the frame time in milliseconds, the graphics card and the processor "
                 "with their temperatures, and memory. Pick any; with nothing picked the counter is "
                 "just its box.", items)
        self.ini_choice(page, "overlay_corner", "Counter corner", "Where the counter sits.",
                        [("0", "Top left"), ("1", "Top right"), ("2", "Bottom left"),
                         ("3", "Bottom right")])
        self.ini_choice(page, "overlay_layout", "Counter layout", "One line, or one item per line.",
                        [("0", "One line"), ("1", "Stacked")])
        self.ini_choice(page, "overlay_scale", "Counter size", "Text size of the counter.",
                        [("75", "75%"), ("100", "100%"), ("125", "125%"), ("150", "150%"),
                         ("200", "200%")])
        self.ini_choice(page, "overlay_opacity", "Counter background",
                        "How dark the counter's box is. None leaves the text on the picture.",
                        [("0", "None"), ("25", "25%"), ("50", "50%"), ("75", "75%"), ("100", "100%")])

    def change_sharpening(self, value):
        if value == "off":
            self.change("rcas", "0")
        else:
            self.ini["rcas"] = "1"
            self.change("rcas_strength", value)
            self.change("rcas", "1")

    def sync_sharpening(self):
        widget = self.widgets.get("sharpening")
        if widget:
            widget.set(self.ini.get("rcas_strength", "75") if self.ini.get("rcas", "1") == "1" else "off")

    def page_performance(self):
        page = self.page()
        tk.Label(page, text="★ marks the recommended choice.", font=self.f_small, bg=BG, fg=DIM,
                 anchor="w").pack(side="top", fill="x", pady=(0, self.px(6)))
        self.ini_choice(page, "fps_limit", "Frame rate limit",
                        "60 is how the games were made. Higher is smoother but experimental: if the game runs too "
                        "fast or Nate falls through the floor, go back to 60. With VSync, your monitor's refresh "
                        f"rate ({monitor_refresh_hz()} Hz) is the smoothest.",
                        [(str(v), label) for v, label in FPS_LIMITS], recommended="60")
        self.ini_choice(page, "frames_queued", "Frames queued",
                        "Keep 1. 2 can add a few FPS in heavy scenes, but in Uncharted 2 Nate and other "
                        "characters then float a little above the floor.",
                        [("1", "1"), ("2", "2"), ("0", "Unlimited")], recommended="1")
        self.ini_toggle(page, "async_shaders", "Compile shaders in the background",
                        "Fewer stutters when you reach new areas. Recommended on.")

        def multithreaded(parent):
            return Toggle(parent, self.s, self.game_settings()["multithreaded"],
                          lambda v: self.change_game_setting("multithreaded", v))
        self.widgets["multithreaded"] = self.row(
            page, "Multithreaded rendering (experimental)",
            "Prepares the picture on a second CPU core: more FPS where the CPU is the limit, mostly "
            "in Uncharted 2 and 3 (on by default in Uncharted 3; keep Frames queued at 1 with it). "
            "Turn it off if a game hangs while loading. Drake's Fortune always runs without it.",
            multithreaded)

        def job_threads(parent):
            value = "off" if self.config.get("job_sleep", "auto") == "off" else "on"
            return Segmented(parent, (("on", "Sleep"), ("off", "Spin")), value,
                             lambda v: self.change_config("job_sleep", v), self.f_small, recommended="on")
        self.row(page, "Idle game threads",
                 "Sleep: the game's helper threads rest while they wait, which keeps 4-6 core CPUs fast "
                 "and saves power. Spin keeps their cores busy, as on the PS4.", job_threads)

        self.fps_note = tk.Label(page, text="", font=self.f_small, bg=BG, fg=GOLD, anchor="w")
        self.fps_note.pack(side="top", fill="x")
        self.update_fps_note()
        return page

    def update_fps_note(self):
        if not hasattr(self, "fps_note"):
            return
        limit = self.ini.get("fps_limit", "60")
        hz = monitor_refresh_hz()
        if int(limit) > 60:
            self.fps_note.config(text=f"Frame rate unlocked up to {limit} FPS (your monitor: {hz} Hz). "
                                      "Applied when the game starts.")
        else:
            self.fps_note.config(text="")

    def page_controls(self):
        page = self.page(scroll=False)
        settings = self.game_settings()
        top = tk.Frame(page, bg=BG)
        top.pack(side="top", fill="x")
        tk.Label(top, text="Mouse", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse"] = Toggle(top, self.s, settings.get("mouse", True),
                                       lambda v: self.change_game_setting("mouse", v))
        self.widgets["mouse"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(top, text="Sensitivity", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_sens"] = Segmented(top, MOUSE_SENSITIVITIES, settings.get("mouse_sens", "1.0"),
                                               lambda v: self.change_game_setting("mouse_sens", v), self.f_small)
        self.widgets["mouse_sens"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(top, text="Invert Y", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_invert"] = Toggle(top, self.s, settings.get("mouse_invert", False),
                                              lambda v: self.change_game_setting("mouse_invert", v))
        self.widgets["mouse_invert"].pack(side="left", padx=(self.px(8), 0))
        self.flat_button(top, "Reset controls", self.reset_controls).pack(side="right")
        aim = tk.Frame(page, bg=BG)
        aim.pack(side="top", fill="x", pady=(self.px(10), 0))
        tk.Label(aim, text="Aiming", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_aim_sens"] = Segmented(aim, MOUSE_AIM_FACTORS, settings.get("mouse_aim_sens", "1.0"),
                                                   lambda v: self.change_game_setting("mouse_aim_sens", v),
                                                   self.f_small)
        self.widgets["mouse_aim_sens"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(aim, text="Up/down", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_vertical"] = Segmented(aim, MOUSE_FACTORS, settings.get("mouse_vertical", "1.0"),
                                                   lambda v: self.change_game_setting("mouse_vertical", v),
                                                   self.f_small)
        self.widgets["mouse_vertical"].pack(side="left", padx=(self.px(8), self.px(22)))
        tk.Label(aim, text="Invert X", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["mouse_invert_x"] = Toggle(aim, self.s, settings.get("mouse_invert_x", False),
                                                lambda v: self.change_game_setting("mouse_invert_x", v))
        self.widgets["mouse_invert_x"].pack(side="left", padx=(self.px(8), 0))
        # Controller vibration (#166): off stops every rumble.
        rumble = tk.Frame(page, bg=BG)
        rumble.pack(side="top", fill="x", pady=(self.px(10), 0))
        tk.Label(rumble, text="Controller vibration", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["vibration"] = Toggle(rumble, self.s, settings.get("vibration", True),
                                           lambda v: self.change_game_setting("vibration", v))
        self.widgets["vibration"].pack(side="left", padx=(self.px(8), 0))
        icons = tk.Frame(page, bg=BG)
        icons.pack(side="top", fill="x", pady=(self.px(10), 0))
        tk.Label(icons, text="Button icons", font=self.f_body_bold, bg=BG, fg=TEXT).pack(side="left")
        self.widgets["prompts"] = Segmented(icons, PROMPT_MODES, settings.get("prompts", "auto"),
                                            self.change_prompts, self.f_small)
        self.widgets["prompts"].pack(side="left", padx=(self.px(8), self.px(12)))
        tk.Label(icons, text="Auto follows the device you play with (a controller's own buttons).",
                 font=self.f_small, bg=BG,
                 fg=MUTED).pack(side="left")
        self.controls_hint = tk.Label(
            page, text="Click a slot, then press a key or a mouse button (right-click a slot to clear it). "
                       "In game the mouse is captured; Alt+Tab or Insert (menu) frees it. A controller "
                       "always works too.", font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
            wraplength=self.px(1080))
        self.controls_hint.pack(side="top", fill="x", pady=(self.px(6), self.px(4)))
        holder = tk.Frame(page, bg=BG)
        holder.pack(side="top", fill="both", expand=True)
        canvas = tk.Canvas(holder, bg=BG, highlightthickness=0)
        bar = ScrollBar(holder, self.s, canvas)
        inner = tk.Frame(canvas, bg=BG)
        inner.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        bar.pack(side="right", fill="y")
        canvas.pack(side="left", fill="both", expand=True)

        self.scroll_canvases.add(str(canvas))
        self.capturing = None
        self.binding_slots = {}
        self.pad_slots = {}
        self.pad_capture = None
        if not hasattr(self, "gamepads"):
            self.gamepads = Gamepads()
        columns = [tk.Frame(inner, bg=BG), tk.Frame(inner, bg=BG)]
        columns[0].grid(row=0, column=0, sticky="n", padx=(0, self.px(36)))
        columns[1].grid(row=0, column=1, sticky="n")
        for index, (group, actions) in enumerate(CONTROL_GROUPS):
            column = columns[0 if index < 2 else 1]
            tk.Label(column, text=group, font=self.f_small, bg=BG, fg=GOLD, anchor="w").pack(
                side="top", fill="x", pady=(self.px(6 if index % 2 else 2), self.px(2)))
            for action, label in actions:
                row = tk.Frame(column, bg=BG)
                row.pack(side="top", fill="x", pady=self.px(1))
                tk.Label(row, text=label, font=self.f_small, bg=BG, fg=TEXT, anchor="w",
                         width=17).pack(side="left")
                for slot in (0, 1):
                    cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14,
                                    pady=self.px(3), cursor="hand2", highlightthickness=1,
                                    highlightbackground=LINE)
                    cell.pack(side="left", padx=(self.px(6), 0))
                    cell.bind("<Button-1>", lambda e, a=action, i=slot: self.start_capture(a, i))
                    cell.bind("<Button-3>", lambda e, a=action, i=slot: self.set_binding(a, i, None))
                    self.binding_slots[(action, slot)] = cell
                # The controller button: its icon; a click, then a controller button rebinds it.
                pad_cell = tk.Label(row, text="", font=self.f_small, bg=PANEL, fg=TEXT, width=14, height=1,
                                    highlightthickness=1, highlightbackground=LINE, compound="center")
                pad_cell.pack(side="left", padx=(self.px(6), 0))
                if action in PAD_DEFAULTS:
                    pad_cell.config(cursor="hand2")
                    pad_cell.bind("<Button-1>", lambda e, a=action: self.start_pad_capture(a))
                    pad_cell.bind("<Button-3>", lambda e, a=action: self.set_pad_binding(a, None))
                self.pad_slots[action] = pad_cell
        self.refresh_bindings()
        self.refresh_pad_slots()
        return page

    def game_controls(self):
        return merged_controls(self.game_settings().get("controls"))

    def change_prompts(self, value):
        self.change_game_setting("prompts", value)
        self.refresh_pad_slots()

    def pad_style(self):
        style = self.game_settings().get("prompts", "auto")
        if style in ("xbox", "playstation"):
            return style
        return self.gamepads.style() or "xbox"  # Auto: the connected controller's

    def refresh_pad_slots(self):
        style = self.pad_style()
        controls = merged_pad_controls(self.game_settings().get("pad_controls"))
        if not hasattr(self, "pad_images"):
            self.pad_images = {}
        height = self.px(22)
        for action, cell in getattr(self, "pad_slots", {}).items():
            if self.pad_capture == action:
                cell.config(image="", text="Press a button…", fg=GOLD, bg=PANEL_HI, width=14, height=1)
                continue
            button = controls.get(action) if action in PAD_DEFAULTS else PAD_FIXED.get(action)
            if not button:
                cell.config(image="", text="—", fg=TEXT, bg=PANEL, width=14, height=1)
                continue
            key = (button, style, height)
            if key not in self.pad_images:
                self.pad_images[key] = ImageTk.PhotoImage(pad_icon(button, style, height))
            cell.config(image=self.pad_images[key], text="", bg=PANEL, width=self.px(100), height=self.px(24))

    def start_pad_capture(self, action):
        self.stop_capture()
        if not self.gamepads.sdl:
            self.controls_hint.config(text="No controller support found (SDL3.dll).", fg=STOP_RED)
            return
        self.pad_capture = action
        self.pad_capture_started = time.monotonic()
        self.pad_capture_held = self.gamepads.held()  # buttons already down do not count
        if not self.gamepads.pads:
            self.controls_hint.config(text="Connect a controller, then click the slot again.", fg=STOP_RED)
            self.pad_capture = None
            return
        self.controls_hint.config(text="Press a button on the controller (Escape cancels). Triggers and sticks "
                                       "stay Aim, Fire, Move and Look.", fg=MUTED)
        self.refresh_pad_slots()
        self.root.bind_all("<Escape>", lambda e: self.stop_pad_capture())
        self.root.after(30, self.poll_pad_capture)

    def poll_pad_capture(self):
        if self.pad_capture is None:
            return
        held = self.gamepads.held()
        new = [b for b in SDL_BUTTONS if b in held and b not in self.pad_capture_held]
        self.pad_capture_held &= held
        if any(t in held and t not in self.pad_capture_held for t in ("lefttrigger", "righttrigger")):
            self.controls_hint.config(text="The triggers stay Aim and Fire. Press a button.", fg=STOP_RED)
        if new:
            self.set_pad_binding(self.pad_capture, new[0])
            return
        if time.monotonic() - self.pad_capture_started > 8:
            self.stop_pad_capture()
            return
        self.root.after(30, self.poll_pad_capture)

    def stop_pad_capture(self):
        self.pad_capture = None
        self.root.unbind_all("<Escape>")
        self.refresh_pad_slots()

    def set_pad_binding(self, action, button):
        settings = self.game_settings()
        pad_controls = dict(settings.get("pad_controls") or {})
        merged = merged_pad_controls(pad_controls)
        if button:
            # A button does one thing: the action that had it swaps to this one's old button.
            for other, current in merged.items():
                if current == button and other != action:
                    pad_controls[other] = merged[action]
        pad_controls[action] = button or ""
        settings["pad_controls"] = {a: b for a, b in merged_pad_controls(pad_controls).items() if b != PAD_DEFAULTS.get(a)}
        self.save_controls()
        self.pad_capture = None
        self.root.unbind_all("<Escape>")
        self.refresh_pad_slots()
        self.controls_hint.config(text="Saved for " + self.current_game()["name"] + ". Right-click a controller "
                                       "slot to clear it; Reset controls restores the defaults.", fg=MUTED)

    def refresh_bindings(self):
        controls = self.game_controls()
        for (action, slot), cell in self.binding_slots.items():
            bindings = controls.get(action, [])
            capturing = self.capturing == (action, slot)
            cell.config(text="Press a key…" if capturing else binding_label(bindings[slot] if slot < len(bindings) else None),
                        bg=PANEL_HI if capturing else PANEL, fg=GOLD if capturing else TEXT)

    def start_capture(self, action, slot):
        self.stop_capture()
        self.capturing = (action, slot)
        self.refresh_bindings()
        self.root.focus_set()
        # After this click: the next key, mouse button or wheel step is the binding.
        self.root.after(200, self.arm_capture)

    def arm_capture(self):
        if self.capturing is None:
            return
        self.root.bind_all("<KeyPress>", self.capture_key)
        self.root.bind_all("<ButtonPress>", self.capture_button)
        self.root.bind_all("<MouseWheel>", self.capture_wheel)

    def stop_capture(self):
        if getattr(self, "capturing", None) is None:
            return
        self.capturing = None
        for sequence in ("<KeyPress>", "<ButtonPress>", "<MouseWheel>"):
            self.root.unbind_all(sequence)
        self.refresh_bindings()

    def capture_key(self, event):
        keysym = event.keysym
        if keysym in RESERVED_KEYS:
            self.controls_hint.config(text=f"{keysym} is used by the game window (Insert opens the menu, F9 "
                                           "records input, comma separates bindings). Choose another key.",
                                      fg=STOP_RED)
            self.stop_capture()
            return "break"
        name = TK_TO_SDL.get(keysym) or (keysym.upper() if len(keysym) == 1 and keysym.isalnum() else None)
        if not name:
            self.controls_hint.config(text=f"The key {keysym} can't be used. Choose another key.", fg=STOP_RED)
            self.stop_capture()
            return "break"
        action, slot = self.capturing
        self.set_binding(action, slot, f"key:{name}")
        return "break"

    def capture_button(self, event):
        # Tk 9 (the bundled Python 3.14) numbers the side buttons 8 and 9 on Windows (#152); 4 and
        # 5 were Tk 8's numbers.
        names = {1: "left", 2: "middle", 3: "right", 4: "x1", 5: "x2", 8: "x1", 9: "x2"}
        if event.num in names:
            self.capture_mouse(names[event.num])
        return "break"

    def capture_wheel(self, event):
        self.capture_mouse("wheel_up" if event.delta > 0 else "wheel_down")
        return "break"

    def capture_mouse(self, name):
        action, slot = self.capturing
        if action in STICK_ACTIONS:
            self.controls_hint.config(text="Stick directions take keys; the mouse moves the camera by itself.",
                                      fg=STOP_RED)
            self.stop_capture()
            return
        self.set_binding(action, slot, f"mouse:{name}")

    def set_binding(self, action, slot, binding):
        controls = self.game_controls()
        if binding:
            # A key or button does one thing: it leaves the action it had.
            for other, bindings in controls.items():
                if binding in bindings and (other, bindings.index(binding)) != (action, slot):
                    bindings.remove(binding)
        bindings = controls[action]
        while len(bindings) <= slot:
            bindings.append(None)
        bindings[slot] = binding
        controls[action] = [b for b in bindings if b][:2]
        self.game_settings()["controls"] = controls
        self.save_controls()
        self.controls_hint.config(
            text="Saved for " + self.current_game()["name"] + ". Click a slot, then press a key or a mouse button "
                 "(right-click a slot to clear it).", fg=MUTED)
        self.capturing = None
        for sequence in ("<KeyPress>", "<ButtonPress>", "<MouseWheel>"):
            self.root.unbind_all(sequence)
        self.refresh_bindings()

    def reset_controls(self):
        name = self.current_game()["name"]
        if not messagebox.askyesno(APP_TITLE, f"Reset {name}'s keyboard, mouse and controller controls?"):
            return
        settings = self.game_settings()
        settings.update({"controls": None, "mouse": True, "mouse_sens": "1.0", "mouse_invert": False,
                         "prompts": "auto", "pad_controls": None, "mouse_aim_sens": "1.0",
                         "mouse_vertical": "1.0", "mouse_invert_x": False, "vibration": True})
        self.save_controls()
        self.refresh_controls()

    def page_cheats(self):
        page = self.page()
        tk.Label(page, text="The games' own developer switches, applied when a game starts. They need update 1.02.",
                 font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x", pady=(0, self.px(6)))
        for name, title, hint in CHEATS:
            def cheat(parent, key="cheat_" + name):
                return Toggle(parent, self.s, self.config.get(key, False), lambda v, k=key: self.change_config(k, v))
            self.row(page, title, hint, cheat)
        return page

    def page_advanced(self):
        page = self.page()

        # Save profiles (#66): profile N plays as PS4 user N, with its saves in savedata\N.
        def save_profile(parent):
            return Segmented(parent, (("1", "Profile 1"), ("2", "Profile 2"), ("3", "Profile 3")),
                             str(self.game_settings().get("save_profile", "1")),
                             lambda v: self.change_game_setting("save_profile", v), self.f_small)
        self.widgets["save_profile"] = self.row(
            page, "Save profile",
            "Separate saves for this game: each profile keeps its own progress and in-game settings, "
            "like another PS4 user. Profile 1 has your saves from earlier versions; switching never "
            "deletes anything.", save_profile)

        def language(parent):
            voices = voice_languages(self.game_dir())
            options = [(code, label + ("  (+ voices)" if code in voices else "")) for code, label in LANGUAGES]
            return Dropdown(parent, options, self.config.get("language", "1"),
                            lambda v: self.change_config("language", v), self.f_small)
        self.widgets["language"] = self.row(
            page, "Language",
            "Menus and subtitles in another language: pick it here, then press Square in a game's main "
            "menu to switch. Languages marked + voices are also spoken in your copy of the game.", language)

        def speakers(parent):
            return Segmented(parent, (("auto", "Auto"), ("2", "Stereo"), ("6", "5.1"), ("8", "7.1")),
                             str(self.config.get("speakers", "auto")),
                             lambda v: self.change_config("speakers", v), self.f_small, recommended="auto")
        self.row(page, "Speakers",
                 "The games have their own stereo, 5.1 and 7.1 sound mixes. Auto picks the one that matches "
                 "your Windows sound output (Sound settings -> speaker setup); headphones get stereo.",
                 speakers)

        def unlock(parent):
            return Toggle(parent, self.s, self.config.get("unlock", False),
                          lambda v: self.change_config("unlock", v))
        self.row(page, "Unlock all chapters",
                 "Every chapter in Chapter Select and the Brutal difficulty from the start, in all three "
                 "games. Your saves and progress stay as they are. Needs update 1.02.", unlock)

        def unlock_rewards(parent):
            return Toggle(parent, self.s, self.config.get("unlock_rewards", False),
                          lambda v: self.change_config("unlock_rewards", v))
        self.row(page, "Unlock all rewards",
                 "Every reward in Bonuses (skins, weapons, render modes, art galleries, gameplay tweaks) "
                 "and every story cutscene, while this is on. Nothing is saved: turned off, the menus show "
                 "your own progress again. Needs update 1.02.", unlock_rewards)

        def diagnostics(parent):
            return Toggle(parent, self.s, self.config.get("diagnostics", True),
                          lambda v: self.change_config("diagnostics", v))
        self.row(page, "Performance log", "Writes frame rate statistics to the log every 5 seconds. Helps when "
                                          "you report a problem.", diagnostics)

        def torch_forward(parent):
            return Toggle(parent, self.s, self.config.get("torch_forward", True),
                          lambda v: self.change_config("torch_forward", v))
        self.row(page, "Torch follows Nate (Uncharted 2)",
                 "The torch lights where Nate points it. Off: as on the PS4, it lights where the camera "
                 "looks. Needs update 1.02.", torch_forward)

        def check_updates(parent):
            return Toggle(parent, self.s, self.config.get("check_updates", True),
                          lambda v: self.change_config("check_updates", v))
        version = updater.installed_version(ROOT)
        self.row(page, "Check for updates at start",
                 f"Installed: {updater.version_text(version) if version else 'source tree (updates with git)'}. "
                 "The launcher asks before it installs anything.", check_updates)

        def section(title, actions, note=""):
            tk.Label(page, text=title, font=self.f_small, bg=BG, fg=GOLD, anchor="w").pack(
                side="top", fill="x", pady=(self.px(10), self.px(4)))
            line = tk.Frame(page, bg=BG)
            line.pack(side="top", fill="x")
            for label, action in actions:
                self.flat_button(line, label, action).pack(side="left", padx=(0, self.px(10)))
            if note:
                tk.Label(line, text=note, font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(
                    side="left", padx=(self.px(6), 0))
        section("SHORTCUTS", (("Desktop shortcut", self.desktop_shortcut), ("Game shortcuts", self.game_shortcuts)),
                "Game shortcuts: one per game on the desktop, each starts its game directly.")
        section("FOLDERS", (("Open log folder", self.open_logs), ("Open save folder", self.open_saves),
                            ("Change game folder", self.choose_game_dir),
                            ("Install game from .pkg", self.install_game_pkg)))
        self.game_dir_label = tk.Label(page, text=f"Game files: {self.game_dir()}", font=self.f_small,
                                       bg=BG, fg=DIM, anchor="w")
        self.game_dir_label.pack(side="top", fill="x", pady=(self.px(6), 0))
        self.update_label = tk.Label(page, text=self.update_text(), font=self.f_small, bg=BG, fg=DIM,
                                     anchor="w", justify="left")
        self.update_label.pack(side="top", fill="x")
        self.ini_label = tk.Label(page, text=f"Settings: {ini_file(self.config['game'])}", font=self.f_small,
                                  bg=BG, fg=DIM, anchor="w")
        self.ini_label.pack(side="top", fill="x")
        section("GAME UPDATE", (("Choose update folder", self.choose_update_folder),
                                ("Install from .pkg", self.install_game_update)),
                "Your extracted update 1.02 folder, wherever it is, or the update's .pkg file.")
        section("TROUBLESHOOTING", (("Check for updates", lambda: self.check_updates(manual=True)),
                                    ("Clear shader cache", self.clear_cache), ("Reset all settings", self.reset)))
        # Credits for the anti-aliasing technologies (NVIDIA's DLSS license asks for its attribution).
        tk.Label(page, text=CREDITS, font=self.f_small, bg=BG, fg=DIM, anchor="w", justify="left",
                 wraplength=self.width - 2 * self.px(40) - self.px(30)).pack(side="top", fill="x",
                                                                              pady=(self.px(10), 0))
        return page

    def flat_button(self, parent, label, action):
        button = tk.Label(parent, text=label, font=self.f_small, bg=PANEL, fg=TEXT, padx=self.px(12), pady=self.px(6),
                          cursor="hand2", highlightthickness=1, highlightbackground=LINE)
        button.bind("<Button-1>", lambda e: action())
        button.bind("<Enter>", lambda e: button.config(bg=PANEL_HI))
        button.bind("<Leave>", lambda e: button.config(bg=PANEL))
        return button

    # -- multiplayer (preview) --------------------------------------------------------------

    def text_entry(self, parent, key, default, width=18):
        """A one-line text box whose text is kept in the launcher's settings under key: a framed
        well (gold frame while typing) with room around the text."""
        value = tk.StringVar(value=str(self.config.get(key, default)))
        well = tk.Frame(parent, bg=BG, highlightthickness=1, highlightbackground=LINE)
        entry = tk.Entry(well, textvariable=value, width=width, font=self.f_body, bg=BG, fg=TEXT,
                         insertbackground=GOLD, relief="flat", highlightthickness=0)
        entry.pack(fill="x", expand=True, padx=self.px(8), pady=self.px(4))
        entry.bind("<FocusIn>", lambda e: well.config(highlightbackground=GOLD))
        entry.bind("<FocusOut>", lambda e: well.config(highlightbackground=LINE))
        well.bind("<Button-1>", lambda e: entry.focus_set())
        value.trace_add("write", lambda *a: self.change_config(key, value.get().strip()))
        return well

    def gold_button(self, parent, label, action, small=False):
        """A main action (Host, Join): gold, like the PLAY button."""
        button = tk.Label(parent, text=label, font=self.f_tab if small else self.f_body_bold, bg=GOLD, fg=INK,
                          padx=self.px(12 if small else 20), pady=self.px(4 if small else 8), cursor="hand2")
        button.bind("<Button-1>", lambda e: action())
        button.bind("<Enter>", lambda e: button.config(bg=GOLD_HI))
        button.bind("<Leave>", lambda e: button.config(bg=GOLD))
        return button

    def caps_label(self, parent, text, bg=PANEL):
        return tk.Label(parent, text=text, font=self.f_caps, bg=bg, fg=DIM, anchor="w")

    def mp_card(self, parent, title, text):
        """A bordered panel with a gold title and a short explanation; returns (card, inside)."""
        card = tk.Frame(parent, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        inside = tk.Frame(card, bg=PANEL)
        inside.pack(fill="both", expand=True, padx=self.px(20), pady=self.px(16))
        tk.Label(inside, text=title, font=self.f_tab, bg=PANEL, fg=GOLD, anchor="w").pack(side="top", fill="x")
        tk.Label(inside, text=text, font=self.f_small, bg=PANEL, fg=MUTED, anchor="w", justify="left",
                 wraplength=self.px(470)).pack(side="top", fill="x", pady=(self.px(3), self.px(14)))
        return card, inside

    def page_multiplayer(self):
        """Uncharted 2's own online games between PCs (the LAN transport, gow3_pc/src/runtime_net.c;
        tools/probe_uncharted.py net_mode). Hidden until it works (multiplayer_preview), and
        shown only while Uncharted 2 is chosen (update_multiplayer_tab)."""
        page = self.page()
        # Header strip: what this is, the preview mark, the player's name.
        banner = tk.Frame(page, bg=PANEL, highlightthickness=1, highlightbackground=LINE)
        banner.pack(side="top", fill="x", pady=(0, self.px(12)))
        tk.Frame(banner, bg=GOLD, width=self.px(4)).pack(side="left", fill="y")
        about = tk.Frame(banner, bg=PANEL)
        about.pack(side="left", fill="both", expand=True, padx=self.px(18), pady=self.px(14))
        head = tk.Frame(about, bg=PANEL)
        head.pack(side="top", fill="x")
        tk.Label(head, text="Uncharted 2 Multiplayer", font=self.f_card_title, bg=PANEL, fg=TEXT).pack(side="left")
        tk.Label(head, text="PREVIEW", font=self.f_caps, bg=GOLD, fg=INK, padx=self.px(7),
                 pady=self.px(1)).pack(side="left", padx=(self.px(12), 0))
        tk.Label(about, text="Naughty Dog's own co-op and competitive modes, played between PCs on your "
                             "network. Not finished yet: a game may not start, or may crash.",
                 font=self.f_small, bg=PANEL, fg=MUTED, anchor="w", justify="left",
                 wraplength=self.px(620)).pack(side="top", fill="x", pady=(self.px(5), 0))
        characters = net_characters(self.game_dir())
        character = tk.Frame(banner, bg=PANEL)
        character.pack(side="right", padx=(0, self.px(20)), pady=self.px(14))
        self.caps_label(character, "CHARACTER").pack(side="top", fill="x")
        value = str(self.config.get("net_skin", ""))
        if value not in {a for a, _ in characters}:
            value = ""
        self.widgets["net_skin"] = Dropdown(character, characters, value,
                                            lambda v: self.change_config("net_skin", v), self.f_body)
        self.widgets["net_skin"].pack(side="top", fill="x", pady=(self.px(4), 0))
        name = tk.Frame(banner, bg=PANEL)
        name.pack(side="right", padx=self.px(20), pady=self.px(14))
        self.caps_label(name, "YOUR NAME").pack(side="top", fill="x")
        self.text_entry(name, "net_name", "Drake", width=22).pack(side="top", pady=(self.px(4), 0))

        columns = tk.Frame(page, bg=BG)
        columns.pack(side="top", fill="both", expand=True)
        columns.columnconfigure(0, weight=1, uniform="multiplayer")
        columns.columnconfigure(1, weight=1, uniform="multiplayer")
        host, inside = self.mp_card(columns, "HOST A GAME",
                                    "Choose a map and mode, then host. Players on your network find your "
                                    "game under Join a game; they load the same map.")
        host.grid(row=0, column=0, sticky="nsew", padx=(0, self.px(8)))
        join, joining = self.mp_card(columns, "JOIN A GAME",
                                     "A game hosted on your network, or a host's address.")
        join.grid(row=0, column=1, sticky="nsew", padx=(self.px(8), 0))

        # Host card.
        games = net_games()
        value = str(self.config.get("net_game", games[0]["index"]))
        if value not in {str(g["index"]) for g in games}:
            value = str(games[0]["index"])
        self.caps_label(inside, "MAP AND MODE").pack(side="top", fill="x")
        self.widgets["net_game"] = Dropdown(inside, [(str(g["index"]), g["label"]) for g in games], value,
                                            lambda v: self.change_config("net_game", v), self.f_body)
        self.widgets["net_game"].pack(side="top", fill="x", pady=(self.px(4), 0))
        self.gold_button(inside, "HOST GAME", self.host_net_game).pack(side="top", anchor="w",
                                                                        pady=(self.px(18), self.px(10)))
        tk.Label(inside, text="Uses port 27000. The first time, Windows asks whether Uncharted may use the "
                              "network: allow it for private networks.",
                 font=self.f_small, bg=PANEL, fg=DIM, anchor="w", justify="left",
                 wraplength=self.px(470)).pack(side="top", fill="x")
        # Local co-op: the same map and mode in two windows on this PC, one controller each.
        tk.Frame(inside, bg=LINE, height=1).pack(side="top", fill="x", pady=(self.px(14), self.px(12)))
        self.caps_label(inside, "ON THIS PC: PLAYER 2'S CHARACTER").pack(side="top", fill="x")
        value = str(self.config.get("net_skin2", ""))
        if value not in {a for a, _ in characters}:
            value = ""
        self.widgets["net_skin2"] = Dropdown(inside, characters, value,
                                             lambda v: self.change_config("net_skin2", v), self.f_body)
        self.widgets["net_skin2"].pack(side="top", fill="x", pady=(self.px(4), 0))
        self.gold_button(inside, "LOCAL CO-OP (2 CONTROLLERS)", self.local_net_game).pack(
            side="top", anchor="w", pady=(self.px(12), self.px(8)))
        tk.Label(inside, text="Two game windows side by side, the first controller plays the left one, the "
                              "second the right one. Needs a strong PC: two games run at once.",
                 font=self.f_small, bg=PANEL, fg=DIM, anchor="w", justify="left",
                 wraplength=self.px(470)).pack(side="top", fill="x")

        # Join card: the games found, then join by address.
        found = tk.Frame(joining, bg=PANEL)
        found.pack(side="top", fill="x")
        self.caps_label(found, "ON YOUR NETWORK").pack(side="left")
        self.flat_button(found, "\u21bb  Refresh", self.search_net_games).pack(side="right")
        self.net_list = tk.Frame(joining, bg=PANEL)
        self.net_list.pack(side="top", fill="x", pady=(self.px(6), self.px(14)))
        self.show_net_games(None)
        tk.Frame(joining, bg=LINE, height=1).pack(side="top", fill="x", pady=(0, self.px(12)))
        self.caps_label(joining, "BY ADDRESS").pack(side="top", fill="x")
        by_address = tk.Frame(joining, bg=PANEL)
        by_address.pack(side="top", fill="x", pady=(self.px(4), self.px(8)))
        self.gold_button(by_address, "JOIN", lambda: self.join_net_game(self.config.get("net_address", "")),
                         small=True).pack(side="right", padx=(self.px(8), 0))
        self.text_entry(by_address, "net_address", "").pack(side="left", fill="x", expand=True)
        tk.Label(joining, text="The host's IP address, with :port when it is not 27000. Over the internet, "
                               "the host opens the port on their router, or you both use a virtual "
                               "network such as Tailscale.",
                 font=self.f_small, bg=PANEL, fg=DIM, anchor="w", justify="left",
                 wraplength=self.px(470)).pack(side="top", fill="x")
        return page

    def search_net_games(self):
        if getattr(self, "net_searching", False) or not hasattr(self, "net_list"):
            return
        self.net_searching = True
        self.show_net_games("searching")
        found = queue.Queue()
        threading.Thread(target=lambda: found.put(self._find_net_games()), daemon=True).start()

        def done():
            try:
                games = found.get_nowait()
            except queue.Empty:
                self.root.after(100, done)
                return
            self.net_searching = False
            self.show_net_games(games)
        self.root.after(100, done)

    @staticmethod
    def _find_net_games():
        if net_lan is None:
            return OSError("launcher\\net_lan.py is missing")
        try:
            return net_lan.find_games()
        except OSError as error:
            return error

    def show_net_games(self, games):
        """The Join card's list: None (not searched), "searching", an error, [] or the games."""
        for child in self.net_list.winfo_children():
            child.destroy()
        if not isinstance(games, list) or not games:
            if isinstance(games, OSError):
                text = f"The search failed: {games}"
            elif games == "searching":
                text = "Looking for games on your network..."
            elif games is None:
                text = "Press Refresh to look for games."
            else:
                text = "No games on your network right now. The host presses HOST GAME first; or join by address."
            box = tk.Frame(self.net_list, bg=BG, highlightthickness=1, highlightbackground=LINE)
            box.pack(side="top", fill="x")
            tk.Label(box, text=text, font=self.f_small, bg=BG, fg=MUTED, anchor="w", justify="left",
                     wraplength=self.px(440)).pack(side="top", fill="x", padx=self.px(12), pady=self.px(12))
            return
        for game in games:
            line = tk.Frame(self.net_list, bg=BG, highlightthickness=1, highlightbackground=LINE)
            line.pack(side="top", fill="x", pady=(0, self.px(6)))
            address = f"{game['address']}:{game['port']}"
            self.gold_button(line, "JOIN", lambda a=address: self.join_net_game(a), small=True) \
                .pack(side="right", padx=self.px(10), pady=self.px(8))
            text = tk.Frame(line, bg=BG)
            text.pack(side="left", fill="x", expand=True, padx=self.px(12), pady=self.px(7))
            tk.Label(text, text=game["title"] or f"Net game {game['game']}", font=self.f_body_bold, bg=BG,
                     fg=TEXT, anchor="w").pack(side="top", fill="x")
            tk.Label(text, text=f"Host {game['host'] or '?'}   \u00b7   {game['players']}/{game['most']} players"
                                f"   \u00b7   {game['address']}",
                     font=self.f_small, bg=BG, fg=MUTED, anchor="w").pack(side="top", fill="x")

    def host_net_game(self):
        games = net_games()
        index = str(self.config.get("net_game", games[0]["index"]))
        label = next((g["label"] for g in games if str(g["index"]) == index), games[0]["label"])
        self.play(net={"mode": "host", "address": "0.0.0.0:27000", "game": index, "title": label})

    def local_net_game(self):
        """Local co-op: this PC hosts on 127.0.0.1 in the left half of the screen with the first
        controller; a second game (instance 2: its own log and user folders) joins it in the right
        half with the second controller (GOW3_PAD_INDEX, GOW3_WINDOW_HALF)."""
        games = net_games()
        index = str(self.config.get("net_game", games[0]["index"]))
        label = next((g["label"] for g in games if str(g["index"]) == index), games[0]["label"])
        self.play(net={"mode": "host", "address": "127.0.0.1:27000", "game": index, "title": label, "local": True})

    def join_net_game(self, address):
        address = (address or "").strip()
        if not address:
            messagebox.showinfo(APP_TITLE, "Type the host's address (e.g. 192.168.1.20), or pick a game "
                                           "found on your network.")
            return
        self.play(net={"mode": "join", "address": address})

    # -- footer -----------------------------------------------------------------------------

    def build_footer(self):
        footer = tk.Frame(self.root, bg=PANEL)
        footer.pack(side="bottom", fill="x")
        tk.Frame(self.root, bg=LINE, height=1).pack(side="bottom", fill="x")
        inner = tk.Frame(footer, bg=PANEL)
        inner.pack(fill="x", padx=self.px(40), pady=self.px(14))
        left = tk.Frame(inner, bg=PANEL)
        left.pack(side="left", fill="x", expand=True)
        self.status = tk.Label(left, text="", font=self.f_body_bold, bg=PANEL, fg=TEXT, anchor="w")
        self.status.pack(side="top", fill="x")
        self.substatus = tk.Label(left, text="Insert or R3 + L2 in game opens the menu.", font=self.f_small,
                                  bg=PANEL, fg=MUTED, anchor="w")
        self.substatus.pack(side="top", fill="x")
        w, h = self.px(210), self.px(54)
        self.play_button = tk.Canvas(inner, width=w, height=h, bg=PANEL, highlightthickness=0, cursor="hand2")
        self.play_button.pack(side="right")
        self.play_button.bind("<Button-1>", lambda e: self.play_or_stop())
        self.play_button.bind("<Enter>", lambda e: self.draw_play(hover=True))
        self.play_button.bind("<Leave>", lambda e: self.draw_play())
        self.draw_play()
        self.update_button = tk.Label(inner, text="UPDATE", font=self.f_body_bold, bg=PANEL_HI, fg=GOLD,
                                      padx=self.px(16), pady=self.px(10), cursor="hand2",
                                      highlightthickness=1, highlightbackground=GOLD)
        self.update_button.bind("<Button-1>", lambda e: self.install_update())
        self.set_status(f"Ready to play {self.current_game()['name']}." if self.config.get("game") else "Ready.")

    def draw_play(self, hover=False):
        canvas = self.play_button
        w, h = int(canvas["width"]), int(canvas["height"])
        canvas.delete("all")
        running = self.process is not None
        fill = (STOP_RED if running else (GOLD_HI if hover else GOLD))
        round_rect(canvas, 2, 2, w - 2, h - 2, self.px(8), fill=fill, outline=fill)
        canvas.create_text(w // 2, h // 2, text="STOP" if running else "PLAY", font=self.f_play,
                           fill=TEXT if running else INK)

    def set_status(self, text, detail=None):
        self.status.config(text=text)
        if detail is not None:
            self.substatus.config(text=detail)

    # -- updates (launcher/updater.py) ------------------------------------------------------

    def check_updates(self, manual=False):
        current = updater.installed_version(ROOT)
        if current is None:
            if manual:
                messagebox.showinfo(APP_TITLE, "This copy runs from the source tree: update it with git.")
            return
        if self.update_busy:
            return
        self.update_busy = True
        if manual:
            self.set_status("Checking for updates...", "")

        def work():
            try:
                self.update_result = (updater.latest_release(current), None)
            except Exception as error:  # offline, GitHub's rate limit
                self.update_result = (None, str(error) or error.__class__.__name__)
        threading.Thread(target=work, daemon=True).start()
        self.root.after(200, lambda: self.update_checked(manual))

    def update_checked(self, manual):
        if self.update_result is None:
            self.root.after(200, lambda: self.update_checked(manual))
            return
        release, error = self.update_result
        self.update_result = None
        self.update_busy = False
        if release:
            self.update_release = release
            version = updater.version_text(release["version"])
            self.update_button.config(text=f"UPDATE TO {version}")
            self.update_button.pack(side="right", padx=(0, self.px(14)))
            if not self.process:
                self.set_status(f"Version {version} is available.",
                                "UPDATE installs it: saves and settings are kept, the launcher restarts.")
        elif manual:
            current = updater.version_text(updater.installed_version(ROOT))
            if error:
                self.set_status("Could not check for updates.", error)
            else:
                self.set_status("No update found.", f"Version {current} is the newest.")

    def install_update(self):
        release = self.update_release
        if not release or self.update_busy:
            return
        if self.process:
            messagebox.showinfo(APP_TITLE, "Close the game first: the update replaces the game's runtime.")
            return
        version = updater.version_text(release["version"])
        if not messagebox.askyesno(APP_TITLE, f"Install version {version} ({release['size'] / 1e6:.0f} MB "
                                   "download)?\n\nSaves, settings and the shader cache are kept. The "
                                   "launcher closes, installs the update and starts again.\n\n"
                                   f"What's new: {release['page']}"):
            return
        self.update_busy = True
        self.update_progress = (0, release["size"])
        self.update_error = None
        self.update_staged = None

        def progress(done, total):
            self.update_progress = (done, total)

        def work():
            try:
                archive = updater.download(release, ROOT, progress)
                self.update_staged = updater.stage(archive, ROOT, release["version"])
            except Exception as error:
                self.update_error = str(error) or error.__class__.__name__
        threading.Thread(target=work, daemon=True).start()
        self.root.after(200, self.update_tick)

    def update_tick(self):
        if self.update_error:
            self.update_busy = False
            self.set_status("The update failed.", "The installed version is unchanged.")
            messagebox.showerror(APP_TITLE, f"The update could not be installed:\n\n{self.update_error}")
            return
        if self.update_staged:
            self.set_status("Installing the update...", "The launcher starts again in a moment.")
            try:
                updater.start_apply(ROOT, self.update_staged)
            except OSError as error:
                self.update_busy = False
                messagebox.showerror(APP_TITLE, f"The update could not be started:\n\n{error}")
                return
            self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
            write_config(self.config)
            self.root.destroy()
            return
        done, total = self.update_progress
        amount = f"{100 * done / total:.0f}%" if total else f"{done / 1e6:.0f} MB"
        self.set_status(f"Downloading the update... {amount}", "Keep the launcher open.")
        self.root.after(200, self.update_tick)

    # -- settings changes -------------------------------------------------------------------

    def change(self, key, value):
        self.ini[key] = value
        self.save_ini({key: value})
        if self.game_settings().get("preset") != "custom":
            self.game_settings()["preset"] = "custom"
            write_config(self.config)
            self.redraw_presets()
        if key == "fps_limit":
            self.update_fps_note()
        if key in ("upscaler", "fsr1"):
            self.refresh_render_size()

    def change_upscaling(self, value):
        """An upscaler: a temporal one takes the runtime's anti-aliasing setting (TAA is kept for
        Off and FSR 1); FSR 1 has no Native AA (it would do nothing)."""
        settings = self.game_settings()
        aa = anti_aliasing_choice(self.ini, settings)
        settings["aa"] = aa
        if value == "fsr1" and settings.get("upscale", "off") == "off":
            settings["upscale"] = "quality"
        write_config(self.config)
        self.ini["fsr1"] = "1" if value == "fsr1" else "0"
        self.save_ini({"fsr1": self.ini["fsr1"]})
        self.change("upscaler", value if value in TEMPORAL_UPSCALERS else aa)

    def change_anti_aliasing(self, value):
        self.change_game_setting("aa", value)
        self.change("upscaler", value)

    def change_resolution(self, value):
        self.game_settings()["resolution"] = value
        self.game_settings()["preset"] = "custom"
        write_config(self.config)
        self.redraw_presets()
        self.refresh_render_size()

    def change_aspect(self, value):
        self.change_game_setting("aspect", value)
        self.refresh_render_size()

    def refresh_render_size(self):
        """The resolution labels and the exact render size for the selected game's aspect ratio."""
        settings = self.game_settings()
        aspect = settings.get("aspect", "16:9")
        if isinstance(self.widgets.get("resolution"), Segmented):
            self.widgets["resolution"].relabel(resolution_options(aspect))
        if hasattr(self, "render_size_note"):
            # With upscaling on, the line under its presets shows the sizes instead.
            render, output = game_sizes(settings, upscaling_choice(self.ini))
            self.render_size_note.config(
                text="" if output else f"Render size: {render.replace('x', ' × ')}")
            if output and self.render_size_note.winfo_manager():
                self.render_size_note.pack_forget()
            elif not output and not self.render_size_note.winfo_manager() and "aspect" in self.widgets:
                self.render_size_note.pack(side="top", fill="x", pady=(0, self.px(10)),
                                           after=self.widgets["resolution"].master)
        self.refresh_upscale()

    def refresh_upscale(self):
        """The upscaler, its quality row (with an upscaler only, right under it) with the size the
        selected game draws at, and the anti-aliasing (greyed with a temporal upscaler)."""
        if not hasattr(self, "upscale_row"):
            return
        settings = self.game_settings()
        upscaling = upscaling_choice(self.ini)
        self.widgets["upscaling"].set(upscaling)
        if upscaling != "off":
            if not self.upscale_row.winfo_manager():
                self.upscale_row.pack(side="top", fill="x", pady=(0, self.px(10)),
                                      after=self.widgets["upscaling"].master)
        elif self.upscale_row.winfo_manager():
            self.upscale_row.pack_forget()
        quality = self.widgets["upscale"]
        presets = [option for option, _ in UPSCALE_PRESETS]
        quality.show(presets[1:] if upscaling == "fsr1" else presets)
        quality.relabel([("off", "DLAA" if upscaling == "dlss" else "Native AA")])
        quality.set(settings.get("upscale", "off"))
        aa = self.widgets["aa"]
        aa.set(anti_aliasing_choice(self.ini, settings))
        aa.set_enabled(upscaling not in TEMPORAL_UPSCALERS)
        render, output = game_sizes(settings, upscaling)
        self.upscale_note.config(
            text=f"Draws {render.replace('x', ' × ')}, upscaled to {output.replace('x', ' × ')}"
            if output else "")

    def change_game_setting(self, key, value):
        self.game_settings()[key] = value
        if key in ("mouse", "mouse_sens", "mouse_invert", "prompts", "mouse_aim_sens", "mouse_vertical",
                   "mouse_invert_x", "vibration"):
            self.save_controls()
        else:
            write_config(self.config)

    def change_config(self, key, value):
        self.config[key] = value
        write_config(self.config)

    # -- game -------------------------------------------------------------------------------

    def play_or_stop(self):
        if self.process:
            self.stop()
        else:
            self.play()

    def play_game(self, key):
        """A double-click on a game's card: choose it and play."""
        self.select_game(key)
        if not self.process:
            self.play()

    def play(self, net=None):
        """net: a Multiplayer page start (host or join an Uncharted 2 net game), else the chosen game."""
        if net and self.process:
            messagebox.showinfo(APP_TITLE, "A game is already running.")
            return
        if not RUN_SCRIPT.exists():
            messagebox.showerror(APP_TITLE, f"Start script missing:\n{RUN_SCRIPT}")
            return
        if not is_game_dir(self.game_dir()):
            messagebox.showinfo(APP_TITLE, "Select your Uncharted: The Nathan Drake Collection game folder "
                                           "(CUSA02344, the folder with eboot.bin and sce_sys).")
            if not self.choose_game_dir():
                return
        if not self.confirm_without_update() or not self.confirm_update_files() or \
                not self.confirm_slow_drive():
            return
        self.follow_ini()  # the in-game menu's last changes, not older values kept here
        self.save_ini({k: self.ini.get(k, v) for k, v in INI_DEFAULTS.items()})
        write_config(self.config)
        game = next(g for g in GAMES if g["key"] == "u2") if net else self.current_game()
        env = os.environ.copy()
        # The Multiplayer page: Uncharted 2's own online game (tools/probe_uncharted.py net_mode).
        for key in ("UNCHARTED_NET_MODE", "UNCHARTED_NET_ADDRESS", "UNCHARTED_NET_GAME", "UNCHARTED_NET_TITLE",
                    "UNCHARTED_NET_NAME", "UNCHARTED_NET_SKIN", "UNCHARTED_INSTANCE", "GOW3_PAD_INDEX",
                    "GOW3_WINDOW_HALF"):
            env.pop(key, None)
        if net:
            env["UNCHARTED_NET_MODE"] = net["mode"]
            env["UNCHARTED_NET_ADDRESS"] = net.get("address", "")
            env["UNCHARTED_NET_NAME"] = str(self.config.get("net_name", ""))[:31]
            env["UNCHARTED_NET_SKIN"] = str(self.config.get("net_skin", ""))
            if net.get("local"):  # local co-op: the left window, the first controller, no mouse
                env.update({"GOW3_PAD_INDEX": "1", "GOW3_WINDOW_HALF": "left", "GOW3_MOUSE": "0"})
            if net["mode"] == "host":
                env["UNCHARTED_NET_GAME"] = net["game"]
                env["UNCHARTED_NET_TITLE"] = net["title"]
        env["UNCHARTED_DIAGNOSTICS"] = "1" if self.config.get("diagnostics", True) else "0"
        env["UNCHARTED_TORCH_FORWARD"] = "1" if self.config.get("torch_forward", True) else "0"
        env["UNCHARTED_JOB_SLEEP"] = "0" if self.config.get("job_sleep", "auto") == "off" else "1"
        env["UNCHARTED_LANGUAGE"] = str(self.config.get("language", "1"))
        env["UNCHARTED_SPEAKERS"] = str(self.config.get("speakers", "auto"))
        env["UNCHARTED_UNLOCK"] = "1" if self.config.get("unlock", False) else "0"
        env["UNCHARTED_UNLOCK_REWARDS"] = "1" if self.config.get("unlock_rewards", False) else "0"
        env["UNCHARTED_HUD"] = str(self.config.get("hud", "16:9"))
        env["UNCHARTED_HUD_WIDE_ASPECT"] = str(self.config.get("hud_wide_aspect", "screen"))
        if self.chosen_update():
            env["UNCHARTED_UPDATE"] = str(self.chosen_update())  # tools/probe_uncharted.py find_update
        env["UNCHARTED_CHEATS"] = ",".join(name for name, *_ in CHEATS if self.config.get("cheat_" + name, False))
        # Every game's own options: the Collection menu starts the others itself, and
        # tools/probe_uncharted.py applies the started game's (its settings file decides the
        # vblank rate for frame rates above 60).
        env.pop("GOW3_VBLANK_HZ", None)
        for key in GAME_KEYS:
            settings = self.config["games"][key]
            # With upscaling the game draws below the resolution chosen, upscaled to it
            # (tools/probe_uncharted.py).
            render, output = game_sizes(settings, upscaling_choice(read_ini(key)))
            env[f"UNCHARTED_RESOLUTION_{key.upper()}"] = render
            if output:
                env[f"UNCHARTED_OUTPUT_RES_{key.upper()}"] = output
            else:
                env.pop(f"UNCHARTED_OUTPUT_RES_{key.upper()}", None)
            env[f"UNCHARTED_MULTITHREADED_{key.upper()}"] = "1" if settings["multithreaded"] else "0"
            env[f"UNCHARTED_SAVE_PROFILE_{key.upper()}"] = str(settings.get("save_profile", "1"))
            # Controls: in each game's settings file (save_controls), where its menu changes them.
        python = Path(sys.executable)
        if python.name.lower() == "pythonw.exe" and python.with_name("python.exe").exists():
            python = python.with_name("python.exe")
        command = [str(python), "-u", str(RUN_SCRIPT), "--boot", "--seconds", "0", "--executable", game["exe"],
                   "--game", str(self.game_dir())]
        # The game window may take the foreground (Windows only lets the foreground process do
        # that; the game starts two processes down): fullscreen and keyboard input need the focus.
        try:
            ctypes.windll.user32.AllowSetForegroundWindow(-1)  # ASFW_ANY
        except (AttributeError, OSError):
            pass
        try:
            self.process = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                            creationflags=NO_WINDOW, text=True, encoding="utf-8", errors="replace")
        except OSError as error:
            messagebox.showerror(APP_TITLE, f"Could not start the game:\n{error}")
            self.process = None
            return
        self.started_at = time.time()
        self.last_output = time.time()
        self.game_started = False
        # The start script's output (preparation, patches, the runtime's log path): kept for
        # "the game does not start" reports.
        try:
            self.run_log = open(USER_DIR / "launcher-last-run.log", "w", encoding="utf-8")
            self.run_log.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {' '.join(command)}\n")
        except OSError:
            self.run_log = None
        threading.Thread(target=self.read_output, args=(self.process,), daemon=True).start()
        self.partner = None
        if net and net.get("local"):
            # The joiner: instance 2 (analysis/<game>-i2, analysis/uncharted-user-i2), the right
            # window, the second controller. It connects again every second until the host listens.
            partner_env = dict(env)
            name = str(self.config.get("net_name", "") or "Drake")
            partner_env.update({"UNCHARTED_INSTANCE": "2", "UNCHARTED_NET_MODE": "join",
                                "UNCHARTED_NET_ADDRESS": "127.0.0.1:27000", "UNCHARTED_NET_NAME": (name + " 2")[:31],
                                "UNCHARTED_NET_SKIN": str(self.config.get("net_skin2", "")),
                                "GOW3_PAD_INDEX": "2", "GOW3_WINDOW_HALF": "right", "GOW3_MOUSE": "0"})
            for key in ("UNCHARTED_NET_GAME", "UNCHARTED_NET_TITLE"):
                partner_env.pop(key, None)
            # 15 s later: the first game has prepared its files (both read the same game folder).
            self.partner_start = (self.process, command, partner_env)
            self.root.after(15000, self.start_partner)
        self.set_status(f"Starting {game['name']}...", "Preparing game files and code patches.")
        self.draw_play()

    def start_partner(self):
        pending = getattr(self, "partner_start", None)
        self.partner_start = None
        if not pending or not self.process or pending[0] is not self.process or self.process.poll() is not None:
            return  # stopped meanwhile
        _, command, env = pending
        try:
            self.partner = subprocess.Popen(command, cwd=str(ROOT), env=env, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                            creationflags=NO_WINDOW, text=True, encoding="utf-8", errors="replace")
            threading.Thread(target=self.read_partner_output, args=(self.partner,), daemon=True).start()
        except OSError as error:
            self.partner = None
            messagebox.showerror(APP_TITLE, f"Could not start the second game:\n{error}")

    def read_partner_output(self, process):
        """Local co-op: the second game's start script output, kept in launcher-last-run-2.log."""
        try:
            log = open(USER_DIR / "launcher-last-run-2.log", "w", encoding="utf-8")
        except OSError:
            log = None
        for line in process.stdout:
            if log:
                try:
                    log.write(line)
                    log.flush()
                except (OSError, ValueError):
                    log = None
        if log:
            try:
                log.write(f"exit code {process.wait()}\n")
                log.close()
            except (OSError, ValueError):
                pass

    def stop_partner(self):
        partner = getattr(self, "partner", None)
        self.partner = None
        self.partner_start = None
        if partner and partner.poll() is None:
            subprocess.run(["taskkill", "/PID", str(partner.pid), "/T", "/F"], creationflags=NO_WINDOW,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def read_output(self, process):
        log = getattr(self, "run_log", None)
        for line in process.stdout:
            self.output.put(line.rstrip())
            if log:
                try:
                    log.write(line)
                    log.flush()
                except (OSError, ValueError):
                    log = None
        if log:
            try:
                log.write(f"exit code {process.wait()}\n")
                log.close()
            except (OSError, ValueError):
                pass

    def poll(self):
        while True:
            try:
                line = self.output.get_nowait()
            except queue.Empty:
                break
            self.last_output = time.time()
            if line.startswith("Collection: switching to"):
                target = line.split("switching to", 1)[1].strip().split()[0]
                name = next((g["name"] for g in GAMES if g["exe"] == target), target)
                self.set_status(f"Switching to {name}...", "The Collection is starting another game.")
                self.game_started = False
        self.poll_count = getattr(self, "poll_count", 0) + 1
        if self.poll_count % 7 == 0:  # about once a second
            self.follow_ini()
        if self.process:
            if self.process.poll() is not None:
                self.finished(self.process.returncode)
            elif not self.game_started and time.time() - self.last_output > 2.5:
                self.game_started = True
                self.set_status(f"Playing {self.current_game()['name']}",
                                "Insert or R3 + L2 opens the in-game menu. STOP closes the game.")
        self.root.after(150, self.poll)

    def latest_log(self):
        logs = [ROOT / "analysis" / g["log"] / "boot.log" for g in GAMES]
        logs = [log for log in logs if log.exists()]
        return max(logs, key=lambda p: p.stat().st_mtime) if logs else None

    def crash_reason(self):
        log = self.latest_log()
        if not log or log.stat().st_mtime < self.started_at:
            return None
        try:
            tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-80:]
        except OSError:
            return None
        for line in reversed(tail):
            if line.startswith("Hang watchdog:") and " report " in line:
                return ("The game froze. A report of where it stopped was saved:\n"
                        + line.split(" report ", 1)[-1] + "\n\nPlease attach it with the log folder's "
                        "boot.log to a GitHub issue.")
        # A crash (exit code 3): the runtime's fault report names where it happened. Players
        # reported just "error code 3" (GitHub #24, #27); the report is what tells the cause.
        for i, line in enumerate(tail):
            if line.startswith("Fault:"):
                where = next((l.strip() for l in tail[i + 1:i + 6] if l.lstrip().startswith(("host #0", "#0"))), "")
                return (line.strip() + (f"\n{where}" if where else "") + "\n\nPlease attach boot.log from the "
                        "log folder (and your graphics card, game version and settings) to a GitHub issue.")
        for i, line in enumerate(tail):
            if line.startswith("STOP:") or "Unreachable code" in line or "Assertion" in line:
                context = [l for l in tail[max(0, i - 2):i + 1] if l.strip()]
                return "\n".join(context)[-600:]
        return None

    def confirm_update_files(self):
        """An update folder with missing or damaged files (#43: a bin.psarc the games couldn't read
        stopped all three at start with "ASSERTION: root") is named before the game starts."""
        update = self.update_dir()
        if not update:
            return True
        try:
            problems = self.update_installer().check_update(update)
        except Exception:  # the check itself must never keep a game from starting
            return True
        if not problems:
            return True
        return messagebox.askokcancel(
            APP_TITLE, "Your game update folder has missing or damaged files:\n\n" + "\n".join(problems[:6]) +
            f"\n\n{update}\n\nThe games will probably stop at start. Extract the update again (or use Install "
            "from .pkg under Advanced -> GAME UPDATE). Play anyway?")

    def confirm_without_update(self):
        """Without update 1.02 most of the port's game fixes (crash fixes among them) are skipped:
        asks once per game folder (a 1.02 copy with the update merged in reports 1.02 itself)."""
        folder = self.game_dir()
        version = app_version(folder)
        if self.update_dir() or not version or version >= "01.02" or \
                self.config.get("warned_no_update") == str(folder):
            return True
        title = folder.parent if folder.name.lower() == "uroot" else folder
        if not messagebox.askokcancel(
                APP_TITLE, f"Your game is version {version.lstrip('0')}, without update 1.02.\n\n"
                "Most of this port's fixes, crash fixes among them, are written for update 1.02, so "
                "without it the games crash more often.\n\nDump update 1.02 from your PS4 and extract it "
                f"next to the game folder as {title.name}-patch (the Advanced tab then shows it).\n\n"
                "Play anyway?"):
            return False
        self.config["warned_no_update"] = str(folder)
        write_config(self.config)
        return True

    def confirm_slow_drive(self):
        """The port or the game on a USB, SD or FAT/exFAT drive (slow_drive_reason): asks once per
        pair of folders."""
        places = []
        for label, folder in (("This port", ROOT), ("The game", self.game_dir())):
            try:
                reason = slow_drive_reason(folder)
            except (OSError, ValueError, ctypes.ArgumentError):
                reason = None
            if reason:
                places.append(f"{label} ({folder}): {reason}.")
        key = f"{ROOT}|{self.game_dir()}"
        if not places or self.config.get("warned_slow_drive") == key:
            return True
        if not messagebox.askokcancel(
                APP_TITLE, "\n".join(places) + "\n\nOn such drives the games freeze for up to a second, "
                "again and again, while the port writes its shader cache, logs and saves (Windows writes "
                "to them without a cache).\n\nFor smooth play, copy this port folder and the game folder "
                "to an internal drive (an SSD such as C:) and start the launcher from there.\n\n"
                "Play anyway?"):
            return False
        self.config["warned_slow_drive"] = key
        write_config(self.config)
        return True

    def play_direct(self):
        self.play()
        if not self.process:  # no game folder chosen, or the start failed (already reported)
            self.root.destroy()

    def finished(self, code):
        self.process = None
        self.stop_partner()  # local co-op: the second game ends with the first
        self.draw_play()
        reason = self.crash_reason()
        # A freeze is reported however the game was closed (STOP, or Task Manager).
        froze = bool(reason) and reason.startswith("The game froze")
        if (code in (0, None) or getattr(self, "stopping", False)) and not froze:
            self.stopping = False
            self.set_status("Game closed.", "Ready to play again.")
        else:
            self.stopping = False
            self.set_status("The game froze." if froze else "The game stopped unexpectedly.",
                            "A report was saved; see the log folder." if froze
                            else f"Exit code {code}. See the log for details.")
            if reason:
                # 24 Oct 2026 (#148, #163, #172): crashes of a 1.00 game were reported as port bugs;
                # most of the port's fixes are written for update 1.02.
                version = app_version(self.game_dir())
                if not self.update_dir() and version and version < "01.02":
                    reason += (f"\n\nYour game is version {version.lstrip('0')} without update 1.02, so most of "
                               "the port's crash fixes are skipped. Install update 1.02 first (Advanced -> "
                               "GAME UPDATE).")
                if messagebox.askyesno(APP_TITLE, f"The game stopped with an error:\n\n{reason}\n\nOpen the log folder?"):
                    self.open_logs()
        if self.direct:
            self.root.after(200, self.root.destroy)

    def stop(self):
        if not self.process:
            return
        self.stopping = True
        subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"], creationflags=NO_WINDOW,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.stop_partner()
        self.set_status("Stopping the game...")

    # -- advanced actions -------------------------------------------------------------------

    def open_logs(self):
        log = self.latest_log()
        folder = log.parent if log else ROOT / "analysis"
        os.startfile(folder)

    def open_saves(self):
        folder = USER_DIR / "savedata"
        os.startfile(folder if folder.exists() else USER_DIR)

    def clear_cache(self):
        cache = USER_DIR / "cache"
        if not cache.exists():
            messagebox.showinfo(APP_TITLE, "There is no shader cache yet.")
            return
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        if not messagebox.askyesno(APP_TITLE, "Delete the shader cache?\n\nShaders are compiled again the next "
                                              "time you play, which brings back stutters for a while."):
            return
        shutil.rmtree(cache, ignore_errors=True)
        messagebox.showinfo(APP_TITLE, "Shader cache cleared.")

    def make_shortcut(self, name, description, play=None):
        """A desktop shortcut to the launcher, or with `play` (u1/u2/u3) to that game directly."""
        link = desktop_folder() / f"{name}.lnk"
        pythonw = Path(sys.executable).with_name("pythonw.exe")
        target = pythonw if pythonw.exists() else Path(sys.executable)
        arguments = f"\"{Path(__file__).resolve()}\""
        if STARTER_EXE.exists():
            target, arguments = STARTER_EXE, ""
        if play:
            arguments = f"{arguments} --play {play}".strip()
        def ps(value):  # a PowerShell single-quoted string ("Drake's" doubles the quote)
            return "'" + str(value).replace("'", "''") + "'"
        # Messages in UTF-8 (a German or Portuguese Windows answers in its own language).
        script = ("[Console]::OutputEncoding=[System.Text.Encoding]::UTF8;"
                  f"$s=(New-Object -ComObject WScript.Shell).CreateShortcut({ps(link)});"
                  f"$s.TargetPath={ps(target)};$s.Arguments={ps(arguments)};"
                  f"$s.WorkingDirectory={ps(ROOT)};$s.IconLocation={ps(ASSETS / 'uncharted.ico')};"
                  f"$s.Description={ps(description)};$s.Save()")
        result = subprocess.run(["powershell", "-NoProfile", "-Command", script], creationflags=NO_WINDOW,
                                capture_output=True, text=True, encoding="utf-8", errors="replace")
        return link, result

    def update_installer(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location("install_update", ROOT / "tools" / "install_update.py")
        installer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(installer)
        return installer

    def choose_update_folder(self):
        """An update the player already extracted, anywhere: checked and remembered (nothing is
        copied); the games get it as UNCHARTED_UPDATE."""
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        game = self.game_dir()
        if not is_game_dir(game):
            messagebox.showwarning(APP_TITLE, "Choose the game folder first (Change game folder).")
            return
        from tkinter import filedialog
        folder = filedialog.askdirectory(title="Choose the extracted update 1.02 folder", parent=self.root,
                                         mustexist=True)
        if not folder:
            return
        installer = self.update_installer()
        try:
            update = installer.check_folder(folder, game)
        except installer.UpdateError as error:
            messagebox.showerror(APP_TITLE, f"This folder can't be used:\n{error}")
            return
        self.config["update_dir"] = str(update)
        write_config(self.config)
        self.update_label.config(text=self.update_text())
        messagebox.showinfo(APP_TITLE, f"Game update {app_version(update) or '?'} is used from now on:\n{update}")

    def install_game_update(self):
        """The player's own update PKG unpacked next to the game folder (tools/install_update.py;
        the port ships no game files)."""
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        game = self.game_dir()
        if not is_game_dir(game):
            messagebox.showwarning(APP_TITLE, "Choose the game folder first (Change game folder).")
            return
        from tkinter import filedialog
        pkg = filedialog.askopenfilename(title="Choose the game update (.pkg)", parent=self.root,
                                         filetypes=[("PS4 package", "*.pkg"), ("All files", "*.*")])
        if not pkg:
            return
        installer = self.update_installer()
        existing = installer.existing_update(game)
        if existing and not messagebox.askyesno(
                APP_TITLE, f"There is an update folder already:\n{existing}\n\nReplace it with this update?"):
            return
        state = {"step": "Checking the update..."}

        def work():
            try:
                state["path"] = installer.install(pkg, game, log=lambda text: state.__setitem__("step", text))
            except installer.UpdateError as error:
                state["error"] = str(error)
            except Exception as error:  # a disk or permission problem: shown as it is
                state["error"] = f"{type(error).__name__}: {error}"

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.update_label.config(text=state["step"])
                self.root.after(200, poll)
                return
            if "path" in state:  # the installed one is used, also over a folder chosen before
                self.config["update_dir"] = str(state["path"])
                write_config(self.config)
            self.update_label.config(text=self.update_text())
            if "error" in state:
                messagebox.showerror(APP_TITLE, f"The update was not installed:\n{state['error']}")
            else:
                messagebox.showinfo(APP_TITLE, f"Update installed:\n{state['path']}\n\nThe games use it from "
                                               "their next start.")
        poll()

    def install_game_pkg(self):
        """The player's own game PKG (for example the US copy, CUSA02320) unpacked into a folder
        they choose, with the metadata the launcher needs (tools/install_update.py install_game;
        the port ships no game files). That folder becomes the game folder."""
        if self.process:
            messagebox.showwarning(APP_TITLE, "Close the game first.")
            return
        from tkinter import filedialog
        pkg = filedialog.askopenfilename(title="Choose the game (.pkg), not the update", parent=self.root,
                                         filetypes=[("PS4 package", "*.pkg"), ("All files", "*.*")])
        if not pkg:
            return
        parent = filedialog.askdirectory(title="Choose where to put the game (about 60 GB)", parent=self.root,
                                         initialdir=str(Path(pkg).parent))
        if not parent:
            return
        installer = self.update_installer()
        state = {"step": "Checking the game package..."}

        def work():
            try:
                state["path"] = installer.install_game(pkg, parent,
                                                       log=lambda text: state.__setitem__("step", text))
            except installer.UpdateError as error:
                state["error"] = str(error)
            except Exception as error:  # a disk or permission problem: shown as it is
                state["error"] = f"{type(error).__name__}: {error}"

        worker = threading.Thread(target=work, daemon=True)
        worker.start()

        def poll():
            if worker.is_alive():
                self.game_dir_label.config(text=state["step"])
                self.root.after(200, poll)
                return
            if "path" in state:
                self.config["game_dir"] = str(state["path"])
                if WINE:
                    self.config["game_dir_unix"] = unix_path(str(state["path"]))
                write_config(self.config)
            self.game_dir_label.config(text=f"Game files: {self.game_dir()}")
            self.update_label.config(text=self.update_text())
            if "error" in state:
                messagebox.showerror(APP_TITLE, f"The game was not installed:\n{state['error']}")
            else:
                messagebox.showinfo(APP_TITLE, f"Game installed:\n{state['path']}\n\nIt is the game folder now. "
                                               "Next, install update 1.02: GAME UPDATE -> Install from .pkg.")
        poll()

    def desktop_shortcut(self):
        link, result = self.make_shortcut("Uncharted", APP_TITLE)
        if result.returncode == 0:
            messagebox.showinfo(APP_TITLE, f"Shortcut created:\n{link}")
        else:
            messagebox.showerror(APP_TITLE, f"Could not create the shortcut:\n{result.stderr.strip()}")

    def game_shortcuts(self):
        """One desktop shortcut per game: starts it directly with the launcher's settings."""
        made, failed = [], []
        for game in GAMES:
            name = game["name"].replace(":", " -")
            link, result = self.make_shortcut(name, f"{game['name']} (starts directly)", play=game["key"])
            (made if result.returncode == 0 else failed).append(link.name if result.returncode == 0
                                                                else result.stderr.strip())
        if failed:
            messagebox.showerror(APP_TITLE, "Could not create the shortcuts:\n" + "\n".join(failed))
        else:
            messagebox.showinfo(APP_TITLE, "Shortcuts created on the desktop:\n" + "\n".join(made) +
                                "\n\nEach starts its game directly with the launcher's settings. From a command "
                                "prompt: \"Uncharted Launcher.exe\" --play u1 (or u2, u3).")

    def reset(self):
        name = self.current_game()["name"]
        if not messagebox.askyesno(APP_TITLE, f"Reset {name}'s settings to the Medium preset?"):
            return
        self.game_settings().update(GAME_SETTING_DEFAULTS)
        self.ini.update(INI_DEFAULTS)
        self.refresh_controls()
        self.apply_preset("medium")

    def close(self):
        if self.process and not messagebox.askyesno(
                APP_TITLE, "The game is still running. Close the launcher anyway? (The game keeps running.)"):
            return
        self.config["window_xy"] = [self.root.winfo_x(), self.root.winfo_y()]
        write_config(self.config)
        self.root.destroy()


def main():
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass
    # Its own taskbar identity: without it the launcher's window is grouped under Python
    # (pythonw.exe) and the taskbar shows Python's icon instead of Uncharted's.
    try:
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("BilalReloaded.Uncharted.Launcher")
    except Exception:
        pass
    # --play u1|u2|u3 (or 1/2/3): start that game directly (desktop game shortcuts, GitHub #25).
    direct = None
    if "--play" in sys.argv[1:]:
        at = sys.argv.index("--play")
        value = sys.argv[at + 1].lower() if at + 1 < len(sys.argv) else ""
        direct = value if value in GAME_KEYS else f"u{value}" if f"u{value}" in GAME_KEYS else None
    root = tk.Tk()
    if direct:
        root.withdraw()
    # Errors in button and timer callbacks go to the error log too (pythonw has no console).
    root.report_callback_exception = lambda kind, value, trace: log_launcher_error(kind, value, trace)
    Launcher(root, direct)
    root.mainloop()


def log_launcher_error(kind, value, trace):
    """Appends an error to analysis/uncharted-user/launcher-error.log: the launcher runs under
    pythonw, so an error at start used to close it without a word (GitHub #21, 0.2.1)."""
    import traceback
    text = "".join(traceback.format_exception(kind, value, trace))
    path = USER_DIR / "launcher-error.log"
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8") as log:
            log.write(time.strftime("%Y-%m-%d %H:%M:%S ") + APP_TITLE + "\n" + text + "\n")
    except OSError:
        pass
    return text, path


if __name__ == "__main__":
    try:
        main()
    except Exception:
        text, path = log_launcher_error(*sys.exc_info())
        try:
            fallback = tk.Tk()
            fallback.withdraw()
            messagebox.showerror(APP_TITLE, f"The launcher could not start:\n\n{text[-1500:]}\n"
                                            f"This was saved to:\n{path}")
            fallback.destroy()
        except Exception:
            pass
        raise
