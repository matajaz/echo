#!/usr/bin/env python3
"""Host tests for the A15 catalogue compiler (tools/gen_entities.py).

Asserts the generator FAILS LOUDLY on invalid catalogues (bad id form,
oversize name, duplicate id, too many entries, empty name, missing version)
and ACCEPTS a valid one. Run: python3 tools/test_gen_entities.py
"""
from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_entities import CatalogueError, parse_catalogue, validate  # noqa: E402

_PASS = 0
_FAIL = 0


def expect_error(name: str, text: str) -> None:
    global _PASS, _FAIL
    try:
        validate(parse_catalogue(text))
    except CatalogueError:
        _PASS += 1
        return
    _FAIL += 1
    print(f"  FAIL: {name} should have raised CatalogueError")


def expect_ok(name: str, text: str) -> None:
    global _PASS, _FAIL
    try:
        validate(parse_catalogue(text))
        _PASS += 1
    except CatalogueError as exc:
        _FAIL += 1
        print(f"  FAIL: {name} should have passed, raised: {exc}")


GOOD = """version: 1
temperature:
  - id: sensor.living_room
    name: Living
battery:
  - id: sensor.living_room_batt
    name: Living
"""

print("gen_entities validation:")
expect_ok("valid catalogue", GOOD)
expect_error("missing version", GOOD.replace("version: 1\n", ""))
expect_error("bad id form (no domain)", GOOD.replace("sensor.living_room\n", "livingroom\n"))
expect_error("uppercase id", GOOD.replace("sensor.living_room\n", "sensor.LivingRoom\n"))
expect_error("empty name", GOOD.replace("    name: Living\n", "    name: \n", 1))
expect_error("oversize name (>23)", GOOD.replace("    name: Living\n",
             "    name: ThisNameIsWayTooLongForThePanel\n", 1))
# duplicate id within a section
dup = """version: 1
temperature:
  - id: sensor.a
    name: A
  - id: sensor.a
    name: B
"""
expect_error("duplicate id", dup)
# >8 entries
toomany = "version: 1\ntemperature:\n" + "".join(
    f"  - id: sensor.s{i}\n    name: S{i}\n" for i in range(10))
expect_error("too many entries (>9)", toomany)

for section, limit in (("battery", 8), ("power", 6)):
    items = f"version: 1\n{section}:\n" + "".join(
        f"  - id: sensor.s{i}\n    name: S{i}\n" for i in range(limit + 1))
    expect_error(f"{section} over capacity", items)
expect_error("power missing role", "version: 1\npower:\n  - id: sensor.grid\n    name: Grid\n    unit: W\n")
expect_error("power wrong unit", "version: 1\npower:\n  - id: sensor.grid\n    name: Grid\n    role: grid_power\n    unit: kWh\n")
expect_ok("power renamed, role stable", "version: 1\npower:\n  - id: sensor.grid\n    name: Arbitrary name\n    role: grid_power\n    unit: W\n")
expect_error("unicode too many bytes", GOOD.replace("name: Living", "name: " + "å" * 12))
expect_error("unescaped backslash", GOOD.replace("name: Living", "name: Living\\room"))
print(f"=== {_PASS} passed, {_FAIL} failed ===")
sys.exit(1 if _FAIL else 0)
