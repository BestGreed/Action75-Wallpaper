"""Shared native-player bindings for the four procedural and sprite examples."""
import json
from pathlib import Path


def with_background(settings):
    for group in settings["groups"]:
        for control in group["controls"]:
            control.setdefault("surface", "foreground")
            if control["id"] == "brightness":
                control["appearance"] = "brightness"
            if control["type"] == "select" and group.get("presentation") == "variant-playback":
                control["appearance"] = "style"
    template = Path(__file__).with_name("background-controls.json")
    settings["groups"].append(json.loads(template.read_text(encoding="utf-8")))
    return settings
