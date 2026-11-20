"""Exercise the existing players with disposable roots, profiles and named pipes."""

import argparse
from contextlib import contextmanager
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import math
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from threading import Event, Timer
import time
import uuid
import zipfile

from PIL import Image, ImageDraw


HERE = Path(__file__).resolve().parent
PROJECT = HERE.parents[1]
PAYLOAD = PROJECT / 'release/payload'
SOURCE = PROJECT / 'release/ACTION75-source-0.3.1.zip'
EFFECT_SDK = PROJECT / 'vendor-source/windows-native/EffekseerForCpp1.80.8.zip'
NATIVE = Path('dist/Action75Native/Action75Native.exe')
MANAGER = Path('dist/Action75Manager/Action75Manager.exe')
BASE = 'tidal-glass'
kernel = C.WinDLL('kernel32', use_last_error=True)
user = C.WinDLL('user32', use_last_error=True)


def api(dll, name, result, *args):
    fn = getattr(dll, name)
    fn.restype = result
    fn.argtypes = args
    return fn


open_file = api(kernel, 'CreateFileW', W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD,
                C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
read_file = api(kernel, 'ReadFile', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
                C.POINTER(W.DWORD), C.c_void_p)
write_file = api(kernel, 'WriteFile', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD,
                 C.POINTER(W.DWORD), C.c_void_p)
close = api(kernel, 'CloseHandle', W.BOOL, W.HANDLE)
wait = api(kernel, 'WaitForSingleObject', W.DWORD, W.HANDLE, W.DWORD)
terminate = api(kernel, 'TerminateProcess', W.BOOL, W.HANDLE, W.UINT)


def check(result):
    if not result:
        raise C.WinError(C.get_last_error())
    return result


def dump(path, obj):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(obj, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


def load(path):
    return json.loads(path.read_text(encoding='utf-8'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def request(pipe, action, *, ok=True, **fields):
    deadline = time.monotonic() + 8
    while True:
        handle = open_file(r'\\.\pipe' + '\\' + pipe, 0xc0000000, 0, None, 3, 0, None)
        if handle and handle != C.c_void_p(-1).value:
            break
        if time.monotonic() >= deadline:
            raise C.WinError(C.get_last_error())
        time.sleep(.006)
    try:
        data = json.dumps(dict(action=action, **fields)).encode() + b'\n'
        count = W.DWORD()
        check(write_file(handle, data, len(data), C.byref(count), None))
        buf = C.create_string_buffer(65536)
        response = bytearray()
        while b'\n' not in response:
            check(read_file(handle, buf, len(buf), C.byref(count), None))
            assert count.value and len(response) <= 1048576
            response.extend(buf.raw[:count.value])
        reply = json.loads(response)
        if ok is None:
            return reply
        assert reply.get('ok') == ok, (action, reply)
        return reply.get('result') if ok else reply['error']
    finally:
        close(handle)


@contextmanager
def player(root, pipe, *, executable=None):
    executable = executable or (root / NATIVE)
    process = subprocess.Popen([str(executable), '--root', str(root), '--state-dir',
                                str(root / 'headless-state'), '--pipe', pipe, '--headless-test',
                                '--headless-hardware', '--perf-stats'], cwd=root,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        try:
            status = request(pipe, 'status')
        except OSError as failed:
            if process.poll() is not None:
                out, err = process.communicate(timeout=3)
                raise RuntimeError((process.returncode, out.decode(errors='replace'),
                                    err.decode(errors='replace'))) from failed
            raise
        assert status.get('playerPid', process.pid) == process.pid and status['headless'], status
        assert not status['rawInputRegistered'], status
        yield status, process
    finally:
        if process.poll() is None:
            try:
                request(pipe, 'stop')
                process.wait(timeout=6)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                process.wait(timeout=5)


def slider(key, label, target, default, minimum, maximum, **binding):
    surface = 'background' if target.startswith('background.') else 'foreground'
    return {'id': key, 'type': 'slider', 'label': label, 'default': default, 'min': minimum,
            'max': maximum, 'step': .05, 'surface': surface, 'unit': '×',
            'binding': {'target': target, **binding}}


def schema(*controls):
    return {'version': 1, 'presentation': 'layered',
            'groups': [{'id': 'author', 'label': '作者参数', 'controls': list(controls)}]}


def asset_pack(directory, pack_id):
    directory.mkdir(parents=True)
    dump(directory / 'manifest.json', {'version': 1, 'id': pack_id, 'name': '青光试验',
         'kind': 'spriteBurst', 'background': 'background.png', 'effect': 'burst.png',
         'preview': 'preview.png', 'settings': 'settings.json', 'columns': 4, 'rows': 2,
         'duration': .80, 'diameter': 242})
    dump(directory / 'settings.json', schema(
        slider('authorGlow', '光纹强度', 'effectBrightness', 1, 0, 2),
        slider('authorScale', '光纹范围', 'effectScale', 1, .5, 2),
        slider('authorBackground', '底图亮度', 'background.brightness', 1, 0, 2)))
    background = Image.new('RGB', (800, 300))
    background.putdata([tuple(round(v * (1 + .65 * max(0, 1 - math.hypot((x - 510) / 520, (y - 155) / 310))))
                              for v in (10, 28, 36)) for y in range(300) for x in range(800)])
    background.save(directory / 'background.png')
    atlas = Image.new('RGB', (256, 128), '#000000')
    for frame in range(8):
        tile = Image.new('RGB', (64, 64), '#000000')
        stamp = ImageDraw.Draw(tile)
        radius = 4 + frame * 3.5
        gain = (8 - frame) / 8
        stamp.ellipse([31.5 - radius, 31.5 - radius, 31.5 + radius, 31.5 + radius],
                      outline=tuple(round(v * gain) for v in (100, 251, 215)), width=2)
        atlas.paste(tile, (frame % 4 * 64, frame // 4 * 64))
    atlas.save(directory / 'burst.png')
    cover = background.copy()
    for x, y, r in [(324, 147, 72), (483, 135, 47), (585, 182, 21)]:
        draw = ImageDraw.Draw(cover)
        draw.ellipse((x-r, y-r, x+r, y+r), outline='#6bf3b9', width=3)
        draw.ellipse((x-r*.6, y-r*.6, x+r*.6, y+r*.6), outline='#417f84', width=2)
    cover.save(directory / 'preview.png')


def material_pack(directory, pack_id, *, rename_uniform=True):
    shutil.copytree(PAYLOAD / 'wallpapers-native/paintball-native', directory)
    # Keep the shipped effect layout and replace one material's source and uniform name.
    material = directory / 'materials/PaintSplash_Native01.efkmat'
    original = material.read_bytes()
    old_name = b'RuntimePipeline'
    new_name = b'AuthorTintScale' if rename_uniform else old_name
    assert len(old_name) == len(new_name) and old_name in original, material
    with zipfile.ZipFile(SOURCE) as archive:
        file = next(p for p in archive.namelist() if p.endswith('/PaintSplash_Native01.hlsl'))
        hlsl = archive.read(file).decode('utf-8').replace(old_name.decode(), new_name.decode())
    close_brace = hlsl.rfind('}')
    hlsl = hlsl[:close_brace] + '\n    emissive *= float3(0.42, 0.90, 0.75);\n' + hlsl[close_brace:]
    changed = bytearray(original[:16])
    offset, replaced = 16, 0
    while offset < len(original):
        tag, size = struct.unpack_from('<4sI', original, offset)
        chunk = original[offset + 8:offset + 8 + size]
        offset += 8 + size
        if tag == b'GENE':
            hlsl_bytes = hlsl.encode() + b'\0'
            chunk = struct.pack('<I', len(hlsl_bytes)) + hlsl_bytes
            replaced += 1
        else:
            chunk = chunk.replace(old_name, new_name)
        changed.extend(struct.pack('<4sI', tag, len(chunk)))
        changed.extend(chunk)
    assert offset == len(original) and replaced == 1
    material.write_bytes(changed)
    dump(directory / 'manifest.json', {'version': 1, 'id': pack_id, 'name': '作者材质试验',
         'kind': 'effekseer', 'effect': 'paintball-01.efk', 'preview': 'preview.png',
         'settings': 'settings.json'})
    dump(directory / 'settings.json', schema(
        slider('authorTint', '作者色阶', 'material.uniform', 1, .4, 1.6,
               name=new_name.decode(), component=0)))
    return {'templateSHA256': hashlib.sha256(original).hexdigest(), 'materialSHA256': digest(material),
            'authorUniform': new_name.decode(), 'sourceChange': 'Added a green-biased emissive tint'}


def catalogue(pipe):
    return {row['id']: row for row in request(pipe, 'listPacks')}


def bounded_select(pipe, process, pack_id, seconds=8):
    timed_out = Event()

    def expire():
        timed_out.set()
        if process.poll() is None:
            process.terminate()

    timer = Timer(seconds, expire)
    timer.daemon = True
    timer.start()
    try:
        reply = request(pipe, 'selectPack', id=pack_id, ok=None)
        return {'status': reply['result']} if reply['ok'] else {'error': reply['error']}
    except OSError:
        if not timed_out.is_set():
            raise
        process.wait(timeout=5)
        return {'timedOutAfterSeconds': seconds}
    finally:
        timer.cancel()


def fixture_path(source, library, name):
    return shutil.copytree(source, library / name)


def stress(pipe, keys, duration=4):
    request(pipe, 'perfReset')
    first = request(pipe, 'status')['framesRendered']
    start = time.perf_counter()
    events = 0
    while time.perf_counter() - start < duration:
        request(pipe, 'testTrigger', code=keys[events % len(keys)])
        events += 1
        time.sleep(max(0, start + events / 40 - time.perf_counter()))
    elapsed = time.perf_counter() - start
    result = request(pipe, 'perfRead')
    status = result['status']
    assert status['error'] is None, status
    return {'events': events, 'seconds': elapsed, 'fps': (status['framesRendered'] - first) / elapsed,
            'workingSetMB': status['memoryMB'], **result['performance']}


def independent_effect(root, pipe, author, library, run_id):
    if not EFFECT_SDK.is_file():
        return {'skipped': 'Pinned Effekseer SDK archive is unavailable'}
    pack_id = 'qa-sdk-effect-' + run_id
    original = author / pack_id
    original.mkdir()
    effect = original / 'square_r.efkefc'
    with zipfile.ZipFile(EFFECT_SDK) as archive:
        effect.write_bytes(archive.read('EffekseerForCpp1.80.8/Examples/Resources/square_r.efkefc'))
    dump(original / 'manifest.json', {'version': 1, 'id': pack_id, 'name': 'SDK 粒子对照',
         'kind': 'effekseer', 'effect': effect.name, 'settings': 'settings.json'})
    dump(original / 'settings.json', schema(slider('authorSize', '粒子尺寸', 'effectScale', 1, .5, 2)))

    with player(root, pipe) as (_, process):
        assert pack_id not in catalogue(pipe)
        fixture_path(original, library, pack_id)
        found = catalogue(pipe)[pack_id]
        assert found['supported'] and found['schemaAvailable']
        result = bounded_select(pipe, process, pack_id)
        if 'status' not in result:
            return dict(id=pack_id, effectSHA256=digest(effect), **result)
        chosen = result['status']
        assert chosen['packId'] == pack_id and not chosen['error'], chosen
        changed = request(pipe, 'setPackSettings', id=pack_id, values={'authorSize': 1.35})
        assert changed['values']['authorSize'] == 1.35, changed
        request(pipe, 'testTrigger', code='KeyF')
        time.sleep(.09)
        live = request(pipe, 'status')
        assert live['activeParticles'] > 0 and not live['error'], live
    with player(root, pipe) as (restarted, _):
        assert restarted['packId'] == pack_id and abs(restarted['effectScale'] - 1.35) < 1e-5
        assert request(pipe, 'testTrigger', code='Space')['handle'] >= 0
    return {'id': pack_id, 'effectSHA256': digest(effect), 'backend': chosen['renderer'],
            'activeParticles': live['activeParticles'], 'storedAuthorSize': 1.35,
            'selectionAndValueSurvivedRestart': True}


class Startup(C.Structure):
    _fields_ = [('cb', W.DWORD), ('lpReserved', W.LPWSTR), ('lpDesktop', W.LPWSTR),
                ('lpTitle', W.LPWSTR)] + [(key, W.DWORD) for key in (
                    'dwX', 'dwY', 'dwXSize', 'dwYSize', 'dwXCountChars', 'dwYCountChars',
                    'dwFillAttribute', 'dwFlags')] + [('wShowWindow', W.WORD),
                ('cbReserved2', W.WORD), ('lpReserved2', C.c_void_p), ('hStdInput', W.HANDLE),
                ('hStdOutput', W.HANDLE), ('hStdError', W.HANDLE)]


class Process(C.Structure):
    _fields_ = [('process', W.HANDLE), ('thread', W.HANDLE), ('pid', W.DWORD), ('tid', W.DWORD)]


def screenshot(root, unique, output):
    create_desktop = api(user, 'CreateDesktopW', W.HANDLE, W.LPCWSTR, W.LPCWSTR, C.c_void_p,
                         W.DWORD, W.DWORD, C.c_void_p)
    close_desktop = api(user, 'CloseDesktop', W.BOOL, W.HANDLE)
    spawn = api(kernel, 'CreateProcessW', W.BOOL, W.LPCWSTR, W.LPWSTR, C.c_void_p, C.c_void_p,
                W.BOOL, W.DWORD, C.c_void_p, W.LPCWSTR, C.POINTER(Startup), C.POINTER(Process))
    exit_code = api(kernel, 'GetExitCodeProcess', W.BOOL, W.HANDLE, C.POINTER(W.DWORD))
    name = 'Action75ImportView-' + unique
    capture = output.with_name(f'{output.stem}-{unique}{output.suffix}')
    desktop = check(create_desktop(name, None, None, 0, 0x01ff, None))
    process = Process()
    try:
        startup = Startup(cb=C.sizeof(Startup), lpDesktop='Winsta0\\' + name)
        command = [str(root / MANAGER), '--root', str(root), '--screenshot', str(capture),
                   '--exit-after-screenshot', '--screenshot-size', '1440', '900']
        line = C.create_unicode_buffer(subprocess.list2cmdline(command))
        check(spawn(command[0], line, None, None, False, 0, None, str(root),
                    C.byref(startup), C.byref(process)))
        if wait(process.process, 20000) != 0:
            return False
        code = W.DWORD()
        check(exit_code(process.process, C.byref(code)))
        if code.value != 0 or not capture.is_file():
            return False
        capture.replace(output)
        return True
    finally:
        if process.process:
            if wait(process.process, 0) != 0:
                terminate(process.process, 1)
                wait(process.process, 3000)
            close(process.thread)
            close(process.process)
        close_desktop(desktop)
        capture.unlink(missing_ok=True)


def run(scratch, output, include_integrated):
    run_id = uuid.uuid4().hex[:10]
    pipe = 'Action75ImportQA-' + run_id
    sprite_id, material_id = 'qa-glow-' + run_id, 'qa-material-' + run_id
    custom_id, invalid_id = 'qa-logic-' + run_id, 'qa-missing-' + run_id
    broken_id = 'qa-decode-' + run_id
    future_id, schema_id = 'qa-future-' + run_id, 'qa-schema-' + run_id
    root, author, library = (scratch / name for name in ('player', 'author', 'outside-library'))
    shutil.copytree(PAYLOAD, root)
    data = root / 'data'
    data.mkdir(exist_ok=True)
    shutil.copyfile(root / 'assets/defaults/fm-calibration.json', data / 'fm-calibration.json')
    calibration = load(data / 'fm-calibration.json')['points']
    (data / 'player-pipe.txt').write_text(pipe, encoding='ascii')
    state_dir = root / 'headless-state'
    dump(state_dir / 'native-player.json', {'version': 1, 'packId': BASE, 'monitorDevice': 'OFFSCREEN_QA'})
    prefs = {'setupComplete': True, 'panelOpen': True, 'wallpaperDir': ''}
    dump(data / 'app-settings.json', prefs)
    asset_pack(author / sprite_id, sprite_id)
    material = material_pack(author / material_id, material_id)
    library.mkdir()
    fixture_path(root / 'wallpapers-native' / BASE, library, BASE)
    report = {'release': {'nativeSHA256': digest(root / NATIVE), 'managerSHA256': digest(root / MANAGER),
                          'installerSHA256': digest(PROJECT / 'release/ACTION75-Setup-0.3.1-x64.exe'),
                          'sourceArchiveSHA256': digest(SOURCE)}, 'material': material}

    with player(root, pipe) as (startup, _):
        assert startup['packId'] == BASE, startup
        assert sprite_id not in catalogue(pipe) and material_id not in catalogue(pipe)
        dump(data / 'app-settings.json', dict(prefs, wallpaperDir=str(library)))
        request(pipe, 'reloadDirectories')
        for name in (sprite_id, material_id):
            fixture_path(author / name, library, name)
        found = catalogue(pipe)
        for name in (sprite_id, material_id):
            assert found[name]['supported'] and found[name]['schemaAvailable'], found[name]
            assert Path(found[name]['path']) == library / name
        report['discovery'] = {'freshIDs': [sprite_id, material_id], 'startedWith': startup['packId'],
                               'externalToRuntimeRoot': not library.is_relative_to(root),
                               'filesystemCopyThenListPacks': True, 'manifestKinds': [
                                   found[name]['kind'] for name in (sprite_id, material_id)]}

        selected = request(pipe, 'selectPack', id=sprite_id)
        assert selected['packId'] == sprite_id and selected['renderer'] == 'D3D11/SpriteBurst', selected
        effects = []
        for key in ('KeyF', 'Space'):
            event = request(pipe, 'testTrigger', code=key)
            assert abs(event['x'] - calibration[key]['x'] * 1920) < .01, event
            assert abs(event['y'] - calibration[key]['y'] * 720) < .01, event
            effects.append({'fixtureKey': key, 'pixel': [event['x'], event['y']]})
        live = request(pipe, 'status')
        assert live['activeEffects'] > 0, live
        stored = state_dir / 'wallpaper-settings' / (sprite_id + '.json')
        before = stored.read_bytes()
        values = request(pipe, 'getPackSettings', id=sprite_id)
        request(pipe, 'previewPackSettings', id=sprite_id, values={'authorGlow': .55})
        assert stored.read_bytes() == before
        assert abs(request(pipe, 'status')['effectBrightness'] - .55) < 1e-5
        edits = request(pipe, 'setPackSettings', id=sprite_id, expectedRevision=values['revision'],
                        values={'authorGlow': .65, 'authorBackground': .75})
        assert load(stored)['values']['authorGlow'] == .65
        stale = request(pipe, 'setPackSettings', id=sprite_id, expectedRevision=values['revision'],
                        values={'authorGlow': .9}, ok=False)
        report['sprite'] = {'backend': selected['renderer'], 'diagnosticHits': effects,
                            'activeEffects': live['activeEffects'], 'previewPreservedFile': True,
                            'committed': edits['values'], 'staleRevisionError': stale,
                            'releasePerformance': stress(pipe, ('KeyF', 'Space'))}
        report['managerScreenshot'] = screenshot(root, run_id, output / 'isolated-library.png')

        request(pipe, 'selectPack', id=sprite_id)
        bad = library / invalid_id
        fixture_path(author / sprite_id, library, invalid_id)
        dump(bad / 'manifest.json', dict(load(bad / 'manifest.json'), id=invalid_id,
                                        effect='missing.png'))
        broken = library / broken_id
        fixture_path(author / sprite_id, library, broken_id)
        dump(broken / 'manifest.json', dict(load(broken / 'manifest.json'), id=broken_id))
        (broken / 'background.png').write_bytes(b'Harmless non-image file in a decoder test')
        future = library / future_id
        fixture_path(author / sprite_id, library, future_id)
        dump(future / 'manifest.json', dict(load(future / 'manifest.json'), id=future_id, version=2))
        wrong_schema = library / schema_id
        fixture_path(author / sprite_id, library, schema_id)
        dump(wrong_schema / 'manifest.json', dict(load(wrong_schema / 'manifest.json'), id=schema_id))
        dump(wrong_schema / 'settings.json', dict(load(wrong_schema / 'settings.json'), version=2))
        custom = library / custom_id
        dump(custom / 'manifest.json', {'version': 1, 'id': custom_id, 'name': '实验自定义渲染器',
             'kind': 'authorRippleGraph', 'shader': 'pixel.hlsl'})
        (custom / 'pixel.hlsl').write_text('float4 main() : SV_Target { return float4(0, 1, 0, 1); }\n')
        foreign = library / ('qa-web-' + run_id)
        dump(foreign / 'project.json', {'title': 'Inert web fixture', 'type': 'web', 'file': 'index.html'})
        (foreign / 'index.html').write_text('<!doctype html><p>Inert test page</p>\n')
        rows = catalogue(pipe)
        assert future_id not in rows and not any(Path(x['path']) == foreign for x in rows.values())
        assert not rows[invalid_id]['supported'] and not rows[custom_id]['supported']
        assert rows[broken_id]['supported'] and rows[broken_id]['schemaAvailable']
        assert rows[schema_id]['supported'] and not rows[schema_id]['schemaAvailable']
        failures = {tag: request(pipe, 'selectPack', id=pack_id, ok=False) for tag, pack_id in (
            ('missingAsset', invalid_id), ('invalidImage', broken_id),
            ('unknownBackend', custom_id), ('futureSchema', schema_id))}
        assert request(pipe, 'status')['packId'] == sprite_id
        assert request(pipe, 'testTrigger', code='KeyF')['handle'] >= 0
        report['failure'] = dict(failures, futureManifestOmitted=True, foreignWebOmitted=True,
                                 activePackSurvived=True)

        duplicate = root / 'data/native-wallpapers' / sprite_id
        fixture_path(author / sprite_id, duplicate.parent, sprite_id)
        dump(duplicate / 'manifest.json', dict(load(duplicate / 'manifest.json'),
                                              id=sprite_id.upper(), name='后备重复包'))
        chosen_path = Path(catalogue(pipe)[sprite_id]['path'])
        assert chosen_path == library / sprite_id
        assert sprite_id.upper() not in catalogue(pipe)
        new_schema = load(chosen_path / 'settings.json')
        new_schema['groups'][0]['controls'][0]['default'] = 1.25
        new_schema['groups'][0]['controls'].append(slider('authorSpeed', '展开速度', 'effect.speed', 1, .5, 2))
        dump(chosen_path / 'settings.json', new_schema)
        request(pipe, 'reloadDirectories')
        upgraded = request(pipe, 'getPackSettings', id=sprite_id)
        assert upgraded['values']['authorGlow'] == .65 and upgraded['values']['authorSpeed'] == 1
        assert upgraded['revision'] > edits['revision']
        report['identity'] = {'configuredLibraryWinsCaseFoldCollision': True,
                              'rangeCompatibleValueSurvivesSchemaUpdate': upgraded['values']['authorGlow'],
                              'newControlDefault': upgraded['values']['authorSpeed'],
                              'profileRevision': upgraded['revision']}

    with player(root, pipe) as (restarted, _):
        persisted = request(pipe, 'getPackSettings', id=sprite_id)
        assert restarted['packId'] == sprite_id and persisted['values']['authorGlow'] == .65
        assert persisted['values']['authorBackground'] == .75 and persisted['values']['authorSpeed'] == 1
        assert request(pipe, 'testTrigger', code='Space')['handle'] >= 0
        report['restart'] = {'selection': restarted['packId'], 'values': persisted['values'],
                             'diagnosticKeyAfterRestart': True}
    dump(output / 'results.json', report)

    if include_integrated:
        snapshot = root / 'current-native-copy.exe'
        shutil.copyfile(PROJECT / NATIVE, snapshot)
        dump(data / 'app-settings.json', dict(prefs, wallpaperDir=str(library), gpuPreference='integrated'))
        with player(root, pipe, executable=snapshot) as (status, process):
            assert status['packId'] == sprite_id, status
            report['integratedTrial'] = {'nativeSHA256': digest(snapshot),
                'adapterReported': status.get('gpuAdapter'), 'preferenceReported': status.get('gpuPreference'),
                'sprite': stress(pipe, ('KeyF', 'Space'))}
            second = bounded_select(pipe, process, material_id)
            if 'status' in second:
                report['integratedTrial']['material'] = stress(pipe, ('KeyF', 'Space'))
            else:
                report['integratedTrial']['material'] = second

    # A material compiler or decoder stall must not prevent checking the resource-only route.
    dump(data / 'app-settings.json', dict(prefs, wallpaperDir=str(library)))
    report['independentEffekseerEffect'] = independent_effect(root, pipe, author, library, run_id)
    dump(output / 'results.json', report)
    control_id = 'qa-original-material-' + run_id
    control = library / control_id
    shutil.copytree(library / material_id, control)
    shipped_material = PAYLOAD / 'wallpapers-native/paintball-native/materials/PaintSplash_Native01.efkmat'
    shutil.copyfile(shipped_material, control / 'materials/PaintSplash_Native01.efkmat')
    dump(control / 'manifest.json', dict(load(control / 'manifest.json'), id=control_id,
                                        name='原样材质对照'))
    control_schema = load(control / 'settings.json')
    control_schema['groups'][0]['controls'][0]['binding']['name'] = 'RuntimePipeline'
    dump(control / 'settings.json', control_schema)
    tint_id = 'qa-tint-only-' + run_id
    report['material']['tintOnlyFixture'] = material_pack(library / tint_id, tint_id, rename_uniform=False)

    for pack_id, label in ((control_id, 'controlMaterial'), (tint_id, 'changedShaderOnly'),
                           (material_id, 'changedMaterial')):
        state_file = state_dir / 'native-player.json'
        dump(state_file, {'version': 1, 'packId': sprite_id, 'monitorDevice': 'OFFSCREEN_QA'})
        with player(root, pipe) as (_, process):
            result = bounded_select(pipe, process, pack_id)
            if 'status' in result:
                chosen = result.pop('status')
                assert chosen['packId'] == pack_id and chosen['effectVariantCount'] == 1, chosen
                assert not chosen['error'], chosen
                changed = request(pipe, 'setPackSettings', id=pack_id, values={'authorTint': 1.2})
                assert changed['values']['authorTint'] == 1.2, changed
                request(pipe, 'testTrigger', code='KeyF', count=3)
                time.sleep(.09)
                live = request(pipe, 'status')
                assert live['activeParticles'] > 0 and not live['error'], live
                result.update({'backend': chosen['renderer'], 'materialUniformStored': 1.2,
                               'activeParticles': live['activeParticles']})
            else:
                result['selectionOnDiskAfterAttempt'] = load(state_file)['packId']
            report['material'][label] = result
    return report


if __name__ == '__main__':
    cli = argparse.ArgumentParser()
    cli.add_argument('--output', type=Path, required=True)
    cli.add_argument('--integrated', action='store_true')
    args = cli.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='Action75ImportQA-') as scratch:
        result = run(Path(scratch), output, args.integrated)
    dump(output / 'results.json', result)
    print(output / 'results.json')
