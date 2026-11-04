def sid(s):
    c=0
    for ch in s.encode():
        c^=ch<<24
        for _ in range(8):
            c=((c<<1)^0x04C11DB7)&0xffffffff if c&0x80000000 else (c<<1)&0xffffffff
    return c
