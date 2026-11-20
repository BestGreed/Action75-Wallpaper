"""Build eight original CPU effects with the Effekseer 1.80.8 CLI."""
import argparse
import json
import math
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from background_controls import with_background
import shutil
import subprocess
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

BASE = Path(__file__).resolve().parent
SOURCE = BASE / "projects"
PACK = BASE / "lantern-almanac"
SHAPES = ("rose-letter", "gilded-willow", "tidal-orbit", "paper-aster",
          "porcelain-peony", "jade-palm", "prism-wheel", "festival-bouquet")
LABELS = ("玫瑰心笺", "金丝垂柳", "潮汐双环", "剪纸星菊", "青瓷牡丹", "翡翠棕榈", "棱镜风轮", "锦簇花束")
COLORS = (((255, 58, 105), (255, 158, 170)), ((255, 177, 54), (255, 222, 136)), ((43, 205, 249), (139, 142, 255)),
          ((255, 85, 45), (255, 207, 115)), ((172, 155, 255), (192, 248, 244)),
          ((47, 241, 162), (171, 255, 185)), ((252, 116, 206), (141, 149, 255)),
          ((255, 119, 139), (255, 218, 127)))


def put(parent, key, value=None):
    child = ET.SubElement(parent, key)
    if value is not None:
        child.text = str(value)
    return child


def fixed_range(parent, key, value):
    node = put(parent, key)
    for field in ("Center", "Min", "Max"):
        put(node, field, round(value, 5))


def vector_range(parent, key, xy):
    node = put(parent, key)
    for axis, value in zip("XYZ", (*xy, 0)):
        fixed_range(node, axis, value * 8)


