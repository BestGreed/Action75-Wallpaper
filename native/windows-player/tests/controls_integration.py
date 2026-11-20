"""Isolated per-wallpaper profiles integration; no desktop or physical input."""
import ctypes,hashlib,json,os,shutil,subprocess,time,uuid
from pathlib import Path

SOURCE=Path(__file__).resolve().parents[3]
BASE=SOURCE/'artifacts/native-player'
BASE.mkdir(parents=True,exist_ok=True)
RUNS=BASE/'qa-runs';RUNS.mkdir(exist_ok=True)
ROOT=RUNS/('profiles-'+uuid.uuid4().hex[:10]);ROOT.mkdir()
STATE=ROOT/'state';STATE.mkdir()
PIPE='Action75ProfilesTest-'+uuid.uuid4().hex
EXE=ROOT/'dist/Action75Native/Action75Native.exe'
EXE.parent.mkdir(parents=True)
shutil.copy2(SOURCE/'dist/Action75Native/Action75Native.exe',EXE)
shutil.copytree(SOURCE/'wallpapers-native',ROOT/'wallpapers-native')
(ROOT/'existing-resources/finalmouse').mkdir(parents=True)
(ROOT/'data').mkdir()
for rel in ['existing-resources/finalmouse/action75-ue-keymap.json','data/fm-calibration.json']:
 shutil.copy2(SOURCE/rel if rel.startswith('existing-resources') else SOURCE/'assets/defaults/fm-calibration.json',ROOT/rel)
legacy={'version':1,'packId':'carrier','monitorDevice':'__A75_TEST_DISCONNECTED__','fps':60,'visible':True,'paused':False,'effectBrightness':4.0,'effectIntensity':.8,'effectScale':1.2,'effectSaturation':2.5,'effectPalette':'original','effectPaletteColors':[[255,80,120],[40,180,235]]}
(STATE/'native-player.json').write_text(json.dumps(legacy),encoding='utf-8')
calhash=hashlib.sha256((ROOT/'data/fm-calibration.json').read_bytes()).hexdigest()
ctypes.windll.kernel32.SetErrorMode(0x0001|0x0002|0x8000)
def command(action,ok=True,**kwargs):
 req={'action':action,**kwargs}
 p=subprocess.run([str(EXE),'--root',str(ROOT),'--pipe',PIPE,'--command',json.dumps(req)],capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=10,creationflags=0x08000000)
 try:reply=json.loads(p.stdout)
 except Exception:raise RuntimeError(f'Bad CLI output {p.returncode}: {p.stdout} {p.stderr}')
 if ok:assert reply.get('ok'),reply
 else:assert reply.get('ok') is False,reply
 return reply.get('result') if ok else reply
