"""Deterministic camera AUs and independent decoding checks for live epochs."""
import json
import re
import struct
import subprocess
from pathlib import Path

QUALITIES = [(1920,1080,'4.0'),(1280,720,'3.1'),(640,360,'3.0')]
SEQUENCE = [2,1,0,2,0,1,2]  # Covers all six directed transitions.

def fixtures(root, sequence=SEQUENCE):
    root=Path(root); root.mkdir(parents=True,exist_ok=True)
    videos={}
    for q in set(sequence):
        w,h,level=QUALITIES[q]; path=root/f'{q}.h264'
        subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i',f'testsrc2=s={w}x{h}:r=15:d=2',
            '-c:v','libx264','-preset','ultrafast','-profile:v','baseline','-level:v',level,
            '-x264-params','aud=1:keyint=15:min-keyint=15:scenecut=0:repeat-headers=1','-b:v','250k','-f','h264',str(path)],check=True,timeout=30)
        raw=path.read_bytes(); starts=[m.start() for m in re.finditer(b'\x00\x00(?:\x00)?\x01\x09',raw)]
        starts.append(len(raw)); videos[q]=[raw[a:b] for a,b in zip(starts,starts[1:])]
        assert len(videos[q])==30
    audio=root/'source.aac'
    subprocess.run(['ffmpeg','-v','error','-y','-f','lavfi','-i',f'sine=frequency=440:sample_rate=8000:duration={len(sequence)*2}',
        '-c:a','aac','-b:a','16k','-ac','1','-f','adts',str(audio)],check=True,timeout=30)
    samples=[]
    for phase,q in enumerate(sequence):
        for i,data in enumerate(videos[q]):
            key=any(n[0]&31==5 for n in re.split(b'\x00\x00(?:\x00)?\x01',data)[1:] if n)
            samples.append((phase*2000+round(i*1000/15),1 if key else 0,data,phase+1))
    raw=audio.read_bytes();at=i=0
    while at<len(raw):
        n=((raw[at+3]&3)<<11)|(raw[at+4]<<3)|(raw[at+5]>>5);ms=i*128
        if ms<len(sequence)*2000:samples.append((ms,8,raw[at:at+n],ms//2000+1))
        at+=n;i+=1
    return sorted(samples,key=lambda s:(s[0],s[1]==8))

def records(raw):
    at=0; result=[]
    while at<len(raw):
        assert len(raw)-at>=16,'truncated header'
        magic,kind,flags,reserved,epoch,n=struct.unpack_from('>4sBBHII',raw,at)
        assert magic==b'MVL2' and kind in (1,2,3,4) and not flags and not reserved
        assert epoch>0 and n<=524288 and at+16+n<=len(raw)
        result.append((kind,epoch,raw[at+16:at+16+n]));at+=16+n
    return result

def verify(root, output, sequence=SEQUENCE, minimum_video=20, source_root=None):
    root=Path(root);source_root=Path(source_root) if source_root is not None else root;epochs={};configs={};numbers=[]
    for kind,epoch,data in records(output):
        if kind==1:
            assert epoch not in configs;configs[epoch]=json.loads(data);epochs[epoch]=bytearray()
        elif kind in (2,3):
            assert epoch in epochs;epochs[epoch]+=data
            if kind==3:
                pos=data.index(b'mfhd');numbers.append(struct.unpack_from('>I',data,pos+8)[0])
    assert len(epochs)==len(sequence),(list(epochs),sequence)
    assert numbers==list(range(1,len(numbers)+1)), 'fragment sequence reset'
    audio=[];report=[];source_hashes={}
    def picture_hashes(path):
        raw=subprocess.check_output(['ffmpeg','-v','error','-i',str(path),'-map','0:v',
            '-fps_mode','passthrough','-f','framemd5','-'],timeout=30).decode()
        return [line.rsplit(',',1)[-1].strip() for line in raw.splitlines() if line and not line.startswith('#')]
    for (epoch,data),quality in zip(epochs.items(),sequence):
        w,h,_=QUALITIES[quality];assert (configs[epoch]['width'],configs[epoch]['height'])==(w,h)
        path=root/f'epoch-{epoch}.mp4';path.write_bytes(data)
        info=json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-show_packets','-of','json',str(path)],timeout=20))
        v=next(s for s in info['streams'] if s['codec_type']=='video');a=next(s for s in info['streams'] if s['codec_type']=='audio')
        assert (v['width'],v['height'])==(w,h);assert a['sample_rate']=='48000'
        vs=[p for p in info['packets'] if p['codec_type']=='video']; assert len(vs)>=minimum_video,(epoch,len(vs))
        assert 'K' in vs[0]['flags'];assert abs(float(vs[0]['pts_time'])-(epoch-1)*2)<.001
        # avc1 uses only its initialization parameters; original SPS copies
        # must not override the low-delay VUI in later IDRs. VCL remains exact.
        for packet in vs:
            pos=int(packet['pos']);sample=data[pos:pos+int(packet['size'])];at=0
            while at<len(sample):
                size=struct.unpack_from('>I',sample,at)[0];at+=4
                assert size and at+size<=len(sample) and sample[at]&31 not in (7,8)
                at+=size
        if quality not in source_hashes:source_hashes[quality]=picture_hashes(source_root/f'{quality}.h264')
        assert picture_hashes(path)==source_hashes[quality][:len(vs)],'video pixels changed during remux'
        aud=[(int(p['pts']),int(p['duration'])) for p in info['packets'] if p['codec_type']=='audio'];audio+=aud
        dec=subprocess.run(['ffmpeg','-v','error','-xerror','-i',str(path),'-map','0:v','-map','0:a','-f','null','-'],capture_output=True,timeout=30)
        assert dec.returncode==0 and not dec.stderr,dec.stderr.decode()
        report.append({'epoch':epoch,'width':w,'height':h,'video_frames':len(vs),'audio_packets':len(aud)})
    assert len(audio)>100
    for (pts,duration),(next_pts,_) in zip(audio,audio[1:]):assert next_pts==pts+duration,(pts,duration,next_pts)
    return report
