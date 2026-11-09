#!/usr/bin/env python3
"""Pack checked SDK tile layers with this game's collision and object records."""
from pathlib import Path
import json
import sys
import xml.etree.ElementTree as ElementTree

sys.dont_write_bytecode = True
from validate_assets import read_rooms, room_error, dungeon_rooms, solid_tile_numbers

PROJECT_ROOT = Path(__file__).resolve().parents[1]
SDK_MAPS_DIR = PROJECT_ROOT / "build/maps"
RUNTIME_DIR = PROJECT_ROOT / "build/runtime/assets/rooms"


def pack_rooms(room_sources):
    """Pack ZTR2 room tiles and a generated wall table; ZTF1 is the sole gameplay object store."""
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    flags = bytearray(32)
    for tile in solid_tile_numbers():
        flags[tile // 8] |= 1 << (tile % 8)
    generated = PROJECT_ROOT / "build/generated"
    generated.mkdir(parents=True, exist_ok=True)
    (generated / "room_walls.c").write_text(
        '#include <stdint.h>\nconst uint8_t room_solid_tiles[32] = {'
        + ','.join(str(value) for value in flags) + '};\n')
    for (floor, room), data in sorted(room_sources.items()):
        map_name = Path(data["name"])
        converted = (SDK_MAPS_DIR / map_name.with_suffix(".ztm")).read_bytes()
        expected = bytes(
            255 if empty else tile
            for tile, empty in zip(data["tiles"], data["empty"])
        )
        if converted != expected:
            room_error(data["name"], "SDK tile conversion differs from checked layer")
        # SDK empty is255; this streamed format has always stored blank cell0.
        tiles = bytes(
            0 if empty else tile
            for tile, empty in zip(converted, data["empty"])
        )
        collision = bytearray(125)
        for index, solid in enumerate(data["solid"]):
            collision[index // 8] |= solid << (index % 8)
        derived = bytearray(125)
        for index, tile in enumerate(tiles[80:1080]):
            if flags[tile // 8] & (1 << (tile % 8)):
                derived[index // 8] |= 1 << (index % 8)
        if derived != collision:
            room_error(data["name"], "stored tile numbers cannot reproduce solid properties")
        header = b"ZTR2" + bytes([0, floor, room, 0])
        (RUNTIME_DIR / map_name.with_suffix(".ztr")).write_bytes(
            header + tiles
        )

    # Floor indexes contain fixed-size object records, not duplicated tile maps.
    for floor in range(8):
        index_data = bytearray(b"ZTF1")
        for room in range(9):
            objects = room_sources[floor, room]["objects"]
            index_data += bytes([len(objects)])
            index_data += b"".join(bytes(record) for record in objects)
            index_data += bytes((16 - len(objects)) * 4)
        (RUNTIME_DIR / f"f{floor + 1}.ztf").write_bytes(index_data)


def main():
    if "--extract" in sys.argv:
        # Room-local inputs for the existing SDK converter, generated only in build/.
        directory = SDK_MAPS_DIR / "source"
        directory.mkdir(parents=True, exist_ok=True)
        for name, root in dungeon_rooms().items():
            data = ElementTree.tostring(root, encoding="UTF-8", xml_declaration=True)
            path = directory / name
            if not path.exists() or path.read_bytes() != data:
                path.write_bytes(data)
        return
    rooms = read_rooms()
    if "--check" not in sys.argv:
        pack_rooms(rooms)
    print("Tiled room checks PASS: 72 playable sources, 8 floors; "
          "authoritative solid/door properties and typed objects.")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError, ElementTree.ParseError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
