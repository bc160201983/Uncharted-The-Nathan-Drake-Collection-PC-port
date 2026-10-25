"""Read-only Rizin audit of the original UC2 multiplayer entry callbacks.

Known reconstructed ELF identities only: addresses are not portable between builds.
Does not run the game or patch its code. Retains raw Rizin output and warnings.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


PROFILES = {
    "31129a2fee4a6442dce0cc2c26ced3519763b1d28500ade47acf6a21d968404c": {
        "build": "PS4 1.00", "machine": 62, "endian": "<",
        "registration": 0x2bd4b1, "callback": 0x2bb1a0,
        "command": "b 2048;pd 12 @ 0x2bd4b1;pd 4 @ 0x2bb1a0",
    },
    "09546f611636dd642fceab51469714e1a7ffd529fa3a009525659902882bad18": {
        "build": "PS4 1.02", "machine": 62, "endian": "<",
        "registration": 0x5ed9db, "callback": 0x5eb160,
        "command": "b 2048;pd 9 @ 0x5ed9db;pd 4 @ 0x5eb160",
    },
    "7ad3b270aac6ec920064eccc12d1a35ebb4b99806ca22b148367e71f8ddb5e09": {
        "build": "PS3 BCUS98123 1.00", "machine": 21, "endian": ">",
        "registration": 0x439394, "callback": 0x43b320,
        "command": "b 4096;pd 10 @ 0x439394;pd 45 @ 0x43b320;pd 190 @ 0x438d98",
    },
}


def file_offset(data, address, endian):
    """Map a single file-backed byte, never synthesize missing ELF memory."""
    phoff = struct.unpack_from(endian + "Q", data, 32)[0]
    entsize, count = struct.unpack_from(endian + "HH", data, 54)
    if entsize != 56:
        raise ValueError("Unexpected ELF64 program header size")
    matches = []
    for index in range(count):
        kind, flags, offset, va, pa, size, memsize, align = struct.unpack_from(
            endian + "II6Q", data, phoff + index * entsize)
        if kind == 1 and va <= address < va + size:
            matches.append(offset + address - va)
    if len(matches) != 1 or matches[0] >= len(data):
        raise ValueError(f"Ambiguous or absent file mapping: {address:#x}")
    return matches[0]


def audit(binary, rizin, output):
    data = binary.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest not in PROFILES:
        raise ValueError("Unknown ELF SHA-256; refusing to apply another build's addresses")
    profile = PROFILES[digest]
    endian = profile["endian"]
    if data[:5] != b"\x7fELF\x02" or struct.unpack_from(endian + "H", data, 18)[0] != profile["machine"]:
        raise ValueError("Unexpected ELF class or architecture")
    offset = file_offset(data, profile["callback"], endian)
    callback_bytes = data[offset:offset + 16].hex()
    command = [str(rizin.resolve()), "-N", "-q", "-e", "scr.color=0",
               "-e", "asm.lines=false", "-c", profile["command"], str(binary.resolve())]
    result = subprocess.run(command, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    listing = result.stdout.decode("utf-8", "replace")
    if f"0x{profile['callback']:08x}" not in listing:
        raise ValueError("Rizin did not return the requested callback address")
    output.mkdir(parents=True, exist_ok=False)
    (output / "rizin.txt").write_bytes(result.stdout)
    (output / "rizin-warnings.txt").write_bytes(result.stderr)
    record = {
        "source": str(binary.resolve()), "sha256": digest, "profile": profile,
        "callback_file_offset": offset, "callback_first_16_bytes": callback_bytes,
        "x86_callback_is_immediate_ret": data[offset] == 0xc3 if profile["machine"] == 62 else None,
        "command": command, "rizin_sha256": hashlib.sha256(rizin.read_bytes()).hexdigest(),
        "limitations": ["Static inspection only, no runtime initialization or multiplayer tested.",
                        "Rizin ELF loader warnings retained; callback byte independently mapped through PT_LOAD.",
                        "PS3 callback uses PowerPC and a TOC/OPD; cannot be copied into x86-64 code.",
                        "Immediate return proves this entry is empty, not that every multiplayer function is absent."],
    }
    (output / "evidence.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"build": profile["build"], "callback": hex(profile["callback"]),
                      "empty_callback": record["x86_callback_is_immediate_ret"], "output": str(output)}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--rizin", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path, help="New evidence directory")
    args = parser.parse_args()
    audit(args.binary, args.rizin, args.out)
