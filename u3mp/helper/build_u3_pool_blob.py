"""Compile and link only the new freestanding helper, without console binaries."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

def link_object(raw):
    if raw[:6] != b'\x7fELF\x02\x01':
        raise ValueError('expected little-endian ELF64 object')
    shoff=struct.unpack_from('<Q',raw,40)[0]
    shsize,count,namesindex=struct.unpack_from('<HHH',raw,58)
    if shsize!=64: raise ValueError('unsupported section headers')
    sections=[struct.unpack_from('<IIQQQQIIQQ',raw,shoff+i*64) for i in range(count)]
    def data(i):
        s=sections[i];return raw[s[4]:s[4]+s[5]]
    names=data(namesindex)
    def name(i):
        n=sections[i][0];return names[n:names.index(0,n)].decode()
    allocated=[i for i,s in enumerate(sections) if s[2]&2 and s[5]]
    allocated.sort(key=lambda i:(name(i)!='.text.entry',i))
    if not allocated or name(allocated[0])!='.text.entry':raise ValueError('missing entry section')
    blob=bytearray();offsets={}
    for i in allocated:
        s=sections[i]
        if s[1]!=1 or s[2]&1: raise ValueError('unexpected writable/NOBITS allocated section')
        align=max(1,s[8]);blob.extend(b'\0'*((-len(blob))%align))
        offsets[i]=len(blob);blob.extend(data(i))
    records=[]
    for i,s in enumerate(sections):
        if s[1]!=4:continue
        if s[7] not in offsets:raise ValueError('relocation to unplaced section')
        symbols=data(s[6]);strings=data(sections[s[6]][6])
        for p in range(0,s[5],24):
            off,info,addend=struct.unpack_from('<QQq',data(i),p)
            index,kind=info>>32,info&0xffffffff
            symname,_,_,section,value,_=struct.unpack_from('<IBBHQQ',symbols,index*24)
            if section not in offsets or kind not in (2,4):raise ValueError('unsupported or external relocation')
            target=offsets[section]+value;place=offsets[s[7]]+off
            struct.pack_into('<i',blob,place,target+addend-place)
            records.append(dict(offset=place,target=target,kind=kind,name=strings[symname:strings.index(0,symname)].decode()))
    return bytes(blob),records

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--out',type=Path,required=True)
    p.add_argument('--clang',type=Path,default=Path('C:/msys64/clang64/bin/clang.exe'))
    p.add_argument('--source',type=Path,help='Alternative freestanding helper with .text.entry')
    a=p.parse_args();source=a.source or Path(__file__).with_name('u3_pool_init.c')
    a.out.parent.mkdir(parents=True,exist_ok=True);obj=a.out.with_suffix('.o')
    command=[str(a.clang),'--target=x86_64-unknown-elf','-std=c11','-O2','-ffreestanding',
             '-fno-builtin','-fPIC','-mno-red-zone','-mno-sse','-fno-stack-protector',
             '-fno-asynchronous-unwind-tables','-fno-jump-tables','-c',str(source),'-o',str(obj)]
    subprocess.run(command,check=True)
    blob,relocations=link_object(obj.read_bytes());a.out.write_bytes(blob)
    manifest=dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  blob_sha256=hashlib.sha256(blob).hexdigest(),bytes=len(blob),
                  entry_offset=0,relocations=relocations,command=command)
    a.out.with_suffix('.json').write_text(json.dumps(manifest,indent=2));print(json.dumps(manifest,indent=2))
if __name__=='__main__':main()
