"""Regression tests for the installer payload and source archive inventory."""

import json
from pathlib import Path
import runpy
import tempfile
import unittest
from zipfile import ZipFile

from release_payload import ROOT, external_wallpaper_assets, inventory, release_files, stage_assets


SOURCE = runpy.run_path(str(ROOT / "tools/package-source.py"))


class ReleasePackaging(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="Action75PackagingTest-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name)
        for entry in SOURCE["SOURCE_ENTRIES"]:
            path = self.root / entry
            if (ROOT / entry).is_dir():
                path.mkdir(parents=True, exist_ok=True)
            else:
                self.write(entry, "source")
        self.write("apps/manager/Cargo.toml", '[package]\nversion = "0.3.1"\n')
        manifest = inventory()
        self.write("tools/release-assets.json", json.dumps(manifest))
        for file in manifest["files"]:
            self.write(file, "release asset")
        for pack_id, files in manifest["wallpapers"].items():
            for file in files:
                contents = json.dumps({"id": pack_id}) if file == "manifest.json" else "wallpaper asset"
                self.write(f"wallpapers-native/{pack_id}/{file}", contents)

    def write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def test_shared_inventory_excludes_personal_files_and_old_payload(self):
        self.write("wallpapers-native/personal-import/manifest.json", '{"id": "personal-import"}')
        self.write("wallpapers-native/paintball-native/personal-photo.png", "personal")
        self.write("release/payload/assets/retired.png", "stale prior release")
        self.write("apps/manager/vendor/egui-wgpu/src/lib.rs", "patched renderer")
        self.write("tools/vendor/compiler.exe", "local tool")
        self.write("tools/build-player.ps1", "build entry point")
        self.write("tools/build-temp/temp.obj", "build output")
        expected = {path.as_posix() for path in release_files(self.root)}

        staged = self.root / "fresh-payload"
        stage_assets(self.root, staged)
        self.assertEqual(
            {path.relative_to(staged).as_posix() for path in staged.rglob("*") if path.is_file()},
            expected,
        )
        output = SOURCE["write_archive"](self.root, self.root / "release")
        with ZipFile(output) as archive:
            asset_roots = ("assets/", "existing-resources/", "Licenses/", "wallpapers-native/")
            files = {name.split("/", 1)[1] for name in archive.namelist()}
            self.assertEqual(files & expected, expected - {p.as_posix() for p in external_wallpaper_assets(self.root)})
            self.assertFalse({name for name in files if name.startswith(asset_roots)} - expected)
            self.assertIn("apps/manager/vendor/egui-wgpu/src/lib.rs", files)
            self.assertNotIn("tools/vendor/compiler.exe", files)
            self.assertIn("tools/build-player.ps1", files)
            self.assertNotIn("tools/build-temp/temp.obj", files)
            self.assertFalse(any(name.startswith("release/") for name in files))
        self.assertTrue((self.root / "release/payload/assets/retired.png").is_file())

    def test_every_shipped_wallpaper_is_a_self_contained_native_pack(self):
        for pack_id, files in inventory()["wallpapers"].items():
            pack = ROOT / "wallpapers-native" / pack_id
            manifest = json.loads((pack / "manifest.json").read_bytes())
            self.assertEqual((manifest["id"], manifest["kind"], manifest["abi"]), (pack_id, "native", 1))
            for field in ("entry", "settings", "preview"):
                self.assertIn(manifest[field], files)
                self.assertTrue((pack / manifest[field]).is_file())
        black_hole = Path("black-hole/pack/wallpaper.dll")
        self.assertEqual((ROOT / "examples/native-wallpapers" / black_hole).read_bytes(),
                         (ROOT / "wallpapers-native/black-hole/wallpaper.dll").read_bytes())

    def test_missing_fireworks_manifest_fails_both_outputs(self):
        (self.root / "wallpapers-native/night-fireworks/manifest.json").unlink()
        staged = self.root / "fresh-payload"
        with self.assertRaises(FileNotFoundError):
            stage_assets(self.root, staged)
        with self.assertRaises(FileNotFoundError):
            SOURCE["write_archive"](self.root, self.root / "release")
        self.assertFalse(staged.exists())


if __name__ == "__main__":
    unittest.main()