def sprite(parent, name, origin, velocity, acceleration, diameter, rgb, life, offset,
           count=1, interval=1, fade=None, root_scale=False):
    node = put(parent, "Node")
    put(node, "Name", name)
    put(node, "IsRendered", "True")
    common = put(node, "CommonValues")
    put(put(common, "MaxGeneration"), "Value", count)
    put(common, "LocationEffectType", 1)
    put(common, "ScaleEffectType", 2 if root_scale else 0)
    put(common, "RotationEffectType", 0)
    fixed_range(common, "Life", life)
    fixed_range(common, "GenerationTime", interval)
    fixed_range(common, "GenerationTimeOffset", offset)
    location = put(node, "LocationValues")
    put(location, "Type", 1)
    pva = put(location, "PVA")
    for key, value in (("Location", origin), ("Velocity", velocity), ("Acceleration", acceleration)):
        vector_range(pva, key, value)
    scale = put(put(put(node, "ScalingValues"), "Fixed"), "Scale")
    for axis in "XYZ":
        put(scale, axis, diameter * 8)
    material = put(node, "RendererCommonValues")
    put(material, "ColorTexture", "spark.png")
    put(material, "AlphaBlend", 2)
    put(material, "FadeOutType", 1)
    put(put(material, "FadeOut"), "Frame", fade or max(3, life // 2))
    drawing = put(node, "DrawingValues")
    put(drawing, "Type", 2)
    color = put(put(drawing, "Sprite"), "ColorAll_Fixed")
    for key, value in zip("RGBA", (*rgb, 255)):
        put(color, key, value)
    return put(node, "Children")


def paths(kind):
    total = (44, 26, 48, 60, 60, 28, 48, 45)[kind]
    for i in range(total):
        t = math.tau * i / total
        if kind == 0:
            x = 8 * math.sin(t) ** 3
            y = .5 * (13*math.cos(t)-5*math.cos(2*t)-2*math.cos(3*t)-math.cos(4*t))
            yield (x/22, y/22), (-x*.00060, -y*.00060-.0018), 4, 47, i % 2
        elif kind == 1:
            u = (i // 2 + .5) / (total/2)
            side = 1 if i % 2 else -1
            yield (side * (.065+.37*u), .50-.29*u**1.65), (-side*.0018*u, -.020+.004*u), 3+i%3, 45+int(14*(1-u)), i//2 % 2
        elif kind == 2:
            a = math.tau*(i//2)/(total//2)
            x, y = (8*math.cos(a), 8*math.sin(a)) if i%2==0 else (10.7*math.cos(a), 3.0*math.sin(a)+3.8*math.cos(a))
            yield (x/24, y/24), (-x*.00052, -y*.00052-.0009), 3+4*(i%2), 47, i%2


        elif kind == 3:
            # Twelve tapered petals, each defined by a five-point scallop.
            petal, point = divmod(i, 5)
            r = (.22, .31, .46, .31, .22)[point]
            angle = math.tau*petal/12 + (point-2)*.075
            yield (r*math.cos(angle), r*math.sin(angle)), (0,-.0016), 3, 48, point==2
        elif kind == 4:
            shell, point = divmod(i, 20)
            angle = math.tau*(point+.5*shell)/20
            r = (.48, .35, .27)[shell] * (1+.12*math.cos(5*angle+shell))
            yield (r*math.cos(angle), r*math.sin(angle)), (0,-.0014), 3+4*shell, 46-shell*2, shell%2
        elif kind == 5:
            # A bright stem followed by seven palm fronds, with four tips per frond.
            frond, leaflet = divmod(i, 4)
            angle = (frond-3)*.41 + (leaflet-1.5)*.055
            speed = .45+.035*leaflet
            yield (speed*math.sin(angle), speed*math.cos(angle)), (0,-.016), 10, 53, frond%2
        elif kind == 6:
            blade, point = divmod(i, 8)
            angle = blade*math.tau/6
            r = .12+.045*point
            twist = .0015+.0008*point
            yield (r*math.cos(angle),r*math.sin(angle)), (-twist*math.sin(angle),twist*math.cos(angle)), 3+point, 47, blade%2
        else:
            # Five chrysanthemum heads open above five rising stems.
            stem, point = divmod(i, 9)
            angle = math.tau*point/9
            yield (.15*math.cos(angle),.14*math.sin(angle)+.04), (0,-.003), 11+(stem%2)*3, 39, stem%2


def emitter_origin(kind, i):
    if kind == 5:
        return (0, 3.4)
    if kind == 7:
        stem = i//9
        return ((stem-2)*2.85, 2.9+(2-abs(stem-2))*2.1)
    return (0, 0)


def build_project(kind):
    project = ET.Element("EffekseerProject")
    root = put(project, "Root")
    put(root, "Name", "Root")
    branches = put(root, "Children")
    palette = COLORS[kind]
    sprite(branches, "Ignition pearl", (0, 0), (0, 0), (0, 0), 4.1, (255, 245, 213), 10, 0, root_scale=True)
    if kind in (5, 7):
        for index in ([0] if kind == 5 else range(0,45,9)):
            x,y = emitter_origin(kind, index)
            stem = sprite(branches, "Rising stem", (0,0), (x/11,y/11), (0,0), 1.1, palette[1], 13, 0, root_scale=True)
            sprite(stem, "Stem glitter", (0,0), (0,0), (0,-.001), .7, palette[1], 15, 0, 12)
    for i, (velocity, acceleration, offset, life, tone) in enumerate(paths(kind)):
        children = sprite(branches, f"Comet {i+1:02}", emitter_origin(kind,i), velocity, acceleration, 1.03,
                          palette[tone], life, offset, root_scale=True)
        sprite(children, "Suspended sparks", (0, 0), (0, -.014), (0, -.0013), .68,
               palette[tone], 14, 1, count=life//3, interval=3, fade=12)
        if i%3==0:
            sprite(children, "Late cinder", (0, 0), (velocity[0]*.12, -.045), (0, -.003),
                   .82, (255, 218, 152), 24, life-4, fade=21)
    put(project, "Version", 3)
    put(project, "StartFrame", 0)
    put(project, "EndFrame", 90)
    put(project, "IsLoop", "True")
    file = SOURCE / (SHAPES[kind]+".efkproj")
    ET.indent(project)
    ET.ElementTree(project).write(file, encoding="utf-8", xml_declaration=True)
    return file


def write_json(name, data):
    if name == "settings.json":
        with_background(data)
    (PACK / name).write_text(json.dumps(data, ensure_ascii=False, indent=2)+"\n", encoding="utf-8")


def graphics():
    spark = Image.new("RGBA", (64, 64), "white")
    spark.putalpha(Image.frombytes("L", spark.size, bytes(
        round(255 * math.exp(-7*((x-31.5)**2+(y-31.5)**2)/32**2)) for y in range(64) for x in range(64))))
    spark.save(SOURCE / "spark.png")
    shutil.copyfile(SOURCE / "spark.png", PACK / "spark.png")
    cover = Image.new("RGB", (960, 360), (9, 12, 27))
    ink = ImageDraw.Draw(cover)
    for k, cx in enumerate((170, 480, 785)):
        for v, a, delay, life, tone in paths(k):
            age = (30, 40, 30)[k]-delay
            for j in range(max(0, age-14), age+1):
                x, y = [v[q]*j + .5*a[q]*j*j for q in (0,1)]
                px, py = cx+x*11, 175-y*11
                intensity = ((j-age+15)/15)**1.5
                rgb = tuple(round(val*intensity) for val in COLORS[k][tone])
                r = 2.2 if j==age else 1.25
                ink.ellipse((px-r, py-r, px+r, py+r), fill=rgb)
    cover.save(PACK / "cover.png")


def main(editor):
    PACK.mkdir(exist_ok=True)
    SOURCE.mkdir(exist_ok=True)
    graphics()
    outputs = []
    for kind, name in enumerate(SHAPES):
        source = build_project(kind)
        output = PACK / (name+".efk")
        subprocess.run([editor, "-cui", "-in", source, "-e", output], check=True, cwd=SOURCE,
                       creationflags=subprocess.CREATE_NO_WINDOW, timeout=90)
        if output.read_bytes()[:4] != b"SKFE":
            raise ValueError(output)
        outputs.append(output.name)
    write_json("manifest.json", dict(version=1, id=PACK.name, name="灯历 · 八式焰花", kind="effekseer",
               settings="settings.json", preview="cover.png", effect=outputs[0], effects=outputs, scale=1))
    controls = [dict(id="flower", label="花型", type="select", default=0,
                    options=[dict(value=0,label="组合")] + [dict(value=i+1,label=s,toggleId=key)
                    for i,(s,key) in enumerate(zip(LABELS,SHAPES))])]
    controls.extend(dict(id=key,label=s,type="toggle",default=True) for s,key in zip(LABELS,SHAPES))
    controls.extend([dict(id="shuffle",label="随机次序",type="toggle",default=False),
                     dict(id="spacing",label="每几键开花",type="slider",default=1,min=1,max=99,step=1,unit="键")])
    write_json("settings.json", dict(version=1, presentation="layered", groups=[
        dict(id="flowers",label="焰花播放",presentation="variant-playback",controls=controls,
             playback=dict(selection="flower",randomize="shuffle",interval="spacing",order=list(range(1,len(SHAPES)+1)))),
        dict(id="appearance",label="外观与节奏",controls=[
             dict(id="size",label="花冠大小",type="slider",default=1,min=.5,max=2,step=.05,
                  unit="×",binding=dict(target="effectScale")),
             dict(id="speed",label="展开速度",type="slider",default=1,min=.5,max=2,step=.05,
                  unit="×",binding=dict(target="effect.speed")),
             dict(id="brightness",label="焰花亮度",type="slider",default=1,min=.25,max=1,step=.05,
                  unit="×",binding=dict(target="effectBrightness"))])]))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    main(parser.parse_args().editor)
