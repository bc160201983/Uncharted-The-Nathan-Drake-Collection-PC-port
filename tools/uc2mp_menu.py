"""Uncharted 2 multiplayer: puts a Multiplayer button back in the main menu.

The Collection's menu script (u2data/build/main/dc1/ps4/menu.bin) still holds every multiplayer
page (PAGE_MULTIPLAYER, PAGE_LOBBY, ...) and the game's text still has MENU_MULTIPLAYER in every
language, but no button leads there. The main menu's hidden "multiplayer medals" button
(BUTTON_MULTIMEDALS, a go-to-page button) is turned into a Multiplayer button: its label becomes
MENU_MULTIPLAYER and its target PAGE_MULTIPLAYER.

Writes <out>/u2data/build/main/dc1/ps4/menu.bin, to be used as an /app0 overlay
(UNCHARTED_MOD_DIR=<out>). The game folder is not modified.

usage: python tools/uc2mp_menu.py <game or update folder> <out> [--keep-condition]

Button record layout (offsets from the button's name pointer): +0x00 name, +0x08 name id,
+0x14 label source (0x2 the label below, 0x20 from code), +0x20 label, +0x28 label id, +0x30 action (dword) and condition (dword), +0x48 target page,
+0x50 target page id. Ids are CRC-32 (MSB first, polynomial 0x04C11DB7, no init, no final xor)
of the name.
"""
import argparse
import pathlib
import struct
import sys

MENU = pathlib.Path("u2data/build/main/dc1/ps4/menu.bin")


def sid(name):
    crc = 0
    for c in name.encode():
        crc ^= c << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def string_offset(data, name):
    """Offset of the NUL-terminated string `name` (DC strings follow a 4-byte type tag)."""
    needle = name.encode() + b"\0"
    pos = data.find(needle)
    while pos > 0 and (chr(data[pos - 1]).isalnum() or data[pos - 1] == ord("_")):  # a longer name
        pos = data.find(needle, pos + 1)
    if pos < 0:
        raise SystemExit(f"{name} not found in menu.bin")
    return pos


def string_at(data, offset):
    end = data.find(b"\0", offset, offset + 128) if 0 < offset < len(data) else -1
    return data[offset:end].decode("ascii", "replace") if end > 0 else None


def pointers_to_string(data, name):
    """Every qword pointing at a copy of `name` (the compiler writes one copy per use)."""
    return [p for p in range(0, len(data) - 7, 8)
            if string_at(data, struct.unpack_from("<Q", data, p)[0]) == name]


def patch_ready_buttons(data):
    """Reuse both Invitations controls for native PC readiness, atomically.

    PS4 click ID 0x0e8bb2d7 maps to the guarded RET island at 0x5ebea0.
    Query48 is originally the invitation count; the PC bridge replaces its
    label/enabled/item-text behavior. Preserve command and click identity.
    """
    name = "BUTTON_MULTIPLAYER_INVITES"
    records = pointers_to_string(data, name)
    if len(records) != 2:
        raise SystemExit(f"expected two Invitations controls, found {len(records)}; no output written")
    seen_flags = []
    for record in records:
        if record + 0x60 > len(data):
            raise SystemExit("truncated Invitations control; no output written")
        identity, = struct.unpack_from("<Q", data, record + 8)
        kind, flags = struct.unpack_from("<II", data, record + 0x10)
        label, label_id = struct.unpack_from("<QQ", data, record + 0x20)
        action = struct.unpack_from("<II", data, record + 0x30)
        target = struct.unpack_from("<QQ", data, record + 0x48)
        if (identity != sid(name) or kind != 2 or flags not in (0x04000010, 0x10)
                or string_at(data, label) != "MENU_MULTIPLAYER_INVITES"
                or label_id != sid("MENU_MULTIPLAYER_INVITES")
                or action != (0x16, 48) or target != (0, 0)
                or struct.unpack_from("<II", data, record + 0x58) != (1, 320)):
            raise SystemExit("unexpected Invitations control contract; no output written")
        seen_flags.append(flags)
    if sorted(seen_flags) != [0x10, 0x04000010]:
        raise SystemExit("unexpected Invitations page pair; no output written")
    # All checks precede mutation; the source file is never modified.
    for record, flags in zip(records, seen_flags):
        struct.pack_into("<I", data, record + 0x14, (flags & ~0x10) | 0x20)
        struct.pack_into("<QQ", data, record + 0x20, 0, 0)
        # Page navigation5a1cc1..5a1ce1 restricts Up/Down to the current
        # descriptor's column (+58). Keep Ready with Custom/Mode/Map in
        # column0; column1 is the roster and cannot be reached by Down.
        struct.pack_into("<I", data, record + 0x58, 0)
    return records


