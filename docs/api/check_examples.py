#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["jsonschema>=4.18", "referencing>=0.30"]
# ///
"""Check docs/api/examples against docs/api/schema.

Every file under examples/server and examples/client must validate against
the board→page or page→board schema; every file under examples/invalid/...
must NOT. Examples are compact, one message per file, exactly as the firmware
serializes them, so firmware tests can compare encoder output byte for byte.

    uv run docs/api/check_examples.py
"""

import json
import sys
from pathlib import Path

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

ROOT = Path(__file__).resolve().parent
SCHEMA_DIR = ROOT / "schema"
EXAMPLES = ROOT / "examples"

CLIENT_MAX_BYTES = 256


def load(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> int:
    schemas = {p.name: load(p) for p in SCHEMA_DIR.glob("*.schema.json")}
    registry = Registry().with_resources(
        (s["$id"], Resource.from_contents(s)) for s in schemas.values()
    )
    validators = {
        side: Draft202012Validator(schemas[f"{side}-message.schema.json"], registry=registry)
        for side in ("server", "client")
    }
    for schema in schemas.values():
        Draft202012Validator.check_schema(schema)

    failures = 0
    checked = 0
    for side, validator in validators.items():
        for expect_valid, folder in ((True, EXAMPLES / side), (False, EXAMPLES / "invalid" / side)):
            for path in sorted(folder.glob("*.json")):
                checked += 1
                raw = path.read_text(encoding="utf-8")
                text = raw.rstrip("\n")
                errors = list(validator.iter_errors(json.loads(text)))
                problems = []
                if expect_valid and errors:
                    problems += [e.message for e in errors]
                if not expect_valid and not errors:
                    problems.append("expected to be rejected, but validated")
                if expect_valid and text != json.dumps(json.loads(text), separators=(",", ":"), ensure_ascii=False):
                    problems.append("not compact (the firmware emits no whitespace)")
                if expect_valid and side == "client" and len(text.encode()) > CLIENT_MAX_BYTES:
                    problems.append(f"over the {CLIENT_MAX_BYTES}-byte client frame limit")
                rel = path.relative_to(ROOT)
                if problems:
                    failures += 1
                    for p in problems:
                        print(f"FAIL {rel}: {p}")
                else:
                    print(f"ok   {rel}")

    print(f"{checked} examples, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
