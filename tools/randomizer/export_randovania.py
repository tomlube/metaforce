#!/usr/bin/env python3
"""
Exports the data Metaforce's built-in randomizer needs from Randovania and randomprime.

Randovania (https://github.com/randovania/randovania) provides the Metroid Prime logic
database, pickup database and starter preset. randomprime
(https://github.com/randovania/randomprime) provides the mapping from Randovania pickup
indices to script objects in the game data, and the serialized pickup objects used to give
a pickup another item's model.

Usage:
  python3 tools/randomizer/export_randovania.py \
      --randovania ~/src/randovania --randomprime ~/src/randomprime \
      --disc ~/prime/files

--disc points at the files directory of an extracted game disc (for example from
`nodtool extract`). It provides the size and facing of every dock, which the room randomizer
needs to pair doors that line up. Without it, the dock shapes of the previous logic.json are
kept, and if there are none the room randomizer is unavailable.

Outputs:
  res/randomizer/prime1/logic.json     compact logic database
  res/randomizer/prime1/pickups.json   item pool definitions and preset defaults
  res/randomizer/prime1/door_assets/   randomprime's door and blast shield textures
  res/randomizer/prime1/pickup_assets/ randomprime's pickup model textures and models
  src/Metaforce/Randomizer/PickupTables.cpp
  src/Metaforce/Randomizer/DoorTables.cpp
"""

import argparse
import json
import math
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_RES = ROOT / "res" / "randomizer" / "prime1"
OUT_TABLES = ROOT / "src" / "Metaforce" / "Randomizer" / "PickupTables.cpp"
OUT_DOOR_TABLES = ROOT / "src" / "Metaforce" / "Randomizer" / "DoorTables.cpp"
OUT_ASSETS = OUT_RES / "door_assets"
OUT_PICKUP_ASSETS = OUT_RES / "pickup_assets"

LOGIC_VERSION = 3

# Pak order as listed in randomprime's ROOM_INFO.
RANDOMPRIME_PAKS = [
    "Metroid1.pak",
    "Metroid2.pak",
    "Metroid3.pak",
    "Metroid4.pak",
    "metroid5.pak",
    "Metroid6.pak",
    "Metroid7.pak",
    "Metroid8.pak",
]


# ---------------------------------------------------------------------------
# Requirements
# ---------------------------------------------------------------------------


def compact_requirement(req: dict):
    """Requirement JSON -> compact list form.

    ["and", [...]] / ["or", [...]] / ["r", type, name, amount, negate] / ["t", template]
    Comments are dropped. Trivially true and false requirements are ["and", []] and ["or", []].
    """
    kind = req["type"]
    data = req["data"]
    if kind in ("and", "or"):
        items = [compact_requirement(item) for item in data["items"]]
        if len(items) == 1:
            return items[0]
        return [kind, items]
    if kind == "resource":
        return ["r", data["type"], data["name"], data["amount"], bool(data["negate"])]
    if kind == "template":
        return ["t", data]
    raise ValueError(f"Unsupported requirement type {kind}")


# ---------------------------------------------------------------------------
# Logic database
# ---------------------------------------------------------------------------


