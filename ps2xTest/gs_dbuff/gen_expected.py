# Usage: python gen_expected.py  -> the kCases initialisers in gs_dbuff_tests.cpp.
# Independent model of libgraph (SLPS-25887 copies), from the decompiles of
# 0x10cf28 (SetDefDBuff), 0x10c738 (SetDefDispEnv), 0x10cc38 (SetDefDrawEnv),
# 0x10cb70 (zbufaddr), 0x10ce20 (SetDefClear).
M=(1<<64)-1
def zbufaddr(psm,w,h,g):
    il,om,ff=g
    wp=(w+63)>>6
    hp=((h+63)>>6) if psm&2 else ((h+31)>>5)
    p=wp*hp
    return p*(1 if (il==1 and ff==0) else 2)
def disp(psm,w,h,g):
    il,om,ff=g
    smode2 = 2 if il==0 else (3 if ff else 1)
    dispfb=((psm&0xf)<<15)|((((w+63)>>6)&0x3f)<<9)
    mult=(w+0x9ff)//w
    dy = 0x32 if il==1 else 0x19
    dh = 2*h-1 if (il==1 and ff) else h-1   # FRAME mode: DH counts frame lines
    display=(0x27c&0xfff)|((dy&0xfff)<<12)|((mult-1)<<23)|((mult*w-1)<<32)|(dh<<44)
    return [0x66,smode2,dispfb,display,0]
def draw(psm,w,h,zt,zpsm,g,old):
    q=[0]*16
    q[1]=0x4c; q[0]=((((w+0x3f)>>6)&0x3f)<<16)|((psm&0xf)<<24)
    q[3]=0x4e; q[2]=zbufaddr(psm,w,h,g)|((zpsm&0xf)<<24)|((1<<32) if zt==0 else 0)
    q[5]=0x18; q[4]=((0x800-(w>>1))*0x10)|((0x800-(h>>1))<<0x24)
    q[7]=0x40; q[6]=((w-1)<<16)|((h-1)<<48)
    q[9]=0x1a; q[8]=old|1
    q[11]=0x46; q[10]=old|1
    q[13]=0x45; q[12]=(old|1) if psm&2 else (old&~1)
    q[15]=0x47; q[14]=0x30000 if zt==0 else (((zt&3)<<17)|0x10000)
    return q
def clear(zt,x,y,w,h):
    q=[0]*12
    q[0]=0x30000;q[1]=0x47;q[2]=6;q[3]=0;q[4]=0x3f80000000000000;q[5]=1
    q[6]=(x<<4)|((y<<4)<<16);q[7]=5;q[8]=((x+w)*16)|(((y+h)*16)<<16);q[9]=5
    q[10]=0x30000 if zt==0 else (((zt&3)<<17)|0x10000);q[11]=0x47
    return q
def dbuff(psm,w,h,zt,zpsm,cl,g,fill):
    old=int.from_bytes(bytes([fill])*8,'little')
    Q=[old]*(0x230//8)
    d=disp(psm,w,h,g)
    Q[0:5]=d; Q[5:10]=d
    Q[12:28]=draw(psm,w,h,zt,zpsm,g,old)
    Q[42:58]=draw(psm,w,h,zt,zpsm,g,old)
    if cl:
        x=0x800-(w>>1); y=0x800-(h>>1)
        Q[28:40]=clear(zt,x,y,w,h); Q[58:70]=clear(zt,x,y,w,h)
    n=14 if cl else 8
    Q[10]=n|0x8000|(1<<60); Q[11]=0xe
    Q[40]=n|0x8000|(1<<60); Q[41]=0xe
    il,om,ff=g
    if (il==1 and ff==1) or il==0:
        z=zbufaddr(psm,w,h,g)>>1
        Q[12]=(Q[12]&~0x1ff&M)|(z&0x1ff)
        Q[7]=(Q[7]&~0x1ff&M)|(z&0x1ff)
    return Q
cases=[
 # name, psm,w,h,ztest,zpsm,clear,gparam(interlace,omode,ffmode),fill
 ("ct32_640x224_z24_ztest1_clear_frame", 0,640,224,1,0x31,1,(1,2,1),0x00),
 ("ct32_640x224_z24_ztest1_clear_frame_dirty", 0,640,224,1,0x31,1,(1,2,1),0x5a),
 ("ct16_640x224_z32_ztest0_noclear_frame", 2,640,224,0,0x30,0,(1,2,1),0x5a),
 ("ct32_512x448_z24_ztest0_clear_field", 0,512,448,0,0x31,1,(1,2,0),0x5a),
 ("ct16_512x448_z32_ztest1_clear_progressive", 2,512,448,1,0x30,1,(0,2,0),0x5a),
 ("ct16s_640x448_z32_ztest3_noclear_frame", 0xa,640,448,3,0x30,0,(1,2,1),0x00),
]
for c in cases:
    Q=dbuff(*c[1:])
    print('// %s'%c[0])
    print('{"%s", %d, %d, %d, %d, 0x%x, %d, {%d, %d, %d}, 0x%02x, {'%(c[0],*c[1:7],*c[7],c[8]))
    for i in range(0,len(Q),4):
        print('    '+', '.join('0x%016xull'%v for v in Q[i:i+4])+',')
    print('}},')
