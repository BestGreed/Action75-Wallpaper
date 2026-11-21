"""Expand an author's JSON recipe into a standalone Action75 settings.json."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


MAX_BYTES = 32 * 1024
DIRECTIVES = {"$include", "$overrides"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def invalid_constant(value):
    raise ValueError(f"{value} is not valid JSON")


def read_json(path):
    try:
        return json.loads(
            path.read_text(encoding="utf-8-sig"),
            object_pairs_hook=unique_object,
            parse_constant=invalid_constant,
        )
    except (OSError, ValueError) as error:
        raise ValueError(f"{path}: {error}") from error


def at_pointer(value, pointer):
    if not pointer:
        return value
    require(pointer.startswith("/"), "fragment must start with /")
    for token in pointer.split("/")[1:]:
        require(not re.search(r"~(?![01])", token), f"invalid pointer token {token!r}")
        key = token.replace("~1", "/").replace("~0", "~")
        if isinstance(value, list):
            require(re.fullmatch(r"0|[1-9][0-9]*", key), f"invalid array index {key!r}")
            index = int(key)
            require(index < len(value), f"array index {index} is out of range")
            value = value[index]
        else:
            require(isinstance(value, dict) and key in value, f"missing key {key!r}")
            value = value[key]
    return value


class Expander:
    def __init__(self):
        self.sources = {}

    def expand(self, source):
        return self._include(source, Path.cwd(), ())

    def _include(self, reference, directory, stack):
        require(isinstance(reference, str) and reference, "$include must be a file reference")
        filename, _, pointer = reference.partition("#")
        require(filename or stack, "the entry recipe must name a JSON file")
        path = (directory / filename).resolve() if filename else stack[-1][0]
        item = (path, pointer)
        require(item not in stack, "circular $include at " + reference)
        if path not in self.sources:
            self.sources[path] = read_json(path)
        try:
            fragment = at_pointer(self.sources[path], pointer)
        except ValueError as error:
            raise ValueError(f"{path}#{pointer}: {error}") from error
        return self._expand(fragment, path.parent, (*stack, item))

    def _expand(self, node, directory, stack):
        if isinstance(node, list):
            return [self._expand(value, directory, stack) for value in node]
        if not isinstance(node, dict):
            return node

        for key in node:
            require(not key.startswith("$") or key in DIRECTIVES, f"unknown directive {key!r}")
        if "$include" in node:
            base = self._include(node["$include"], directory, stack)
            if len(node) == 1:
                return base
            require(isinstance(base, dict), "overriding fields requires an object $include")
        else:
            base = {}

        for key, value in node.items():
            if key not in DIRECTIVES:
                base[key] = self._expand(value, directory, stack)
        if "$overrides" in node:
            overrides = self._expand(node["$overrides"], directory, stack)
            apply_overrides(base, overrides)
        return base


def apply_overrides(panel, overrides):
    require(isinstance(overrides, dict), "$overrides must map control IDs to objects")
    groups = [panel] if "controls" in panel else panel.get("groups")
    require(isinstance(groups, list), "$overrides belongs to a panel or group")
    controls = {}
    for group in groups:
        require(isinstance(group, dict) and isinstance(group.get("controls"), list),
                "$overrides needs expanded groups and controls")
        for control in group["controls"]:
            require(isinstance(control, dict) and isinstance(control.get("id"), str),
                    "$overrides needs controls with IDs")
            control_id = control["id"]
            require(control_id not in controls, f"$overrides cannot use duplicate control ID {control_id!r}")
            controls[control_id] = control
    for control_id, fields in overrides.items():
        require(control_id in controls, f"$overrides refers to missing control {control_id!r}")
        require(isinstance(fields, dict) and "id" not in fields,
                f"$overrides for {control_id!r} must keep its ID")
        controls[control_id].update(fields)


def encode_settings(settings):
    require(isinstance(settings, dict) and type(settings.get("version")) is int
            and settings["version"] == 1 and isinstance(settings.get("groups"), list),
            "expanded settings require version 1 and a groups array")
    payload = (json.dumps(settings, ensure_ascii=False, allow_nan=False, indent=2) + "\n").encode("utf-8")
    if len(payload) > MAX_BYTES:
        payload = (json.dumps(settings, ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n").encode("utf-8")
    require(len(payload) <= MAX_BYTES, "settings exceed the player's 32 KiB limit")
    return payload


def native_check(validator, payload, kind=None):
    with tempfile.TemporaryDirectory(prefix="action75-schema-") as scratch:
        path = Path(scratch) / "settings.json"
        path.write_bytes(payload)
        path = path.resolve(strict=True)
        command = [str(validator.resolve(strict=True)), "--check-settings"]
        if kind:
            command.extend(("--kind", kind))
        result = subprocess.run(
            [*command, str(path)], capture_output=True, encoding="utf-8", errors="replace",
        )
    require(result.returncode == 0, result.stderr.strip() or "native settings check failed")
    require(
        f"PASS {path}" in result.stdout.splitlines(),
        "Native checker did not report PASS for the submitted file. "
        "Rebuild action75_pack_settings_tests with --check-settings support.",
    )


def write_output(output, payload):
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output.parent, suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
    try:
        temporary.write_bytes(payload)
        temporary.replace(output)
    finally:
        temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", help="recipe.json or recipe.json#/fragment")
    parser.add_argument("-o", "--output", type=Path, help="write standalone settings.json")
    parser.add_argument("--compare", type=Path, help="require exact JSON values and array order")
    parser.add_argument("--native-check", type=Path, metavar="EXE",
                        help="also validate with action75_pack_settings_tests")
    parser.add_argument("--kind", help="manifest kind used by --native-check, for example native")
    args = parser.parse_args()
    if args.kind and not args.native_check:
        parser.error("--kind requires --native-check")
    try:
        expander = Expander()
        settings = expander.expand(args.source)
        payload = encode_settings(settings)
        if args.compare:
            expected = read_json(args.compare)
            require(json.dumps(settings, sort_keys=True) == json.dumps(expected, sort_keys=True),
                    f"expanded settings differ from {args.compare}")
        if args.native_check:
            native_check(args.native_check, payload, args.kind)
        if args.output:
            require(args.output.resolve() not in expander.sources, "the output would overwrite a recipe source")
            write_output(args.output, payload)
        elif not args.compare and not args.native_check:
            sys.stdout.buffer.write(payload)
    except (OSError, ValueError, RecursionError) as error:
        parser.exit(1, f"error: {error}\n")


if __name__ == "__main__":
    main()