def export_logic(prime1: Path, dock_shapes: dict | None) -> dict:
    db_dir = prime1 / "logic_database"
    header = json.loads((db_dir / "header.json").read_text())
    rdb = header["resource_database"]

    resources = {
        "items": [
            {
                "name": name,
                "long_name": item["long_name"],
                "max": item["max_capacity"],
                "id": item["extra"].get("item_id", -1),
            }
            for name, item in rdb["items"].items()
        ],
        "events": [{"name": name, "long_name": ev["long_name"]} for name, ev in rdb["events"].items()],
        "tricks": [
            {
                "name": name,
                "long_name": trick["long_name"],
                "description": trick["description"],
                "levels": header["used_trick_levels"].get(name, []),
            }
            for name, trick in rdb["tricks"].items()
        ],
        "damage": [{"name": name, "long_name": dmg["long_name"]} for name, dmg in rdb["damage"].items()],
        "misc": [{"name": name, "long_name": misc["long_name"]} for name, misc in rdb["misc"].items()],
    }

    damage_reductions = {
        entry["name"]: [{"item": r["name"], "multiplier": r["multiplier"]} for r in entry["reductions"]]
        for entry in rdb["damage_reductions"]
    }

    templates = {
        name: compact_requirement(template["requirement"]) for name, template in rdb["requirement_template"].items()
    }

    dock_weaknesses = {}
    dock_types = {}
    for type_name, dock_type in header["dock_type_database"]["types"].items():
        for weakness_name, weakness in dock_type["items"].items():
            lock = weakness.get("lock")
            extra = weakness.get("extra", {})
            dock_weaknesses[f"{type_name}/{weakness_name}"] = {
                "open": compact_requirement(weakness["requirement"]),
                "lock": compact_requirement(lock["requirement"]) if lock else None,
                "lock_type": lock["lock_type"] if lock else None,
                # What randomprime turns the door into: its shield color and blast shield.
                "shield": extra.get("shieldType"),
                "blast_shield": extra.get("blastShieldType"),
                "unsafe": bool(weakness.get("unsafe_target_in_distributor_wtw", False)),
            }
        # Randovania's door lock randomizer settings for the type.
        distributor = dock_type.get("weakness_distributor")
        if distributor is not None:
            def names(weaknesses: list[str]) -> list[str]:
                return [f"{type_name}/{name}" for name in weaknesses]

            dock_types[type_name] = {
                "unlocked": f"{type_name}/{distributor['unlocked']}",
                "locked": f"{type_name}/{distributor['locked']}" if distributor["locked"] else None,
                "change_from": names(distributor["change_from"]),
                "change_to": names(distributor["change_to"]),
                "label": distributor["ui_label"],
            }

    regions = []
    for region_file in header["regions"]:
        region = json.loads((db_dir / region_file).read_text())
        areas = []
        for area_name, area in region["areas"].items():
            nodes = []
            for node_name, node in area["nodes"].items():
                if "default" not in node["layers"]:
                    continue
                out = {
                    "name": node_name,
                    "type": node["node_type"],
                    "heal": node["heal"],
                    "connections": {
                        target: compact_requirement(req) for target, req in node["connections"].items()
                    },
                }
                if node["valid_starting_location"]:
                    out["start"] = True
                world_pos = node["extra"].get("world_position")
                if world_pos is not None:
                    out["pos"] = world_pos
                if node["node_type"] == "pickup":
                    out["index"] = node["pickup_index"]
                    out["category"] = node["location_category"]
                elif node["node_type"] == "event":
                    out["event"] = node["event_name"]
                elif node["node_type"] == "dock":
                    target = node["default_connection"]
                    dock_index = node["extra"].get("dock_index")
                    out["dock"] = {
                        "type": node["dock_type"],
                        "index": dock_index,
                        "nonstandard": bool(node["extra"].get("nonstandard", False)),
                        "weakness": f"{node['dock_type']}/{node['default_dock_weakness']}",
                        "target": [target["region"], target["area"], target["node"]],
                        "open": compact_requirement(node["override_default_open_requirement"])
                        if node["override_default_open_requirement"]
                        else None,
                        "lock": compact_requirement(node["override_default_lock_requirement"])
                        if node["override_default_lock_requirement"]
                        else None,
                        "exclude": bool(node.get("exclude_from_dock_rando", False)),
                        "incompatible": [
                            f"{node['dock_type']}/{name}" for name in node.get("incompatible_dock_weaknesses", [])
                        ],
                    }
                    shape = (dock_shapes or {}).get((area["extra"].get("asset_id", 0), dock_index))
                    if shape is not None:
                        out["dock"]["shape"] = shape
                nodes.append(out)
            areas.append(
                {
                    "name": area_name,
                    "asset_id": area["extra"].get("asset_id", 0),
                    "default_node": area["default_node"],
                    "save_station": bool(area["extra"].get("unlocked_save_station", False)),
                    "nodes": nodes,
                }
            )
        regions.append({"name": region["name"], "asset_id": region["extra"].get("asset_id", 0), "areas": areas})

    start = header["starting_location"]
    return {
        "version": LOGIC_VERSION,
        "resources": resources,
        "damage_reductions": damage_reductions,
        "templates": templates,
        "dock_weaknesses": dock_weaknesses,
        "dock_types": dock_types,
        "starting_location": [start["region"], start["area"], start["node"]],
        "victory": compact_requirement(header["victory_condition"]),
        "regions": regions,
    }


