def crc32_string(chk, s):
    for c in s.encode()+b'\0':
        v=c<<24
        for _ in range(8):
            fb=0x04c11db7 if ((v^chk)&0x80000000) else 0
            chk=((chk<<1)&0xffffffff)^fb; v=(v<<1)&0xffffffff
    return chk
def pid(name): return crc32_string(0,name)&0x7fffffff