report={'fixture':str(ROOT),'pipe':PIPE,'checks':[],'windowsShown':0,'physicalKeysInjected':0}
def passed(name):report['checks'].append(name)
server=subprocess.Popen([str(EXE),'--root',str(ROOT),'--state-dir',str(STATE),'--pipe',PIPE,'--headless-test'],cwd=ROOT,stdin=subprocess.DEVNULL,stdout=(ROOT/'stdout.log').open('wb'),stderr=(ROOT/'stderr.log').open('wb'),creationflags=0x08004000)
try:
 for _ in range(50):
  if server.poll() is not None:raise RuntimeError((ROOT/'stdout.log').read_text(errors='replace'))
  try:s=command('status');break
  except Exception:time.sleep(.05)
 else:raise RuntimeError('Headless server not ready')
 assert s['headless'] is True
 carrier=command('getPackSettings',id='carrier')
 assert abs(carrier['values']['brightness']-(3.2/4)**.5*100)<.01,carrier
 afterimage=command('getPackSettings',id='afterimage')
 assert afterimage['values']['brightness']==80,afterimage
 assert abs(carrier['values']['duration']-.63)<.001,carrier
 passed('Legacy appearance and original duration migrate only to active wallpaper')
 carrier=command('setPackSettings',id='carrier',expectedRevision=carrier['revision'],values={'brightness':73,'duration':.55,'palette':{'preset':'custom','colors':[[255,0,0],[0,180,255]]}})
 old_status=command('status')
 paint=command('getPackSettings',id='paintball-native')
 paint=command('setPackSettings',id='paintball-native',expectedRevision=paint['revision'],values={'brightness':25,'density':20,'speed':2.0})
 still=command('status');assert still['packId']=='carrier' and still['effectBrightness']==old_status['effectBrightness']
 passed('Inactive edits do not alter active wallpaper')
 command('selectPack',id='paintball-native')
 active=command('status');assert abs(active['effectBrightness']-.25)<.001 and abs(active['effectIntensity']-.4)<.001,active
 assert command('getPackSettings',id='paintball-native')['values']['speed']==2
 command('selectPack',id='carrier')
 restored=command('getPackSettings',id='carrier')
 assert restored['values']['brightness']==73 and restored['values']['duration']==.55
 assert restored['values']['palette']['colors']==[[255,0,0],[0,180,255]]
 passed('Independent settings survive repeated switches')
 before=command('getPackSettings',id='carrier')
 for patch in [{'brightness':101,'duration':.8},{'unknownNewField':1},{'palette':{'preset':'wrong','colors':[[1,2,3]]}},{'palette':{'preset':'custom','colors':[]}}]:
  command('setPackSettings',ok=False,id='carrier',values=patch)
  assert command('getPackSettings',id='carrier')['values']==before['values']
 command('setPackSettings',ok=False,id='carrier',expectedRevision=0,values={'brightness':1})
 command('getPackSettings',ok=False,id='../escape')
 passed('Invalid patches, unknown fields and stale revisions preserve previous profile')
 command('resetPackSettings',id='paintball-native')
 assert command('getPackSettings',id='paintball-native')['values']['brightness']==85
 assert command('getPackSettings',id='carrier')['values']['brightness']==73
 passed('Reset affects only requested wallpaper')
 command('selectPack',id='afterimage')
 snow=command('getPackSettings',id='afterimage')
 assert not any(c['binding']['target']=='effectIntensity' for g in snow['schema']['groups'] for c in g['controls'])
 assert command('status')['effectIntensity']==1
 passed('Wallpaper without intensity control uses its own neutral value')
 # Add an arbitrary named shader control through data only, without changing the host/GUI.
 schemafile=ROOT/'wallpapers-native/paintball-native/settings.json'
 schema=json.loads(schemafile.read_text(encoding='utf-8'))
 schema['groups'].append({'id':'future','label':'Future','controls':[{'id':'futureExposure','type':'slider','label':'Custom exposure','min':0,'max':100,'step':1,'default':50,'binding':{'target':'material.uniform','name':'RuntimePipeline','component':0,'mapping':{'type':'linear','inputMin':0,'inputMax':100,'outputMin':0,'outputMax':2}}}]})
 schemafile.write_text(json.dumps(schema),encoding='utf-8')
 command('listPacks')
 extended=command('getPackSettings',id='paintball-native')
 assert extended['values']['futureExposure']==50,extended
 command('setPackSettings',id='paintball-native',values={'futureExposure':75})
 command('selectPack',id='paintball-native')
 assert command('getPackSettings',id='paintball-native')['values']['futureExposure']==75
 passed('Data-only new arbitrary field binds named material uniform')
 # Unknown uniform is rejected when activating it, and previous output remains usable.
 command('selectPack',id='carrier')
 schema['groups'][-1]['controls'][0]['binding']['name']='DefinitelyMissingUniform'
 schemafile.write_text(json.dumps(schema),encoding='utf-8')
 command('listPacks')
 command('selectPack',ok=False,id='paintball-native')
 assert command('status')['packId']=='carrier'
 passed('Invalid shader binding preserves previous theme and settings')
 assert hashlib.sha256((ROOT/'data/fm-calibration.json').read_bytes()).hexdigest()==calhash
 passed('Calibration unchanged')
 command('stop');server.wait(timeout=5)
 # Restart with existing per-pack profile files to verify actual disk persistence.
 server=subprocess.Popen([str(EXE),'--root',str(ROOT),'--state-dir',str(STATE),'--pipe',PIPE,'--headless-test'],cwd=ROOT,stdin=subprocess.DEVNULL,stdout=(ROOT/'restart-stdout.log').open('wb'),stderr=(ROOT/'restart-stderr.log').open('wb'),creationflags=0x08004000)
 for _ in range(50):
  try:current=command('status');break
  except Exception:time.sleep(.05)
 restored=command('getPackSettings',id='carrier')
 assert restored['values']['brightness']==73 and restored['values']['duration']==.55
 assert len(list((STATE/'wallpaper-settings').glob('*.json')))>=2
 passed('Independent profile files survive actual process restart')
 report['ok']=True
finally:
 try:
  if server.poll() is None:command('stop');server.wait(timeout=5)
 except Exception:
  if server.poll() is None:server.terminate();server.wait(timeout=5)
 report.setdefault('ok',False)
 (BASE/'profile-integration-result.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
 print(json.dumps(report,indent=2))