# ---------------------------------------------------------------------------
# Dock shapes from the game disc
# ---------------------------------------------------------------------------

WORLD_PAKS = ["Metroid1.pak", "Metroid2.pak", "Metroid3.pak", "Metroid4.pak", "metroid5.pak", "Metroid6.pak",
              "Metroid7.pak", "Metroid8.pak"]


class _Reader:
    def __init__(self, data: bytes, offset: int = 0):
        self.data, self.offset = data, offset

    def unpack(self, fmt: str):
        values = struct.unpack_from(">" + fmt, self.data, self.offset)
        self.offset += struct.calcsize(">" + fmt)
        return values if len(values) > 1 else values[0]

    def skip_array(self, element_size: int) -> None:
        count = self.unpack("I")
        self.offset += count * element_size


def _pak_resources(data: bytes, fourcc: bytes) -> list[bytes]:
    r = _Reader(data, 8)
    for _ in range(r.unpack("I")):
        r.offset += 8
        r.skip_array(1)
    out = []
    for _ in range(r.unpack("I")):
        compressed, kind, _res_id, size, offset = r.unpack("I4sIII")
        if kind == fourcc:
            raw = data[offset : offset + size]
            out.append(zlib.decompress(raw[4:]) if compressed else raw)
    return out


def _transform(xf, p):
    return [xf[r * 4] * p[0] + xf[r * 4 + 1] * p[1] + xf[r * 4 + 2] * p[2] + xf[r * 4 + 3] for r in range(3)]


def read_dock_shapes(files: Path) -> dict:
    """(MREA asset id, dock index) -> [width, height, facing].

    Width and height are the dock plane's edge lengths. Facing is 0 for a wall dock, 1 for a dock
    in a ceiling, -1 for one in a floor and 2 for a tilted wall dock, from the plane normal the
    game tests crossings with, which points out of the dock's room.
    """
    shapes = {}
    for pak in WORLD_PAKS:
        for mlvl in _pak_resources((files / pak).read_bytes(), b"MLVL"):
            r = _Reader(mlvl)
            magic, _version, _strg, _savw, _sky = r.unpack("IIIII")
            if magic != 0xDEAFBABE:
                raise ValueError(f"{pak}: bad MLVL")
            r.skip_array(11)  # memory relays
            area_count = r.unpack("I")
            r.unpack("I")
            for _ in range(area_count):
                r.unpack("I")  # name
                xf = r.unpack("12f")
                r.offset += 24  # bounds
                mrea, _area_id = r.unpack("II")
                r.skip_array(2)  # attached areas
                r.skip_array(8)  # dependencies (unused)
                r.skip_array(8)  # dependencies
                r.skip_array(4)  # layer dependency offsets
                for dock_index in range(r.unpack("I")):
                    r.skip_array(8)  # connections
                    points = [_transform(xf, r.unpack("3f")) for _ in range(r.unpack("I"))]
                    if len(points) < 3:
                        continue
                    a, b, c = points[0], points[1], points[2]
                    u = [b[i] - a[i] for i in range(3)]
                    v = [c[i] - a[i] for i in range(3)]
                    n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]]
                    length = math.sqrt(sum(x * x for x in n)) or 1.0
                    nz = n[2] / length
                    if nz > 0.7:
                        facing = 1
                    elif nz < -0.7:
                        facing = -1
                    elif abs(nz) < 0.02:
                        facing = 0
                    else:
                        facing = 2  # tilted wall dock, which can't line up with any other

                    shapes[(mrea, dock_index)] = [round(math.dist(a, b), 2), round(math.dist(b, c), 2), facing]
    return shapes


