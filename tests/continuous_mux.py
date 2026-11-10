#!/usr/bin/env python3
"""All resolution transitions preserve audio and global decode timestamps."""
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from continuous_media import fixtures,verify

with tempfile.TemporaryDirectory(prefix='vava-continuous-mux-') as tmp:
    root=Path(tmp);samples=fixtures(root)
    data=b''.join(struct.pack('>BBIQI',kind==8,kind==1,epoch,ms,len(payload))+payload for ms,kind,payload,epoch in samples)
    src=root/'input';out=root/'output';src.write_bytes(data)
    p=subprocess.run([sys.argv[1],str(src),str(out)],capture_output=True,timeout=30)
    assert p.returncode==0,p.stderr.decode()
    print(verify(root,out.read_bytes()))
    # Real cameras stamp packet delivery with modest jitter. Sample count,
    # rather than each packet's arrival stamp, must pace continuous audio.
    jittered=[];i=0
    for ms,kind,payload,epoch in samples:
        if kind==8:
            ms+=70 if i==0 else ((i*37)%90)-40;i+=1
            ms=max(0,ms)
        jittered.append((ms,kind,payload,epoch))
    jittered.sort(key=lambda s:(s[0],s[1]==8))
    src.write_bytes(b''.join(struct.pack('>BBIQI',kind==8,kind==1,epoch,ms,len(payload))+payload for ms,kind,payload,epoch in jittered))
    p=subprocess.run([sys.argv[1],str(src),str(out)],capture_output=True,timeout=30)
    assert p.returncode==0,p.stderr.decode()
    verify(root,out.read_bytes())
    # Regressing the camera clock is a different failure, not a codec switch.
    src.write_bytes(data+struct.pack('>BBIQI',0,1,8,1,len(samples[0][2]))+samples[0][2])
    p=subprocess.run([sys.argv[1],str(src),str(out)],capture_output=True,timeout=30)
    assert p.returncode!=0,'accepted regressing video clock'
