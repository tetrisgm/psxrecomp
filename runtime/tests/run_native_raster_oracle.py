import ctypes as c
import os
from pathlib import Path
import random
import json
import argparse

ap=argparse.ArgumentParser(description="Compare native GP0 packets with a separately built Beetle GPU oracle")
ap.add_argument('--oracle',type=Path,required=True)
ap.add_argument('--native',type=Path,required=True)
ap.add_argument('--output',type=Path,required=True)
ap.add_argument('--dll-dir',type=Path,action='append',default=[])
ap.add_argument('--cases',type=int,default=10000)
ap.add_argument('--coordinate-stress',action='store_true')
args=ap.parse_args()
root=args.output.resolve()
root.mkdir(parents=True,exist_ok=True)
dll_dirs=[os.add_dll_directory(str(p.resolve())) for p in args.dll_dir] if os.name=='nt' else []
ref=c.CDLL(str(args.oracle.resolve()))
native=c.CDLL(str(args.native.resolve()))
U16=c.c_uint16*524288
class State(c.Structure):
    _fields_=[(k,c.c_int32) for k in ('left','top','right','bottom','offset_x','offset_y')]+[(k,c.c_uint32) for k in ('page','window','set_mask','check_mask')]
native.native_raster_create.argtypes=[c.POINTER(c.c_uint16)]
native.native_raster_create.restype=c.c_void_p
native.native_raster_gp0.argtypes=[c.c_void_p,c.POINTER(State),c.POINTER(c.c_uint32),c.c_size_t]
native.native_raster_restore.argtypes=[c.c_void_p,c.POINTER(c.c_uint16)]
native.native_raster_read.argtypes=[c.c_void_p,c.c_int,c.c_int,c.c_int,c.c_int,c.POINTER(c.c_uint16)]
native.native_raster_destroy.argtypes=[c.c_void_p]
assert ref.oracle_init()
rng=random.Random(0x20261028)
initial=U16(*[((i*173+(i>>10)*31)^0x5317)&65535 for i in range(524288)])
nr=native.native_raster_create(initial)
a,b=U16(),U16()
def write(words):
    values=(c.c_uint32*len(words))(*words)
    ref.oracle_write(values,len(words))
def xy(x,y):return (x&0xffff)|((y&0xffff)<<16)
count=0
failures=[]
fixtures=[]
try:
    for case in range(args.cases):
        op=rng.choice(list(range(0x20,0x40))+list(range(0x40,0x48))+list(range(0x50,0x58))+list(range(0x60,0x80)))
        s=State(0,0,1023,511,rng.randrange(-1024,1024) if args.coordinate_stress else rng.randrange(-20,20),rng.randrange(-1024,1024) if args.coordinate_stress else rng.randrange(-20,20),rng.randrange(4)*32|rng.choice([0,512])|rng.randrange(3)*128|12|rng.choice([0,4096,8192,12288]),0,rng.randrange(2),rng.randrange(2))
        x,y=(rng.randrange(-1024,1024),rng.randrange(-1024,1024)) if args.coordinate_stress else (rng.randrange(40,800),rng.randrange(40,400))
        pts=[xy(x+rng.randrange(-30,50),y+rng.randrange(-30,50)) for _ in range(4)]
        colors=[rng.randrange(0x1000000) for _ in range(4)]
        p=[(op<<24)|colors[0]]
        if op<0x40:
            for i in range(4 if op&8 else 3):
                if i and op&16:p.append(colors[i])
                p.append(pts[i])
                if op&4:p.append(rng.randrange(256)|(rng.randrange(256)<<8)|(((480<<6) if i==0 else s.page&511 if i==1 else 0)<<16))
        elif op<0x60:
            p.append(pts[0])
            if op&16:p.append(colors[1])
            p.append(pts[1])
        else:
            p.append(pts[0])
            if op&4:p.append(rng.randrange(256)|(rng.randrange(256)<<8)|((480<<6)<<16))
            if (op&24)==0:p.append(xy(rng.randrange(1,40),rng.randrange(1,40)))
        native.native_raster_restore(nr,initial)
        ref.oracle_reset(initial)
        write([0xe1000000|s.page,0xe2000000|s.window,0xe3000000,
               0xe4000000|1023|(511<<10),0xe5000000|(s.offset_x&2047)|((s.offset_y&2047)<<11),
               0xe6000000|s.set_mask|(s.check_mask<<1)])
        write(p)
        packet=(c.c_uint32*len(p))(*p)
        assert native.native_raster_gp0(nr,c.byref(s),packet,len(p))
        ref.oracle_read(a)
        native.native_raster_read(nr,0,0,1024,512,b)
        count+=1
        if case in [0,1,2,3,4,5,161,809,2366,5001,5071,5131,6088,6192,8685,8868]:
            digest=14695981039346656037
            for value in a:
                digest=((digest^(value&255))*1099511628211)&0xffffffffffffffff
                digest=((digest^(value>>8))*1099511628211)&0xffffffffffffffff
            fixtures.append(dict(state=[getattr(s,k) for k,_ in s._fields_],packet=p,digest=digest))
        if bytes(a)!=bytes(b):
            diff=[(i%1024,i//1024,aa,bb) for i,(aa,bb) in enumerate(zip(a,b)) if aa!=bb]
            failures.append(dict(case=case,op=hex(op),state=[getattr(s,k) for k,_ in s._fields_],packet=p,differences=len(diff),first=diff[:8]))
            if len(failures)>=10:break
finally:native.native_raster_destroy(nr)
result=dict(cases=count,failures=failures,reference='53246dc2d00f3c00cc56baa1061902da7a6651e4',pgxp='enabled')
(root/'native-fixtures.json').write_text(json.dumps(fixtures,indent=2))
(root/'native-parity.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
raise SystemExit(bool(failures))