def read_existing_dock_shapes(logic_path: Path) -> dict | None:
    """Dock shapes from a previous export, for re-exporting without the game disc."""
    if not logic_path.is_file():
        return None
    logic = json.loads(logic_path.read_text())
    shapes = {}
    for region in logic["regions"]:
        for area in region["areas"]:
            for node in area["nodes"]:
                dock = node.get("dock")
                if dock is not None and "shape" in dock:
                    shapes[(area["asset_id"], dock["index"])] = dock["shape"]
    return shapes or None


# ---------------------------------------------------------------------------
# Pickups
# ---------------------------------------------------------------------------

ARTIFACTS = ["Truth", "Strength", "Elder", "Wild", "Lifegiver", "Warrior", "Chozo", "Nature", "Sun", "World", "Spirit", "Newborn"]


def export_pickups(prime1: Path, logic: dict) -> dict:
    pickup_db = json.loads((prime1 / "pickup_database" / "pickup-database.json").read_text())
    preset = json.loads((prime1 / "presets" / "starter_preset.rdvpreset").read_text())["configuration"]
    items = {item["name"]: item for item in logic["resources"]["items"]}
    standard_state = preset["standard_pickup_configuration"]["pickups_state"]
    ammo_state = preset["ammo_pickup_configuration"]["pickups_state"]

    def item_type(name: str) -> int:
        item_id = items[name]["id"]
        return item_id if 0 <= item_id < 41 else -1

    standard = []
    for name, pickup in pickup_db["standard_pickups"].items():
        progression = pickup.get("progression", [])
        ammo = pickup.get("ammo", [])
        state = standard_state.get(name, {})
        # In-game effect: the first real item among progression and ammo.
        grant_type = -1
        for res in progression + ammo:
            if item_type(res) >= 0:
                grant_type = item_type(res)
                break
        standard.append(
            {
                "name": name,
                "category": pickup["gui_category"],
                "major": pickup.get("preferred_location_category", "major") == "major",
                "model": pickup["model_name"],
                "item_type": grant_type,
                "progression": progression,
                "ammo": ammo,
                "default_shuffled": state.get("num_shuffled_pickups", 0),
                "default_starting": state.get("num_included_in_starting_pickups", 0),
                "default_ammo": state.get("included_ammo", []),
                "hidden": bool(pickup.get("hide_from_gui", False)),
            }
        )

    ammo_pickups = []
    for name, pickup in pickup_db["ammo_pickups"].items():
        state = ammo_state.get(name, {})
        resource = pickup["items"][0]
        refill = items[resource]["id"] >= 1000
        extra = next(i for i in logic["resources"]["items"] if i["name"] == resource)
        ammo_pickups.append(
            {
                "name": name,
                "category": pickup["gui_category"],
                "model": pickup["model_name"],
                "resource": resource,
                "item_type": item_type(resource) if not refill else _refill_type(prime1, resource),
                "refill": refill,
                "default_count": state.get("pickup_count", 0),
                "default_ammo": state.get("ammo_count", [0])[0],
            }
        )
        del extra

    artifacts = [
        {"name": f"Artifact of {name}", "resource": name, "item_type": item_type(name), "model": f"Artifact of {name}"}
        for name in ARTIFACTS
    ]

    return {
        "standard": standard,
        "ammo": ammo_pickups,
        "artifacts": artifacts,
        "defaults": {
            "artifact_target": preset["artifact_target"],
            "artifact_required": preset["artifact_required"],
            "energy_per_tank": preset["energy_per_tank"],
            "damage_strictness": preset["damage_strictness"],
            "heat_damage": preset["heat_damage"],
        },
    }


