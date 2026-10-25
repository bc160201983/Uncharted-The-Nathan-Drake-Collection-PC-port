"""Bounded read-only snapshot of verified UC2 1.02 level/camera/hero fields.

No guest calls, process suspension, memory writes, or fabricated transforms.
Supply the owned runtime PID and image base printed in its boot log.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import struct
from pathlib import Path
import re


def sid(name):
    value = 0
    for byte in name.lower().encode('ascii'):
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


class Reader:
    def __init__(self, pid):
        self.k = c.WinDLL('kernel32', use_last_error=True)
        self.k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
        self.k.OpenProcess.restype = w.HANDLE
        self.k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t, c.POINTER(c.c_size_t)]
        self.k.ReadProcessMemory.restype = w.BOOL
        self.k.CloseHandle.argtypes = [w.HANDLE]
        self.handle = self.k.OpenProcess(0x10 | 0x1000, False, pid)  # VM_READ + QUERY_LIMITED_INFORMATION
        if not self.handle:
            raise OSError(c.get_last_error(), 'OpenProcess read-only failed')

    def read(self, address, size):
        if not (0x10000 <= address < 0x800000000000 and 0 < size <= 4096):
            raise ValueError(f'Invalid bounded read {address:#x}+{size:#x}')
        buf = c.create_string_buffer(size)
        got = c.c_size_t()
        if not self.k.ReadProcessMemory(self.handle, address, buf, size, c.byref(got)) or got.value != size:
            raise OSError(c.get_last_error(), f'ReadProcessMemory {address:#x}+{size:#x}')
        return buf.raw

    def unpack(self, fmt, address):
        return struct.unpack(fmt, self.read(address, struct.calcsize(fmt)))[0]

    def player_state(self, obj):
        """Individually bounded reads; a missing subobject cannot hide other fields."""
        result = {'caveat': 'Live non-atomic reads. Raw fields do not establish animation semantics.'}

        def field(target, name, address, fmt, raw_hex=False):
            try:
                value = self.unpack(fmt, address)
                target[name] = hex(value) if raw_hex else value
                return value
            except (OSError, ValueError) as error:
                target[name] = {'error': str(error)}
                return None

        def pointer(name, offset):
            target = {}
            result[name] = target
            value = field(target, 'pointer', obj + offset, '<Q', True)
            if value is None:
                return target, None
            if not value:
                target['readable'] = False
                target['error'] = 'Null pointer; no dereference performed'
                return target, None
            try:
                self.read(value, 1)
                target['readable'] = True
                target['pointer_read_stable'] = self.unpack('<Q', obj + offset) == value
                if not target['pointer_read_stable']:
                    target['error'] = 'Pointer changed while validating; child reads skipped'
                    return target, None
            except (OSError, ValueError) as error:
                target['readable'] = False
                target['error'] = str(error)
                return target, None
            return target, value

        field(result, 'flags_u16_at_34', obj + 0x34, '<H', True)
        context, address = pointer('time_context_at_70', 0x70)
        if address is not None:
            field(context, 'frame_u32_at_0c', address + 0x0c, '<I')
            bits = field(context, 'dt_raw_u32_at_14', address + 0x14, '<I', True)
            if bits is not None:
                context['dt_float32_at_14'] = struct.unpack('<f', struct.pack('<I', bits))[0]
        machine, address = pointer('state_machine_at_50', 0x50)
        if address is not None:
            current = field(machine, 'current_pointer_at_00', address, '<Q', True)
            field(machine, 'next_pointer_at_08', address + 8, '<Q', True)
            if current:
                descriptor = field(machine, 'current_descriptor_at_10', current + 0x10, '<Q', True)
                if descriptor:
                    for name, offset in (('class_name', 0), ('state_name', 8)):
                        value = field(machine, name + '_pointer', descriptor + offset, '<Q', True)
                        if value:
                            try:
                                machine[name] = self.read(value, 64).split(b'\0')[0].decode('ascii', errors='replace')
                            except (OSError, ValueError) as error:
                                machine[name] = {'error': str(error)}
                    field(machine, 'constructor_pointer_at_10', descriptor + 0x10, '<Q', True)
                    field(machine, 'state_sid_at_18', descriptor + 0x18, '<I', True)
        pointer('skeleton_at_188', 0x188)
        control, address = pointer('anim_control_at_190', 0x190)
        if address is not None:
            for offset in (8, 0x130, 0x148):
                field(control, f'raw_u64_at_{offset:03x}', address + offset, '<Q', True)
        return result

    def handle_snapshot(self, address, camera=False):
        initial = self.read(address, 12)
        slot, generation = struct.unpack('<QI', initial)
        result = dict(handle=f'{address:#x}', slot=f'{slot:#x}', generation=generation, valid=False)
        if not slot or not generation:
            return result
        obj = self.unpack('<Q', slot)
        result['object'] = f'{obj:#x}'
        if not obj:
            return result
        actual = self.unpack('<I', obj + 0x44)
        result['object_generation'] = actual
        if actual != generation:
            return result
        result['vtable'] = hex(self.unpack('<Q', obj))
        result['process_type_sid'] = f'{self.unpack("<I", obj + 0x30):08x}'
        if camera:
            result['position4'] = struct.unpack('<4f', self.read(obj + 0x370, 16))
            result['quaternion4'] = struct.unpack('<4f', self.read(obj + 0x380, 16))
        elif result['process_type_sid'] == 'a5a63e9b':
            result['player_state'] = self.player_state(obj)
            # 994080..994206 consumes the descriptor's world transform,
            # resolves/caches its parent at +f0/+100, then writes the relative
            # transform at +110/+120. Do not call either setter while probing.
            transform = self.read(obj + 0xf0, 64)
            values = struct.unpack('<16f', transform)
            result['local_position4'] = values[8:12]
            result['local_quaternion4'] = values[12:16]
            result['cached_parent_position4'] = values[:4]
            result['cached_parent_quaternion4'] = values[4:8]
            result['transform_read_stable'] = self.read(obj + 0xf0, 64) == transform
            result['transform_caveat'] = ('Native relative transform; cached parent may lag. '
                                          'No world-position inference or guest call.')
        result['valid'] = (self.read(address, 12) == initial and self.unpack('<Q', slot) == obj
                           and self.unpack('<I', obj + 0x44) == generation)
        return result

    def level(self, manager, name):
        key = sid(name)
        buckets = self.unpack('<Q', manager + 0x10)
        count = self.unpack('<i', manager + 0x4c)
        result = dict(name=name, sid=f'{key:08x}', buckets=hex(buckets), bucket_count=count, found=False)
        if not 1 <= count <= 65536:
            return result
        hashed = 0x811c9dc5
        for byte in struct.pack('<I', key):
            hashed = ((hashed ^ byte) * 0x1000193) & 0xffffffff
        node = self.unpack('<Q', buckets + 8 * (hashed % count))
        sentinel = self.unpack('<Q', buckets + 8 * count)
        seen = set()
        for _ in range(64):
            if not node or node == sentinel or node in seen:
                return result
            seen.add(node)
            before = self.read(node, 32)
            next_node, _, node_key, _, level = struct.unpack('<QQIIQ', before)
            if node_key == key and level:
                flags = self.unpack('<H', level + 0x18)
                state = self.unpack('<I', level + 0x30)
                result.update(found=True, node=hex(node), level=hex(level), flags=hex(flags), state=state,
                              ready_predicate=state == 7 and bool(flags & 0x80),
                              specification=hex(self.unpack('<Q', level + 0x908)),
                              node_stable=self.read(node, 32) == before)
                return result
            node = next_node
        result['chain_limit'] = True
        return result

    def hero_assets(self, base):
        # 97533f..975427: native autoSpawn checks this exact 350-slot table.
        table = self.unpack('<Q', base + 0x1241418 + 0x38)
        wanted = {sid(name): name for name in ('hero-default', 'proto-mesh')}
        entries = []
        for index in range(350):
            slot = table + index * 0x2c0
            kind = self.unpack('<I', slot + 0x20)
            if kind != 12:
                continue
            keys = (self.unpack('<I', slot + 0x144), self.unpack('<I', slot + 0xc0))
            for key in keys:
                if key in wanted:
                    entries.append(dict(name=wanted[key], index=index, type=kind, address=hex(slot)))
        skin = self.read(base + 0x1164450 + 8, 32).split(b'\0')[0].decode('ascii', errors='replace')
        return dict(table=hex(table), requested_real_player_skin=skin, cached=entries,
                    caveat='Current registry contents; not proof of contents at spawn time.')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--pid', type=int)
    p.add_argument('--image-base', type=lambda s: int(s, 0))
    p.add_argument('--instance', help='Resolve PID/image base from the isolated launcher metadata')
    p.add_argument('--level', action='append', default=[])
    p.add_argument('--out')
    a = p.parse_args()
    if a.instance:
        if a.pid is not None or a.image_base is not None or not re.fullmatch(r'[A-Za-z0-9_-]{1,32}',a.instance):
            p.error('--instance requires a valid isolated name and excludes --pid/--image-base')
        folder=Path(__file__).resolve().parents[1]/'work/local-lobby/instances'/a.instance
        diagnostic=folder/'analysis102/big2-ps4_Shipping'
        metadata=json.loads((diagnostic/'boot.running.json').read_text(encoding='utf-8'))
        expected=(folder/'runtime/Uncharted.exe').resolve()
        if not metadata.get('active') or Path(metadata['runtime']).resolve()!=expected:
            p.error('Instance metadata is not an active isolated runtime')
        match=re.search(r'Guest image base: ([0-9a-fA-F]+)',(diagnostic/'boot.log').read_text(errors='replace'))
        if not match: p.error('Runtime has not published its image base yet')
        a.pid=int(metadata['pid']); a.image_base=int(match[1],16)
    elif a.pid is None or a.image_base is None:
        p.error('Provide --instance, or both --pid and --image-base')
    reader = Reader(a.pid)
    try:
        base = a.image_base
        if reader.read(base + 0x969320, 11) != bytes.fromhex('554889e54156534883ec60'):
            raise ValueError('UC2 1.02 autoSpawnPlayer prologue mismatch; no field reads performed')
        if reader.read(base + 0x994080, 16) != bytes.fromhex('488b46104885c00f8479010000c5f828'):
            raise ValueError('UC2 1.02 actor-transform setter mismatch; no field reads performed')
        result = dict(pid=a.pid, image_base=hex(base), signature_verified=True, read_only=True,
                      caveat='Live best-effort snapshot; engine is not suspended, no ownership claimed.')
        for label, address, is_camera in [('camera', 0x1114208, True), ('hero', 0x115bd20, False)]:
            try:
                result[label] = reader.handle_snapshot(base + address, is_camera)
            except (OSError, ValueError) as e:
                result[label] = {'error': str(e)}
        try:
            result['hero_assets'] = reader.hero_assets(base)
            result['auto_spawn_requested'] = bool(reader.unpack('<B', base + 0x1059640 + 0x4818))
        except (OSError, ValueError) as e:
            result['hero_assets'] = {'error': str(e)}
        result['levels'] = []
        for name in a.level or ['stage-multi']:
            try:
                result['levels'].append(reader.level(base + 0x1130390, name))
            except (OSError, ValueError) as e:
                result['levels'].append(dict(name=name, error=str(e)))
        text = json.dumps(result, indent=2)
        if a.out:
            Path(a.out).write_text(text, encoding='utf-8')
        print(text)
    finally:
        reader.k.CloseHandle(reader.handle)


if __name__ == '__main__':
    main()
