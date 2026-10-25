"""Isolated local-session backend experiment using verified PS4 1.00 leaf code.

This does not attach to the game, restore its menu, create a PSN room, or support
remote players. The room token is opaque test storage, never a game room object.
Run this native-code experiment in its own subprocess.
"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct

from uc2mp_entry_audit import PROFILES, file_offset

SOURCE_SHA256 = "31129a2fee4a6442dce0cc2c26ced3519763b1d28500ade47acf6a21d968404c"
# Exact leaf ranges independently inspected in Rizin/Capstone. No calls, stack
# accesses, RIP-relative operands, or external dependencies occur in these bytes.
LEAVES = {
    "reset": (0x2F70E0, 0x2F71CA),
    "by_id": (0x2F7230, 0x2F7262),
    "by_index": (0x2F7270, 0x2F728F),
    "selector_b_member": (0x2F7290, 0x2F72C8),
    "selector_a_member": (0x2F72E0, 0x2F7318),
    "set_selector_a": (0x2F7320, 0x2F7327),
    "set_selector_b": (0x2F7330, 0x2F7337),
    "count": (0x2F74E0, 0x2F7545),
}


def extract_leaves(binary):
    data = Path(binary).read_bytes()
    if hashlib.sha256(data).hexdigest() != SOURCE_SHA256:
        raise ValueError("Unsupported ELF SHA-256: only the verified PS4 1.00 image is accepted")
    if PROFILES[SOURCE_SHA256]["machine"] != 62 or data[:6] != b"\x7fELF\x02\x01":
        raise ValueError("Expected little-endian ELF64 x86-64")
    result = {}
    for name, (start, end) in LEAVES.items():
        offset = file_offset(data, start, "<")
        if file_offset(data, end - 1, "<") != offset + end - start - 1:
            raise ValueError("Non-contiguous leaf mapping")
        code = data[offset:offset + end - start]
        if code[-1:] != b"\xc3":
            raise ValueError("Leaf range does not end at RET")
        result[name] = code
    return result


class NativeLeaves:
    def __init__(self, binary):
        leaves = extract_leaves(binary)
        if os.name != "nt" or C.sizeof(C.c_void_p) != 8:
            raise RuntimeError("This experiment requires 64-bit Windows Python")
        self.kernel = C.WinDLL("kernel32", use_last_error=True)
        self.kernel.VirtualAlloc.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.c_uint32]
        self.kernel.VirtualAlloc.restype = C.c_void_p
        self.kernel.VirtualProtect.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32, C.POINTER(C.c_uint32)]
        self.kernel.VirtualProtect.restype = C.c_int
        self.kernel.VirtualFree.argtypes = [C.c_void_p, C.c_size_t, C.c_uint32]
        self.kernel.VirtualFree.restype = C.c_int
        self.kernel.GetCurrentProcess.restype = C.c_void_p
        self.kernel.FlushInstructionCache.argtypes = [C.c_void_p, C.c_void_p, C.c_size_t]
        self.kernel.FlushInstructionCache.restype = C.c_int
        self.address = self.kernel.VirtualAlloc(None, 4096, 0x3000, 0x04)  # RW
        if not self.address:
            raise C.WinError(C.get_last_error())
        self.functions = {}
        try:
            offset = 0
            for name, code in leaves.items():
                target = self.address + offset
                C.memmove(target, code, len(code))
                offset = (offset + len(code) + 15) & ~15
                # Windows args RCX/RDX -> SysV RDI/RSI. Preserve Windows
                # nonvolatile RDI/RSI and align RSP to 16 before CALL.
                thunk = bytes.fromhex("57564883ec084889cf4889d648b8") + struct.pack("<Q", target)
                thunk += bytes.fromhex("ffd04883c4085e5fc3")
                C.memmove(self.address + offset, thunk, len(thunk))
                self.functions[name] = C.CFUNCTYPE(C.c_uint64, C.c_void_p, C.c_uint64)(self.address + offset)
                offset = (offset + len(thunk) + 15) & ~15
            old = C.c_uint32()
            if not self.kernel.VirtualProtect(self.address, 4096, 0x20, C.byref(old)):  # RX
                raise C.WinError(C.get_last_error())
            if not self.kernel.FlushInstructionCache(self.kernel.GetCurrentProcess(), self.address, 4096):
                raise C.WinError(C.get_last_error())
        except BaseException:
            self.dispose()
            raise

    def call(self, name, address, argument=0):
        if not self.address:
            raise RuntimeError("Native backend disposed")
        return self.functions[name](address, argument)

    def dispose(self):
        self.functions.clear()
        if self.address:
            self.kernel.VirtualFree(self.address, 0, 0x8000)
            self.address = None


class LocalSession:
    CAPACITY = 10
    SIZE = 0xB10

    def __init__(self, binary):
        self.native = NativeLeaves(binary)
        self.buffer = C.create_string_buffer(self.SIZE)
        self.token = C.create_string_buffer(8)
        self.address = C.addressof(self.buffer)
        self.open = False
        self.native.call("reset", self.address)

    def _call(self, name, arg=0):
        return self.native.call(name, self.address, arg)

    @staticmethod
    def _id(value):
        if type(value) is not int or not 0 <= value <= 0xFFFFFFFF:
            raise ValueError("Member identifiers must be uint32 integers")
        return value

    def create_local(self, member_id=1):
        self._id(member_id)
        if self.open:
            raise RuntimeError("Close the existing session before creating another")
        self.close()
        struct.pack_into("<Q", self.buffer, 0x10, C.addressof(self.token))
        self.open = True
        self.add_member(member_id, member_id)
        self._call("set_selector_a", member_id)
        self._call("set_selector_b", member_id)
        return self.snapshot()

    def add_member(self, member_id, identity=None):
        """Add backend test data; this does not connect another player."""
        self._id(member_id)
        identity = member_id if identity is None else self._id(identity)
        if not self.open:
            raise RuntimeError("Session is closed")
        if self._call("by_id", member_id):
            raise ValueError("Duplicate member identifier")
        for index in range(self.CAPACITY):
            if not self._call("by_index", index):
                start = 0x20C + index * 0xD0
                struct.pack_into("<II", self.buffer, start + 0x28, member_id, identity)
                self.buffer[start + 0x25] = 1
                return index
        raise OverflowError("Session member capacity reached")

    def member(self, index):
        # Native index getter has no range check. Never call it on user input
        # before validating the index here.
        if type(index) is not int or not 0 <= index < self.CAPACITY:
            raise IndexError("Member index must be 0..9")
        ptr = self._call("by_index", index)
        if not ptr:
            return None
        expected = self.address + 0x20C + index * 0xD0
        if ptr != expected:
            raise RuntimeError("Native member pointer outside expected slot")
        member_id, identity = struct.unpack_from("<II", self.buffer, ptr - self.address + 0x28)
        return {"slot": index, "id": member_id, "identity": identity}

    def snapshot(self):
        members = [m for i in range(self.CAPACITY) if (m := self.member(i)) is not None]
        return {"scope": "isolated backend only; no in-game lobby or remote networking",
                "open": self.open, "members": members, "logical_count": len(members),
                # Original count returns 1 when the room pointer is NULL, even
                # after reset; expose this instead of misreporting engine state.
                "native_count": self._call("count"),
                "selector_a_slot": self._selected_slot("selector_a_member"),
                "selector_b_slot": self._selected_slot("selector_b_member")}

    def _selected_slot(self, name):
        ptr = self._call(name)
        if not ptr:
            return None
        offset = ptr - self.address - 0x20C
        if offset < 0 or offset % 0xD0 or offset // 0xD0 >= self.CAPACITY:
            raise RuntimeError("Invalid native selector result")
        return offset // 0xD0

    def close(self):
        self._call("reset")
        C.memset(self.address, 0, self.SIZE)
        self._call("reset")
        self.open = False

    def dispose(self):
        if self.native.address:
            self.close()
            self.native.dispose()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.dispose()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    with LocalSession(args.binary) as session:
        print(json.dumps(session.create_local(), indent=2))
