"""Re-run the existing performance audit's isolated trial and timing/IPC sequence."""
import argparse,json
from pathlib import Path
p=argparse.ArgumentParser()
p.add_argument("project",type=Path)
p.add_argument("baseline",type=Path)
args=p.parse_args()
root=args.project.resolve()
source=Path(__file__).resolve().parent.parent
original=root/"artifacts/performance-audit-migration/run.py"
# Reuse the original trial setup verbatim, stopping before its all-wallpaper loop.
prefix=original.read_text().split("player=root/")[0].replace("root=Path.cwd()","root=project")
env={"project":root}
exec(compile(prefix,str(original),"exec"),env)
player=root/"artifacts/integration-build/dist/Action75Native/Action75Native.exe"
audit=source/"Measurements"
for n,(name,pack) in enumerate([("old",args.baseline),("new",source/"Ferrofluid"),("new",source/"Ferrofluid"),("old",args.baseline)]):
    print(n+1,name,flush=True)
    env["ns"]["trial"](player,pack,audit/f"trial-{n+1}-{name}.json",True)
