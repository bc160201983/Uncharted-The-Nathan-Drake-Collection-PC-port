"""Prepare and run Uncharted: The Nathan Drake Collection on the local runtime.

The launcher (launcher/uncharted_launcher.pyw) starts this with --boot --seconds 0. Its
environment carries the options: UNCHARTED_DIAGNOSTICS=0 leaves out the measurement logging and
live trigger files, GOW3_VBLANK_HZ=0 unlocks the frame rate (480 Hz vblank, menu limit).
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import shutil
import struct

ROOT = Path(__file__).resolve().parent.parent

# Code patches, applied to the linked image by byte pattern (hex, "??" = any byte). A patch lists
# variants (description, [(pattern, offset in pattern, original bytes, replacement bytes), ...]):
# the first variant whose pattern occurs exactly once is applied. The variants cover the game
# versions 1.00 and 1.02 (the update recompiled all three executables). A variant can also make
# several changes, all or none: (pattern, [(offset, original bytes, replacement bytes), ...]).
def pattern_regex(text):
    import re
    text = text.replace(" ", "")
    return re.compile(b"".join(b"." if text[i:i + 2] == "??" else re.escape(bytes.fromhex(text[i:i + 2]))
                               for i in range(0, len(text), 2)), re.S)


# The wait for a frame's GPU end-of-pipe stamp gives up after 2.0 s (vucomiss [2.0f]; ja). On PC
# the command processor can pause that long (shader compilation, readbacks), never on a PS4.
# 1.00 then asserts m_gfxEopTick of frame N-3 in FrameBegin (fg-draw-mgr.cpp:234; U1 0x3fd816);
# 1.02 halts right there (int 0x41; U1 0x4fdf96). NOP the `ja` that leaves the loop on timeout.
# The three games have the same loop; only the rip-relative offset of the 2.0f differs.
GPU_WAIT_PATCH = ("wait for GPU frame completion without the 2 s timeout", [
    ("c5f258c0 c5f82e05 ???????? 7739 f390 beffffffff", 12, bytes.fromhex("7739"), bytes.fromhex("9090")),
    ("c5f258c0 c5f82e05 ???????? 7710 f390 498b00", 12, bytes.fromhex("7710"), bytes.fromhex("9090")),
])
EBOOT_PATCHES = [
    GPU_WAIT_PATCH,
    # Silent sound effects (footsteps, gunfire, ambience; music and speech fine): the sound config
    # loader (biglib audio acm 0x515a50) opens reverbs/mixers/environments/prototypes.txt through
    # the sound file callback 0x8ef80 with flag 1, which `testb $5,%cl` at 0x8f1da turns into O_CREAT.
    # Our Fios2 rejects READ|CREAT on read-only /app0 with SCE_FIOS_ERROR_READ_ONLY (0x80820015,
    # mapped to EROFS 0x1e) before any kernel call, so no sound prototypes load and every effect
    # request resolves to nothing. Test bit 2 only: a read request opens read-only. (1.00 and 1.02)
    ("sound config files open read-only (no O_CREAT on /app0)", [
        ("4a8d542b14f6c1050f95c00fb6c0c1e0068db000020000f6c106", 5,
         bytes.fromhex("f6c105"), bytes.fromhex("f6c104")),
    ]),
]

# Uncharted 2's player torch aims with the camera. The torch update (Flashlight Active
# 0x69a190) aims the light along the torch joint (camera mode 9: the camera's horizontal forward,
# 0x69afbd), and then, every frame, the player light (record 0 of the light table 0x1157760) is
# re-aimed: target = pos + dir * K, dir = the camera's forward vector (0x104e820 + 0x270),
# pos = target - dir * K (function 0x698b50 and a copy inlined next to UpdatePadLightBar,
# 0x3fd78d). Turn the camera to face Nate and the light falls on the wall behind him. Skip both
# re-aims and take the joint path in every camera mode: the light follows the torch, like the
# beam drawn from it. Optional (UNCHARTED_TORCH_FORWARD=0 keeps the original); 1.02 only (1.00
# lays this code out differently).
TORCH_FORWARD_PATCHES = [
    ("Uncharted 2 torch: no re-aim along the camera (frame update copy)", [
        ("488d05cc9fd500 8b4850 84c9 7440 8a50", 12, bytes.fromhex("7440"), bytes.fromhex("eb40")),
    ]),
    ("Uncharted 2 torch: no re-aim along the camera (flashlight.cpp)", [
        ("488d0509ecab00 8b4850 84c9 7440 8a50", 12, bytes.fromhex("7440"), bytes.fromhex("eb40")),
    ]),
    ("Uncharted 2 torch: aimed by its joint in every camera mode", [
        ("833df040de0009 0f8534010000 4885c00f842b010000", 7,
         bytes.fromhex("0f8534010000"), bytes.fromhex("90e934010000")),
    ]),
]

# Idle job workers sleep (all three games, 1.02). The engine's 6 NdJobWorkerThreads spin on their
# job queues with `pause` unless the job manager's sleep flag (byte +0x59, set by its own
# SetSleepMode, U3 0xd96440) is on; with it on, an idle worker waits on the manager's condition
# variable after 1000 spins (10 ms timeout) and every job submission broadcasts to wake them. On
# the PS4 each worker owns a core and the flag stays off; on a 4-core PC the six spinning workers
# took 390% of 400% CPU (GPU command thread 5%, title screen 19-27 FPS on an RTX 5080). Treat the
# flag as on in the worker loop, its re-check before waiting, and the submit path.
# Another retail 1.02 build (EP9000-CUSA02344, found by currantsforeyes, Supermedo/uncharted_pc
# #346) has the same three sites with other registers: the re-check loads the manager through r13
# instead of r15, and the submit path tests rdi and jumps with a short je; its worker loop is the
# same. Each site takes whichever form matches. UNCHARTED_JOB_SLEEP=0 keeps the original.
JOB_SLEEP_PATCHES = [
    ("idle job workers sleep (worker loop)", [
        ("4d8d7001 807f5900 74?? 4981f8e8030000 7c??", 8, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
    ("idle job workers sleep (re-check before waiting)", [
        ("498b07 80785900 74?? 4889c7 4883ef80", 7, bytes.fromhex("74"), bytes.fromhex("9090")),
        ("498b4500 80785900 74?? 4889c7 4883ef80", 8, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
    ("idle job workers sleep (job submission wakes them)", [
        ("80785900 0f84????0000 4883c060", 4, bytes.fromhex("0f84"), bytes.fromhex("660f1f440000")),
        ("807f5900 74?? 4883c760 beffffffff", 4, bytes.fromhex("74"), bytes.fromhex("9090")),
    ]),
]

# The games' own Options > Controls > Aiming > Sensitivity (1.02): a 10-notch slider from 0 to 1.0
# with 0.5 in the middle (setting 0x1abac2d6; Reset Defaults sets 0.32). The aim speed is mixed
# linearly from two tuning values by it, and the aiming code asserts it is at most 1 ("sensitivity
# <= 1.0f"). Mouse aiming goes through the stick there, so the game's aim speed caps it: lift the
# check and raise the slider's top. The value is a float in the save file, read with or without
# this. UNCHARTED_AIM_RANGE=0 keeps the original sliders.
AIM_RANGE_PATCHES = {
    # Uncharted 1: the check at 0x276308; the slider (0x1453e4) holds steps, min, mid and max at
    # +0x20..+0x2c and gets mid 2.5, max 5.0: a notch is 0.5, the second one the old maximum.
    "eboot.bin": [
        ("Uncharted 1 aim sensitivity above 1 (aim speed range check)", [
            ("c5f82e05 ???????? 763d c5fa1145f4", 8, bytes.fromhex("763d"), bytes.fromhex("eb3d")),
        ]),
        ("Uncharted 1 aim sensitivity slider up to 10", [
            ("488d0dd5738000 48894818 48c740200a000000 48b9 0000003f0000803f 48894828", 21,
             bytes.fromhex("0000003f0000803f"), bytes.fromhex("0000a04000002041")),
        ]),
    ],
    # Uncharted 2: the check in 0x719d80; the slider item (0x5d25b1) takes its max from a constant
    # (mid = max / 2): point it at a 5.0, so a notch is 0.5 and the second one the old maximum.
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 aim sensitivity above 1 (aim speed range check)", [
            ("c5f82e05 ???????? 7649 488b05", 8, bytes.fromhex("7649"), bytes.fromhex("eb49")),
        ]),
        ("Uncharted 2 aim sensitivity slider up to 10", [
            ("81f9d6c2ba1a 0f85???????? 4989ff 4c8d35???????? c5fa1005 75fa8800", 26,
             bytes.fromhex("75fa8800"), bytes.fromhex("4de08800")),
        ]),
    ],
    # Uncharted 3: the check in UpdateAiming3Settings (0x927fe5). The slider's value -> notch
    # (0x62bf60) and notch -> value (0x62c090) functions keep min/mid/max in registers (0, 0.5,
    # 1.0); the aim setting's branch in each is rewritten in place: the same per-profile address
    # (array 0x17f97a0 + profile * 64 + 0x20, in fewer bytes: imul, then sign extension) and max =
    # 1.0 doubled twice = 4.0. The left half keeps its 0.1 notches, the right half goes 0.5 to 4.0.
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 aim sensitivity above 1 (aim speed range check)", [
            ("c5fa1015???????? c5f82ed0 7351 488b05", 12, bytes.fromhex("7351"), bytes.fromhex("eb51")),
        ]),
        ("Uncharted 3 aim sensitivity slider up to 10 (value -> notch)", [
            ("81f9d6c2ba1a7547 488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b", 8,
             bytes.fromhex("488d05dd63190148630048c1e006488d0dcfd71c01488d440120eb2b"),
             bytes.fromhex("6b05dd63190140488d0df6d71c014801c8f30f590d13c1b4ffeb2c90")),
        ]),
        ("Uncharted 3 aim sensitivity slider up to 10 (notch -> value)", [
            ("3dd6c2ba1a7520 488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36", 7,
             bytes.fromhex("488d0da662190148630948c1e106488d1598d61c01488d4c0a20c5f828daeb36"),
             bytes.fromhex("6b0da662190140488d15bfd61c014801d1c5f828daf30f590dd8bfb4ffeb3790")),
        ]),
    ],
}

# FrameBegin (ndlib fg-draw-mgr.cpp, all three games, 1.02) frees frame N-3's instances and
# asserts that frame's end-of-pipe GPU timestamp is set (m_gfxEopTick of the render frame params,
# 32 slots: +0xa28, Uncharted 1 +0xa08). Players' crash logs (GitHub #11/#12/#17, RTX 3050 and
# 5060, and ours) show it after 40-55 ms stalls (shader compiles, waits for a GPU that fell behind):
# the timestamp, written when the GPU gets there (or after a ray-cast readback lands), was not
# there yet. On the PS4 the GPU never lags that far. The assert's path (59 bytes up to where the
# check continues) becomes a wait for the timestamp: pause in a loop until it is set, at most
# 50 million times (about 0.1-1.5 s by CPU), then carry on. rdx (frame N-3) is kept; the rest of
# the path's registers were scratch there. UNCHARTED_FRAME_WAIT=0 keeps the assert.
def frame_begin_wait(offset):
    code = (bytes.fromhex("488dbc08") + offset.to_bytes(4, "little")  # lea rdi, [rax + rcx + offset]
            + bytes.fromhex("be80f0fa02")  # mov esi, 50000000
            + bytes.fromhex("48833f00")  # loop: cmp qword ptr [rdi], 0
            + bytes.fromhex("7506")  # jne done
            + bytes.fromhex("f390")  # pause
            + bytes.fromhex("ffce")  # dec esi
            + bytes.fromhex("75f4")  # jnz loop
            + bytes.fromhex("eb20"))  # done: jmp to where the check continues
    return code + b"\xcc" * (59 - len(code))


FRAME_BEGIN_ASSERTS = {  # executable: (the check and its assert path, timestamp offset)
    "eboot.bin": ("4883bc08080a000000753b488b057035be004885c0742d488d3dd2053400488d35710434004889d3"
                  "488d15fd053400488d0dd01c350041b8ea0000004531c9ffd04889dacd41", 0xa08),
    "big2-ps4_Shipping.elf": ("4883bc08280a000000753b488b05609b99004885c0742d488d3dad5f2e00488d354c5e2e00"
                              "4889d3488d15d85f2e00488d0d38ff210041b8ea0000004531c9ffd04889dacd41", 0xa28),
    "big3-ps4_Shipping.elf": ("4883bc08280a000000753b488b05f06edb004885c0742d488d3d7df56400488d351cf46400"
                              "4889d3488d15a8f56400488d0d6409570041b8ea0000004531c9ffd04889dacd41", 0xa28),
}
FRAME_BEGIN_WAIT_PATCHES = {
    executable: [(f"FrameBegin waits for frame N-3's GPU timestamp (no m_gfxEopTick assert)", [
        (code, 11, bytes.fromhex(code)[11:], frame_begin_wait(offset)),
    ])]
    for executable, (code, offset) in FRAME_BEGIN_ASSERTS.items()
}

# Uncharted 3 (1.02) Player::UpdateFocus (player.cpp:1897, 0xbcee10) springs the point Nate looks
# at ([player + 0xef0], velocity + 0xf00) towards the target the player gives (kept at rbp-0x80) and
# asserts the result is not NaN (GitHub #13: crash climbing the fort in chapter 8). Once NaN it
# stays NaN, and the function's own reset (0xbcf27a, taken when the point is over 6 m off) never
# runs for it: NaN compares false. The assert's path (53 bytes) now takes that reset instead when
# the target itself is a number (velocity register xmm5 zeroed first, as on the reset's usual
# way in); a NaN target still asserts. UNCHARTED_FOCUS_RESET=0 keeps the original.
FOCUS_NAN_PATCHES = {
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 player focus: reset instead of asserting on NaN", [
            ("c4e37904cb00c5f0c2c900c5f850c183f80f743a488b05f73302014885c07427488d3d74ff8900"
             "488d3546fd8900488d1583ff8900488d0d6ece7d0041b86a0700004531c9ffd0cd41", 20,
             bytes.fromhex("488b05f73302014885c07427488d3d74ff8900488d3546fd8900488d1583ff8900"
                           "488d0d6ece7d0041b86a0700004531c9ffd0cd41"),
             bytes.fromhex("c5d057edc5f8284580c5f8c2c800c5f850c183f80f0f84a5feffffcd41") + b"\xcc" * 24),
        ]),
    ],
    # Uncharted 2 (1.02) PlayerBase::UpdateFocus (player-base.cpp:1101, 0x7272e0): the same spring
    # (point [player + 0x2b0], velocity + 0x2c0, target at rbp-0x60) and the same NaN assert (GitHub
    # #24, and the user's own U2 session on 16 Oct). Its reset when the point is far off (0x72769a)
    # snaps the point to the target. The assert's path (53 bytes) now clears the velocity and takes
    # that snap when the target is a number; a NaN target still asserts.
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 player focus: reset instead of asserting on NaN", [
            ("c4e37904c900 c5f0c2c900 c5f850c1 83f80f 743a 488b05???????? 4885c0 7427 488d3d????????"
             " 488d35???????? 488d15???????? 488d0d???????? 41b84d040000 4531c9 ffd0 cd41", 20,
             bytes.fromhex("488b05e267e4004885c07427488d3d7d4c7600488d352c4b7600488d158c4c7600"
                           "488d0dbdcb6c0041b84d0400004531c9ffd0cd41"),
             bytes.fromhex("c5f8284da0c5f0c2d100c5f850c283f80f7511c5e857d2c5f82993c0020000"
                           "e9e7feffffcd41") + b"\xcc" * 15),
        ]),
    ],
}

# The most common Drake's Fortune crash (GitHub #27, #30, #42, #62: "execute access to 0", called
# from 0x64011f; Uncharted 2 and 3 have the same code). For each shader the constant-update code
# fills a table (a 0x28-byte header, then 0x30-byte entries whose 16-bit word picks the member
# function a draw calls later) in a leaf function (U1 1.02 0x63df10, U2 0xd645f0, U3 0x12d6cf0)
# that keeps its running entry indices in the 128 bytes below rsp. The tables lie in a ring next
# to GPU data, so one can cross into a page the GPU tracker has write-protected, and the store
# there faults in the middle of the function. Windows builds the exception frame on the same stack
# and its x64 ABI promises nothing below rsp (the 26300 build on our test PC happens to leave
# 1.6 KB free, which is why it never crashed here; a reporter runs Windows 10). An index then
# reads 0, an entry is written over another, and the walk calls the null member its stale word
# selects. The function's start now jumps to a stub that writes each page of the table once
# (lock or byte [p], 0: no value changes) before anything lives below rsp, then carries on: a
# protected page faults there, harmlessly. The stub sits in a dead assert path nearby, which
# still starts with int 0x41, so that assert still stops the game. UNCHARTED_CUE_PRETOUCH=0: off.
CUE_TABLE_FILL = "55 41 57 41 56 41 55 41 54 53 49 b9 00 24 00 0d 10 00 00 00 48 c7 07 00 00 00 00 45 31 f6"


def cue_pretouch_variant(cave, original):
    """The table fill's first 5 bytes (push rbp; push r15; push r14) become a jump to the stub at
    `cave` (from the start of the function), which replaces the 53-byte assert path `original`."""
    stub = bytes.fromhex(
        "cd41"            # int 0x41 (the assert path's end, now its start)
        "89d0"            # mov eax, edx                 entries
        "486bc030"        # imul rax, rax, 0x30
        "488d440727"      # lea rax, [rdi + rax + 0x27]  the table's last byte
        "4889f9"          # mov rcx, rdi
        "f0800900"        # touch: lock or byte [rcx], 0
        "6681c9ff0f"      # or cx, 0xfff
        "48ffc1"          # inc rcx                      the next page
        "4839c1"          # cmp rcx, rax
        "76ef"            # jbe touch
        "55 4157 4156".replace(" ", ""))  # the moved instructions
    stub += b"\xe9" + struct.pack("<i", 5 - (cave + len(stub) + 5))  # jmp to the function's 6th byte
    original = bytes.fromhex(original)
    assert len(stub) <= len(original) == 53
    return (CUE_TABLE_FILL, [
        (cave, original, stub + b"\xcc" * (len(original) - len(stub))),
        (0, bytes.fromhex("5541574156"), b"\xe9" + struct.pack("<i", cave + 2 - 5)),
    ])


CUE_PRETOUCH_PATCHES = {  # 1.02: the assert path (r14d < 0x10000 in U1, line 0xae4 in U2/U3)
    "eboot.bin": [("constant tables: pages written before the fill (no fault inside it)", [
        cue_pretouch_variant(0xfd7a, "488b056780a6004885c07427488d3de3671d00488d35f8671d00488d1524681d00"
                                     "488d0dca671d0041b8ad0100004531c9ffd0cd41"),
    ])],
    "big2-ps4_Shipping.elf": [("constant tables: pages written before the fill (no fault inside it)", [
        cue_pretouch_variant(-0xa850, "488b05d14181004885c07427488d3dac8e1500488d35e9801500488d15b28e1500"
                                      "488d0daca5090041b8e40a00004531c9ffd0cd41"),
    ])],
    "big3-ps4_Shipping.elf": [("constant tables: pages written before the fill (no fault inside it)", [
        cue_pretouch_variant(-0xb690, "488b05517192004885c07427488d3d4c841b00488d35b4761b00488d1552841b00"
                                      "488d0dc80b0e0041b8e40a00004531c9ffd0cd41"),
    ])],
}

# Uncharted 2 "read access to 0 at guest offset 0xd09665" (GitHub #61 #113 #115 #164 #181 #187;
# 24 Oct 2026): the particle setup's run grouping (particle-setup.cpp; the same leaf in all three
# games: 1.02 U1 0x5fa2f0, U2 0xd09580, U3 0xf6bef0; 1.00 U1 0x4d0b80, U2 0x661150, U3 0x60dd70)
# keeps its loop bounds below rsp: [rsp-8] = array + 0x10, [rsp-0x10] = count - 1 (1.00:
# 1 - count). A page fault inside it (GPU write tracking) lets Windows build the exception frame
# right below the 16-aligned rsp: [rsp-0x10] comes back wrong, the end-of-array test fails and the
# look-ahead reads the entry one past the end (stale memory) and dereferences it. The reloads now
# take the same values from r10 (array) and r11 (count), which the function never changes. Same
# length, in place (analysis/triage-1022/d09665.md). UNCHARTED_RUN_GROUP_FIX=0: off.
RUN_GROUP_PATCH = ("particle run grouping: loop bounds from registers, not below rsp", [
    # 1.02: mov rcx,[rsp-8] -> lea rcx,[r10+0x10]; nop | mov r15,[rsp-0x10] -> lea r15,[r11-1]; nop
    ("4c8b27 488b4c24f8 4a8d1c31 4c8b7c24f0 4929f7", [
        (3, bytes.fromhex("488b4c24f8"), bytes.fromhex("498d4a1090")),
        (12, bytes.fromhex("4c8b7c24f0"), bytes.fromhex("4d8d7bff90")),
    ]),
    # 1.00: mov rcx,[rsp-8] -> the same lea | mov rcx,[rsp-0x10]; lea r8,[rcx+rsi]
    #       -> mov r8,rsi; sub r8,r11; inc r8   (rcx is zeroed right after, r8 = i + 1 - count)
    ("4c8b27 488b4c24f8 4a8d1c31 488b4c24f0 4c8d0431 4531ed", [
        (3, bytes.fromhex("488b4c24f8"), bytes.fromhex("498d4a1090")),
        (12, bytes.fromhex("488b4c24f04c8d0431"), bytes.fromhex("4989f04d29d849ffc0")),
    ]),
])

# Everything unlocked (launcher -> Advanced -> Unlock all chapters, UNCHARTED_UNLOCK=1): the three
# games keep Bluepoint's tweak flags "Unlock Chapter Select by default" and "Unlock Brutal
# difficulty by default" (debug menu; +2/+1 after the Crushing flag, which the Collection already
# sets). At start the game fills the first and the chapter one from the *bp-ucctweaks* table
# (Crushing on, chapters off); Brutal starts off. The chapter flag is now set where the table is
# read, Brutal where it is cleared (1.02; U1 has two copies of that constructor).
# Cheats (launcher -> Advanced -> Cheats, UNCHARTED_CHEATS=invincible,ammo,noreload,grenades,onehit):
# the games keep Naughty Dog's debug-menu switches ("Invincible Player", "Infinite Ammo", "No
# Reload", "Infinite Grenades", "One Hit Kills"; the menu itself is unreachable), each a byte the
# menu item points at. The runtime keeps the chosen ones set (GOW3_POKE). Update 1.02 addresses
# (guest image offsets); Uncharted 3's invincibility sits in an options block the game allocates,
# reached through the pointer at 0x16e2f80.
CHEATS = {
    "eboot.bin": {"invincible": "aa5ff8", "ammo": "ab9c01"},
    "big2-ps4_Shipping.elf": {"invincible": "104f877", "ammo": "104f84a", "noreload": "104f84b"},
    "big3-ps4_Shipping.elf": {"invincible": "*16e2f80+a7", "ammo": "17faa91", "grenades": "17faa92",
                              "noreload": "17faa93", "onehit": "17fab94"},
}

# 19 Oct: Uncharted 1 and 2 keep "Infinite Ammo" in their settings system (the Ultra Easy option
# sets it there, id 0xa6effd08 in U2), which writes the byte back every frame: a poke does not
# stick. Each place that reads it (`cmp byte [flag], 0; jne <infinite ammo>`) takes the jump as if
# it were set (1.02 code; the lea's displacement makes each pattern unique).
def _ammo_reader(pattern, jne_at):
    return (pattern, jne_at, b"\x75", b"\xeb")


# Unlock all rewards (launcher -> Advanced, UNCHARTED_UNLOCK_REWARDS=1): Naughty Dog's debug menu
# switches "Give all Store Items" and "Give All Movies" (the menu itself is unreachable), kept set
# like the cheats (GOW3_POKE): the games' unlock checks treat every reward (skins, weapons, render
# modes, art galleries, tweaks) and every story cutscene as unlocked while they are set. Nothing
# is written to the save: without them the Bonuses menus show the player's own progress again
# (21 Oct, Uncharted 3: Rewards 81/81, 47/47, 3/3, 6/6, 7/7 with them; 1/81 ... from the same save
# without). Update 1.02 image offsets.
REWARD_POKES = {
    "big2-ps4_Shipping.elf": ["1114178", "1114179"],
    "big3-ps4_Shipping.elf": ["17b2a94", "17b2a95"],
}
# Drake's Fortune keeps each reward's status in its own record (bit 0 unlocked) and its debug
# switch also writes them, so its checks are patched instead: every reward reads as unlocked,
# usable and counted ("Unlocked 47 of 47"), and the Story Cutscenes list skips no chapter. The
# status bytes are not touched (nothing saved; earned rewards are still recorded). 1.02 code.
REWARD_PATCHES = {
    # Uncharted 2 and 3 let some rewards (tweaks, render modes) be enabled only once the game was
    # finished on the current difficulty or a harder one: the reward screen's item info sets
    # "needs the game finished" (+0xe) to 1 and clears it from the save's finished-difficulty bits.
    # It is now set to 0 (the user, 21 Oct: "cannot enable until you beat the game").
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 rewards can be enabled before the game is finished", [
            ("7441c6430e01833d401fa800", 5, bytes.fromhex("01"), bytes.fromhex("00"))]),
    ],
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 rewards can be enabled before the game is finished", [
            ("74054c39f0725c41c6450e01488d05", 11, bytes.fromhex("01"), bytes.fromhex("00"))]),
    ],
    "eboot.bin": [
        # IsUnlocked(item): `and al, 1` on status bit 0 -> `mov al, 1`
        ("Uncharted 1 rewards unlocked (item check)", [
            ("b001803f00750d4885f674098a86880000002401", 18, bytes.fromhex("24"), bytes.fromhex("b0"))]),
        # IsUnlocked(category, id)
        ("Uncharted 1 rewards unlocked (id check)", [
            ("75134d85e4740c418a8424880000002401eb02", 15, bytes.fromhex("24"), bytes.fromhex("b0"))]),
        # the "Unlocked N of 47" counter: `je skip` -> `je +0`
        ("Uncharted 1 rewards unlocked (counter)", [
            ("4c89f275154885c07422f68088000000017419", 18, bytes.fromhex("19"), bytes.fromhex("00"))]),
        # usable-by-id check: `je not` -> `je +0`
        ("Uncharted 1 rewards unlocked (usable by id)", [
            ("41803e00750af684088800000001741e488d55d4", 15, bytes.fromhex("1e"), bytes.fromhex("00"))]),
        # CanEnable: `test [item+0x88],1; sete dl` (dl = locked) -> `setb dl` (0: test clears CF)
        ("Uncharted 1 rewards unlocked (enable check)", [
            ("41f68688000000010f94c24584ff0f95c184c0", 9, bytes.fromhex("94"), bytes.fromhex("92"))]),
        # CanEnable's "finish the game on this difficulty first" reason (bit 1, the menu's
        # STORE_REQUIREMENT_CURRENT_DIFFICULTY_ENABLING): `mov r13b, 1` before the finished-
        # difficulty loop -> `mov r13b, 0` (the user, 21 Oct: a problem with rewards in U1)
        ("Uncharted 1 rewards can be enabled before the game is finished", [
            ("448bb87848000041b5014183ff067338", 9, bytes.fromhex("01"), bytes.fromhex("00"))]),
        # Story Cutscenes list: `cmp [node+0x50], 0; js skip` (chapter not reached) -> `js +0`
        ("Uncharted 1 story cutscenes unlocked", [
            ("488b7840837f50000f88880000004c8b4210", 10, bytes.fromhex("88"), bytes.fromhex("00"))]),
    ],
}


def cheat_names(executable):
    """The cheats asked for (UNCHARTED_CHEATS, launcher -> Cheats)."""
    return [name for name in os.environ.get("UNCHARTED_CHEATS", "").replace(" ", "").split(",") if name]


CHEAT_PATCHES = {
    "eboot.bin": {"ammo": [
        (f"Uncharted 1 infinite ammo (reader {n})", [_ammo_reader(pattern, at)])
        for n, (pattern, at) in enumerate((("488d15caca8200 b101 803a00 75", 12),
                                           ("488d0d79673800 803900 75", 10),
                                           ("488d0d3a5b3800 803900 75", 10),
                                           ("488d0d0a023800 803900 75", 10)), 1)] + [
        # 19 Oct (user: "Infinite ammo was not working"): Drake's Fortune's own Infinite Ammo only
        # spares the reserve when reloading (0x345e40/0x345ec0); each shot still takes a round from
        # the magazine (0x346440: `dec [inventory + slot*0x30 + 0x440]`, skipped for weapons whose
        # definition says so). With the cheat on that decrement is gone: the magazine stays full.
        ("Uncharted 1 infinite ammo (magazine)",
         [("89f3 4989fe e8???????? 4885c0 741b 83b82801000000 7512 89d8 488d0440 48c1e004 "
           "41ff8c0640040000 5b415e5dc3", 34, bytes.fromhex("41ff8c0640040000"),
           bytes.fromhex("0f1f840000000000"))])]},
    "big2-ps4_Shipping.elf": {"ammo": [
        (f"Uncharted 2 infinite ammo (reader {n})", [_ammo_reader(pattern, at)])
        for n, (pattern, at) in enumerate((("488d0dbc8d6f00 80793a00 75", 11),
                                           ("488d0d63736f00 80793a00 75", 11),
                                           ("488d0d09436f00 80793a00 75", 11),
                                           ("488d0d98635600 80793a00 75", 11),
                                           ("488d0d3b635600 80793a00 75", 11),
                                           ("4c8d2d285f5600 41807d3a00 75", 12),
                                           ("e8fd3e2600 41807d3a00 75", 10)), 1)]},
}

UNLOCK_PATCHES = {
    "eboot.bin": [
        (f"Uncharted 1 chapter select unlocked ({copy})", [
            (f"e8{call} 4885c0 7411 8a08 888b302c0000 8a4001 8883322c0000", 18,
             bytes.fromhex("8a40018883322c0000"), bytes.fromhex("c683322c0000016690")),
        ]) for copy, call in (("1", "fe250000"), ("2", "f51d0000"))
    ] + [
        (f"Uncharted 1 Brutal difficulty unlocked ({copy})", [
            (f"e8{call} c683302c000001 c683312c000000", 12,
             bytes.fromhex("c683312c000000"), bytes.fromhex("c683312c000001")),
        ]) for copy, call in (("1", "ea3b0000"), ("2", "e1330000"))
    ],
    "big2-ps4_Shipping.elf": [
        ("Uncharted 2 chapter select unlocked", [
            ("833800 0f9583a02d0000 83780400 0f9583a22d0000", 10,
             bytes.fromhex("837804000f9583a22d0000"), bytes.fromhex("c683a22d0000010f1f4000")),
        ]),
        ("Uncharted 2 Brutal difficulty unlocked", [
            ("c683a02d000001 c683a12d000000 c683a22d000000", 7,
             bytes.fromhex("c683a12d000000"), bytes.fromhex("c683a12d000001")),
        ]),
    ],
    "big3-ps4_Shipping.elf": [
        ("Uncharted 3 chapter select unlocked", [
            ("833800 0f9583602c0000 83780400 0f9583622c0000", 10,
             bytes.fromhex("837804000f9583622c0000"), bytes.fromhex("c683622c0000010f1f4000")),
        ]),
        ("Uncharted 3 Brutal difficulty unlocked", [
            ("c683602c000001 c683612c000000 c683622c000000", 7,
             bytes.fromhex("c683612c000000"), bytes.fromhex("c683612c000001")),
        ]),
    ],
}

# Ultrawide (GitHub #20): the games keep a 16:9 camera's horizontal field of view at every aspect
# ratio and lose the top and bottom of the picture instead (Vert-). The render camera holds
# zoom = 1/tan(horizontal FOV / 2) and aspect = height/width (U2/U3 +0x254/+0x250, Drake's Fortune
# +0x294/+0x290); the projection (x scale zoom, y scale zoom/aspect) and the view frustum's culling
# planes are built from them. Where those read the zoom of a perspective camera it becomes
# zoom * min(1, aspect * 16/9): wider than 16:9 keeps the 16:9 picture's vertical view and shows
# more at the sides (Hor+), 16:9 and narrower views and square cameras are unchanged, and
# orthographic cameras keep their own branches. The zoom loads jump to small stubs in dead assert
# branches of the projection code (each now starts with int 0x41, so it still stops the game), and
# back. Applied only to render sizes wider than 16:9 (UNCHARTED_HOR_PLUS=0: never).
_GPR = {name: number for number, name in enumerate(
    ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
     "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"))}
_NOPS = {3: b"\x0f\x1f\x00", 4: b"\x0f\x1f\x40\x00", 5: b"\x0f\x1f\x44\x00\x00"}


def _vex_mem(op, dst, src, base, disp):
    """op xmm<dst>, xmm<src>, [base + disp32] (scalar single; vmovss: src 0): VEX 2-byte for
    rax..rdi, 3-byte for r8..r15, with the SIB byte that rsp/r12 as a base need."""
    reg = _GPR[base]
    modrm = bytes([0x80 | dst << 3 | (reg & 7)]) + (b"\x24" if reg & 7 == 4 else b"")
    if reg < 8:
        prefix = bytes([0xc5, 0x80 | (~src & 15) << 3 | 2])
    else:
        prefix = bytes([0xc4, 0xc1, (~src & 15) << 3 | 2])
    return prefix + bytes([op]) + modrm + struct.pack("<i", disp)


def _vex_rip(op, dst, src, at, target):
    """op xmm<dst>, xmm<src>, [rip -> target], the 8-byte instruction placed at `at`."""
    return bytes([0xc5, 0x80 | (~src & 15) << 3 | 2, op, dst << 3 | 5]) + struct.pack("<i", target - (at + 8))


def hor_plus_variant(pattern, zoom, aspect, sites, caves):
    """One patch variant: sites (offset, base register, xmm with the zoom, a free xmm) are
    `vmovss xmm, [base + zoom]` loads; caves (offset, size, original first bytes) are the dead
    assert branches that take the stubs. Offsets are from the start of the pattern's match (a
    cave may lie before it or in a neighbouring function: its original bytes are checked too)."""
    edits, cave_list = [], [list(cave) + [2] for cave in caves]  # + bytes used (after int 0x41)
    stubs = {}
    for offset, base, dst, free in sites:
        assert dst < 8 and free < 8 and dst != free
        length = len(_vex_mem(0x10, dst, 0, base, zoom))
        size = 21 + 2 * length
        cave = next(c for c in cave_list if c[3] + size + 8 <= c[1])
        stubs.setdefault(cave[0], []).append((cave[0] + cave[3], offset, base, dst, free, length))
        cave[3] += size
    for cave_offset, size, original, used in cave_list:
        if cave_offset not in stubs:
            continue
        k, one = cave_offset + used, cave_offset + used + 4
        code = b"\xcd\x41"
        for at, site, base, dst, free, length in stubs[cave_offset]:
            mul_at = at + 8
            min_at = mul_at + length
            jmp_at = min_at + 8 + length
            code += (_vex_rip(0x10, free, 0, at, k) +                     # vmovss free, [16/9]
                     _vex_mem(0x59, free, free, base, aspect) +           # vmulss free, free, [aspect]
                     _vex_rip(0x5d, free, free, min_at, one) +            # vminss free, free, [1.0]
                     _vex_mem(0x59, dst, free, base, zoom) +              # vmulss dst, free, [zoom]
                     b"\xe9" + struct.pack("<i", site + length - (jmp_at + 5)))  # jmp back
            edits.append((site, _vex_mem(0x10, dst, 0, base, zoom),
                          b"\xe9" + struct.pack("<i", at - (site + 5)) + _NOPS[length - 5]))
        edits.insert(0, (cave_offset, original, code + struct.pack("<ff", 16 / 9, 1.0)))
    return pattern, edits


HOR_PLUS_PATCHES = {
    # Render camera update 0x2c9660 (perspective: infinite far 0x2c982e, finite 0x2c9ac7) and its
    # frustum planes 0x2ca4ca (Drake's Fortune 1.02).
    "eboot.bin": [("Drake's Fortune ultrawide: wider view instead of a cropped one", [
        hor_plus_variant("c5fa108b98020000 c5fa109b90020000 c5fa109394020000 c5f857c0 c5f82ec2 7251",
                         0x294, 0x290,
                         [(0x10, "rbx", 2, 0), (0x2a9, "rbx", 3, 0), (0xcac, "rdi", 0, 1)],
                         [(0x22, 0x51, bytes.fromhex("c5fa115dc0c5fa1155d0")),
                          (0x2c1, 0x63, bytes.fromhex("c5fa114db0c5fa115dc0"))]),
    ])],
    # 0x9dc750 (infinite far 0x9dc8fe, finite 0x9dcb55), frustum planes 0x9dd41d (Uncharted 2 1.02).
    "big2-ps4_Shipping.elf": [("Uncharted 2 ultrawide: wider view instead of a cropped one", [
        hor_plus_variant("c5fa10ab58020000 c5fa109b50020000 c5fa10a354020000 c5f857c0 c5f82ec4 0f82b6000000",
                         0x254, 0x250,
                         [(0x10, "rbx", 4, 0), (0x267, "rbx", 4, 0), (0xb2f, "rdi", 0, 1)],
                         [(0x26, 0xb6, bytes.fromhex("488b055d16b9004885c0"))]),
    ]),
    # The water's planar reflection camera 0xc902b4 is square (aspect 1.0, so the camera update
    # leaves it alone) with the render camera's own zoom: it kept the 16:9 horizontal view, and
    # past the 16:9 edges the water sampled outside the reflection (streaks). Its zoom load takes
    # the render camera's (rdx) aspect instead; the stub goes in the dead assert branch 0xc90485.
    ("Uncharted 2 ultrawide: water reflections as wide as the view", [
        hor_plus_variant("c5fa108a54020000 c5f82815 ????????", 0x254, 0x250,
                         [(0, "rdx", 1, 2)],
                         [(0x1d1, 0x35, bytes.fromhex("488b05ecda8d004885c0"))]),
    ]),
    # Shadows past the 16:9 area (Uncharted 2): the culling frustums of the sun, rain and local
    # (flashlight, lamp) shadows are built from the render camera's own zoom and aspect, in
    # copies of the frustum-plane code (view-frustum.cpp MakeSunShadowViewFrustum 0xcccc90,
    # MakeRainShadowViewFrustum 0xccd080, MakeLocalShadowViewFrustum 0xccda70). With the 16:9
    # zoom their side planes stopped at the 16:9 picture: casters past it were culled from the
    # shadow maps, and shadows at the sides came and went as the camera turned. Their zoom loads
    # get the same zoom * min(1, aspect * 16/9); each stub goes in that function's own numPlanes
    # assert (UNCHARTED_UW_SHADOWS=0: not applied).
    ("Uncharted 2 ultrawide: sun shadows as wide as the view", [
        hor_plus_variant("c5fa10b350020000 c5fa10ab54020000 c4637904ce00", 0x254, 0x250,
                         [(0x8, "rbx", 5, 0)],
                         [(-0x194, 0x35, bytes.fromhex("488b0573128a004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: rain shadows as wide as the view", [
        hor_plus_variant("c5fa10ab50020000 c5fa10a354020000 c4637904c500", 0x254, 0x250,
                         [(0x8, "rbx", 4, 3)],
                         [(-0x124, 0x35, bytes.fromhex("488b05a50e8a004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: local light shadows as wide as the view", [
        hor_plus_variant("c4c17a10942454020000 c4c17a10ac2450020000", 0x254, 0x250,
                         [(0x0, "r12", 2, 5)],
                         [(0x183, 0x35, bytes.fromhex("488b05c6018a004885c0"))]),
    ]),
    # The scene view's GPU constants (0xcfcb00, called from the scene view setup 0xc901c0): two more copies of the frustum code and the normalized side planes
    # written into a constant buffer, for the render camera (r12) and a second view (r13). Same
    # zoom (a square view keeps its own: min(1, 1.0 * 16/9) = 1). Stubs in the function's two
    # SetConstantBuffers asserts and two of the neighbouring function 0xd01020's
    # (UNCHARTED_UW_VIEW_CONSTANTS=0: not applied).
    ("Uncharted 2 ultrawide: view constants as wide as the view (1)", [
        hor_plus_variant("c4c17a109c2454020000 c4417a10bc2450020000", 0x254, 0x250,
                         [(0x0, "r12", 3, 5)],
                         [(0x10e7, 0x38, bytes.fromhex("488b0549fa86004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: view constants as wide as the view (2)", [
        hor_plus_variant("c4c17a10842454020000 c57a100d???????? c5b25ec8", 0x254, 0x250,
                         [(0x0, "r12", 0, 1)],
                         [(0x2a24, 0x38, bytes.fromhex("488b0519da86004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: view constants as wide as the view (3)", [
        hor_plus_variant("c4c17a109d54020000 c4c17a10a550020000", 0x254, 0x250,
                         [(0x0, "r13", 3, 4)],
                         [(0x28d4, 0x35, bytes.fromhex("488b0504c486004885c0"))]),
    ]),
    ("Uncharted 2 ultrawide: view constants as wide as the view (4)", [
        hor_plus_variant("c4c17a108554020000 c5da5ec8", 0x254, 0x250,
                         [(0x0, "r13", 0, 1)],
                         [(0x2189, 0x35, bytes.fromhex("488b0568c186004885c0"))]),
    ])],
    # Infinite far 0x11c1107 (both of its paths), finite 0x11c11e1 and 0x11c1406, frustum planes
    # 0x11c1f21 (Uncharted 3 1.02).
    "big3-ps4_Shipping.elf": [("Uncharted 3 ultrawide: wider view instead of a cropped one", [
        hor_plus_variant("80bb6902000000 c5fa10a358020000 c5fa108b50020000 c5fa109354020000 0f840a020000",
                         0x254, 0x250,
                         [(0x17, "rbx", 2, 3), (0xf1, "rbx", 2, 0), (0x316, "rbx", 1, 0), (0xe31, "rdi", 0, 1)],
                         [(0x10d, 0x9f, bytes.fromhex("488b05b415a3004885c0")),
                          (0x32e, 0x65, bytes.fromhex("488b059313a3004885c0"))]),
    ])],
}


def hor_plus_patches(executable):
    """HOR_PLUS_PATCHES of the game, without the groups switched off in the environment."""
    skip = []
    if os.environ.get("UNCHARTED_UW_SHADOWS", "1") == "0":
        skip.append("shadows as wide as the view")
    if os.environ.get("UNCHARTED_UW_VIEW_CONSTANTS", "1") == "0":
        skip.append("view constants as wide as the view")
    return [patch for patch in HOR_PLUS_PATCHES.get(executable, [])
            if not any(text in patch[0] for text in skip)]


PATCHES = {
    "eboot.bin": EBOOT_PATCHES,
    "big2-ps4_Shipping.elf": [GPU_WAIT_PATCH],
    "big3-ps4_Shipping.elf": [GPU_WAIT_PATCH],
}
EXECUTABLES = tuple(PATCHES)
# Exit code of sceSystemServiceLoadExec (runtime_services.c): the game asked for another
# executable of the Collection; GOW3_LOADEXEC_FILE holds its /app0 path and arguments.
LOADEXEC_EXIT = 30


def job_sleep_enabled():
    """JOB_SLEEP_PATCHES: on unless UNCHARTED_JOB_SLEEP=0 (launcher: Idle game threads -> Spin).
    Spinning job threads starve 4-6 core CPUs (GPU thread 5% of a core), heat laptops and, on a
    16-core desktop, used about 4 more cores for 2% fewer FPS."""
    return os.environ.get("UNCHARTED_JOB_SLEEP", "auto").strip().lower() not in ("0", "off", "no")


def apply_patches(image, patches):
    data = bytearray(image.read_bytes())
    for description, variants in patches:
        for variant in variants:
            pattern = variant[0]
            found = [match.start() for match in pattern_regex(pattern).finditer(data)]
            if len(found) != 1:
                continue
            if len(variant) == 2:  # several changes: all of them, or none if any bytes differ
                edits = [(found[0] + offset, original, replacement) for offset, original, replacement in variant[1]]
                if any(data[at:at + len(original)] != original for at, original, _ in edits):
                    continue
                for at, _, replacement in edits:
                    data[at:at + len(replacement)] = replacement
                print(f"Patched: {description} (image offset {found[0]:#x}, {len(edits)} changes)")
                break
            _, offset, original, replacement = variant
            at = found[0] + offset
            if data[at:at + len(original)] != original:
                raise SystemExit(f"Patch '{description}': unexpected bytes at {at:#x}")
            data[at:at + len(replacement)] = replacement
            print(f"Patched: {description} (image offset {at:#x})")
            break
        else:
            # An unknown game version: start without it.
            print(f"Patch '{description}' NOT applied: no variant matches this game version")
    image.write_bytes(data)


def game_executable():
    """The runtime as Uncharted.exe (Task Manager, Windows' graphics settings and overlays show
    this name), copied from the build output whenever that is newer."""
    built = ROOT / "gow3_pc/out/gow3-probe.exe"
    game_exe = ROOT / "gow3_pc/out/Uncharted.exe"
    if os.environ.get("UNCHARTED_TEST_PROBE_EXE") == "1":  # test: not under the game's name
        return built
    # Tests of another build (UNCHARTED_PROBE_EXE=<file>) run it as Uncharted.exe and leave the
    # deployed build alone; any difference to the deployed build (not only a newer one) is copied
    # back at the next normal start.
    if override := os.environ.get("UNCHARTED_PROBE_EXE"):
        built = Path(override)
    try:
        built_stat = built.stat()
        if not game_exe.exists() or built_stat.st_mtime != game_exe.stat().st_mtime or \
                built_stat.st_size != game_exe.stat().st_size:
            shutil.copy2(built, game_exe)
    except OSError as error:  # still running from a previous start: use the build output
        print(f"Using {built.name}: {error}")
        return built
    return game_exe


def game_window_icon(game):
    """The game's own icon (sce_sys/icon0.png) as a BMP for the window (SDL loads only BMP)."""
    icon = ROOT / "launcher/assets/window_icon.bmp"
    if icon.exists():
        return icon
    try:
        from PIL import Image
        icon.parent.mkdir(parents=True, exist_ok=True)
        Image.open(game / "sce_sys/icon0.png").convert("RGB").resize((64, 64)).save(icon)
        return icon
    except Exception as error:  # Pillow missing or no icon: the default icon stays
        print(f"No window icon: {error}")
        return None


