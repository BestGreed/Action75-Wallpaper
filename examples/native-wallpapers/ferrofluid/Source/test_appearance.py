"""Apply each declared appearance setting through the SDK settings event and verify layer isolation."""
import argparse,json,subprocess
from pathlib import Path
import numpy as np
from PIL import Image
parser=argparse.ArgumentParser()
parser.add_argument('reader',type=Path)
parser.add_argument('calibration',type=Path)
parser.add_argument('work',type=Path)
parser.add_argument('--pack',type=Path)
args=parser.parse_args()
project=Path(__file__).resolve().parent.parent
args.work.mkdir(parents=True,exist_ok=True)
pack=args.pack or project/'Ferrofluid'
controls={c['id']:c for g in json.loads((pack/'settings.json').read_text(encoding='utf-8-sig'))['groups'] for c in g['controls']}
def render(values):
    params=args.work/'params.json'
    params.write_text(json.dumps(values))
    subprocess.run([args.reader,pack,args.calibration,args.work,params],check=True)
    return np.fromfile(args.work/'0.rgba',np.uint8).reshape(480,1280,4)[:,:,:3].astype(int)
default=render({})
explicit=render({k:c['default'] for k,c in controls.items()})
assert np.array_equal(default,explicit)
liquid=default.max(-1)<85
plate=default.min(-1)>135
entries={}
colors={}
values=dict(style=2,brightness=.5,hue=120,saturation=0,color=[255,68,36])
for key,c in controls.items():
    if 'appearance' not in c:continue
    role,layer=c['appearance'],c['surface']
    candidate=render({key:values[role]})
    diff=abs(candidate-default).max(-1)
    # Only use a remote plate sample and the solid interior of the liquid,
    # excluding edges where multisampling and shadows could mix surface colors.
    fluid=int(diff[liquid].max());background=int(diff[:120].max())
    if layer=='foreground':assert fluid>0 and background==0,(key,fluid,background)
    else:assert background>0 and diff[220:235,630:650].max()==0,(key,background)
    entries[key]=dict(value=values[role],fluidInteriorMaxDelta=int(diff[220:235,630:650].max()),distantFloorMaxDelta=background,wholeImageMaxDelta=int(diff.max()))
    if role=='color':colors[layer]=candidate
report=dict(defaultEqualsExplicit=True,settingsEventCases=entries)
assert len(entries)==10
(args.work/'appearance.json').write_text(json.dumps(report,indent=2))
for k,frame in colors.items():
    Image.fromarray(frame.astype('uint8')).save(args.work/('appearance-'+k+'.png'))
print(json.dumps(report))
