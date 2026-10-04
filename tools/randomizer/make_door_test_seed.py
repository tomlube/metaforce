#!/usr/bin/env python3
"""
Writes a door lock test seed for the built-in randomizer: every blast shield and door color
around the start in Main Plaza, the special cases a short warp away, and every item from the
start. Pickups are copied from an existing seed. The logic isn't checked; it's a cheat seed.

Usage:
  python3 tools/randomizer/make_door_test_seed.py [--seeds <dir>] [--base <hash>]

--seeds defaults to %APPDATA%/Metaforce/randomizer/seeds. --base is the seed whose pickups are
copied, by default the newest one there. Arm "DOORTEST" in the randomizer window and start a
new game.
"""

import argparse
import json
import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOGIC = ROOT / "res" / "randomizer" / "prime1" / "logic.json"
HASH = "DOORTEST"

# (region, room, dock): (shieldType, blastShieldType or "", what it tests)
TEST_DOORS = {
    # Main Plaza, where the game starts
    ("Chozo Ruins", "Main Plaza", 0): ("Power Bomb", "Power Bomb", "Power Bomb blast shield (no lock-on)"),
    ("Chozo Ruins", "Main Plaza", 1): ("Super Missile", "Super Missile", "Super Missile blast shield"),
    # Dock 2 keeps its missile blast shield: the game's own is replaced by randomprime's
    ("Chozo Ruins", "Main Plaza", 3): ("Charge Beam", "Charge Beam", "Charge Beam blast shield"),
    # The rooms next to it
    ("Chozo Ruins", "Ruined Fountain Access", 0): ("Power Beam Only", "", "Power Beam Only door (behind the Power Bomb shield)"),
    ("Chozo Ruins", "Ruined Fountain Access", 1): ("Wavebuster", "Wavebuster", "Wavebuster blast shield"),
    ("Chozo Ruins", "Ruins Entrance", 0): ("Ice Beam", "", "Ice door (behind the Super Missile shield)"),
    ("Chozo Ruins", "Ruins Entrance", 1): ("Bomb", "Bomb", "Bomb blast shield (no lock-on)"),
    ("Chozo Ruins", "Nursery Access", 0): ("Flamethrower", "Flamethrower", "Flamethrower blast shield"),
    ("Chozo Ruins", "Nursery Access", 1): ("Plasma Beam", "", "Plasma door (behind the Charge Beam shield)"),
    ("Chozo Ruins", "Ruined Shrine Access", 0): ("Ice Spreader", "Ice Spreader", "Ice Spreader blast shield"),
    ("Chozo Ruins", "Ruined Fountain", 0): ("Super Missile", "", "Super Missile door color, no blast shield"),
    ("Chozo Ruins", "Ruined Fountain", 2): ("Disabled", "", "Permanently locked door"),
    # Special cases, a warp away
    ("Chozo Ruins", "Hive Totem", 0): ("Super Missile", "Super Missile", "tilted door (Hive Totem west case)"),
    # Hive Totem dock 1 keeps its missile blast shield: the other tilted case
    ("Chozo Ruins", "Tower of Light", 1): ("Ice Spreader", "Ice Spreader", "vertical door, ceiling"),
    ("Chozo Ruins", "Tower Chamber", 0): ("Charge Beam", "Charge Beam", "vertical door, floor"),
    ("Phendrana Drifts", "Ruined Courtyard", 3): ("Blue", "Power Bomb", "unpowered door"),
    ("Tallon Overworld", "Biotech Research Area 1", 1): ("Blue", "Flamethrower", "tilted and unpowered door"),
    ("Tallon Overworld", "Biohazard Containment", 1): ("Wave Beam", "Missile", "unpowered door, Wave color under a missile shield"),
}

MISSILE_SHIELD = "door/Missile Blast Shield (randomprime)"


def grant(item_type: int, amount: int = 1) -> dict:
    return {"type": item_type, "capacity": amount, "amount": amount}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seeds", type=Path, default=Path(os.environ.get("APPDATA", "")) / "Metaforce" / "randomizer" / "seeds")
    parser.add_argument("--base", help="seed to copy pickups from")
    args = parser.parse_args()

    bases = sorted((p for p in args.seeds.iterdir() if (p / "seed.json").is_file() and p.name != HASH),
                   key=lambda p: (p / "seed.json").stat().st_mtime, reverse=True)
    if args.base:
        bases = [args.seeds / args.base]
    if not bases:
        print(f"error: no seed in {args.seeds} to copy pickups from", file=sys.stderr)
        return 1
    base = json.loads((bases[0] / "seed.json").read_text())

    logic = json.loads(LOGIC.read_text())
    doors = []
    found = set()
    for region in logic["regions"]:
        for area in region["areas"]:
            for node in area["nodes"]:
                dock = node.get("dock")
                if dock is None or dock["type"] != "door":
                    continue
                key = (region["name"], area["name"], dock["index"])
                door = {"world": region["asset_id"], "area": area["asset_id"], "dock": dock["index"],
                        "name": f"{region['name']} / {area['name']} / {node['name']}"}
                if key in TEST_DOORS:
                    shield, blast, test = TEST_DOORS[key]
                    found.add(key)
                    doors.append({**door, "shield": shield, "blast_shield": blast, "weakness": test, "changed": True})
                elif dock["weakness"] == MISSILE_SHIELD:
                    # Like the generator: the game's own missile blast shields become randomprime's.
                    doors.append({**door, "shield": "Blue", "blast_shield": "Missile",
                                  "weakness": "Missile Blast Shield", "changed": False})
    missing = set(TEST_DOORS) - found
    if missing:
        print(f"error: no such docks: {sorted(missing)}", file=sys.stderr)
        return 1

    starting = []
    pickups = json.loads((ROOT / "res" / "randomizer" / "prime1" / "pickups.json").read_text())
    for item in pickups["standard"]:
        if item["name"] == "Missile Launcher":
            starting.append({"name": item["name"], "grant": grant(item["item_type"], 250)})
        elif item["name"] == "Power Bomb":
            starting.append({"name": item["name"], "grant": grant(item["item_type"], 8)})
        elif item["name"] == "Energy Tank":
            starting += [{"name": item["name"], "grant": grant(item["item_type"])}] * 14
        else:
            starting.append({"name": item["name"], "grant": grant(item["item_type"])})

    seed = {
        "format_version": 2,
        "hash": HASH,
        "seed": "door-lock-test",
        "settings": base.get("settings", {}),
        "start": {"name": "Chozo Ruins / Main Plaza / Door to Ruins Entrance", "world": 0x83F6FF6F,
                  "area": 0xD5CDB809, "random": False},
        "shuffled_artifacts": base["shuffled_artifacts"],
        "locations": base["locations"],
        "starting_items": starting,
        "spheres": [],
        "warnings": ["Door lock test seed: logic is not checked."],
        "door_locks": {"lock_on": True, "doors": doors},
    }
    out = args.seeds / HASH
    out.mkdir(parents=True, exist_ok=True)
    (out / "seed.json").write_text(json.dumps(seed, indent=1))
    print(f"Wrote {out / 'seed.json'} ({len(doors)} doors, pickups from {bases[0].name})")
    for key, (shield, blast, test) in TEST_DOORS.items():
        print(f"  {' / '.join(map(str, key))}: {test}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
