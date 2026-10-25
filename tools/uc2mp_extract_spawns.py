"""Extract verified multiplayer spawn metadata (every map the Collection kept); never creates or
injects a PS4 PAK.

The exact source hashes and native PS3 transform contracts are pinned. Runtime
consumers must still verify the loaded PC Level origin and readiness.
"""
import argparse
from pathlib import Path
import hashlib
import json
import math
import re
import struct

PS3_ELF_SHA256 = '13b11b420508086cf5fdf3f7f99a712352b7c2dc1172228c55be00aca0c80eeb'
KNOWN = {
    'village-multi': {
        'pc': '8644a35aeeb45dc862c6dad4306d5a06c564ddf18596e804fda80beaaab12be1',
        'ps3': 'e9a1a95ea6859e3b074fffb97a850cc62705490160e33545c73a4de609733808',
        'wrapper': '6ab6453c70238d4424a8bec46dda8aab1c2987de8b94f6d2d9557e55a1d6b382',
        'common_count': 248, 'players': 41, 'cameras': 10,
    },
    'warzone-multi': {
        'pc': 'd8a53c95d99169607adf7a56ed9b1bac2394728b78e979f683df0ce7eece7b07',
        'ps3': '2e9b9461b6a00e191abd9cf17a7a55dc9930e81beef21e97cb61e6e7a84db983',
        'wrapper': 'be7184ab479804680fc06c38738bc583ed2cab81f9bfccf9afac94b0e681c15a',
        'common_count': 272, 'players': 35, 'cameras': 9,
    },
    # Their PS3 <map>-ingame10.pak comes from the disc pak22.psarc (--ps3-pak22): the RPCS3
    # cache holds patched variants of these. Wrappers 1-<map>-pak-ingame10.pak: the cache.
    # ruins-multi is left out: its scenery (ruins-multi-wide) came with PS3 DLC and is not in
    # the Collection; the map renders empty.
    'gompa-multi': {
        'pc': '5e937098ab539174441b63d910240f8fb9bf5dbbd662e39fa730b0d5073570e3',
        'ps3': 'de7a3a904c8b5b38bd4fc01f713d61c3847899297eb890c76140c1afee825f20',
        'wrapper': '3ace8a8d7719b29af975c7642a7ae9e971b541b4b06d3ffeeb4475974e5dc585',
        'common_count': 352, 'players': 36, 'cameras': 17,
    },
    'ice-cave-multi': {
        'pc': '0d8e604a664045e0caa3f5d31e2f50edcf2c7308af74f2ef2d7a764dbbe7a5df',
        'ps3': '62fc0ef98bd1f95835033214f731b87589cf0865c025d16eaa5142c3c505aa06',
        'wrapper': '6ef3836b13f638c390d005f149c02cda84f0c4d630213a36c525ae82e943c24d',
        'common_count': 83, 'players': 33, 'cameras': 13,
    },
    'shambhala-multi': {
        'pc': 'cfdc45b00164d3cb807a9a683358f2343d257be112545c250c5cb1939cf6484b',
        'ps3': '048b2a7169d86cb86c95a8f92a8e2f4252639841af87ca95bd512b098d8a4cb7',
        'wrapper': '3908538c78232dcae7b49c28e45cff8d3dc550866d246e1c3741a9e36ebbea3f',
        'common_count': 424, 'players': 37, 'cameras': 10,
    },
    'the-fort-multi': {
        'pc': '16af42789232ea4e81b7b012ebd9bc8da03cab38774547bd83c40e36ded7683e',
        'ps3': 'd655f3afd14dfab6a105e49a940cec5549dea194b74ad3aa14d86e2dacb26b54',
        'wrapper': '32585cabb9865e733a85c74531d130216331f84c9499d267de42822d77b5b1af',
        'common_count': 126, 'players': 29, 'cameras': 6,
    },
    'train-wreck-multi': {
        'pc': '8e5437d42dfda371ac2b36c0663ed361c16337ab091f98ceab9e54290726d1d1',
        'ps3': '130f7b7c553d2fdd888e08cdd62d8ac9b4d4ae4a11edbda63bfeac5f56dda1dd',
        'wrapper': '67c00aa9ca94ea45b5dcbd3800ceba1f104d13631e3cb2c87f1dfc33e96ef37b',
        'common_count': 106, 'players': 34, 'cameras': 15,
    },
    'treasure-multi': {
        'pc': '7188660052345c52f883a00ab0fb55a6605ca9cfe25a7906fdd22bd9a87a295b',
        'ps3': '8355e3db31657b91f808a2af85e19276c557151b49ce114231f59198457b1d1d',
        'wrapper': '67757aeafc38fdd37557eb96b8f8cbb07e0fde3e6a21b4ba3effd1ec23efe41c',
        'common_count': 370, 'players': 31, 'cameras': 20,
    },
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sid(name):
    value = 0
    for byte in name.lower().encode('ascii'):
        value ^= byte << 24
        for _ in range(8):
            value = ((value << 1) ^ (0x04c11db7 if value & 0x80000000 else 0)) & 0xffffffff
    return value


def records(path, ps3=False, label=None):
    """path: a PAK file, or its bytes (then `label` names it in the result)."""
    data = path if isinstance(path, bytes) else path.read_bytes()
    endian, width, pointer_fmt = ('>', 4, '>I') if ps3 else ('<', 8, '<Q')
    require(struct.unpack_from(endian + 'I', data)[0] == 0xa79, 'Invalid PAK bounds, transform or header')
    count, table = struct.unpack_from(endian + 'II', data, 0x10)
    require(0 < count <= 64 and table + count * 12 <= len(data), 'Invalid PAK bounds, transform or header')
    result = []
    for chunk_index in range(count):
        base, size, flags = struct.unpack_from(endian + 'III', data, table + chunk_index * 12)
        require(base + size <= len(data), 'Invalid PAK bounds, transform or header')
        chunk = data[base:base + size]
        for tag in re.finditer(b'SPAWNER_GROUP\0', chunk):
            if tag.start() and chunk[tag.start() - 1] == ord('B'):
                continue
            for ref in re.finditer(re.escape(struct.pack(pointer_fmt, tag.start())), chunk):
                header = ref.start() - width
                magic_offset, array_offset, count_offset = (0xc, 0x1c, 0x20) if ps3 else (0x14, 0x28, 0x30)
                if header < 0 or header + count_offset + 4 > size:
                    continue
                if struct.unpack_from(endian + 'I', chunk, header + magic_offset)[0] != 0x12345:
                    continue
                array = struct.unpack_from(pointer_fmt, chunk, header + array_offset)[0]
                length = struct.unpack_from(endian + 'I', chunk, header + count_offset)[0]
                require(length < 20000 and array + length * width <= size, 'Invalid PAK bounds, transform or header')
                for index in range(length):
                    offset = struct.unpack_from(pointer_fmt, chunk, array + index * width)[0]
                    require(offset + 0x70 <= size, 'Invalid PAK bounds, transform or header')
                    name_pointer = struct.unpack_from(pointer_fmt, chunk, offset + 0x20)[0]
                    require(name_pointer < size, 'Invalid PAK bounds, transform or header')
                    end = chunk.find(b'\0', name_pointer)
                    require(name_pointer < end < min(name_pointer + 512, size), 'Invalid PAK bounds, transform or header')
                    name = chunk[name_pointer:end].decode('ascii')
                    key = struct.unpack_from(endian + 'I', chunk, offset + (0x24 if ps3 else 0x28))[0]
                    require(sid(name) == key, 'Spawner name SID mismatch')
                    pose = struct.unpack_from(endian + '8f', chunk, offset)
                    require(all(math.isfinite(value) for value in pose[:3] + pose[4:]), 'Invalid PAK bounds, transform or header')
                    require(max(abs(value) for value in pose[:3]) < 100000, 'Invalid PAK bounds, transform or header')
                    norm = sum(value * value for value in pose[4:])
                    require(0.95 < norm < 1.05, 'Invalid PAK bounds, transform or header')
                    result.append(dict(name=name, sid=f'{key:08x}', chunk=chunk_index,
                                       file_offset=hex(base + offset), position_xyz=pose[:3],
                                       raw_position_w=pose[3], quaternion=pose[4:],
                                       raw_parent_candidate=hex(struct.unpack_from(pointer_fmt, chunk, offset + (0x40 if ps3 else 0x60))[0])))
    return dict(path=label or str(path), sha256=hashlib.sha256(data).hexdigest(), bytes=len(data),
                endian='big' if ps3 else 'little', pointer_bytes=width, records=result)


def ps3_source(cache, name, expected, pak22):
    """The PS3 file whose hash is pinned: the cache copy, else the disc pak22.psarc entry."""
    path = cache / name
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == expected:
        return path
    require(pak22 is not None, f'{name}: cache copy differs from the audited asset; pass --ps3-pak22')
    from uc2mp_lightmaps import psarc_entry
    return psarc_entry(pak22, name)


def extract(base, patch, cache, ps3_elf, pak22=None):
    roots = ([Path(patch).resolve()] if patch else []) + [Path(base).resolve()]
    cache, ps3_elf = Path(cache).resolve(), Path(ps3_elf).resolve()
    elf = ps3_elf.read_bytes()
    require(hashlib.sha256(elf).hexdigest() == PS3_ELF_SHA256, 'PS3 ELF hash differs from audited transform contract')
    # The audited ELF PT_LOAD maps file offset + 0x10000 for these functions.
    require(elf[0x7c7c8 - 0x10000:0x7c7c8 - 0x10000 + 4] == bytes.fromhex('80840040'), 'PS3 parent read signature differs')
    require(elf[0xb25dc - 0x10000:0xb25dc - 0x10000 + 4] == bytes.fromhex('100004c4'), 'PS3 zero-origin signature differs')
    result = dict(schema=1, metadata_only=True, runtime_validated=False,
                  runtime_requirement='PC Level state7, flags80/40, and root translation+20 must be zero; otherwise reject these poses.',
                  transform_contract=dict(ps3_elf=str(ps3_elf), sha256=PS3_ELF_SHA256,
                      world_helper='0x7c7ac', parent_offset='0x40', owner_offset='0x3c',
                      owner_translation_offset='0x20', origin_initializer='0xb25d0',
                      source_position_w_discarded=True, pc_position_w=0), maps=[])
    for name, expected in KNOWN.items():
        relative = Path('u2data/build/main/pak22/ps4') / (name + '-ingame10.pak')
        pc_path = next((root / relative for root in roots if (root / relative).is_file()), None)
        require(pc_path is not None, f'Missing PC reference package: {name}')
        sources = dict(pc=pc_path, ps3=ps3_source(cache, name + '-ingame10.pak', expected['ps3'], pak22),
                       wrapper=cache / ('1-' + name + '-pak-ingame10.pak'))
        parsed = {}
        for kind, path in sources.items():
            data = path if isinstance(path, bytes) else path.read_bytes()
            require(hashlib.sha256(data).hexdigest() == expected[kind], f'{name} {kind} source hash differs; audit the new asset before use')
            parsed[kind] = records(path, kind != 'pc', f'pak22.psarc:{name}-ingame10.pak' if isinstance(path, bytes) else None)
        pc_by_name = {record['name']: record for record in parsed['pc']['records']}
        require(len(pc_by_name) == len(parsed['pc']['records']), 'Duplicate PC record names')
        common = []
        for record in parsed['ps3']['records']:
            other = pc_by_name.get(record['name'])
            if other is None:
                continue
            require(record['position_xyz'] == other['position_xyz'], 'PS3/PC coordinate mismatch')
            same_quat = record['quaternion'] == other['quaternion'] or all(a == -b for a, b in zip(record['quaternion'], other['quaternion']))
            require(same_quat, 'PS3/PC quaternion mismatch')
            require(record['raw_parent_candidate'] == other['raw_parent_candidate'] == '0x0', 'Non-root comparison anchor')
            common.append(record['name'])
        require(len(common) == expected['common_count'] and len(common) >= 5, 'Missing common coordinate anchors')
        entries = []
        for record in parsed['wrapper']['records']:
            if not record['name'].startswith(('online-spawner-', 'camera-entity-')):
                continue
            require(record['raw_parent_candidate'] == '0x0', 'Selected spawner has a parent; requires composition')
            entries.append(dict(name=record['name'], sid=record['sid'], source_offset=record['file_offset'],
                                parent_null=True, position_xyz=record['position_xyz'], quaternion=record['quaternion'],
                                pc_pose32=list(record['position_xyz']) + [0.0] + list(record['quaternion'])))
        require(sum(entry['name'].startswith('online-') for entry in entries) == expected['players'], 'Player count differs')
        require(sum(entry['name'].startswith('camera-') for entry in entries) == expected['cameras'], 'Camera count differs')
        require(len({entry['sid'] for entry in entries}) == len(entries), 'Duplicate selected SID')
        result['maps'].append(dict(map=name, exact_common_transform_count=len(common), comparison_anchor_names=common,
            sources={kind: {key: value for key, value in item.items() if key != 'records'} for kind, item in parsed.items()}, entries=entries))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', type=Path, required=True, help='PC base uroot')
    parser.add_argument('--patch', type=Path, help='PC update uroot')
    parser.add_argument('--ps3-cache', type=Path, required=True, help='RPCS3 DATA2 build/main/pak22 directory')
    parser.add_argument('--ps3-elf', type=Path, required=True)
    parser.add_argument('--ps3-pak22', type=Path, help='PS3 disc USRDIR/build/main/pak22.psarc (maps the cache patched)')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    destination = args.out.resolve()
    protected = [args.base.resolve(), args.ps3_cache.resolve(), args.ps3_elf.resolve()]
    if args.patch:
        protected.append(args.patch.resolve())
    require(all(destination != path and not destination.is_relative_to(path) for path in protected), 'Output must stay outside original assets')
    result = extract(args.base, args.patch, args.ps3_cache, args.ps3_elf, args.ps3_pak22)
    encoded = json.dumps(result, indent=2) + '\n'
    if destination.exists():
        require(destination.read_text(encoding='utf-8') == encoded, 'Different output already exists; choose a new output path')
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(encoded, encoding='utf-8')
    print(json.dumps(dict(output=str(destination), sha256=hashlib.sha256(encoded.encode()).hexdigest(),
                          maps=[dict(map=item['map'], entries=len(item['entries']), exact_anchors=item['exact_common_transform_count']) for item in result['maps']]), indent=2))


if __name__ == '__main__':
    main()
