"""Compile an original two-marker CPU effect with one author-defined input."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from background_controls import with_background
import subprocess
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw

BASE = Path(__file__).resolve().parent
PACK = BASE / "tidemark-bearing"


def child(node, key, value=None):
    e = ET.SubElement(node, key)
    if value is not None:
        e.text = str(value)
    return e


def main(editor):
    PACK.mkdir(exist_ok=True)
    project = ET.Element("EffekseerProject")
    dynamics = child(project, "Dynamic")
    inputs = child(dynamics, "Inputs")
    for value in (.5, 0, 0, 0):
        child(child(inputs, "DynamicInput"), "Input", value)
    eq = child(child(dynamics, "Equations"), "DynamicEquation")
    child(eq, "Name", "Horizontal displacement in local units")
    child(eq, "Code", "@O.x = @In0 * 60")
    nodes = child(child(project, "Root"), "Children")
    for name, dynamic, color, dims in (
        ("Tide gauge origin", False, (75, 221, 220), (5, 18)),
        ("Copper float", True, (255, 136, 62), (12, 10))
    ):
        n = child(nodes, "Node")
        child(n,"Name",name)
        child(n,"IsRendered","True")
        common = child(n,"CommonValues")
        child(child(common,"MaxGeneration"),"Value",1)
        life = child(common,"Life")
        for f in ("Center","Min","Max"):
            child(life,f,120)
        xyz = child(child(child(n,"LocationValues"),"Fixed"),"Location")
        for axis in "XYZ":
            child(xyz,axis,0)
        if dynamic:
            child(xyz,"DynamicEquation",0)
        scale = child(child(child(n,"ScalingValues"),"Fixed"),"Scale")
        for axis, val in zip("XYZ", (*dims,1)):
            child(scale,axis,val)
        draw = child(n,"DrawingValues")
        child(draw,"Type",2)
        rgba = child(child(draw,"Sprite"),"ColorAll_Fixed")
        for channel,val in zip("RGBA", (*color,255)):
            child(rgba,channel,val)
        child(n,"Children")
    child(project,"Version",3)
    child(project,"StartFrame",0)
    child(project,"EndFrame",120)
    child(project,"IsLoop","True")
    source = BASE / "tidemark.efkproj"
    ET.indent(project)
    ET.ElementTree(project).write(source,encoding="utf-8",xml_declaration=True)
    dest = PACK/"tidemark.efk"
    subprocess.run([editor,"-cui","-in",source,"-e",dest],cwd=BASE,check=True,
                   creationflags=subprocess.CREATE_NO_WINDOW)
    assert dest.read_bytes()[:4] == b"SKFE"
    manifest = dict(version=1,id=PACK.name,name="潮标 · 偏岸",kind="effekseer",
                    effect=dest.name,settings="settings.json",preview="cover.png")
    control = dict(id="bearing",label="浮标偏岸",type="slider",min=-1,max=1,step=.05,default=.5,
                   binding=dict(target="effect.dynamicInput",index=0))
    settings = dict(version=1,presentation="layered",
                    groups=[dict(id="tide",label="潮标方位",controls=[control])])
    with_background(settings)
    for name,data in (("manifest",manifest),("settings",settings)):
        (PACK/(name+".json")).write_text(json.dumps(data,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
    image = Image.new("RGB",(480,180),(10,25,34))
    draw = ImageDraw.Draw(image)
    draw.rectangle((237,66,243,114),fill=(75,221,220))
    draw.rectangle((290,80,314,100),fill=(255,136,62))
    image.save(PACK/"cover.png")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor",required=True)
    main(parser.parse_args().editor)
