"""Update Effekseer material shader chunks, preserving texture and uniform metadata."""
from pathlib import Path
import struct


def compile_material(material: Path, shader: Path):
    data = material.read_bytes()
    assert data[:4] == b"EFKM"
    output = bytearray(data[:16])
    offset = 16
    updated = 0
    while offset < len(data):
        tag, size = struct.unpack_from("<4sI", data, offset)
        payload = data[offset + 8:offset + 8 + size]
        offset += 8 + size
        if tag == b"GENE":
            code = shader.read_text(encoding="utf-8").encode("utf-8") + b"\0"
            payload = struct.pack("<I", len(code)) + code
            updated += 1
        output += struct.pack("<4sI", tag, len(payload)) + payload
    assert offset == len(data) and updated == 1
    material.write_bytes(output)


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    shaders = sorted((root / "native/windows-player/materials/paintball").glob("*.hlsl"))
    for shader in shaders:
        compile_material(root / "wallpapers-native/paintball-native/materials" /
                         (shader.stem + ".efkmat"), shader)
    print(f"Updated {len(shaders)} Paintball materials")