def patch_local_choices(data):
    """Use the game's ordinary choices for the two native lobby settings.

    Kind8 is a rich list: its item/layout queries depend on removed online
    metadata, which makes the restored rows overlap. Kind6 is already used
    for difficulty/language/audio choices and adjacent multiplayer settings.
    Keep the label, command, width and column; only change kind/option flags.
    """
    pages = [p for p in pointers_to_string(data, "PAGE_LOBBY")
             if struct.unpack_from("<Q", data, p + 8)[0] == sid("PAGE_LOBBY")]
    if len(pages) != 1:
        raise SystemExit("expected one custom lobby page; no output written")
    array, = struct.unpack_from("<Q", data, pages[0] + 0x38)
    count, base = struct.unpack_from("<QQ", data, array)
    if count != 26 or base + count * 0x70 > len(data):
        raise SystemExit("unexpected custom lobby controls; no output written")
    expected = {57: (3, "MENU_MULTIPLAYER_CUSTOMGAMETYPE"),
                58: (1, "MENU_MULTIPLAYER_CUSTOMMAP")}
    records = {}
    for record in range(base, base + count * 0x70, 0x70):
        action, command = struct.unpack_from("<II", data, record + 0x30)
        if action != 22 or command not in expected:
            continue
        flags, label = expected[command]
        label_pointer, label_id = struct.unpack_from("<QQ", data, record + 0x20)
        if (command in records or struct.unpack_from("<II", data, record + 0x10) != (8, flags)
                or string_at(data, label_pointer) != label or label_id != sid(label)
                or struct.unpack_from("<II", data, record + 0x58) != (0, 400)):
            raise SystemExit("unexpected native choice descriptor; no output written")
        records[command] = record
    if set(records) != set(expected):
        raise SystemExit("missing native choice descriptors; no output written")
    for record in records.values():
        struct.pack_into("<II", data, record + 0x10, 6, 0)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("game", type=pathlib.Path, help="folder holding u2data (the game's uroot or the update's)")
    parser.add_argument("out", type=pathlib.Path, help="overlay folder to write")
    parser.add_argument("--keep-condition", action="store_true",
                        help="keep the button's visibility condition (it hides the button by default)")
    parser.add_argument("--local-lobby", action="store_true",
                        help="dispatch BUTTON_MULTIPLAYER to the experimental PC lobby bridge (requires patched runtime)")
    args = parser.parse_args()
    source = args.game / MENU
    if not source.is_file():
        source = args.game / "uroot" / MENU
    data = bytearray(source.read_bytes())
    if args.local_lobby:
        for record in patch_ready_buttons(data):
            print(f"button at 0x{record:x}: Invitations -> native Ready toggle (command 48)")
        for command, record in patch_local_choices(data).items():
            print(f"choice at 0x{record:x}: native setting {command}, standard choice layout")

    multiplayer_page = string_offset(data, "PAGE_MULTIPLAYER")
    patched = 0
    for name in pointers_to_string(data, "BUTTON_MULTIMEDALS"):
        target, = struct.unpack_from("<Q", data, name + 0x48)
        if string_at(data, target) != "PAGE_MULTIMEDALS" or \
                struct.unpack_from("<I", data, name + 0x08)[0] != sid("BUTTON_MULTIMEDALS"):
            continue
        struct.pack_into("<Q", data, name + 0x28, sid("MENU_MULTIPLAYER"))
        if args.local_lobby:
            # Dispatch uses the compiled ID, not the diagnostic name string.
            # Keep the original string pointer: this format has relocation/type
            # metadata, so appending an untyped string is not safe.
            struct.pack_into("<Q", data, name + 0x08, sid("BUTTON_MULTIPLAYER"))
            struct.pack_into("<II", data, name + 0x30, 0, 0)
            struct.pack_into("<QQ", data, name + 0x48, 0, 0)
        else:
            struct.pack_into("<Q", data, name + 0x48, multiplayer_page)
            struct.pack_into("<Q", data, name + 0x50, sid("PAGE_MULTIPLAYER"))
        if not args.keep_condition:
            struct.pack_into("<I", data, name + 0x34, 0)
        patched += 1
        route = "callback BUTTON_MULTIPLAYER" if args.local_lobby else "target PAGE_MULTIPLAYER"
        print(f"button at 0x{name:x}: label MENU_MULTIPLAYER, {route}")
    if not patched:
        raise SystemExit("no BUTTON_MULTIMEDALS go-to-page button found")
    if args.local_lobby:
        # Page descriptors reserve +60/+68 for their Back callback name/ID.
        # EULA and MOTD already use this exact callback pair. Bind this page
        # too, so leaving also releases the host-owned local lobby.
        leave_name = "BUTTON_MULTIPLAYER_LEAVE_MULTIPLAYER"
        leave_pointer = string_offset(data, leave_name)
        bound = 0
        for page in pointers_to_string(data, "PAGE_MULTIPLAYER"):
            if struct.unpack_from("<Q", data, page + 8)[0] != sid("PAGE_MULTIPLAYER"):
                continue
            if struct.unpack_from("<QQ", data, page + 0x60) != (0, 0):
                raise SystemExit("unexpected multiplayer Back binding; no output written")
            struct.pack_into("<QQ", data, page + 0x60, leave_pointer, sid(leave_name))
            bound += 1
        if bound != 1:
            raise SystemExit(f"expected one multiplayer page, found {bound}; no output written")
        print("multiplayer Back: callback BUTTON_MULTIPLAYER_LEAVE_MULTIPLAYER")
    # PAGE_MULTIPLAYER's buttons and its stats panel keep their records untouched: their texts
    # come from the runtime (uc2mp_menu_bridge.h, uc2mp_text_bridge.h). A label written here
    # (label source 0x2) also changes the layout: such a button is drawn centered.
    out = args.out / MENU
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"wrote {out} ({patched} buttons)")


if __name__ == "__main__":
    sys.exit(main())
