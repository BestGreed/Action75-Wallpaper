import json
import os
from pathlib import Path
from subprocess import CompletedProcess
import tempfile
import unittest
from unittest.mock import patch

from expand import Expander, encode_settings, native_check, read_json


HERE = Path(__file__).resolve().parent
BUILTINS = HERE / "builtins.json"
ROOT = HERE.parents[2]
PACKS = ROOT / "wallpapers-native"
NATIVE_CHECK = os.environ.get("ACTION75_SCHEMA_CHECKER")


class SettingsTemplateTests(unittest.TestCase):
    def test_every_builtin_expands_to_the_checked_in_settings(self):
        manifests = {path.parent.name: path for path in PACKS.glob("*/manifest.json")}
        self.assertEqual(set(read_json(BUILTINS)), set(manifests))
        for pack, manifest in sorted(manifests.items()):
            with self.subTest(pack=pack):
                expander = Expander()
                definition = read_json(manifest)
                expected = read_json(manifest.parent / definition["settings"])
                actual = expander.expand(f"{BUILTINS}#/{pack}")
                self.assertEqual(json.dumps(actual, sort_keys=True), json.dumps(expected, sort_keys=True))
                payload = encode_settings(actual)
                if NATIVE_CHECK:
                    native_check(Path(NATIVE_CHECK), payload, definition.get("kind"))

    @unittest.skipUnless(NATIVE_CHECK, "set ACTION75_SCHEMA_CHECKER for native schema validation")
    def test_both_sdk_examples_accept_author_controls_without_bindings(self):
        for example in ("key-game", "tidal-signal"):
            with self.subTest(example=example):
                pack = ROOT / "examples/native-wallpapers" / example / "pack"
                manifest = read_json(pack / "manifest.json")
                settings = read_json(pack / manifest["settings"])
                native_check(Path(NATIVE_CHECK), encode_settings(settings), manifest["kind"])

    def test_author_example_overrides_values_ranges_units_and_binding(self):
        expanded = Expander().expand(str(HERE / "amber-atlas.json"))
        controls = {item["id"]: item for group in expanded["groups"] for item in group["controls"]}
        self.assertEqual(controls["brightness"]["unit"], "%")
        self.assertEqual(controls["size"]["default"], 1.25)
        self.assertEqual(controls["backgroundColor"]["default"], [255, 238, 212])
        duration = controls["duration"]
        self.assertEqual((duration["min"], duration["max"]), (0.2, 2))
        self.assertEqual(duration["binding"]["target"], "sprite.duration")
        self.assertEqual(duration["binding"]["mapping"]["outputMax"], 1.6)
        payload = encode_settings(expanded)
        self.assertNotIn(b'"$include"', payload)
        self.assertNotIn(b'"$overrides"', payload)
        if NATIVE_CHECK:
            native_check(Path(NATIVE_CHECK), payload)

    def test_refuses_bad_includes_and_control_patches(self):
        with tempfile.TemporaryDirectory(prefix="action75-template-") as directory:
            recipe = Path(directory) / "recipe.json"
            cases = [
                ({"$include": "recipe.json"}, r"circular \$include"),
                ({"$include": "#/absent"}, "missing key"),
                ({"$include": "missing.json"}, "missing.json"),
                ({"groups": [], "$overrides": {"missing": {"default": 1}}}, "missing control"),
                ({"controls": [{"id": "same"}, {"id": "same"}], "$overrides": {}}, "duplicate control ID"),
                ({"$typo": 1}, "unknown directive"),
            ]
            for data, message in cases:
                with self.subTest(data=data):
                    recipe.write_text(json.dumps(data), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, message):
                        Expander().expand(str(recipe))

    def test_native_checker_must_acknowledge_the_submitted_file(self):
        for stdout in (
            "PASS dynamic schema and numeric mappings\nPackSettings: 21 tests passed.\n",
            f"PASS {HERE / 'settings.json'}\n",
        ):
            with self.subTest(stdout=stdout):
                result = CompletedProcess([], 0, stdout=stdout, stderr="")
                with patch("expand.subprocess.run", return_value=result):
                    with self.assertRaisesRegex(ValueError, "Rebuild action75_pack_settings_tests"):
                        native_check(Path(__file__), b'{"version":1,"groups":[]}')

    @unittest.skipUnless(NATIVE_CHECK, "set ACTION75_SCHEMA_CHECKER for native schema validation")
    def test_the_native_parser_rejects_an_unknown_renderer_binding(self):
        settings = Expander().expand(str(HERE / "amber-atlas.json"))
        settings["groups"][0]["controls"][0]["binding"] = {"target": "unknown.target"}
        payload = encode_settings(settings)
        with self.assertRaisesRegex(ValueError, "unknown.target"):
            native_check(Path(NATIVE_CHECK), payload)


if __name__ == "__main__":
    unittest.main(verbosity=2)
