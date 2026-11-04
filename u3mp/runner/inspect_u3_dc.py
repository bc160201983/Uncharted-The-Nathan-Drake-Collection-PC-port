#!/usr/bin/env python3
"""Read-only Uncharted 3 PS4 DC resource inspector. Writes JSON to stdout only.

Example:
  python inspect_u3_dc.py --game "D:/.../Collection" --image ".../big3-memory-image.bin"

The image must be a zero-based reconstructed PS4 U3 runtime image, not a SELF.
This inspects known header fields and the retained *custom-characters* schema;
it does not implement the game's complete relocation or resource loader.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import pathlib
import struct
import sys

CRC_TABLE_OFFSET = 0x13A6FF0
TARGET_NAME = '*custom-characters*'

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def u32(data, offset):
    return struct.unpack_from('<I', data, offset)[0]

def u64(data, offset):
    return struct.unpack_from('<Q', data, offset)[0]

def bounded(data, offset, size, label):
    if offset < 0 or size < 0 or offset + size > len(data):
        raise ValueError(f'{label}: range 0x{offset:x}+0x{size:x} exceeds file size 0x{len(data):x}')
    return data[offset:offset+size]

def read_c_string(data, offset):
    bounded(data, offset, 1, 'string')
    end = data.find(b'\0', offset)
    if end < 0:
        raise ValueError(f'unterminated string at 0x{offset:x}')
    return data[offset:end].decode('utf-8', 'strict')

def parse_dc(path, hash_name):
    data = path.read_bytes()
    bounded(data, 0, 0x18, 'DC header')
    if data[:4] != b'00CD' or u32(data, 4) != 1:
        raise ValueError(f'{path}: expected native PS4 DC00 version 1')
    section_count, count = u32(data, 0x10), u32(data, 0x14)
    if section_count != 1:
        raise ValueError(f'{path}: unsupported DC section count {section_count}')
    bounded(data, 0x18, count*16, 'symbol table')
    metadata_offset = u64(data, 8)
    if metadata_offset > len(data):
        raise ValueError('post-data metadata offset exceeds file')
    dci_path = path.with_suffix('.dci')
    source_names = {}
    dci_evidence = None
    if dci_path.is_file():
        dci = dci_path.read_bytes()
        import re
        match = re.search(rb'\(export\s+(.*?)\)\s*\)', dci, re.S)
        if match:
            for token in match[1].split():
                name = token.decode('utf-8')
                source_names.setdefault(hash_name(token), []).append(name)
        dci_evidence = {'path': str(dci_path.resolve()), 'size': len(dci), 'sha256': sha256(dci), 'export_name_count': sum(map(len, source_names.values()))}
    exports = []
    for index in range(count):
        offset = 0x18+index*16
        name_hash, type_hash, value = struct.unpack_from('<IIQ', data, offset)
        bounded(data, value, 1, 'export value')
        exports.append({'index':index, 'record_offset':hex(offset), 'name_hash':f'0x{name_hash:08x}', 'name_candidates':source_names.get(name_hash, []), 'type_hash':f'0x{type_hash:08x}', 'value_offset':hex(value)})
    if [int(e['name_hash'],16) for e in exports] != sorted(int(e['name_hash'],16) for e in exports):
        raise ValueError('expected sorted symbol table')
    return data, {'path': str(path.resolve()), 'size':len(data), 'sha256':sha256(data), 'header':{'magic':'00CD','version':1,'section_count':section_count,'symbol_count':count,'symbol_record_stride':16,'post_data_metadata_offset':hex(metadata_offset)}, 'dci':dci_evidence, 'exports':exports}

def inspect(game, image_path):
    image = image_path.read_bytes()
    table_bytes = bounded(image, CRC_TABLE_OFFSET, 1024, 'CRC table')
    table = struct.unpack('<256I', table_bytes)
    if table[0] != 0 or table[1] != 0x04C11DB7:
        raise ValueError('CRC table identity mismatch; use the zero-based U3 image')
    def hash_name(value):
        h=0
        for char in value:
            h=((h<<8)&0xffffffff)^table[(h>>24)^char]
        return h
    if hash_name(b'TaskManagerOnline') != 0xB7B3C967:
        raise ValueError('CRC known-control mismatch')
    base = game/'u3data'/'build'/'main'/'dc1'/'ps4'
    resources=[]
    net_data=None
    net_report=None
    for filename in ['net.bin','net-always-loaded.bin','menu.bin','ss-multiplayer.bin']:
        data, resource = parse_dc(base/filename, hash_name)
        resources.append(resource)
        if filename=='net.bin':
            net_data,net_report=data,resource
    target_hash=hash_name(TARGET_NAME.encode())
    targets=[e for e in net_report['exports'] if int(e['name_hash'],16)==target_hash]
    if len(targets)!=1:
        raise ValueError('expected one custom-character symbol')
    target=targets[0]
    if TARGET_NAME not in target['name_candidates']:
        raise ValueError('companion DCI did not verify character symbol name')
    value=int(target['value_offset'],16)
    bounded(net_data,value,16,'character array header')
    count,records_offset=u32(net_data,value),u64(net_data,value+8)
    bounded(net_data,records_offset,count*0x70,'character records')
    characters=[]
    for index in range(count):
        offset=records_offset+index*0x70
        name_offset=u64(net_data,offset)
        characters.append({'index':index,'record_offset':hex(offset),'name_offset':hex(name_offset),'name':read_c_string(net_data,name_offset),'id':f'0x{u32(net_data,offset+8):08x}','secondary_id':f'0x{u32(net_data,offset+0x10):08x}','category':u32(net_data,offset+0x14)})
    return {'schema_version':1,'read_only':True,'game':str(game.resolve()),'image':{'path':str(image_path.resolve()),'size':len(image),'sha256':sha256(image)},'hash_algorithm':{'kind':'CRC32-MSB','polynomial':'0x04c11db7','seed':'0x0','final_xor':'0x0','case_sensitive':True,'table_image_offset':hex(CRC_TABLE_OFFSET),'table_sha256':sha256(table_bytes),'known_control':{'name':'TaskManagerOnline','hash':'0xb7b3c967'}},'resources':resources,'custom_characters':{'name':TARGET_NAME,'name_hash':f'0x{target_hash:08x}','symbol_record_offset':target['record_offset'],'value_offset':hex(value),'count':count,'records_offset':hex(records_offset),'record_stride':0x70,'records':characters},'limitations':['Static file inspection only; no game execution.','Offset pointers are interpreted inside the file; complete relocation metadata remains undecoded.','DCI names are candidate names verified by hash, not independent proof against hash collisions.','Record fields beyond name, two identifiers and category remain uninterpreted.','Presence of these resources does not establish successful game loading or playable multiplayer.']}

def main():
    parser=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--game',required=True,type=pathlib.Path,help='PS4 Collection root containing u3data')
    parser.add_argument('--image',required=True,type=pathlib.Path,help='Zero-based reconstructed native PS4 U3 runtime image')
    args=parser.parse_args()
    try:
        result=inspect(args.game,args.image)
    except (OSError,ValueError,struct.error,UnicodeError) as error:
        print(f'Inspection failed: {error}',file=sys.stderr)
        return 2
    json.dump(result,sys.stdout,indent=2,ensure_ascii=False)
    print()
    return 0
if __name__=='__main__':
    raise SystemExit(main())