def _refill_type(prime1: Path, resource: str) -> int:
    header = json.loads((prime1 / "logic_database" / "header.json").read_text())
    return header["resource_database"]["items"][resource]["extra"]["refill_id"]


# ---------------------------------------------------------------------------
# randomprime tables
# ---------------------------------------------------------------------------

ROOM_RE = re.compile(r"room_id: ResId::<res_id::MREA>::new\(0x([0-9A-Fa-f]+)\)")
PICKUP_RE = re.compile(
    r"PickupLocation \{\s*"
    r"location: ScriptObjectLocation \{ layer: (\d+), instance_id: (\d+) \},\s*"
    r"attainment_audio: ScriptObjectLocation \{ layer: (\d+), instance_id: (\d+) \},\s*"
    r"hudmemo: ScriptObjectLocation \{ layer: (\d+), instance_id: (\d+) \},\s*"
    r"memory_relay: ScriptObjectLocation \{ layer: (\d+), instance_id: (\d+) \},\s*"
    r"position: \[([^\]]+)\]",
    re.S,
)


def parse_room_pickups(meta: str) -> dict[int, list[dict]]:
    rooms = {}
    room_starts = [(m.start(), int(m.group(1), 16)) for m in ROOM_RE.finditer(meta)]
    room_starts.append((len(meta), None))
    for (start, mrea), (end, _) in zip(room_starts, room_starts[1:]):
        body = meta[start:end]
        rooms[mrea] = [
            {
                "pickup": int(m.group(2)),
                "audio": int(m.group(4)),
                "hudmemo": int(m.group(6)),
                "relay": int(m.group(8)),
                "position": [float(v) for v in m.group(9).split(",")],
            }
            for m in PICKUP_RE.finditer(body)
        ]
    return rooms


DOOR_RE = re.compile(
    r"DoorLocation \{\s*"
    r"door_location: (None|Some\(ScriptObjectLocation \{ layer: \d+, instance_id: (\d+) \}\)),\s*"
    r"door_rotation: (None|Some\(\[([^\]]+)\]\)),\s*"
    r"door_force_locations: &\[([^\]]*)\],\s*"
    r"door_shield_locations: &\[([^\]]*)\],\s*"
    r"dock_number: (\d+),\s*"
    r"dock_position: \[([^\]]+)\],",
    re.S,
)
INSTANCE_RE = re.compile(r"instance_id: (\d+)")

# Docks whose door is in a floor or ceiling, which randomprime gives the vertical shield models.
# From randomprime's patches.rs.
VERTICAL_DOCKS = {
    (0x11BD63B7, 0),  # Tower Chamber
    (0x0D72F1F7, 1),  # Tower of Light
    (0xFB54A0CB, 4),  # Hall of the Elders
    (0xE1981EFC, 0),  # Elder Chamber
    (0x43E4CC25, 1),  # Research Lab Hydra
    (0x37BBB33C, 1),  # Observatory Access
    (0xD8E905DD, 1),  # Research Core Access
    (0x21B4BFF6, 1),  # Research Lab Aether
    (0x3F375ECC, 2),  # Omega Research
    (0xF517A1EA, 1),  # Dynamo Access
    (0x8A97BB54, 1),  # Elite Research
    (0xA20201D4, 0),  # Security Access B
    (0xA20201D4, 1),  # Security Access B
    (0x956F1552, 1),  # Mine Security Station
    (0xC50AF17A, 2),  # Elite Control
    (0x90709AAC, 1),
}

# Shield actors randomprime uses in place of the ones in its table. From randomprime's patches.rs.
SHIELD_OVERRIDES = {
    (0xD5CDB809, 4): [0x20004],  # Main Plaza
}


