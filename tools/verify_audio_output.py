"""Verify actual WASAPI loopback output; retain spectral measurements, never desktop audio.

Requires numpy and PyAudioWPatch (test-only dependencies). Play only the generated probe tones.
"""
import argparse, json, subprocess, sys, threading, time
from pathlib import Path
import numpy as np

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--exe',required=True)
    parser.add_argument('--out',required=True)
    args=parser.parse_args()
    sys.path.insert(0,str(Path('build/qa-python').resolve()))
    import pyaudiowpatch as pa
    out=Path(args.out).resolve();out.mkdir(parents=True,exist_ok=True)
    measures=[]; phases={}; checks=[]; lines=[]
    with pa.PyAudio() as audio:
        info=audio.get_default_wasapi_loopback()
        rate=int(info['defaultSampleRate']); channels=int(info['maxInputChannels'])
        frames=round(rate*.05)
        frequencies=(330,660,990)
        kernel=np.exp(-2j*np.pi*np.outer(frequencies,np.arange(frames)/rate))
        def callback(data, count, times, flags):
            if count==frames:
                samples=np.frombuffer(data,dtype=np.float32).reshape(-1,channels).mean(axis=1)
                amplitudes=2*np.abs(kernel@samples)/frames
                measures.append((time.perf_counter(),*map(float,amplitudes)))
            return (None,pa.paContinue)
        stream=audio.open(format=pa.paFloat32,channels=channels,rate=rate,input=True,
            input_device_index=info['index'],frames_per_buffer=frames,stream_callback=callback)
        time.sleep(.2)
        proc=subprocess.Popen([str(Path(args.exe).resolve())],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,encoding='utf-8',errors='replace')
        deadline=time.perf_counter()+35
        def read():
            for line in proc.stdout:
                lines.append(line.rstrip())
                if line.startswith('PHASE '):phases[line.split()[1]]=time.perf_counter()
        reader=threading.Thread(target=read); reader.start()
        while proc.poll() is None and time.perf_counter()<deadline:time.sleep(.05)
        if proc.poll() is None:proc.kill();raise RuntimeError('audio probe timeout')
        reader.join();time.sleep(.15);stream.stop_stream();stream.close()
    (out/'probe.txt').write_text('\n'.join(lines),encoding='utf-8')
    if proc.returncode!=0:raise RuntimeError(f'audio probe exit={proc.returncode}')
    values=np.array(measures)
    def level(phase, start, end, frequency):
        # Device/callback buffering adds a small lag; wide windows avoid relying on one sample.
        stamp=phases[phase]
        selected=values[(values[:,0]>=stamp+start)&(values[:,0]<stamp+end),frequency+1]
        if not len(selected):raise AssertionError(f'missing samples: {phase} {start} {end}')
        return float(np.mean(selected))
    baseline=level('start_fade',1.0,1.25,0)
    def check(ok,name,actual):
        checks.append(dict(pass_=bool(ok),name=name,actual=actual))
        print(('PASS ' if ok else 'FAIL ')+name+' '+str(actual),flush=True)
    check(baseline>.005,'generated tone reaches real output',baseline)
    def relative(phase,a,b,f):return level(phase,a,b,f)/baseline
    check(relative('start_fade',.08,.22,0)<relative('start_fade',.4,.6,0)<.9,'start fades in',relative('start_fade',.4,.6,0))
    middle=relative('pause',.22,.42,0); quiet=relative('pause',.8,1.0,0)
    check(.1<middle<.85 and quiet<.05,'pause fades out to silence',dict(mid=middle,tail=quiet))
    check(relative('resume',.12,.28,0)<relative('resume',.5,.7,0)<1.05,'resume fades in',relative('resume',.5,.7,0))
    a=relative('crossfade',1.2,1.4,0);b=relative('crossfade',1.2,1.4,1)
    check(.45<a<.9 and .45<b<.9,'crossfade contains both old and new tracks',dict(old=a,new=b))
    check(.75<a*a+b*b<1.3,'crossfade midpoint maintains combined power',a*a+b*b)
    check(relative('crossfade',2.8,2.98,0)<.05 and relative('crossfade',2.8,2.98,1)>.85,'old track fades out completely',relative('crossfade',2.8,2.98,0))
    check(.1<relative('stop',.22,.42,1)<.85 and relative('stop',.8,.98,1)<.05,'stop fades out to silence',relative('stop',.22,.42,1))
    middle=relative('natural_tail',.6,1.0,2); tail=relative('natural_tail',1.4,1.55,2)
    check(middle>.8 and .03<tail<middle*.85 and relative('natural_tail',1.8,1.98,2)<.05,'one-shot effect fades at its natural end',dict(mid=middle,tail=tail))
    check(relative('sequential_switch',.1,.3,1)<.05 and relative('sequential_switch',.9,1.1,1)>.85,'mix off waits before fading in new track',relative('sequential_switch',.1,.3,1))
    (out/'checks.json').write_text(json.dumps(checks,indent=2),encoding='utf-8')
    # No raw loopback samples are retained or written.
    if not all(c['pass_'] for c in checks):raise SystemExit(1)

if __name__=='__main__':main()