# Render resolution (Drake's Fortune). The engine's display settings (constructors and the
# 1080p/720p mode switch, all within ~0x700 bytes) store the size in several fields: display
# and render size (+0x08/+0x18/+0x24/+0x38), aspect, tile grid (+0x40 = width/8, +0x44 =
# height/4, as the engine's own SetResolution computes them) and a larger size at +0x30/+0x34
# (2560x1216 at 1080p), scaled with the rest. UNCHARTED_RESOLUTION=<width>x<height>.
class ResolutionTooLarge(Exception):
    pass


# Drake's Fortune above what its GPU area holds: these 16:9 sizes in turn (width/8, height/4 whole).
U1_FALLBACK_SIZES = [(3200, 1800), (2880, 1620), (2560, 1440), (1920, 1080)]


def resolution_patch(data, width, height, move=True):
    import re
    # mov [rdi/rax+8],1920; mov [rdi/rax+0xc],1080 (1.00 starts with rdi, 1.02 with rax)
    anchor = pattern_regex("c7 ?? 08 80070000 c7 ?? 0c 38040000")
    match = anchor.search(data)
    if not match:
        raise SystemExit("Resolution patch: display settings code not found")
    first = match.start()
    start, end = first - 0x40, first + 0x800
    region = bytearray(data[start:end])
    stores = [
        (0x08, 1920, width), (0x18, 1920, width), (0x24, 1920, width), (0x38, 1920, width),
        (0x0c, 1080, height), (0x1c, 1080, height), (0x28, 1080, height), (0x3c, 1080, height),
        (0x40, 240, width // 8), (0x44, 270, height // 4),
        (0x30, 2560, 2560 * width // 1920), (0x34, 1216, 1216 * height // 1080),
    ]
    # Diagnostics: UNCHARTED_RES_KEEP=<hex field offsets> leaves those fields at 1080p.
    keep = {int(v, 16) for v in os.environ.get("UNCHARTED_RES_KEEP", "").replace(",", " ").split()}
    stores = [store for store in stores if store[0] not in keep]
    changed = 0
    for base in (0x47, 0x40):  # [rdi+disp] and [rax+disp]
        for disp, old, new in stores:
            old_bytes = bytes([0xc7, base, disp]) + old.to_bytes(4, "little")
            new_bytes = bytes([0xc7, base, disp]) + new.to_bytes(4, "little")
            changed += region.count(old_bytes)
            region[:] = region.replace(old_bytes, new_bytes)
    packed_old = bytes.fromhex("48b8") + (1920).to_bytes(4, "little") + (1080).to_bytes(4, "little")
    packed_new = bytes.fromhex("48b8") + width.to_bytes(4, "little") + height.to_bytes(4, "little")
    changed += region.count(packed_old)
    region[:] = region.replace(packed_old, packed_new)
    data[start:end] = region
    # The screen wipe of Uncharted 2 and 3 (menus, loading: black bands sweeping left to right)
    # is drawn in the 1280x720 UI space, but from the render size: its bands end at the render
    # height (a quarter short at 540p) and it crosses the render width plus 600. Its code
    # (lea rax,[display]; mov ecx,[rax+8]; mov eax,[rax+0xc]; vcvtsi2ss xmm0,xmm0,rax) takes the
    # PlayStation 4's 1920x1080 instead: mov ecx,1920; mov eax,1080.
    wipe = [match.start() for match in pattern_regex("48 8d 05 ???????? 8b 48 08 8b 40 0c c4 e1 fa 2a c0").finditer(data)]
    if len(wipe) == 1:
        data[wipe[0]:wipe[0] + 13] = bytes.fromhex("b9 80070000 b8 38040000 0f1f00")
        print(f"Patched: screen wipe in the PS4's 1920x1080 (image offset {wipe[0]:#x})")
    elif wipe:  # (Drake's Fortune has no such code)
        print(f"Screen wipe patch NOT applied: {len(wipe)} matches in this game version")
    # The render targets come from a fixed "Render Target Heap" (497 MiB at 1080p) inside the
    # 2988 MiB GPU (garlic) block of the engine's memory table; both grow with the pixel count
    # (+10%), and the direct memory reported to the game by the same amount (GOW3_DMEM_MB).
    mib = 1 << 20
    factor = max(1.0, width * height / (1920 * 1080))

    def table_entry(entry_id, parent, align, min_size):
        """The memory table entry (id, parent, size, alignment, 0) by structure; sizes differ per game."""
        found = []
        key = struct.pack("<II", entry_id, parent)
        at = data.find(key)
        while at >= 0:
            size, entry_align, pad = struct.unpack_from("<QQQ", data, at + 8)
            if entry_align == align and pad == 0 and size >= min_size:
                found.append((at, size))
            at = data.find(key, at + 1)
        if len(found) != 1:
            raise SystemExit(f"Resolution patch: memory table entry {entry_id:#x} found {len(found)} times")
        return found[0]

    # Memory areas (a table entry's second field is the context it lies in): entry 2 = CPU
    # ("onion", context 5) and entry 3 = GPU ("garlic", context 6). An area holds at most 4096 MiB
    # (MemoryArea keeps one physical page id per MiB in a 4096-entry array), so above ~1620p the
    # Render Target Heap moves to the CPU area; on PC both areas are the same memory. There it is
    # created before the level and animation heaps (0x82, 0x80) and pushes them up by its size,
    # past 2 GiB. Uncharted 2 and 3 don't mind; Drake's Fortune can't address them there: its
    # offset pointers (BP/Shared TGenericOffsetPtr.h GenerateOffset) cover [0, 2 GiB) (its own
    # image: rodata strings are encoded too) and [0x1000000000, +2 GiB) ("offsetTableIndex != -1"
    # assert at 4K, pointer 0x10e92cbd1c). Context 8 (on-demand direct memory) doesn't take the
    # heap (null heap at boot). So Drake's Fortune stops where the GPU area is full (move=False).
    onion_at, onion_old = table_entry(2, 2, 0x100000, 0x40000000)
    garlic_at, garlic_old = table_entry(3, 2, 0x100000, 0x80000000)
    heap_at, heap_old = table_entry(0x85, 6, 0x80, 0x10000000)       # Render Target Heap
    rt_at, rt_old = table_entry(0x84, 6, 0x80, 0x4000000)            # Render Target
    step = 64 * mib  # the engine's direct memory blocks are large-page multiples

    def layout(margin):
        def grown(old):
            return old + (-(-int(old * (factor * margin - 1)) // step) * step if factor > 1 else 0)

        heap_new, rt_new = grown(heap_old), grown(rt_old)
        onion_new, garlic_new, heap_context = onion_old, garlic_old + (heap_new - heap_old) + (rt_new - rt_old), 6
        if garlic_new >= area_limit:
            if not move:
                raise ResolutionTooLarge(f"{width}x{height}: the render targets don't fit the GPU area")
            # Area sizes must be whole MiB (MemoryArea::Initialize asserts): the moved heap is
            # rounded up, and the GPU area's rounding goes to the other render target heap.
            heap_new = -(-heap_new // step) * step
            onion_new = onion_old + heap_new
            rest = garlic_old - heap_old - rt_old
            garlic_new = -(-(rest + rt_new) // step) * step
            rt_new = garlic_new - rest
            heap_context = 5
        # Uncharted 3 1.02 maps its CPU area 100 MiB larger than the table says (4132 MiB at 4K
        # wrapped to 36 MiB, "HeapArena::Allocate: out of memory"): keep 128 MiB below the limit.
        if onion_new + headroom >= area_limit or garlic_new >= area_limit:
            raise ResolutionTooLarge(f"{width}x{height} needs more memory than the game's areas hold")
        return heap_new, rt_new, onion_new, garlic_new, heap_context

    # The render target heaps grow with the pixel count plus 10%, less when that doesn't fit.
    area_limit, headroom = 4096 * mib, 128 * mib
    for margin in (1.1, 1.05, 1.0):
        try:
            heap_new, rt_new, onion_new, garlic_new, heap_context = layout(margin)
            break
        except ResolutionTooLarge:
            if margin == 1.0:
                raise
    extra = (onion_new - onion_old) + (garlic_new - garlic_old)
    struct.pack_into("<IIQ", data, heap_at, 0x85, heap_context, heap_new)
    struct.pack_into("<Q", data, rt_at + 8, rt_new)
    struct.pack_into("<Q", data, onion_at + 8, onion_new)
    struct.pack_into("<Q", data, garlic_at + 8, garlic_new)
    # The memory table check (ValidateContext: lea rsi,[rbp-0x20 or -0x30]; movabs rdx,<total>)
    # expects the direct memory blocks (context 2) to add up to 4608 MiB.
    total_check = pattern_regex("488d75 ?? 48ba 0000002001000000")
    found = [m.start() for m in total_check.finditer(data)]
    if len(found) != 1:
        raise SystemExit(f"Resolution patch: memory total check found {len(found)} times")
    data[found[0] + 6:found[0] + 14] = (0x120000000 + extra).to_bytes(8, "little")
    print(f"Patched: render resolution {width}x{height} ({changed} stores; render target heaps "
          f"{heap_new // mib} + {rt_new // mib} MiB, the first in the {'CPU' if heap_context == 5 else 'GPU'} area)")
    return extra // mib


GAME_KEYS = {"eboot.bin": "u1", "big2-ps4_Shipping.elf": "u2", "big3-ps4_Shipping.elf": "u3"}
UPDATE_SUFFIXES = ("-patch", "-update", "_patch", "_update")


def find_update(game):
    """The game's update (e.g. 1.02, extracted from its PKG): UNCHARTED_UPDATE (a folder, or 0 for
    none), else a folder next to the game's named like it plus -patch/-update (any case), with or
    without uroot. An update folder holds eboot.bin and sce_sys/param.sfo."""
    chosen = os.environ.get("UNCHARTED_UPDATE", "")
    if chosen == "0":
        return None
    if chosen:
        candidates = [Path(chosen), Path(chosen) / "uroot"]
    else:
        title = game.parent if game.name.lower() == "uroot" else game
        names = {title.name.lower() + suffix for suffix in UPDATE_SUFFIXES}
        try:
            siblings = [entry for entry in title.parent.iterdir() if entry.name.lower() in names]
        except OSError:
            siblings = []
        candidates = [path for entry in siblings for path in (entry / "uroot", entry)]
    for candidate in candidates:
        if (candidate / "eboot.bin").is_file() and (candidate / "sce_sys/param.sfo").is_file():
            return candidate.resolve()
    if chosen:
        print(f"UNCHARTED_UPDATE={chosen} holds no eboot.bin and sce_sys/param.sfo; starting without it")
    return None


def sfo_value(game, key):
    try:
        sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
        from patches import game_sfo
        return game_sfo(game).get(key)
    except Exception:
        return None
    finally:
        sys.path.pop(0)


def game_version(game):
    """APP_VER of the game folder; 01.02 when its executables are 1.02 although param.sfo says
    otherwise (dumps with the update merged in that kept the 1.00 param.sfo). The check is Uncharted
    2's torch code as 1.02 compiled it (TORCH_FORWARD_PATCHES; 1.00 lays it out differently)."""
    version = sfo_value(game, "APP_VER")
    if version and version < "01.02":
        try:
            code = (game / "big2-ps4_Shipping.elf").read_bytes()
        except OSError:
            return version
        if pattern_regex(TORCH_FORWARD_PATCHES[0][1][0][0]).search(code):
            return "01.02"
    return version


def game_view(game, update):
    """The game with its update laid over it, as a folder of links (gow3_pc/scripts/game_update.py).
    It is made on the game's drive when possible: file links can't cross drives (else copies)."""
    sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
    try:
        from game_update import create_view
    finally:
        sys.path.pop(0)
    places = [ROOT / "analysis/views"]
    if Path(game.anchor).resolve() != Path(ROOT.anchor).resolve():
        places.insert(0, game.parent.parent / ".uncharted-views")
    for place in places:
        try:
            return create_view(game, update, place)
        except OSError as error:
            print(f"Game view in {place} failed: {error}")
    raise SystemExit("Could not combine the game and its update")


def boot_view(base, update):
    """For drives that hold no links (exFAT, FAT32): copies of the files the prepare scripts read
    (the executables, sce_module, sce_sys), the update's over the game's, in analysis/views
    (about 90 MB, copied again only when a source changes). The runtime reads the update over
    the game itself (--app0-overlay)."""
    chosen = {}
    executables = {name.lower() for name in GAME_KEYS}
    for layer in (base, update):  # the update last: its files win
        for entry in layer.iterdir():
            if entry.is_file() and entry.name.lower() in executables:
                chosen[entry.name.lower()] = (entry.name, entry)
            elif entry.is_dir() and entry.name.lower() in ("sce_module", "sce_sys"):
                for path in entry.rglob("*"):
                    if path.is_file():
                        relative = path.relative_to(layer)
                        chosen[str(relative).lower()] = (str(relative), path)
    title, version = sfo_value(update, "TITLE_ID") or "game", sfo_value(update, "APP_VER") or "update"
    view = ROOT / "analysis/views" / f"boot-{title}-{version}"
    for relative, source in chosen.values():
        dest = view / relative
        wanted = source.stat()
        try:
            current = dest.stat()
            # FAT32 keeps write times in 2-second steps.
            if current.st_size == wanted.st_size and abs(current.st_mtime - wanted.st_mtime) <= 2:
                continue
        except OSError:
            pass
        dest.parent.mkdir(parents=True, exist_ok=True)
        # copyfile, not copy2: a read-only source attribute would make the next refresh fail.
        shutil.copyfile(source, dest)
        os.utime(dest, ns=(wanted.st_atime_ns, wanted.st_mtime_ns))
    return view


def ini_value(path, key, default):
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.split("=", 1)[0].strip() == key and "=" in line:
                return line.split("=", 1)[1].strip()
    except OSError:
        pass
    return default


def run_once(args, executable, guest_args, loadexec):
    base = args.game.resolve()
    update = find_update(base)
    if not update:
        version = game_version(base)
        if version and version < "01.02":
            title = base.parent if base.name.lower() == "uroot" else base
            print(f"Game version {version} without update 1.02: the fixes written for 1.02 are skipped "
                  f"(the patches marked NOT applied). Extract update 1.02 next to the game folder as "
                  f"{title.name}-patch.")
        return run_game(args, base, executable, guest_args, loadexec)
    print(f"Update {sfo_value(update, 'APP_VER') or '?'}: {update}")
    try:
        if os.environ.get("UNCHARTED_TEST_OVERLAY") == "1":  # test: the drive-without-links path
            raise SystemExit("UNCHARTED_TEST_OVERLAY=1")
        view = game_view(base, update)
    except SystemExit as error:
        # exFAT/FAT32 drives (USB sticks) hold no links ([WinError 1] Incorrect function): the
        # runtime lays the update over the game itself, and the prepare scripts read copies of
        # the few files they need.
        print(f"{error}: the runtime lays the update over the game instead (drive without links)")
        try:
            boot = boot_view(base, update)
        except OSError as copy_error:
            print(f"Update files not copied ({copy_error}): starting the game without its update")
            return run_game(args, base, executable, guest_args, loadexec)
        return run_game(args, boot, executable, guest_args, loadexec, app0=base, overlays=[update])
    try:
        return run_game(args, view, executable, guest_args, loadexec)
    finally:
        sys.path.insert(0, str(ROOT / "gow3_pc/scripts"))
        try:
            from game_update import remove_view
            remove_view(view)
        except OSError as error:
            print(f"Game view {view} not removed: {error}")
        finally:
            sys.path.pop(0)


def hang_watchdog(process, log_path, dumps, seconds, reports):
    """Freeze report: the runtime logs a Frame stats line every 5 s while frames are shown. With
    none for `seconds` while the game runs, every thread's stack goes to hang-<time>.txt beside the
    log (tools/hang_report.py; small enough to attach to an issue; appended to `reports`) and, in
    the source tree or with UNCHARTED_HANG_DUMP=1, a minidump to dumps/ (hundreds of MB; the newest
    2 stay). Once per freeze; frames coming back re-arm it. Long-session freezes left no trace
    before: the log just stopped. (Not written into the log itself: the game writes it through its
    own file position, over anything appended meanwhile.)"""
    import re
    import hang_report
    position, last_frame, armed, seen, guest_base = 0, time.monotonic(), True, False, None
    while process.poll() is None:
        time.sleep(2)
        try:
            with open(log_path, "rb") as log:
                log.seek(position)
                chunk = log.read()
                position += len(chunk)
        except OSError:
            continue
        if guest_base is None and (found := re.search(rb"Guest image base: ([0-9a-fA-F]+)", chunk)):
            guest_base = int(found.group(1), 16)
        if b"Frame stats" in chunk:
            last_frame, armed, seen = time.monotonic(), True, True
            continue
        if not (seen and armed) or time.monotonic() - last_frame < seconds or process.poll() is not None:
            continue
        armed = False
        stamp = time.strftime("%Y%m%d-%H%M%S")
        lines = [f"Hang watchdog: no new frame for {seconds} s (game frozen) at {stamp}; stacks of every thread:"]
        started = time.monotonic()
        try:
            lines += hang_report.thread_report(process, guest_base)
        except Exception as error:  # the report must never take the launcher down
            lines.append(f"  (thread report failed: {error})")
        lines.append(f"(report took {time.monotonic() - started:.1f} s)")
        if os.environ.get("UNCHARTED_HANG_DUMP", "0" if (ROOT / "VERSION").exists() else "1") == "1":
            dumps.mkdir(parents=True, exist_ok=True)
            for old in sorted(dumps.glob("hang-*.dmp"))[:-1]:  # with the new one, the newest 2 stay
                old.unlink(missing_ok=True)
            dump = dumps / f"hang-{stamp}.dmp"
            try:
                lines.append(f"Hang watchdog: minidump {dump}" if hang_report.write_minidump(process, dump)
                             else "Hang watchdog: the minidump failed")
            except Exception as error:
                lines.append(f"Hang watchdog: the minidump failed ({error})")
        report = log_path.parent / f"hang-{stamp}.txt"
        for old in sorted(log_path.parent.glob("hang-*.txt"))[:-9]:  # with the new one, the newest 10 stay
            old.unlink(missing_ok=True)
        report.write_text("\n".join(lines) + "\n", encoding="utf-8")
        reports.append(report)
        print(f"Hang watchdog: the game froze; report {report}", flush=True)


def remove_pipeline_binaries(cache):
    """Deletes the pipeline binaries earlier versions saved (<cache>/<game>*/pipeline-binaries,
    ~2 MB each, 20 GB on a tester's PC) on a background thread while the game runs: they are
    no longer used (24 Oct 2026). A start that ends first leaves the rest to the next start."""
    folders = [path for path in cache.glob("*/pipeline-binaries") if path.is_dir()]
    if not folders:
        return

    def work():
        files = size = 0
        for folder in folders:
            for path in folder.iterdir():
                try:
                    size += path.stat().st_size
                    path.unlink()
                    files += 1
                except OSError:
                    pass
            try:
                folder.rmdir()
            except OSError:
                pass
        print(f"Pipeline binaries: {files} old files removed ({size / 2**30:.1f} GB), no longer used",
              flush=True)

    threading.Thread(target=work, name="remove-pipeline-binaries", daemon=True).start()


def instance_suffix():
    """UNCHARTED_INSTANCE=<n> (n >= 2; co-op tests, 25 Oct 2026): another game on this PC with its
    own folders (analysis/<game>-i<n>, analysis/uncharted-user-i<n>), so that two instances can
    play each other over the LAN transport (gow3_pc/src/runtime_net.c). Instance 1, the default,
    uses the normal folders."""
    value = os.environ.get("UNCHARTED_INSTANCE", "1").strip()
    return f"-i{int(value)}" if value.isdigit() and int(value) >= 2 else ""


# What a new instance's user folder starts with (copied once from the main one): settings, saves,
# trophies and the shader caches (an empty cache would compile for minutes).
INSTANCE_USER_SEED = ("probe.ini", "probe-u1.ini", "probe-u2.ini", "probe-u3.ini",
                      "disabled-optimizations.txt", "skip.txt", "savedata", "trophy", "cache", "shader")


def user_dir():
    """The user folder (saves, settings, caches) of this instance."""
    main_user = ROOT / "analysis/uncharted-user"
    suffix = instance_suffix()
    if not suffix:
        return main_user
    user = ROOT / f"analysis/uncharted-user{suffix}"
    if not user.exists():
        user.mkdir(parents=True)
        for name in INSTANCE_USER_SEED:
            source = main_user / name
            if source.is_dir():
                shutil.copytree(source, user / name)
            elif source.is_file():
                shutil.copy2(source, user / name)
        print(f"Instance {suffix[2:]}: new user folder {user} (settings, saves and caches copied)")
    return user


def net_mode():
    """LAN multiplayer (the launcher's Multiplayer page, Uncharted 2 only, 25 Oct 2026):
    UNCHARTED_NET_MODE=host|join starts Uncharted 2's own online game ("task online") with the
    LAN transport (gow3_pc/src/runtime_net.c) and the stand-ins for the removed session layer
    (runtime_coop.c's netinit). Off unless set."""
    mode = os.environ.get("UNCHARTED_NET_MODE", "").strip().lower()
    return mode if mode in ("host", "join") else ""


def net_game_env(env, user, roots):
    """The runtime settings of a net game; returns the /app0 overlay it needs (tools/net_overlay.py,
    built from the player's own game files; roots = the /app0 folders, topmost first).
      UNCHARTED_NET_ADDRESS  host: [address:]port to listen on (default 0.0.0.0:27000);
                             join: the host's address[:port] (port 27000 unless given)
      UNCHARTED_NET_GAME     host: the net game (*net-games* index; 114 = The Village co-op)
      UNCHARTED_NET_TITLE    host: its title in the LAN search
      UNCHARTED_NET_NAME     the player's name
      UNCHARTED_NET_SKIN     the player's character: an actor of the game (e.g. mp-hero-elena,
                             hero-default); every machine shows this player with it (GOW3_NET_SKIN,
                             runtime_net.c / runtime_coop_mp.c). Ignored when the game lacks it.
    A joiner gets its player id and the net game from the host's welcome: the boot commands wait
    for it (netwait) before setting up the net game (netinit auto). The per-game settings (levels,
    teams, default character) follow from the net game inside the runtime (runtime_coop_mp.c)."""
    from net_overlay import build_overlay, actor_exists
    skin = os.environ.get("UNCHARTED_NET_SKIN", "").strip().lower()
    if skin and not actor_exists(roots, skin):
        print(f"Net game: character {skin} is not in the game files: the default one is used")
        skin = ""
    overlay = build_overlay(roots, user / "net-overlay", [skin] if skin else [])
    if skin:
        env["GOW3_NET_SKIN"] = skin
    boot = user / "net-boot.txt"
    # mpmenu auto (runtime_coop_menu.c): the game's own results pages after a round (25 Oct, run f6)
    boot.write_text("at 1\nnetwait\nnetinit auto 64\nmpmenu auto\n", encoding="ascii")
    env["GOW3_COOP_BOOT"] = str(boot)
    # Multiplayer data the Collection does not ship (e.g. the bullets' MP explosion settings,
    # the first shot faulted) falls back to its single-player twin (runtime_coop_mp.c).
    env.setdefault("GOW3_COOP_MP_TWINS", "1")
    address = os.environ.get("UNCHARTED_NET_ADDRESS", "").strip()
    if name := os.environ.get("UNCHARTED_NET_NAME", "").strip():
        env["GOW3_NET_NAME"] = name[:31]
    if net_mode() == "host":
        env["GOW3_NET_HOST"] = address or "0.0.0.0:27000"
        env["GOW3_NET_GAME"] = os.environ.get("UNCHARTED_NET_GAME", "114").strip() or "114"
        env["GOW3_NET_TITLE"] = os.environ.get("UNCHARTED_NET_TITLE", "")[:47]
        print(f"Net game: hosting net game {env['GOW3_NET_GAME']} on {env['GOW3_NET_HOST']}")
    else:
        if address and ":" not in address:
            address += ":27000"
        env["GOW3_NET_JOIN"] = address or "127.0.0.1:27000"
        env["GOW3_NET_ID"] = "auto"
        print(f"Net game: joining {env['GOW3_NET_JOIN']}")
    return overlay


def run_game(args, game, executable, guest_args, loadexec, app0=None, overlays=()):
    """game: the folder the prepare scripts read; app0 (default: game) with overlays laid over
    it is what the game sees as /app0."""
    stem = "uncharted" if executable == "eboot.bin" else Path(executable).stem
    out = args.analysis_root / stem if args.analysis_root else ROOT / "analysis" / (stem + instance_suffix())
    user = args.user_dir
    user.mkdir(parents=True, exist_ok=True)
    scripts = ROOT / "gow3_pc/scripts"
    for script in ("prepare.py", "link_modules.py", "content_profile.py"):
        prepare_command = [sys.executable, str(scripts / script), str(game), "--out", str(out)]
        if script != "content_profile.py":
            prepare_command += ["--executable", executable]
        subprocess.run(prepare_command, check=True)
    patches = list(PATCHES[executable])
    if executable == "big2-ps4_Shipping.elf" and os.environ.get("UNCHARTED_TORCH_FORWARD", "1") != "0":
        patches.extend(TORCH_FORWARD_PATCHES)
    if os.environ.get("UNCHARTED_AIM_RANGE", "1") != "0":
        patches.extend(AIM_RANGE_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_FRAME_WAIT", "1") != "0":
        patches.extend(FRAME_BEGIN_WAIT_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_UNLOCK", "0") == "1":
        patches.extend(UNLOCK_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_UNLOCK_REWARDS", "0") == "1":
        patches.extend(REWARD_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_FOCUS_RESET", "1") != "0":
        patches.extend(FOCUS_NAN_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_CUE_PRETOUCH", "1") != "0":
        patches.extend(CUE_PRETOUCH_PATCHES.get(executable, []))
    if os.environ.get("UNCHARTED_RUN_GROUP_FIX", "1") != "0":
        patches.append(RUN_GROUP_PATCH)
    for cheat in cheat_names(executable):
        patches.extend(CHEAT_PATCHES.get(executable, {}).get(cheat, []))
    # Ultrawide render sizes (the launcher's aspect ratio): the wider view (HOR_PLUS_PATCHES).
    size = (os.environ.get(f"UNCHARTED_RESOLUTION_{GAME_KEYS[executable].upper()}")
            or os.environ.get("UNCHARTED_RESOLUTION", "")).lower()
    if "x" in size and os.environ.get("UNCHARTED_HOR_PLUS", "1") != "0":
        size_w, size_h = (int(value) for value in size.split("x"))
        if size_w / size_h > 16 / 9 + 0.01:
            patches.extend(hor_plus_patches(executable))
    if job_sleep_enabled():
        patches.extend(JOB_SLEEP_PATCHES)
        # Diagnostics: UNCHARTED_JOB_SLEEP_SPINS=<n> idle spins before a worker sleeps (1000).
        if spins := os.environ.get("UNCHARTED_JOB_SLEEP_SPINS", "").strip():
            patches.append(("idle job workers: spins before sleeping", [
                ("4d8d7001 807f5900 ???? 4981f8e8030000 7c??", 13, bytes.fromhex("e8030000"),
                 int(spins).to_bytes(4, "little")),
            ]))
    apply_patches(out / "boot-linked.bin", patches)
    # Settings per game (the launcher keeps them apart): the started game's own, also when the
    # Collection menu switches games.
    game_key = GAME_KEYS[executable]
    resolution = (os.environ.get(f"UNCHARTED_RESOLUTION_{game_key.upper()}")
                  or os.environ.get("UNCHARTED_RESOLUTION", "")).lower()
    width, height = (int(value) for value in resolution.split("x")) if resolution else (1920, 1080)
    render_size = (1920, 1080)
    if (width, height) != (1920, 1080):
        # (The earlier Uncharted 3 failures above 1080p - "Command context dcb full", a corrupted
        # pointer while loading - came with asynchronous readbacks, which also broke it at 1080p.)
        image = out / "boot-linked.bin"
        data = bytearray(image.read_bytes())
        # Drake's Fortune keeps its render targets in the GPU area (see resolution_patch): above
        # ~1620p it renders at the largest size that fits there, and the output is upscaled.
        move = executable != "eboot.bin"
        candidates = [(width, height)] + [size for size in U1_FALLBACK_SIZES if size[0] * size[1] < width * height]
        if abs(width / height - 16 / 9) > 0.01:
            # Ultrawide: smaller sizes of the same shape (width/16 and height/4 whole: Uncharted 3
            # asserts widths in steps of 16 in its post processing).
            candidates = [(width, height)] + [
                (int(width * scale) // 16 * 16, int(height * scale) // 4 * 4)
                for scale in (0.9, 0.8, 0.75, 2 / 3, 0.6, 0.5)]
        extra_mib = 0
        for candidate in candidates:
            patched = bytearray(data)
            try:
                extra_mib = resolution_patch(patched, *candidate, move=move)
            except ResolutionTooLarge as error:
                print(f"Render resolution {error}")
                continue
            if candidate != (width, height):
                print(f"Drake's Fortune renders at {candidate[0]}x{candidate[1]} instead of {width}x{height}")
            data, render_size = patched, candidate
            break
        image.write_bytes(data)
    else:
        extra_mib = 0
    env = os.environ.copy()
    wanted = cheat_names(executable)
    rewards = os.environ.get("UNCHARTED_UNLOCK_REWARDS", "0") == "1"
    if (wanted or rewards) and "GOW3_POKE" not in env:
        if (game_version(game) or "") >= "01.02":
            pokes = [CHEATS[executable][name] for name in wanted if name in CHEATS.get(executable, {})]
            if wanted:
                print(f"Cheats: {', '.join(n for n in wanted if n in CHEATS.get(executable, {}))}")
            if rewards and REWARD_POKES.get(executable):
                pokes += REWARD_POKES[executable]
                print("Unlock all rewards: on")
            if pokes:
                env["GOW3_POKE"] = ",".join(pokes)
        else:
            print("Cheats and unlocked rewards: need update 1.02 (not used)")
    # Measurement logging and live trigger files (frame/fault/wait statistics every 5 s, GPU
    # checkpoints, capture/readback/DMA triggers). On unless the launcher turns it off.
    diagnostics = env.get("UNCHARTED_DIAGNOSTICS", "1") != "0"
    msys = Path(env.get("MSYS2_ROOT", "C:/msys64"))
    env["PATH"] = str(msys / "clang64/bin") + os.pathsep + env.get("PATH", "")
    # UNCHARTED_TEST_GPU_USER_DIR: GPU caches in another folder (tests from an empty shader cache).
    env["GOW3_GPU_USER_DIR"] = env.get("UNCHARTED_TEST_GPU_USER_DIR") or str(user)
    settings = user / f"probe-{game_key}.ini"
    if not settings.exists() and (user / "probe.ini").exists():
        shutil.copy2(user / "probe.ini", settings)  # earlier versions shared one file
    # UNCHARTED_CONFIG_FILE: another settings file (tests that must not touch the player's).
    env["GOW3_CONFIG"] = os.environ.get("UNCHARTED_CONFIG_FILE") or str(settings)
    # Frame rates above 60 need the fast vblank (GOW3_VBLANK_HZ=0); an explicit one is kept.
    if "GOW3_VBLANK_HZ" not in os.environ:
        limit = ini_value(Path(env["GOW3_CONFIG"]), "fps_limit", "60")
        if limit.isdigit() and (int(limit) == 0 or int(limit) > 60):
            env["GOW3_VBLANK_HZ"] = "0"
    # Save profiles (launcher Advanced, GitHub #66, 24 Oct 2026): profile N plays as PS4 user N,
    # whose saves are in analysis\uncharted-user\savedata\N (profile 1: the folder of every earlier
    # version). GOW3_USER_ID set outside the launcher (tests) wins.
    profile = os.environ.get(f"UNCHARTED_SAVE_PROFILE_{game_key.upper()}", "1")
    env.setdefault("GOW3_USER_ID", profile if profile.isdigit() and 1 <= int(profile) <= 16 else "1")
    multithreaded = os.environ.get(f"UNCHARTED_MULTITHREADED_{game_key.upper()}")
    # Drake's Fortune: its weapons (and at times other objects) drew with another draw's look on
    # some frames with multithreaded rendering (flat grey guns, the player and GitHub #23, 18 Oct
    # 2026), and it gains nothing from it (4 cores: 101.8 vs 103.2 FPS). Off until that is fixed;
    # UNCHARTED_U1_MULTITHREADED_TEST=1 keeps the launcher's choice (tests).
    if executable == "eboot.bin" and multithreaded == "1" and \
            os.environ.get("UNCHARTED_U1_MULTITHREADED_TEST") != "1":
        print("Multithreaded rendering: off for Drake's Fortune (its weapons flicker with it)")
        multithreaded = "0"
    # Drake's Deception froze for good with multithreaded rendering (GitHub #51, Steam Deck, 19 Oct
    # 2026: a fence write on the Vulkan recording thread faulted while the GPU command thread
    # waited for that thread), fixed in the runtime the same day. 21 Oct 2026: the launcher's
    # choice counts again: no freeze in 15-20 min soaks (laptop, desktop walking), and in
    # a heavy Cartagena spot on the NVIDIA GPU it is the difference between 53.7 FPS with 11 stalls
    # over 100 ms (off) and 60 FPS with 1 (on), the single GPU command thread being the limit.
    if multithreaded is not None:
        env["GOW3_DRAW_PIPE"] = multithreaded
        env["GOW3_VK_RECORD_THREAD"] = multithreaded
    # Keyboard and mouse (launcher Controls tab, per game; explicit GOW3_* values win).
    for launcher_key, runtime_key in (("KEY_MAP", "GOW3_KEY_MAP"), ("MOUSE_MAP", "GOW3_MOUSE_MAP"),
                                      ("MOUSE", "GOW3_MOUSE"), ("MOUSE_SENS", "GOW3_MOUSE_SENS"),
                                      ("MOUSE_INVERT", "GOW3_MOUSE_INVERT"), ("PROMPTS", "GOW3_PROMPTS"),
                                      ("PAD_MAP", "GOW3_PAD_MAP")):
        value = os.environ.get(f"UNCHARTED_{launcher_key}_{game_key.upper()}")
        if value is not None and runtime_key not in os.environ:
            env[runtime_key] = value
    # GPU readbacks are asynchronous. Unprotected (the runtime default, 2) crashed Uncharted 3
    # within a minute in the Cartagena museum (guest reads of 0xffff... pointers, a compute ring
    # with a garbage indirect buffer) and let characters fly or NPCs fall from the sky (U2, U3,
    # 16 Oct, also on an RTX 5060 laptop). Waiting for every readback (0) was correct but cost U2
    # ~20 FPS. Protected (1) since 16 Oct: new GPU data stays read-protected until it lands,
    # recurring results (the same piece read back again, e.g. luminance) stay readable one frame
    # old as on the console, and pages where the CPU reads other data beside GPU bytes are left
    # readable (BufferCache::noisy_pages): U2 and U3 at 60 FPS, U3 crash-free for 6 minutes.
    # GOW3_ASYNC_READBACK in the environment overrides it (0 waits, 1 protected, 2 unprotected).
    if executable in ("big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"):
        env.setdefault("GOW3_ASYNC_READBACK", "1")
    # Uncharted 2 reads back data its 64-wide copy shader (cs 6a6422a9) writes on the GPU: read
    # back with the submission it arrived a frame late, and NPCs floated in the air while Nate's
    # feet sank into the floor (also in 0.1.1 and other ports). It now comes back at the frame's
    # end-of-pipe fence, which waits for it (16 Oct; precise readbacks fixed it too, at ~50 FPS).
    # 17 Oct: Uncharted 3 too (user: Nate's legs flicker a little, NPCs float a bit; the same
    # engine and copy shader). Two 90 s U3 runs at 1440p: stable, 56-58 FPS.
    # 19 Oct: Drake's Fortune too (GitHub #6: from the second shot on, enemy grenade launchers'
    # grenades exploded right in front of Drake; the shot started from a frame-old copy of what
    # this shader writes). The user, at the first grenade launcher of chapter 4-5: with it every
    # shot flies to its target, 60 FPS as without it. (Protected readbacks alone did not help.)
    if executable in ("eboot.bin", "big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"):
        env.setdefault("GOW3_COPY64_EOP_READBACK", "1")
    if executable in ("big2-ps4_Shipping.elf", "big3-ps4_Shipping.elf"):
        # Its aim depth of field reads the focus distance from a 1x1 render target the GPU writes;
        # never copied back it stayed 0 and everything behind Nate was blurred while aiming. Tiny
        # GPU-written images now come back with the frame's end-of-pipe fence (16 Oct).
        # 19 Oct: Uncharted 3 too (user: the same blur when aiming; its focus value read 0.0 at
        # 1440p, the same reader: global 0x1a5fdd0 + 0x35f8 in 1.02). Drake's Fortune aims sharp
        # without it, so it skips the copy and the wait.
        env.setdefault("GOW3_TINY_IMAGE_READBACK", "1")
    # 24 Oct (GitHub #140, Uncharted 3 washed out in sunny areas): Uncharted 3 draws its bloom into
    # mips 1 and 2 of a 7-level texture; their render targets became images of their own, so the
    # tonemap read a bloom mip that nothing wrote. GOW3_MIP_TARGETS=1 draws them into the texture's
    # mips (image_info.cpp, texture_cache.cpp). Uncharted 1 and 2 share this post-processing (their
    # bloom mips are lost the same way): to be turned on for them after a test there.
    if executable == "big3-ps4_Shipping.elf":
        env.setdefault("GOW3_MIP_TARGETS", "1")
    # The PS4's system language (launcher -> Advanced -> Language; runtime_services.c): each game's
    # main menu offers it next to English (Square). 1 = English (US).
    language = os.environ.get("UNCHARTED_LANGUAGE", "").strip()
    if language.isdigit():
        env.setdefault("GOW3_LANGUAGE", language)
    # Speakers (launcher -> Advanced): the layout the games mix their sound for (runtime_audio.c);
    # auto = the Windows output device's (stereo, 5.1 or 7.1), or 2, 6, 8.
    speakers = os.environ.get("UNCHARTED_SPEAKERS", "").strip().lower()
    if speakers in ("auto", "2", "6", "8"):
        env.setdefault("GOW3_AUDIO_SPEAKERS", speakers)
    # Temporal anti-aliasing (in-game menu -> Image: TAA, FSR 3.1/FSR 4 Native AA, DLAA) at the
    # render size, as in the reference Uncharted port (docs: analysis/STATUS.md): the camera comes
    # from the motion blur's camera velocity pass (shader f4b18264; all three games share the post
    # processing), and the anti-aliasing runs on the lit scene before the post processing starts
    # (compute aec539f0). UNCHARTED_TEMPORAL_AA=0 switches it off (no camera tracking at all).
    if os.environ.get("UNCHARTED_TEMPORAL_AA", "1") == "0":
        env["GOW3_UPSCALER"] = "none"
    else:
        env.setdefault("GOW3_VELOCITY_PS", "f4b18264")
        env.setdefault("GOW3_UPSCALE_BEFORE_CS", "aec539f0")
        # Full-screen passes that rebuild positions from the jittered scene depth get the jitter
        # too: the sun shadow pass, shared by the three games (its shadows flickered under TAA,
        # FSR and DLAA; Supermedo/uncharted_pc #261).
        env.setdefault("GOW3_JITTER_PS", "f614041e")
        env["GOW3_SCENE_SIZE"] = f"{render_size[0]}x{render_size[1]}"
        # FSR 4's model files (gow3_pc/tools/fetch_fsr4_assets.sh), when installed; DLAA needs
        # gow3_dlss.dll and nvngx_dlss.dll beside Uncharted.exe.
        fsr4 = ROOT / "gow3_pc/fsr4_shaders"
        if fsr4.is_dir():
            env.setdefault("GOW3_FSR4_DIR", str(fsr4))
        # The renderer's own default is FSR 3; here it starts off until it is picked.
        config = Path(env["GOW3_CONFIG"])
        if ini_value(config, "upscaler", None) is None:
            try:
                text = config.read_text(encoding="utf-8", errors="replace") if config.exists() else ""
                if text and not text.endswith("\n"):
                    text += "\n"
                config.write_text(text + "upscaler=off\n", encoding="utf-8")
            except OSError:
                env.setdefault("GOW3_UPSCALER", "off")
    if extra_mib:
        env["GOW3_DMEM_MB"] = str(5056 + extra_mib + 64)
    # The movie player reads its (1080p) movies with the render size: the decoder scales them to it
    # (gpu/shim/videodec2; else movies come out green and tiled above 1080p).
    if render_size != (1920, 1080):
        env["GOW3_MOVIE_SIZE"] = f"{render_size[0]}x{render_size[1]}"
    # Output resolution above the render resolution (UNCHARTED_OUTPUT_RES[_U1/_U2/_U3]=WxH): the
    # game draws at the render size and its finished frame is upscaled to the output size where
    # it copies the frame back into the display buffer (pixel shader 749bbec9, the same post
    # processing in all three games): by FSR 3.1, FSR 4 or DLSS, else TAA (at the render size) or
    # nothing, then FSR 1 when its setting is on or a bilinear stretch. The HUD and menus are
    # drawn after that at the output size (TemporalUpscaler::OnComposite). Needs the temporal
    # anti-aliasing's camera tracking.
    output = (os.environ.get(f"UNCHARTED_OUTPUT_RES_{game_key.upper()}")
              or os.environ.get("UNCHARTED_OUTPUT_RES", "")).lower()
    if "x" not in output and render_size != (width, height):
        # Drake's Fortune above the size it can render at: that size is upscaled to the one chosen,
        # with FSR 1 unless a temporal upscaler is chosen (the presets' FSR 1 to the screen before).
        output = f"{width}x{height}"
        env.setdefault("GOW3_FSR1", "1")
    if "x" in output and os.environ.get("UNCHARTED_TEMPORAL_AA", "1") != "0":
        out_w, out_h = (int(value) for value in output.split("x"))
        if out_w > render_size[0] or out_h > render_size[1]:
            env["GOW3_RENDER_RES"] = f"{render_size[0]}x{render_size[1]}"
            env["GOW3_OUTPUT_RES"] = f"{out_w}x{out_h}"
            env.setdefault("GOW3_UI_COMPOSITE_PS", "749bbec9")
            # The tonemap draw finishing the frame: Uncharted 3 draws world-anchored prompts
            # into its target after it, so the upscale runs at the draw after it.
            env.setdefault("GOW3_UI_TONEMAP_PS", "484cdbf5")
            print(f"Upscaling: {render_size[0]}x{render_size[1]} -> {out_w}x{out_h}")
    # HDR at the game's own size (all three games share the tone pass, gow3_pc/hdr):
    # the scene HDR and the float HUD chain live on the upscaling path, so the frame takes it at
    # 1:1 (GOW3_COMPOSITE_NATIVE). The anti-aliasing then runs on the finished frame, as when
    # upscaling. Read at start: switching HDR on in the game takes a restart for the scene.
    # UNCHARTED_HDR_NATIVE=0 keeps the native path (anti-aliasing before post processing, SDR scene).
    # The launcher cannot see the display: without HDR output (no HDR display) the renderer removes
    # the variables listed in GOW3_COMPOSITE_NATIVE_ENV and the session is the native one.
    if ("GOW3_RENDER_RES" not in env and game_key in ("u1", "u2", "u3")
            and os.environ.get("UNCHARTED_TEMPORAL_AA", "1") != "0"
            and os.environ.get("UNCHARTED_HDR_NATIVE", "1") != "0"
            and ini_value(Path(env["GOW3_CONFIG"]), "hdr", "0") == "1"):
        added = ["GOW3_RENDER_RES", "GOW3_OUTPUT_RES"]
        added += [name for name in ("GOW3_UI_COMPOSITE_PS", "GOW3_UI_TONEMAP_PS") if name not in env]
        env["GOW3_RENDER_RES"] = env["GOW3_OUTPUT_RES"] = f"{render_size[0]}x{render_size[1]}"
        env["GOW3_COMPOSITE_NATIVE"] = "1"
        env.setdefault("GOW3_UI_COMPOSITE_PS", "749bbec9")
        env.setdefault("GOW3_UI_TONEMAP_PS", "484cdbf5")
        env["GOW3_COMPOSITE_NATIVE_ENV"] = ",".join(added)
        print(f"HDR: {render_size[0]}x{render_size[1]} through the upscaling path at 1:1 "
              "(scene HDR, if the display offers HDR10)")
    pad_file=user / "probe-pad.txt"
    pad_file.write_text("",encoding="ascii")
    env["GOW3_PAD_FILE"] = str(pad_file)
    # Start with a serialized renderer while investigating Uncharted loading.
    # The base runtime enables these concurrency optimizations for GOW3.
    env.setdefault("GOW3_DRAW_PIPE", "0")
    env.setdefault("GOW3_VK_RECORD_THREAD", "0")
    env.setdefault("GOW3_NATIVE_TSC", "1")
    # Grey-world diagnosis: timestamped opens/reads of level packs and streams (boot.log).
    # env.setdefault("GOW3_FILE_TRACE", ".pak")  # pack read trace (grey-world diagnosis, done)
    # Frame captures on demand: write a pass count (e.g. 600) into this file; captures land in
    # analysis/uncharted-user/captures. Every draw's/dispatch's buffer addresses are listed.
    if diagnostics:
        env.setdefault("GOW3_CAPTURE_TRIGGER", str(user / "capture.trigger"))
        env.setdefault("GOW3_CAPTURE_ALL_BUFFERS", "1")
    # Grey-world diagnosis: GPU-side writes into the lighting record table the ground shader reads.
    # env.setdefault("GOW3_WATCH_RANGE", "1038e00000:1038f00000")  # done: writer was cs 2d215c3b
    # Dark-interior diagnosis: who writes the 256x8 light ramp LUT at 0x1134ab0000 (all zero).
    # (done: the LUT writer is ps 2d7325a4) Temple frame drops: who writes the two windows the CPU
    # keeps writing into after the GPU (each CPU write downloads with a full GPU wait).
    # (done: GDS-to-memory DMA copies and cs 6a6422a9; fixed by lazy writes and async readbacks)
    # if executable == "eboot.bin":
    #     env.setdefault("GOW3_WATCH_RANGE", "1036200000:1036300000,1061e00000:1061f00000")
    # Grey-world fix test: the ground's constants are written on the GPU (copy shader 2d215c3b) and
    # read from guest memory by the CPU-side SRT walker. Relaxed readbacks (1) never sync GPU writes
    # to guest memory unless the CPU later writes that page; Precise (2) reads them back on CPU reads.
    # env.setdefault("GOW3_READBACKS", "2")  # Precise hangs on Continue (18:35): not usable
    # Retest 14 Oct 21:5x after the capture-job and draw-prep deadlock fixes: hair/cobweb shadow
    # casters read light matrices that a GPU copy shader writes in the same submission.
    # Result: no visual change, ~3 FPS slower, no hang on Continue any more. Off.
    # U2 flashlight test (15 Oct): the light sits exactly at the camera (its source, a GPU-written
    # joint transform, may read as zero). The file analysis/uncharted-user/precise-readbacks turns
    # Precise readbacks on for the next start.
    if (user / "precise-readbacks").exists():
        env.setdefault("GOW3_READBACKS", "2")
        print("Readbacks: precise (analysis/uncharted-user/precise-readbacks)")
    # Test settings for the next start without code changes: KEY=VALUE lines (# comments) in
    # analysis/uncharted-user/test-env.txt; they override everything set above.
    test_env = user / "test-env.txt"
    if test_env.exists():
        for line in test_env.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, value = line.split("=", 1)
                env[key.strip()] = value.strip()
                print(f"Test setting: {key.strip()}={value.strip()}")
    # GPU-written constants back to guest memory for the SRT walker: write "auto"/"off" or
    # "<lo> <hi>" (hex range, one-off) into this file. GOW3_SYNC_GPU_CONSTANTS=1 starts it on.
    if diagnostics:
        env.setdefault("GOW3_READBACK_TRIGGER", str(user / "readback.trigger"))
    # Grey-world fix (confirmed 14 October 2026): GPU-written constants back to guest memory each submission.
    env.setdefault("GOW3_SYNC_GPU_CONSTANTS", "1")
    # Sound-effects diagnosis (gunfire/ambience silent, music fine): Ajm instances, failed jobs,
    # RIFF jobs and 5 s decoder totals in boot.log.
    # env.setdefault("GOW3_AUDIO_TRACE", "1")  # sound diagnosis (done: config read-only patch)
    # env.setdefault("GOW3_FILE_TRACE", "sound1/config")  # sound config opens/reads (done)
    # Sound config load diagnosis: open callback entry, path result, Fios open result, loader result.
    # env.setdefault("GOW3_EXEC_WATCH", "515cc9")  # config loader open result (0 = ok, 0x1e = EROFS)
    # Dark-interior diagnosis: CPU writes to the light ramp LUT texels (0x1134ab0000, all zero).
    # (done: 0 hits from startup; nothing writes the LUT on the CPU)
    # if executable == "eboot.bin":
    #     env.setdefault("GOW3_EXEC_WATCH", "w:1134ab0000,w:1134ab0100,w:1134ab4000,w:1134ab8000")
    # Dark interiors (14 Oct): with async shader compilation a draw whose pipeline is still
    # compiling is skipped, and U1's one-time light ramp LUT draw (ps 2d7325a4, 256x8) never ran,
    # so U1 compiled everything synchronously. Since 15 Oct the pipeline cache compiles draws to
    # small targets (<= 256x256, SmallTargetDraw) synchronously in every game, which covers it;
    # synchronous U1 froze for 1-2.4 s per new area on a Ryzen laptop (18 Oct). Back to the
    # async_shaders setting; UNCHARTED_U1_SYNC_SHADERS=1 restores synchronous compilation.
    if executable == "eboot.bin" and os.environ.get("UNCHARTED_U1_SYNC_SHADERS") == "1":
        env.setdefault("GOW3_ASYNC_SHADERS", "0")
    # Frame drops in the temple (45-57 FPS): GPU thread waits in DownloadMemory for CPU read faults
    # on GPU-modified pages. Every 5 s: fault counts/time, hottest guest instructions and windows.
    if diagnostics:
        env.setdefault("GOW3_FAULT_STATS", "1")
    # Frame rate diagnosis: flip rate/frame time spread every 5 s, compile time on the GPU thread,
    # and details of frames over 40 ms.
    if diagnostics:
        env.setdefault("GOW3_FRAME_STATS", "1")
    # DMA sync mode for the tiled lighting shader, switchable live: write "full", "off" or
    # "pointers <KiB>" into this file (the full sync costs ~10 ms/frame in the temple).
    if diagnostics:
        env.setdefault("GOW3_DMA_TRIGGER", str(user / "dma.trigger"))
    # DMA shaders (tiled lighting; MUBUF ADDR64 shadow cascade loads of hair/cobwebs/characters):
    # make 256 KiB windows at the guest addresses in their user data resident and current,
    # instead of uploading every CPU-dirty resident range before each such draw ("full").
    env.setdefault("GOW3_DMA_SYNC", "pointers 256")
    # GPU time per render pass/dispatch, printed every 5 s (frame-rate diagnosis). Uncharted's
    # frames are marked at the display pass after submitting the recorded work.
    # Off for play: it ends every render pass to time it, which costs frame time itself.
    # env.setdefault("GOW3_GPU_PROFILE", "1")
    # Which code waits for the GPU (scheduler waits by caller), reported with the frame stats.
    if diagnostics:
        env.setdefault("GOW3_PERF_DIAG", "1")
    # Uncharted's tiled lighting shader (cs 0x43b8ee5e) reads per-light records with an
    # index computed on the GPU (waterfall loop over v_readfirstlane); the CPU-side SRT walker
    # cannot pre-read those, so they are read from guest memory on the GPU.
    env.setdefault("GOW3_DIRECT_MEMORY_ACCESS", "1")
    # GPU checkpoints (which draw a lost device was on) cost ~2% of the GPU command thread in heavy
    # Uncharted 2 scenes: only on request (UNCHARTED_GPU_CHECKPOINTS=1), no longer with the
    # performance log every player has on (16 Oct).
    if os.environ.get("UNCHARTED_GPU_CHECKPOINTS") == "1":
        env.setdefault("GOW3_GPU_CHECKPOINTS", "1")
    # The game asserts when the GPU finishes a frame more than 2 s late (FrameBegin,
    # m_gfxEopTick); cached pipelines avoid recompiling every shader on each run.
    # Set GOW3_PIPELINE_CACHE=0 when testing shader recompiler changes.
    env.setdefault("GOW3_PIPELINE_CACHE", "1")
    # No pipeline binaries (24 Oct 2026). Each pipeline made from one kept its ~2 MB in memory:
    # Uncharted 2 used 15.5 GB of RAM with them and 7.0 GB without, and loaded its pipelines in
    # 40 s instead of 9.6 s (the graphics driver's own shader cache does the job, faster).
    # GOW3_PIPELINE_BINARIES=1 (test-env.txt) turns them back on.
    env.setdefault("GOW3_PIPELINE_BINARIES", "0")
    if env["GOW3_PIPELINE_BINARIES"] == "0":
        remove_pipeline_binaries(Path(env["GOW3_GPU_USER_DIR"]) / "cache")
    env["GOW3_TOGGLE_FILE"] = str(user / "disabled-optimizations.txt")
    env["GOW3_LOADEXEC_FILE"] = str(loadexec)
    if args.capture:
        capture=out / "captures" / str(time.time_ns())
        capture.mkdir(parents=True)
        trigger=capture / "trigger"
        env["GOW3_FRAME_DUMP_TRIGGER"]=str(trigger)
        env["GOW3_DUMP_DIR"]=str(capture)
        trigger.touch()
    else:
        # On-demand image dumps (no trigger file at start, so nothing is dumped until asked):
        # create analysis/uncharted/dumps/trigger, plus guest-images.trigger listing addresses.
        if diagnostics:
            dumps=out / "dumps"
            dumps.mkdir(parents=True, exist_ok=True)
            env.setdefault("GOW3_FRAME_DUMP_TRIGGER", str(dumps / "trigger"))
            env.setdefault("GOW3_DUMP_DIR", str(dumps))
    window_icon = game_window_icon(game)
    if window_icon:
        env.setdefault("GOW3_WINDOW_ICON", str(window_icon))
    command = [str(args.runtime_executable or game_executable()), str(out / "boot-linked.bin"),
               "--no-game-hooks", "--low-image", "--content-profile", str(out / "content.bin"),
               "--app0", str(app0 or game), "--user", str(user)]
    # gow3 24 Oct 2026, co-op research: UNCHARTED_COOP_OVERLAY=<dir> lays one more folder over
    # /app0, ahead of the others (e.g. a U2 package manifest without the missing co-op skins,
    # analysis/triage-1022/coop-work/make_coop_overlay.py). Off unless set.
    if coop_overlay := os.environ.get("UNCHARTED_COOP_OVERLAY"):
        command += ["--app0-overlay", coop_overlay]
    if executable == "big2-ps4_Shipping.elf" and net_mode():
        command += ["--app0-overlay", str(net_game_env(env, user, [*overlays, app0 or game]))]
    # UNCHARTED_MOD_DIR: folders laid over the game before the update (the first holding a file
    # wins), e.g. the multiplayer menu built by tools/uc2mp_menu.py.
    mods = [m for m in os.environ.get("UNCHARTED_MOD_DIR", "").split(os.pathsep) if m]
    for overlay in [*mods, *overlays]:
        command += ["--app0-overlay", str(overlay)]
    # guest_args is the game's complete argv; argv[0] is the program name.
    command += ["--guest-path", guest_args[0] if guest_args else "/app0/" + executable]
    for value in guest_args[1:]:
        command += ["--guest-arg", value]
    # Diagnostics: UNCHARTED_RENDERDOC=<RenderDoc folder or renderdoccmd.exe> loads RenderDoc's
    # Vulkan layer (its capture library) into the game: F12 captures a frame (RenderDoc's default
    # folder, %TEMP%\RenderDoc). Found through this process's environment only; nothing is
    # registered, and the game's log stays as it is.
    renderdoc = os.environ.get("UNCHARTED_RENDERDOC")
    if renderdoc:
        layer_dir = Path(renderdoc)
        layer_dir = layer_dir if layer_dir.is_dir() else layer_dir.parent
        if (layer_dir / "renderdoc.json").is_file():
            env["VK_ADD_LAYER_PATH"] = str(layer_dir) + (os.pathsep + env["VK_ADD_LAYER_PATH"]
                                                         if env.get("VK_ADD_LAYER_PATH") else "")
            env["VK_INSTANCE_LAYERS"] = "VK_LAYER_RENDERDOC_Capture"
            env["ENABLE_VULKAN_RENDERDOC_CAPTURE"] = "1"
            env["GOW3_PIPELINE_BINARIES"] = "0"
            print(f"RenderDoc: Vulkan layer from {layer_dir}")
    if not args.boot:
        command.append("--check-imports")
    if args.cpu_only:
        command.append("--cpu-only")
    name = "boot" if args.boot else "check-imports"
    if args.cpu_only:
        name += "-cpu"
    result = {"executable": executable, "command": command, "guest_execution_requested": args.boot, "timeout_seconds": args.seconds}
    if args.capture:
        result["capture_directory"]=str(capture)
    start = time.monotonic()
    with (out / (name + ".log")).open("wb") as log:
        process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        result["process_id"] = process.pid
        running = {"pid": process.pid, "runtime": str(Path(command[0]).resolve()),
                   "started_unix_ns": time.time_ns(), "active": True}
        (out / (name + ".running.json")).write_text(json.dumps(running, indent=2), encoding="utf-8")
        # Freeze watchdog (needs the Frame stats lines, i.e. diagnostics on). UNCHARTED_HANG_SECONDS
        # sets its wait (30 s), 0 turns it off.
        hang_seconds = int(os.environ.get("UNCHARTED_HANG_SECONDS", "30") or 0)
        hang_reports = []
        if os.name == "nt" and args.boot and hang_seconds > 0 and env.get("GOW3_FRAME_STATS") == "1":
            import threading
            threading.Thread(target=hang_watchdog, daemon=True,
                             args=(process, out / (name + ".log"), out / "dumps", hang_seconds, hang_reports)).start()
        try:
            if args.capture:
                while process.poll() is None:
                    remaining=args.seconds-(time.monotonic()-start) if args.seconds else 3
                    if remaining<=0:
                        raise subprocess.TimeoutExpired(command,args.seconds)
                    try:
                        process.wait(timeout=min(3,remaining))
                    except subprocess.TimeoutExpired:
                        trigger.touch()
                result["exit_code"]=process.returncode
            else:
                result["exit_code"] = process.wait(timeout=args.seconds or None)
            result["timed_out"] = False
        except subprocess.TimeoutExpired:
            process.kill()
            result["exit_code"] = process.wait()
            result["timed_out"] = True
    result["elapsed_seconds"] = round(time.monotonic() - start, 3)
    running.update(active=False, exit_code=result["exit_code"])
    (out / (name + ".running.json")).write_text(json.dumps(running, indent=2), encoding="utf-8")
    (out / (name + ".json")).write_text(json.dumps(result, indent=2), encoding="utf-8")
    # The game has ended: the log can take the freeze reports' names (the launcher looks there).
    if args.boot and hang_reports:
        with open(out / (name + ".log"), "ab") as log:
            log.write("".join(f"\nHang watchdog: the game froze; report {report}" for report in hang_reports)
                      .encode("utf-8") + b"\n")
    attempt = out / "attempts" / (name + "-" + str(time.time_ns()))
    attempt.mkdir(parents=True)
    for suffix in (".log", ".json"):
        shutil.copy2(out / (name + suffix), attempt / (name + suffix))
    for report in hang_reports if args.boot else []:
        shutil.copy2(report, attempt / report.name)
    print(json.dumps(result, indent=2))
    print(f"Log: {out / (name + '.log')}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, default=ROOT / "game/CUSA02344/uroot")
    parser.add_argument("--analysis-root", type=Path,
                        help="Separate build/log directory for isolated experiments")
    parser.add_argument("--user-dir", type=Path,
                        help="Separate player-data directory for isolated experiments")
    parser.add_argument("--runtime-executable", type=Path,
                        help="Run this runtime directly without replacing the installed executable")
    parser.add_argument("--boot", action="store_true", help="Execute guest code after preparation")
    parser.add_argument("--executable", choices=EXECUTABLES, default="eboot.bin")
    parser.add_argument("--guest-arg", action="append", default=None,
                        help="The game's argv, program name first (repeatable). A game started directly gets "
                             "the Collection menu's argv: eboot.bin ispartofuccollection")
    parser.add_argument("--collection-menu", action="store_true",
                        help="Start eboot.bin with the Collection's game selection instead of Drake's Fortune")
    parser.add_argument("--cpu-only", action="store_true")
    parser.add_argument("--capture", action="store_true", help="Request a renderer frame dump every three seconds")
    parser.add_argument("--seconds", type=int, default=30, help="External process time limit; 0 disables it")
    args = parser.parse_args()
    if args.runtime_executable:
        args.runtime_executable = args.runtime_executable.resolve()
        if not args.runtime_executable.is_file():
            parser.error("--runtime-executable must name an existing file")
    if args.seconds < 0:
        parser.error("--seconds must be nonnegative")
    user = args.user_dir.resolve() if args.user_dir else user_dir()
    args.user_dir = user
    if args.analysis_root:
        args.analysis_root = args.analysis_root.resolve()
    user.mkdir(parents=True, exist_ok=True)
    loadexec = user / "loadexec.txt"
    executable = args.executable
    guest_args = args.guest_arg
    if guest_args is None:
        # As the Collection menu starts each game: "ispartofuccollection" makes eboot.bin start
        # Drake's Fortune itself (without it, it shows the Collection's game selection, whose
        # cursor is on the last game played: the U1 card started U2/U3 after one press of Cross).
        if executable == "eboot.bin" and args.collection_menu:
            guest_args = ["/app0/eboot.bin"]
        else:
            guest_args = ["eboot.bin", "ispartofuccollection"]
            if executable == "big2-ps4_Shipping.elf" and net_mode():
                guest_args += ["task", "online"]  # Uncharted 2's own online game (net_mode)
    while True:
        loadexec.unlink(missing_ok=True)
        result = run_once(args, executable, guest_args, loadexec)
        if not args.boot or result["timed_out"] or result["exit_code"] != LOADEXEC_EXIT or not loadexec.exists():
            break
        # The Collection switched games (menu -> Uncharted 2/3, or back to the menu).
        lines = loadexec.read_text(encoding="utf-8", errors="replace").splitlines()
        target = Path(lines[0].strip()).name if lines else ""
        if target not in PATCHES:
            print(f"Game asked to start {lines[0] if lines else '(nothing)'}; not part of the Collection")
            break
        # LoadExec's argv is the new program's complete argv (the menu passes "eboot.bin" as argv[0]
        # for Uncharted 2/3; an extra leading argument makes them skip the menu and load nothing).
        executable, guest_args = target, lines[1:] or ["/app0/" + target]
        print(f"Collection: switching to {executable} {' '.join(guest_args)}")
    return 124 if result["timed_out"] else result["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
