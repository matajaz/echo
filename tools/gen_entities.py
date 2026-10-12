#!/usr/bin/env python3
"""A15 — HA entity catalogue compiler.

Reads catalogue/entities.yaml and generates:
  - main/ha_entities.generated.h : the compile-time C arrays ha_client.c uses
  - core/test/fixtures/ha_entities.json : a fixture for host tests

Validates: section counts (<=8), entity_id form `domain.object_id` (lowercase
+ digits/underscore), lengths (id<=79, name<=23), no duplicate ids. Any
violation aborts with a clear message and a non-zero exit (so a bad catalogue
fails generation, never produces a silently-wrong firmware). No third-party
deps: a tiny purpose-built parser handles this flat catalogue (no PyYAML).

Usage: tools/gen_entities.py            # generate (default paths)
       tools/gen_entities.py --check    # verify generated files are up to date
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOGUE = ROOT / "catalogue" / "entities.yaml"
HEADER = ROOT / "main" / "ha_entities.generated.h"
FIXTURE = ROOT / "core" / "test" / "fixtures" / "ha_entities.json"

MAX_PER_SECTION = {"temperature": 9, "battery": 8, "power": 6}
ROLES = {r: "HA_ROLE_" + r.upper() for r in (
    "none", "outdoor", "indoor", "solar_power", "grid_power", "grid_export_power",
    "grid_export_today", "grid_import_today", "price", "solar_today")}
ROLE_SECTION = {"outdoor": "temperature", "indoor": "temperature",
                **{r: "power" for r in ROLES if r not in ("none", "outdoor", "indoor")}}
MAX_ID = 79
MAX_NAME = 23
_ID_RE = re.compile(r"^[a-z0-9_]+\.[a-z0-9_]+$")


class CatalogueError(Exception):
    """Raised for any catalogue validation failure."""


def parse_catalogue(text: str) -> dict[str, list[dict[str, str]]]:
    """Parse the restricted catalogue format into {section: [{id,name}, ...]}.

    Supports exactly the structure in entities.yaml: a top-level `version:`,
    and `temperature:` / `battery:` lists of `- id:` / `name:` pairs. Comments
    (`#`) and blank lines are ignored. Deliberately strict — unknown shapes
    raise rather than being silently skipped.
    """
    sections: dict[str, list[dict[str, str]]] = {}
    cur_section: str | None = None
    cur_item: dict[str, str] | None = None
    version_seen = False
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if re.match(r"^version:\s*\d+\s*$", line):
            version_seen = True
            continue
        m = re.match(r"^([a-z_]+):\s*$", line)
        if m:
            cur_section = m.group(1)
            if cur_section not in ("temperature", "battery", "power"):
                raise CatalogueError(f"unknown section '{cur_section}'")
            sections.setdefault(cur_section, [])
            cur_item = None
            continue
        m = re.match(r"^\s*-\s*id:\s*(\S+)\s*$", line)
        if m:
            if cur_section is None:
                raise CatalogueError("entity listed before any section")
            cur_item = {"id": m.group(1), "name": ""}
            sections[cur_section].append(cur_item)
            continue
        m = re.match(r"^\s+name:\s*(.+?)\s*$", line)
        if m:
            if cur_item is None:
                raise CatalogueError(f"'name' without a preceding '- id': {line!r}")
            cur_item["name"] = m.group(1)
            continue
        m = re.match(r"^\s+(unit|role):\s*(\S+)\s*$", line)
        if m:
            if cur_item is None:
                raise CatalogueError(f"'unit' without a preceding '- id': {line!r}")
            cur_item[m.group(1)] = m.group(2)
            continue
        raise CatalogueError(f"unparseable line: {line!r}")
    if not version_seen:
        raise CatalogueError("missing top-level 'version:'")
    return sections


def validate(sections: dict[str, list[dict[str, str]]]) -> None:
    """Raise CatalogueError on any invalid entry; otherwise return cleanly."""
    all_roles = set()
    for sect, items in sections.items():
        limit = MAX_PER_SECTION[sect]
        if len(items) > limit:
            raise CatalogueError(f"{sect}: {len(items)} entries exceeds max {limit}")
        seen: set[str] = set()
        for it in items:
            eid, name = it["id"], it["name"]
            if not _ID_RE.match(eid):
                raise CatalogueError(f"{sect}: bad entity_id {eid!r} (want domain.object_id)")
            if len(eid) > MAX_ID:
                raise CatalogueError(f"{sect}: entity_id {eid!r} exceeds {MAX_ID} chars")
            if not name:
                raise CatalogueError(f"{sect}: {eid} has an empty name")
            if len(name) > MAX_NAME:
                raise CatalogueError(f"{sect}: name {name!r} exceeds {MAX_NAME} chars")
            if len(name.encode("utf-8")) > MAX_NAME:
                raise CatalogueError(f"{sect}: name exceeds {MAX_NAME} UTF-8 bytes")
            role = it.get("role", "none")
            if role not in ROLES or (role != "none" and ROLE_SECTION[role] != sect):
                raise CatalogueError(f"{sect}: invalid role {role!r}")
            if role != "none":
                if role in all_roles: raise CatalogueError(f"duplicate role {role!r}")
                all_roles.add(role)
            if sect == "power" and role == "none":
                raise CatalogueError("power entries require a semantic role")
            unit = it.get("unit", "C" if sect == "temperature" else "%")
            if sect != "power" and unit != ("C" if sect == "temperature" else "%"):
                raise CatalogueError(f"{sect}: unit must be canonical")
            expected = {"price": "SEK/kWh", "grid_import_today": "kWh", "grid_export_today": "kWh", "solar_today": "kWh"}.get(role, "W")
            if sect == "power" and unit != expected:
                raise CatalogueError(f"{role}: expected unit {expected}")
            if any(c in name for c in ('"', '\\')):
                raise CatalogueError(f"{sect}: quote in {eid!r}/{name!r}")
            if eid in seen:
                raise CatalogueError(f"{sect}: duplicate entity_id {eid!r}")
            seen.add(eid)


def render_header(sections: dict[str, list[dict[str, str]]]) -> str:
    text = '// GENERATED by tools/gen_entities.py. Edit catalogue/entities.yaml.\n#pragma once\n#include "ha_model.h"\n\n'
    text += 'typedef struct { const char *entity_id, *name, *unit; ha_role_t role; } ha_entity_def_t;\n'
    for sect, array, cap in (("temperature", "k_entities", "HA_MAX_SENSORS"), ("battery", "k_batt_entities", "HA_MAX_BATT"), ("power", "k_power_entities", "HA_MAX_POWER")):
        text += f"static const ha_entity_def_t {array}[] = {{\n"
        for item in sections.get(sect, []):
            unit = item.get("unit", "C" if sect == "temperature" else "%")
            text += f'    {{ "{item["id"]}", "{item["name"]}", "{unit}", {ROLES[item.get("role", "none")]} }},\n'
        text += '};\n'
        text += f'_Static_assert(sizeof({array}) / sizeof({array}[0]) <= {cap}, "{sect} capacity");\n\n'
    return text.rstrip() + "\n"


def main(argv: list[str]) -> int:
    """Generate (or --check) the header + fixture from the catalogue."""
    check = "--check" in argv[1:]
    try:
        sections = parse_catalogue(CATALOGUE.read_text())
        validate(sections)
    except (CatalogueError, OSError) as exc:
        print(f"catalogue error: {exc}", file=sys.stderr)
        return 2

    header = render_header(sections)
    fixture = json.dumps(sections, indent=2, ensure_ascii=True) + "\n"

    if check:
        ok = True
        for path, want in ((HEADER, header), (FIXTURE, fixture)):
            have = path.read_text() if path.exists() else ""
            if have != want:
                print(f"OUT OF DATE: {path} (run tools/gen_entities.py)", file=sys.stderr)
                ok = False
        if ok:
            print("generated files are up to date")
        return 0 if ok else 1

    HEADER.write_text(header)
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE.write_text(fixture)
    n_t = len(sections.get("temperature", []))
    n_b = len(sections.get("battery", []))
    n_p = len(sections.get("power", []))
    print(f"generated {HEADER.relative_to(ROOT)} ({n_t} temp, {n_b} battery, {n_p} power)")
    print(f"generated {FIXTURE.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
