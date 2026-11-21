"""Regression tests for the installer payload and source archive inventory."""

import hashlib
import io
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from release_build import OUTPUTS, build_inputs, build_release, validate_receipt
from release_payload import ROOT, WORKER, digest, external_wallpaper_assets, inventory, release_files, stage_assets


SOURCE = runpy.run_path(str(ROOT / "tools/package-source.py"))
RESTORE = runpy.run_path(str(ROOT / "tools/restore-wallpaper-assets.py"))


class ReleasePackaging(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix="Action75PackagingTest-")
        self.addCleanup(scratch.cleanup)
        self.root = Path(scratch.name).resolve()
        for entry in SOURCE["SOURCE_ENTRIES"]:
            path = self.root / entry
            if (ROOT / entry).is_dir():
                self.write(path / "example.txt", "source")
            else:
                self.write(entry, "source")
        self.write("apps/manager/Cargo.toml", '[package]\nversion = "0.3.1"\n')
        self.write("apps/manager/Cargo.lock", "test lock")
        header = SOURCE["JSON_HEADER"]
        shutil.copyfile(ROOT / header, self.root / header)
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
            self.assertEqual((manifest["id"], manifest["kind"]), (pack_id, "native"))
            self.assertIn((manifest["abi"], manifest.get("renderer")), {(1, None), (1, "d3d11"), (2, "d3d12")})
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

    def test_offline_source_rearchive_with_sdk(self):
        author_dependencies = (
            "examples/author-wallpapers/sources/dx12/night-orbit-trio/vendor/nlohmann-LICENSE.txt",
            "examples/author-wallpapers/sources/dx12/night-orbit-trio/vendor/nlohmann/json.hpp",
            "examples/author-wallpapers/sources/dx12/soundings-echo/vendor/nlohmann/json.hpp",
        )
        for dependency in author_dependencies:
            self.write(dependency, "bundled author dependency")
        first = SOURCE["write_archive"](self.root, self.root / "release")
        unpacked = self.root / "unpacked"
        with ZipFile(first) as archive:
            archive.extractall(unpacked)
            files = {name: archive.read(name) for name in archive.namelist()}
        source = unpacked / "ACTION75-0.3.1"
        self.assertTrue((source / SOURCE["JSON_HEADER"]).is_file())
        self.assertTrue((source / "Licenses/nlohmann-json-LICENSE.txt").is_file())
        for dependency in author_dependencies:
            self.assertTrue((source / dependency).is_file(), dependency)
        self.assertFalse(any((source / path).exists() for path in external_wallpaper_assets(source)))
        with self.assertRaises(FileNotFoundError):
            release_files(source)
        second = SOURCE["write_archive"](source, self.root / "again")
        with ZipFile(second) as archive:
            self.assertEqual({name: archive.read(name) for name in archive.namelist()}, files)

    def test_new_source_shaders_are_not_treated_as_historical_external_assets(self):
        items = inventory(self.root)
        relative = Path("wallpapers-native/paintball-native/shaders/NewPixel.cso")
        items["wallpapers"]["paintball-native"].append("shaders/NewPixel.cso")
        self.write("tools/release-assets.json", json.dumps(items))
        self.write(relative, "test shader")
        self.assertIn(relative, release_files(self.root, include_external=False))

    def test_generated_textures_follow_the_complete_staged_pack(self):
        items = inventory(self.root)
        names = ["textures/baked/canvas.dds", "textures/baked/splash.dds"]
        items["generatedWallpaperFiles"] = {"paintball-native": names}
        self.write("tools/release-assets.json", json.dumps(items))
        pack = Path("wallpapers-native/paintball-native")
        generated = {pack / name for name in names}
        self.assertTrue(generated.isdisjoint(release_files(self.root)))
        self.write(pack / names[0], "canvas mip chain")
        with self.assertRaisesRegex(ValueError, "Incomplete generated assets"):
            release_files(self.root)
        self.write(pack / names[1], "splash mip chain")
        stage = self.root / "payload"
        stage_assets(self.root, stage)
        self.assertTrue(all((stage / file).is_file() for file in generated))
        self.assertTrue(generated.isdisjoint(release_files(self.root, include_external=False)))
        self.assertTrue(generated.isdisjoint(external_wallpaper_assets(self.root)))

    def test_staged_library_overrides_only_wallpaper_assets(self):
        library = self.root / "candidate-library"
        shutil.copytree(self.root / "wallpapers-native", library)
        binary = Path("afterimage/wallpaper.dll")
        (library / binary).write_bytes(b"verified DX12 wallpaper")
        staged = self.root / "payload"
        stage_assets(self.root, staged, wallpapers=library)
        relative = Path("wallpapers-native") / binary
        self.assertEqual((staged / relative).read_bytes(), b"verified DX12 wallpaper")
        self.assertEqual((self.root / relative).read_bytes(), b"wallpaper asset")
        archive = SOURCE["write_archive"](self.root, self.root / "release", wallpapers=library)
        with ZipFile(archive) as zip_file:
            self.assertEqual(zip_file.read(f"ACTION75-0.3.1/{relative.as_posix()}"),
                             b"verified DX12 wallpaper")

    def test_build_receipt_rejects_wrong_worker_and_version(self):
        receipt = self.root / "compiled/build.json"
        for relative, path in OUTPUTS.items():
            self.write(receipt.parent / path, str(relative))
        contents = build_inputs(self.root, None)
        contents["sha256"] = {
            relative.as_posix(): digest(receipt.parent / path) for relative, path in OUTPUTS.items()
        }
        self.write(receipt, json.dumps(contents))
        with patch("release_build.file_version", return_value="0.3.1"):
            validate_receipt(self.root, receipt)
            worker = receipt.parent / OUTPUTS[WORKER]
            original = worker.read_bytes()
            worker.unlink()
            with self.assertRaisesRegex(ValueError, "Action75WallpaperWorker"):
                validate_receipt(self.root, receipt)
            worker.write_bytes(b"worker copied from a different build")
            with self.assertRaisesRegex(ValueError, "Action75WallpaperWorker"):
                validate_receipt(self.root, receipt)
            worker.write_bytes(original)
        with patch("release_build.file_version", return_value="0.3.0"):
            with self.assertRaisesRegex(ValueError, "Wrong executable version"):
                validate_receipt(self.root, receipt)

    def test_failed_rebuild_invalidates_the_previous_receipt(self):
        receipt = self.root / "compiled/build.json"
        for relative, path in OUTPUTS.items():
            self.write(receipt.parent / path, str(relative))
        previous = build_inputs(self.root, None)
        previous["sha256"] = {
            relative.as_posix(): digest(receipt.parent / path) for relative, path in OUTPUTS.items()
        }
        self.write(receipt, json.dumps(previous))
        with patch("release_build.file_version", return_value="0.3.1"):
            validate_receipt(self.root, receipt)
        failed = subprocess.CalledProcessError(1, ["cargo", "build"])
        with patch("release_build.subprocess.run", side_effect=failed):
            with self.assertRaises(subprocess.CalledProcessError):
                build_release(receipt.parent, root=self.root, cargo="cargo")
        self.assertFalse(receipt.exists())

    def test_source_archive_rejects_redirected_inputs(self):
        path = self.root / "docs/redirected.md"
        self.write(path, "private file")
        real_resolve = Path.resolve

        def resolve(file, *, strict=False):
            if file == path:
                return self.root.parent / "unrelated.md"
            return real_resolve(file, strict=strict)

        output = self.root / "release/ACTION75-source-0.3.1.zip"
        self.write(output, "previous archive")
        with patch.object(Path, "resolve", resolve):
            with self.assertRaisesRegex(ValueError, "Source archive input redirects"):
                SOURCE["write_archive"](self.root, output.parent)
        self.assertEqual(output.read_text(), "previous archive")

    def run_asset_restore(self, files, archive_members, before_download=lambda: None):
        def restore_data(*args, **kwargs):
            before_download()
            return io.BytesIO(frozen)

        archive_data = io.BytesIO()
        with ZipFile(archive_data, "w") as archive:
            for name, value in archive_members.items():
                archive.writestr(name, value)
        frozen = archive_data.getvalue()
        run = RESTORE["main"]
        with patch.dict(run.__globals__, {
            "ROOT": self.root,
            "external_wallpaper_assets": lambda root: files,
            "urlopen": restore_data,
            "SHA256": hashlib.sha256(frozen).hexdigest(),
        }):
            run()

    def test_asset_restore_leaves_existing_file_created_during_download(self):
        relative = Path("wallpapers-native/fixture/a.txt")
        missing = Path("wallpapers-native/fixture/b.txt")
        self.run_asset_restore([relative, missing], {"fixture/a.txt": "archive", "fixture/b.txt": "restored"},
                               before_download=lambda: self.write(relative, "new local asset"))
        self.assertEqual((self.root / relative).read_text(), "new local asset")
        self.assertEqual((self.root / missing).read_text(), "restored")

    def test_asset_restore_rejects_missing_member_before_writing(self):
        first, second = (Path("wallpapers-native/fixture") / f"{name}.txt" for name in ("a", "b"))
        with self.assertRaisesRegex(ValueError, "Wallpaper archive is missing fixture/b.txt"):
            self.run_asset_restore([first, second], {"fixture/a.txt": "archive"})
        self.assertFalse((self.root / first).exists())


if __name__ == "__main__":
    unittest.main()