def parse_room_doors(meta: str) -> dict[int, list[dict]]:
    rooms = {}
    room_starts = [(m.start(), int(m.group(1), 16)) for m in ROOM_RE.finditer(meta)]
    room_starts.append((len(meta), None))
    for (start, mrea), (end, _) in zip(room_starts, room_starts[1:]):
        doors = []
        for m in DOOR_RE.finditer(meta[start:end]):
            if m.group(2) is None:
                continue  # dock without a door, like an open passage
            dock = int(m.group(7))
            shields = [int(v) for v in INSTANCE_RE.findall(m.group(6))]
            doors.append(
                {
                    "dock": dock,
                    "door": int(m.group(2)) & 0x3FFFFFF,
                    "rotation": [float(v) for v in m.group(4).split(",")] if m.group(4) else [0.0, 0.0, 0.0],
                    "forces": [int(v) & 0x3FFFFFF for v in INSTANCE_RE.findall(m.group(5))],
                    "shields": [v & 0x3FFFFFF for v in SHIELD_OVERRIDES.get((mrea, dock), shields)],
                    "vertical": (mrea, dock) in VERTICAL_DOCKS,
                }
            )
        if doors:
            rooms[mrea] = doors
    return rooms


# randomprime's textures for the door colors and blast shields Randovania can ask for. The custom
# models that use them are built from the game's own models at runtime (see CustomAssets.cpp).
DOOR_ASSET_PREFIXES = [
    "charge_beam",
    "flamethrower",
    "ice_spreader",
    "morph_ball_bombs",
    "power_bomb",
    "super_missile",
    "wavebuster",
]
DOOR_ASSET_SUFFIXES = ["animated_glow", "glow_border", "glow_trim", "holorim", "metal_body", "metal_trim"]
DOOR_ASSET_EXTRA = ["power_beam_holorim.TXTR", "orange.txtr", "pink.txtr", "yellow.txtr", "testbnew.txtr"]


def copy_door_assets(randomprime: Path, out: Path) -> None:
    files = [f"{p}_{s}.TXTR" for p in DOOR_ASSET_PREFIXES for s in DOOR_ASSET_SUFFIXES] + DOOR_ASSET_EXTRA
    copy_extra_assets(randomprime, out, files)


# randomprime's textures and models for the pickup models it adds to the game. The models are
# built from these and the game's own models at runtime (see PickupAssets.cpp).
PICKUP_ASSETS = [
    "phazon_suit_texure_1.txtr",
    "phazon_suit_texure_2.txtr",
    "nothing_texture.txtr",
    "zoomer.CMDL",
    "cog.CMDL",
    "randovania_gamecube.CMDL",
    "randovania_gamecube.TXTR",
    "randovania_gamecube_text.TXTR",
    "flamethrower_vertice_color.TXTR",
    "flamethrower_cap_glow.TXTR",
    "flamethrower_color_body.TXTR",
]


def copy_extra_assets(randomprime: Path, out: Path, files: list[str]) -> None:
    out.mkdir(parents=True, exist_ok=True)
    for name in files:
        (out / name.lower()).write_bytes((randomprime / "extra_assets" / name).read_bytes())


def write_door_tables(randomprime: Path, out: Path) -> None:
    meta_in = (randomprime / "src" / "pickup_meta.rs.in").read_text()
    rooms = parse_room_doors(meta_in)

    def ids(values: list[int]) -> str:
        values = (values + [0, 0])[:2]
        return "{" + ", ".join(f"0x{v:08X}" for v in values) + "}"

    lines = [
        "// Generated by tools/randomizer/export_randovania.py. Do not edit.",
        "// Door script objects are derived from randomprime",
        "// (https://github.com/randovania/randomprime, MIT License).",
        "",
        '#include "Metaforce/Randomizer/DoorTables.hpp"',
        "",
        "namespace metaforce::randomizer {",
        "",
        "const DoorLocationInfo kDoorLocations[] = {",
    ]
    for mrea in sorted(rooms):
        for door in sorted(rooms[mrea], key=lambda d: d["dock"]):
            rot = ", ".join(f"{v}f" for v in door["rotation"])
            lines.append(
                f"    {{0x{mrea:08X}, {door['dock']}, 0x{door['door']:08X}, {ids(door['forces'])}, "
                f"{ids(door['shields'])}, {{{rot}}}, {'true' if door['vertical'] else 'false'}}},"
            )
    lines += [
        "};",
        "const int kDoorLocationCount = sizeof(kDoorLocations) / sizeof(kDoorLocations[0]);",
        "",
        "} // namespace metaforce::randomizer",
        "",
    ]
    out.write_text("\n".join(lines))


