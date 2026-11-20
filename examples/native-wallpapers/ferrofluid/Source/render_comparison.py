"""Compare raw WARP readbacks from visual_check.exe and export preview media."""
import argparse,csv,hashlib,json
from pathlib import Path
import numpy as np
from PIL import Image
parser=argparse.ArgumentParser()
parser.add_argument('before',type=Path)
parser.add_argument('after',type=Path)
args=parser.parse_args()
project=Path(__file__).resolve().parent.parent
results=[]
for i,name in [(20,'rest'),(100,'single'),(155,'chord'),(230,'release-newest'),(499,'released')]:
    files=[d/f'{i}.rgba' for d in (args.before,args.after)]
    a,b=[np.fromfile(f,np.uint8).reshape(480,1280,4)[:,:,:3].astype(float) for f in files]
    diff=abs(a-b)
    results.append(dict(frame=i,phase=name,maxChannel=int(diff.max()),rms=np.sqrt((diff**2).mean()),
                        equalRGBA=files[0].read_bytes()==files[1].read_bytes(),
                        darkAreaBefore=int((a.max(-1)<90).sum()),darkAreaAfter=int((b.max(-1)<90).sum())))
    Image.fromarray(b.astype('uint8')).save(project/'Preview'/f'{name}.png')
frames=[]
for i in range(0,500,15):
    raw=(args.after/f'{i}.rgba').read_bytes()
    frames.append(Image.frombytes('RGBA',(1280,480),raw).convert('RGB').resize((768,288),Image.Resampling.LANCZOS))
frames[0].save(project/'Preview/interaction.webp',save_all=True,append_images=frames[1:],quality=91,duration=250,loop=0)
report=dict(dllSha256=hashlib.sha256((project/'Ferrofluid/wallpaper.dll').read_bytes()).hexdigest(),deterministicWARP=results,
            maxLoggedMassError=max(float(row['relativeError']) for row in csv.DictReader((project/'Measurements/solver.csv').open(encoding='utf-8-sig'))))
(project/'Measurements/accuracy.json').write_text(json.dumps(report,indent=2))
print(report)
