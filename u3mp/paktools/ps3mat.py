"""PS3 MATERIAL_TABLE_1 (binary version 0x79) reader - just enough to recover each
material's name, name-hash and the textures bound by its first technique pass.

Layout (32-bit BE, page-relative pointers via fixups):
  +0x00 u32 version (0x79)       +0x04 u32 numTextures      +0x08 u32 numShaders(3)
  +0x0c u32 numMaterials         +0x10 u32 ? (1)            +0x14 pad
  +0x18 ptr u32 textureHash[numTextures]      (sid of short texture path, == VRAM_DESC hash)
  +0x1c ptr shader records (numShaders x 0x10: {0,0,ptr fragprog,ptr ucode})
  +0x20 ptr Material*[numMaterials]
  +0x24 ptr ... (technique names, "standard")  +0x28 ptr ...  +0x2c ptr end-of-data
  +0x30 ptr char* textureName[numTextures]    (short path "art/.../x.tga/HASH.ndb")
Material (0x20 bytes, name string stored just before it):
  +0x00 ptr name ("general/net/mode-buttons/customgame")
  +0x08 u32 0  +0x0c u32 numTechniqueSlots (5)
  +0x10 ptr params: 4 groups x 6 slot pointers (stride 0x18); group 0 = this material
  +0x14 u32 hash (shader/param set?)   +0x18 u32 sid(name)   +0x1c 0
Group-0 slot node: array of 0x20-byte pass records, pointer to a binding block at +0x10.
Binding block (0x90 bytes): +0x00 u32 flags(0 / 0x10000 / 0x20000) +0x04 u32 numTextures,
  RSX state words, texture refs at +0x50 (stride 0x10): {ptr &textureHash[i], u32 flags, 0, 0}.
"""
from ndpak import Pak, sid


def read_materials(p: Pak):
    assert p.platform == 'ps3'
    mts = p.items_of('MATERIAL_TABLE_1')
    if not mts:
        return [], []
    mt = mts[0]
    pg = mt.page
    b = mt.data_off
    ver = p.pu32(pg, b)
    if ver != 0x79:
        raise ValueError('unsupported MATERIAL_TABLE_1 version %#x' % ver)
    ntex, nsh, nmat = p.pu32(pg, b + 4), p.pu32(pg, b + 8), p.pu32(pg, b + 0xc)
    texh = p.resolve(pg, b + 0x18)[1]
    mats = p.resolve(pg, b + 0x20)[1]
    names = p.resolve(pg, b + 0x30)[1]
    textures = []
    for i in range(ntex):
        textures.append(dict(hash=p.pu32(pg, texh + 4 * i),
                             name=p.pstr(pg, p.resolve(pg, names + 4 * i)[1])))
    out = []
    for i in range(nmat):
        m = p.resolve(pg, mats + 4 * i)[1]
        name = p.pstr(pg, p.resolve(pg, m)[1])
        rec = dict(name=name, sid=p.pu32(pg, m + 0x18), hashA=p.pu32(pg, m + 0x14),
                   slots=p.pu32(pg, m + 0xc), textures=[], blockFlags=None)
        assert rec['sid'] == sid(name), name
        r = p.resolve(pg, m + 0x10)
        if r:
            node = p.resolve(pg, r[1])
            if node:
                bb = p.resolve(pg, node[1] + 0x10)
                if bb:
                    bb = bb[1]
                    rec['blockFlags'] = p.pu32(pg, bb)
                    n = p.pu32(pg, bb + 4)
                    for k in range(n):
                        rr = p.resolve(pg, bb + 0x50 + 0x10 * k)
                        if rr and texh <= rr[1] < texh + 4 * ntex:
                            rec['textures'].append((rr[1] - texh) // 4)
                        else:
                            rec['textures'].append(None)
        out.append(rec)
    return out, textures