MODEL_RE = re.compile(r"PickupModel::(\w+) => &\[([^\]]*)\]", re.S)
MODEL_NAME_RE = re.compile(r'PickupModel::(\w+) => "([^"]+)"')


def parse_models(meta_rs: str, meta_in: str) -> tuple[dict[str, bytes], dict[str, str]]:
    raw_start = meta_in.index("fn raw_pickup_data")
    models = {}
    for m in MODEL_RE.finditer(meta_in, raw_start):
        values = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(2))]
        models[m.group(1)] = bytes(values)
    names = {}
    name_fn = meta_rs.index("impl PickupModel")
    for m in MODEL_NAME_RE.finditer(meta_rs, name_fn):
        names.setdefault(m.group(1), m.group(2))
    return models, names


ALTERNATE_MODEL_NAMES = {
    "combat": "CombatVisor",
    "scan": "Visor",
    "scan visor": "Visor",
    "thermal": "ThermalVisor",
    "x-ray": "XRayVisor",
    "xray": "XRayVisor",
    "x-ray visor": "XRayVisor",
    "xray visor": "XRayVisor",
    "gamecube": "RandovaniaGamecube",
    "power suit": "RandovaniaGamecube",
    "power beam": "RandovaniaGamecube",
}


def write_tables(logic: dict, randomprime: Path, out: Path) -> None:
    meta_in = (randomprime / "src" / "pickup_meta.rs.in").read_text()
    meta_rs = (randomprime / "src" / "pickup_meta.rs").read_text()
    rooms = parse_room_pickups(meta_in)
    models, model_names = parse_models(meta_rs, meta_in)

    # Randovania sends each room's pickups to randomprime sorted by pickup index, and randomprime
    # pairs them with the room's pickup locations in order.
    locations: dict[int, dict] = {}
    for region in logic["regions"]:
        for area in region["areas"]:
            indices = sorted(n["index"] for n in area["nodes"] if n["type"] == "pickup")
            if not indices:
                continue
            room = rooms.get(area["asset_id"])
            if room is None or len(room) < len(indices):
                raise ValueError(f"randomprime has no matching pickups for {region['name']} / {area['name']}")
            for index, loc in zip(indices, room):
                locations[index] = {**loc, "mlvl": region["asset_id"], "mrea": area["asset_id"]}

    count = max(locations) + 1
    missing = [i for i in range(count) if i not in locations]
    if missing:
        raise ValueError(f"Missing pickup locations: {missing}")

    lines = [
        "// Generated by tools/randomizer/export_randovania.py. Do not edit.",
        "// Pickup locations and pickup models are derived from randomprime",
        "// (https://github.com/randovania/randomprime, MIT License).",
        "",
        '#include "Metaforce/Randomizer/PickupTables.hpp"',
        "",
        "namespace metaforce::randomizer {",
        "",
        "const PickupLocationInfo kPickupLocations[kPickupLocationCount] = {",
    ]
    for i in range(count):
        loc = locations[i]
        pos = ", ".join(f"{v}f" for v in loc["position"])
        lines.append(
            f"    {{0x{loc['mlvl']:08X}, 0x{loc['mrea']:08X}, 0x{loc['pickup']:08X}, 0x{loc['hudmemo']:08X}, "
            f"0x{loc['audio']:08X}, 0x{loc['relay']:08X}, {{{pos}}}}}, // {i}"
        )
    lines += ["};", ""]

    for name, data in models.items():
        lines.append(f"static const unsigned char kModel{name}[] = {{")
        for off in range(0, len(data), 16):
            lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[off : off + 16]) + ",")
        lines.append("};")
    lines += ["", "const PickupModelTemplate kPickupModels[] = {"]
    for name, data in models.items():
        lines.append(f'    {{"{model_names.get(name, name)}", kModel{name}, sizeof(kModel{name})}},')
    lines += ["};", "const int kPickupModelCount = sizeof(kPickupModels) / sizeof(kPickupModels[0]);", ""]

    # Model-space bounds of each pickup CMDL, stored as float bit patterns, used to keep the
    # replacement model centered where the original was.
    aabb_block = meta_in[meta_in.index("const PICKUP_CMDL_AABBS") :]
    aabb_block = aabb_block[: aabb_block.index("];")]
    aabbs = re.findall(r"\(0x([0-9A-Fa-f]+), \[([^\]]+)\]\)", aabb_block)
    lines.append("const PickupModelAabb kPickupModelAabbs[] = {")
    for cmdl, values in aabbs:
        bits = ", ".join(f"0x{int(v.strip(), 16):08X}" for v in values.split(","))
        lines.append(f"    {{0x{int(cmdl, 16):08X}, {{{bits}}}}},")
    lines += ["};", "const int kPickupModelAabbCount = sizeof(kPickupModelAabbs) / sizeof(kPickupModelAabbs[0]);", ""]

    lines.append("const ModelAlias kModelAliases[] = {")
    for alias, name in ALTERNATE_MODEL_NAMES.items():
        lines.append(f'    {{"{alias}", "{model_names.get(name, name)}"}},')
    lines += ["};", "const int kModelAliasCount = sizeof(kModelAliases) / sizeof(kModelAliases[0]);", ""]
    lines += ["} // namespace metaforce::randomizer", ""]

    if count != 100:
        raise ValueError(f"Expected 100 pickup locations, found {count}")
    out.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--randovania", type=Path, required=True, help="path to a Randovania checkout")
    parser.add_argument("--randomprime", type=Path, required=True, help="path to a randomprime checkout")
    parser.add_argument("--disc", type=Path, help="files directory of an extracted game disc, for dock shapes")
    args = parser.parse_args()

    prime1 = args.randovania / "randovania" / "games" / "prime1"
    if not prime1.is_dir():
        print(f"error: {prime1} not found", file=sys.stderr)
        return 1

    if args.disc:
        dock_shapes = read_dock_shapes(args.disc)
    else:
        dock_shapes = read_existing_dock_shapes(OUT_RES / "logic.json")
        if dock_shapes is None:
            print("warning: no --disc given, the room randomizer will be unavailable", file=sys.stderr)
        else:
            print("No --disc given, keeping the dock shapes of the previous export")
    logic = export_logic(prime1, dock_shapes)
    pickups = export_pickups(prime1, logic)

    OUT_RES.mkdir(parents=True, exist_ok=True)
    (OUT_RES / "logic.json").write_text(json.dumps(logic, separators=(",", ":")))
    (OUT_RES / "pickups.json").write_text(json.dumps(pickups, indent=1))
    write_tables(logic, args.randomprime, OUT_TABLES)
    write_door_tables(args.randomprime, OUT_DOOR_TABLES)
    copy_door_assets(args.randomprime, OUT_ASSETS)
    copy_extra_assets(args.randomprime, OUT_PICKUP_ASSETS, PICKUP_ASSETS)
    print(f"Wrote {OUT_RES / 'logic.json'}, {OUT_RES / 'pickups.json'}, {OUT_TABLES}, {OUT_DOOR_TABLES}, "
          f"{OUT_ASSETS} and {OUT_PICKUP_ASSETS}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
