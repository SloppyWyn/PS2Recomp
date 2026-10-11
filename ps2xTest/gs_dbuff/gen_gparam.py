# Usage: python gen_gparam.py  -> the kZbufCases / kDrawCases initialisers in gs_gparam_tests.cpp.
# Independent model of libgraph (SLPS-25887 copies), from the decompiles of
# 0x10cb70 (sceGszbufaddr) and 0x10cc38 (sceGsSetDefDrawEnv); sceGsSetDefDrawEnv2 is the same
# code with the context-2 register addresses (FRAME_2 0x4d, ZBUF_2 0x4f, XYOFFSET_2 0x19,
# SCISSOR_2 0x41, TEST_2 0x48).
M = (1 << 64) - 1


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def zbufaddr(psm, w, h, g):
    # g = sceGsGParam shorts (interlace, omode, ffmode, version); libgraph tests the first
    # doubleword & 0xffff0000ffff == 1, i.e. interlace == 1 and ffmode == 0.
    il, om, ff, ver = g
    w, h = s16(w), s16(h)
    wp = (w + 63 if w + 63 >= 0 else w + 126) >> 6
    if psm & 2:
        hp = (h + 63 if h + 63 >= 0 else h + 126) >> 6
    else:
        hp = (h + 31 if h + 31 >= 0 else h + 62) >> 5
    p = wp * hp
    v = (p * (0x10000 if (il == 1 and ff == 0) else 0x20000)) & 0xffffffff
    if v & 0x80000000:
        v -= 1 << 32
    return v >> 16


def draw(psm, w, h, zt, zpsm, g, old, ctx2):
    regs = (0x4d, 0x4f, 0x19, 0x41, 0x48) if ctx2 else (0x4c, 0x4e, 0x18, 0x40, 0x47)
    q = [0] * 16
    q[1] = regs[0]; q[0] = ((((w + 0x3f) >> 6) & 0x3f) << 16) | ((psm & 0xf) << 24)
    q[3] = regs[1]; q[2] = (zbufaddr(psm, w, h, g) & M) | ((zpsm & 0xf) << 24) | ((1 << 32) if zt == 0 else 0)
    q[5] = regs[2]; q[4] = (((0x800 - (w >> 1)) * 0x10) | ((0x800 - (h >> 1)) << 0x24)) & M
    q[7] = regs[3]; q[6] = (((w - 1) << 16) | ((h - 1) << 48)) & M
    q[9] = 0x1a; q[8] = old | 1
    q[11] = 0x46; q[10] = old | 1
    q[13] = 0x45; q[12] = (old | 1) if psm & 2 else (old & ~1 & M)
    q[15] = regs[4]; q[14] = 0x30000 if zt == 0 else (((zt & 3) << 17) | 0x10000)
    return q


FRAME = (1, 2, 1, 0)
FIELD = (1, 2, 0, 0)
PROG = (0, 2, 0, 0)

zcases = [
    # name, psm, w, h, gparam
    ("ct32_640x224_frame", 0, 640, 224, FRAME),
    ("ct16_640x448_frame", 2, 640, 448, FRAME),
    ("ct32_640x448_field", 0, 640, 448, FIELD),
    ("ct16s_512x448_field", 0xa, 512, 448, FIELD),
    ("ct32_320x240_progressive", 0, 320, 240, PROG),
]
print('// kZbufCases')
for n, psm, w, h, g in zcases:
    print('{"%s", %d, %d, %d, {%d, %d, %d}, %d},' % (n, psm, w, h, g[0], g[1], g[2], zbufaddr(psm, w, h, g)))

dcases = [
    # name, ctx2, psm, w, h, ztest, zpsm, gparam, fill
    ("env1_ct32_640x224_z24_ztest1_frame", 0, 0, 640, 224, 1, 0x31, FRAME, 0x5a),
    ("env1_ct16_640x448_z16_ztest0_field", 0, 2, 640, 448, 0, 0x32, FIELD, 0x00),
    ("env2_ct32_640x224_z24_ztest2_frame", 1, 0, 640, 224, 2, 0x31, FRAME, 0x5a),
    ("env2_ct16s_512x448_z32_ztest3_field", 1, 0xa, 512, 448, 3, 0x30, FIELD, 0x5a),
]
print('// kDrawCases')
for n, c2, psm, w, h, zt, zpsm, g, fill in dcases:
    old = int.from_bytes(bytes([fill]) * 8, 'little')
    q = draw(psm, w, h, zt, zpsm, g, old, c2)
    print('{"%s", %s, %d, %d, %d, %d, 0x%x, {%d, %d, %d}, 0x%02x, {' % (
        n, 'true' if c2 else 'false', psm, w, h, zt, zpsm, g[0], g[1], g[2], fill))
    for i in range(0, 16, 4):
        print('    ' + ', '.join('0x%016xull' % v for v in q[i:i + 4]) + ',')
    print('}},')
