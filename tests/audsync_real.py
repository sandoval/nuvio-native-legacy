#!/usr/bin/env python3
"""Run real matched subtitle windows and real soundtrack/wrong-movie controls."""
import hashlib,json,os,platform,subprocess,sys
from pathlib import Path
exe,cache=sys.argv[1:];cache=Path(cache)
cases=[('sintel-positive-2500','sintel-master-st.raw','sintel_en.srt',60,2500,'positive','none'),
       ('sintel-positive-minus1200','sintel-master-st.raw','sintel_en.srt',60,-1200,'positive','none'),
       ('sintel-later-positive-2500','sintel-master-st.raw','sintel_en.srt',300,2500,'positive','none'),
       ('sintel-music-effects-negative','sintel-m+e-st.raw','sintel_en.srt',60,0,'negative','none'),
       ('elephants-dream-wrong-movie','ED-CM-St-16bit.raw','sintel_en.srt',60,0,'negative','none'),
       ('elephants-dream-positive-2500','ED-CM-St-16bit.raw','ED-captions.vtt',60,2500,'positive','none'),
       ('elephants-dream-positive-minus1200','ED-CM-St-16bit.raw','ED-captions.vtt',60,-1200,'positive','none'),
       ('elephants-dream-later-positive-2500','ED-CM-St-16bit.raw','ED-captions.vtt',240,2500,'positive','none'),
       ('elephants-dream-drift-negative','ED-CM-St-16bit.raw','ED-captions.vtt',60,0,'negative','drift'),
       ('elephants-dream-cut-negative','ED-CM-St-16bit.raw','ED-captions.vtt',60,0,'negative','cut'),
       ('silence-negative','ED-CM-St-16bit.raw','ED-captions.vtt',60,0,'negative','silence')]
results=[]
for label,pcm,subtitle,start,shift,kind,variant in cases:
 p=subprocess.run([exe,str(cache/pcm),str(cache/subtitle),str(start),str(shift),kind,label,variant],capture_output=True,text=True)
 sys.stderr.write(p.stderr)
 if p.returncode not in (0,2):raise SystemExit(f'{label}: harness failure {p.returncode}: {p.stdout}')
 result=json.loads(p.stdout);results.append(result);print(json.dumps(result),flush=True)
def checksum(path):
 h=hashlib.sha256()
 with Path(path).open('rb') as f:
  while block:=f.read(1024*1024):h.update(block)
 return h.hexdigest()
metadata={'platform':platform.platform(),
          'compiler':subprocess.check_output([os.environ.get('CC','cc'),'--version'],text=True).splitlines()[0],
          'model_sha256':checksum(os.environ['NUVIO_SILERO_ONNX']),
          'runtime_sha256':checksum(Path(os.environ['NUVIO_ORT_ROOT'])/'lib/libonnxruntime.so'),
          'pcm_sha256':{name:checksum(cache/name) for name in sorted({c[1] for c in cases})},
          'subtitle_sha256':{name:checksum(cache/name) for name in sorted({c[2] for c in cases})},
          'parameters':{'window_seconds':300,'threshold':0.5,'gap_ms':300,'minimum_speech_ms':200,'search_radius_ms':30000,'acceptance':'existing QUICK guards'}}
report={'metadata':metadata,'corpus':'Blender Sintel official subtitles + Dolby ED demo captions + Xiph lossless audio masters','results':results,
        'positive_passes':sum(r['gate_pass'] for r in results if r['positive']),
        'false_acceptances':sum(r['accepted'] for r in results if not r['positive']),
        'release_gate_pass':all(r['gate_pass'] for r in results)}
(cache/'native-offset-results.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='results'}))
raise SystemExit(0 if report['release_gate_pass'] else 2)
