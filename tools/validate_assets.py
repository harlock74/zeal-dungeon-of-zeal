#!/usr/bin/env python3
"""Validate editable art and loadable Tiled data; retain the build's packer API."""
from pathlib import Path
import argparse
import collections
import re
import hashlib
import struct
import sys
import xml.etree.ElementTree as ET
import xml.etree.ElementTree as ElementTree

PROJECT_ROOT = Path(__file__).resolve().parents[1]
OBJECT_KINDS = dict(enemy=0, key_site=1, pile_site=2, picture_site=3, transport_site=4, chest=5, final_site=6)

def require(ok, message):
    if not ok:
        raise ValueError(message)

# Approved palette identity; the colour values live only in the saved Aseprite source.
PALETTE_RGB_SHA256 = 'd0cc513e59f9a63f99190038635b353be6cb29d0b49d5c0772f0f9275ad64ffe'

def load():
    data = (PROJECT_ROOT / 'assets/zdungeon.gif').read_bytes()
    require(data[:6] in (b'GIF87a', b'GIF89a') and struct.unpack_from('<HH', data, 6) == (256,256), 'GIF must be 256x256')
    require(data[10] & 128 and (1 << ((data[10] & 7) + 1)) == 16, 'GIF must have16 indexed colours')
    colours = [tuple(data[i:i+3]) for i in range(13,61,3)]
    require(hashlib.sha256(data[13:61]).hexdigest() == PALETTE_RGB_SHA256, 'GIF palette identity changed')
    source = (PROJECT_ROOT / 'assets/zdungeon.aseprite').read_bytes()
    require(len(source) >= 144 and struct.unpack_from('<H',source,4)[0] == 0xa5e0, 'Invalid Aseprite file')
    require(struct.unpack_from('<HHHH',source,6) == (1,256,256,8) and source[28] == 0, 'Use one indexed256x256 Aseprite frame, transparent index0')
    header = (PROJECT_ROOT / 'src/art_ids.h').read_text()
    count = re.search(r'#define\s+ART_TILE_COUNT\s+(\d+)',header)
    require(count is not None and 1 <= int(count[1]) <= 256, '256-tile budget exceeded')
    count = int(count[1])
    arrays = re.findall(r'ART_(\w+)\[(\d+)\]\s*=\s*\{([\d,]+)\}',header)
    cells = []
    for name,size,values in arrays:
        values = [int(v) for v in values.split(',')]
        require(len(values) == int(size) and all(0 <= v < count for v in values), 'Named cells outside loaded tile budget')
        cells.extend(values)
    require(len({name for name,_,_ in arrays}) == len(arrays) and set(cells) == set(range(count)), 'Named cells must cover every loaded tile')
    return dict(tile_count=count, palette=colours, source='assets/zdungeon.aseprite', image='assets/zdungeon.gif'), colours

def validate():
    return load()[0]

def read_properties(element):
    return {p.get('name'): p.get('value', p.text) for p in element.findall('properties/property')}

def room_error(file, message):
    match = re.match(r'f(\d+)_r(\d+)_(\d+)', str(file))
    label = f'Floor {match[1]}, room r{match[2]}_{match[3]}' if match else str(file)
    raise ValueError(f'assets/dungeon.tmx: {label}: {message}')

def splash_error(message):
    raise ValueError(f'assets/splash.tmx: {message}')

def map_layers(path, names, tileset):
    root = ET.parse(path).getroot()
    require(tuple(root.get(k) for k in ('width', 'height', 'tilewidth', 'tileheight')) == ('40', '30', '16', '16'), f'{path}: wrong size: use40x30 cells of16x16 pixels')
    ts, layers = root.find('tileset'), root.findall('layer')
    require(ts is not None and ts.get('source') == tileset and ts.get('firstgid') == '1', f'{path}: wrong tileset')
    require([l.get('name') for l in layers] == names, f'{path}: missing layer: keep the required tile layers in order')
    count = load()[0]['tile_count']
    values = []
    for layer in layers:
        require((layer.get('width'), layer.get('height')) == ('40', '30'), f'{path}: wrong layer size')
        require(all(float(layer.get(k, 0)) == 0 for k in ('offsetx', 'offsety', 'x', 'y')), f'{path}: shifted layer cannot be loaded')
        data = layer.find('data')
        require(data is not None and data.get('encoding') == 'csv', f'{path}: use CSV tile data')
        cells = [int(v) for v in data.text.replace('\n', '').split(',') if v.strip()]
        require(len(cells) == 1200 and all(0 <= v <= count for v in cells), f'{path}: missing cells or unloaded/flipped tile')
        values.append(cells)
    return root, values

def dungeon_rooms():
    """Decode the single authoring map into room-local Tiled records, without changing their order."""
    import copy
    path = PROJECT_ROOT / 'assets/dungeon.tmx'
    root = ET.parse(path).getroot()
    def check(ok, message, name='f1_r0_0.tmx'):
        if not ok:
            room_error(name, message)
    check(tuple(root.get(k) for k in ('orientation', 'width', 'height', 'tilewidth', 'tileheight', 'infinite')) ==
          ('orthogonal', '120', '90', '16', '16', '0'), 'dungeon must be orthogonal 120x90 tiles of16x16')
    tilesets = root.findall('tileset')
    check(len(tilesets) == 1 and tilesets[0].get('source') == 'zdungeon.tsx' and tilesets[0].get('firstgid') == '1', 'shared zdungeon.tsx required')
    groups = root.findall('group')
    check(len(groups) == 8 and [g.get('name') for g in groups] == [f'Floor {f}' for f in range(1, 9)], 'exactly 72 rooms in8 ordered Floor groups required')
    check(not root.findall('layer') and not root.findall('objectgroup'), 'all room layers must belong to floor groups')
    sources = {}
    ids = set()
    for f, group in enumerate(groups):
        context = f'f{f+1}_r0_0.tmx'
        for layer in [group, *list(group)]:
            check(all(float(layer.get(k, 0)) == 0 for k in ('offsetx', 'offsety', 'x', 'y')), 'shifted layer cannot be loaded', context)
        check([c.tag for c in group] == ['layer', 'objectgroup', 'objectgroup'] and [c.get('name') for c in group] == ['scene', 'actors_and_sites', 'rooms'], 'scene, actors_and_sites and rooms layers required', context)
        scene, actors, rectangles = list(group)
        check((scene.get('width'), scene.get('height')) == ('120', '90'), 'scene must be120x90', context)
        data = scene.find('data')
        check(data is not None and data.get('encoding') == 'csv', 'save the scene layer as CSV', context)
        grid = [int(v) for v in (data.text or '').replace('\n', '').split(',') if v.strip()]
        check(len(grid) == 10800, 'scene must contain10800 cells', context)
        rooms = {}
        for rect in rectangles.findall('object'):
            pr = read_properties(rect)
            try:
                r = int(pr['room']); floor = int(pr['floor'])
            except (KeyError, ValueError):
                room_error(context, 'room rectangle requires floor, room and accent properties')
            name = f'f{f+1}_r{r//3}_{r%3}.tmx'
            check(floor == f and 0 <= r < 9 and r not in rooms, 'exactly 72 unique room rectangles required', name)
            check(rect.get('name') == f'r{r//3}_{r%3}' and 'accent' in pr, 'room rectangle name/accent missing', name)
            ox, oy = r % 3 * 640, r // 3 * 480
            check(tuple(float(rect.get(k, 0)) for k in ('x','y','width','height','rotation')) == (ox,oy,640,480,0) and not any(rect.find(k) is not None for k in ('ellipse','polygon','point','polyline')), 'room rectangle must match its40x30 cell slot', name)
            virtual = ET.Element('map', width='40', height='30', tilewidth='16', tileheight='16')
            virtual.append(copy.deepcopy(rect.find('properties')))
            ET.SubElement(virtual, 'tileset', firstgid='1', source='zdungeon.tsx')
            layer = ET.SubElement(virtual, 'layer', id='1', name='scene', width='40', height='30')
            cells = [grid[(r//3*30+y)*120+r%3*40+x] for y in range(30) for x in range(40)]
            ET.SubElement(layer,'data',encoding='csv').text=','.join(map(str,cells))
            ET.SubElement(virtual,'objectgroup',id='2',name='objects')
            rooms[r] = virtual
        missing = set(range(9)) - set(rooms)
        if missing:
            r=min(missing);room_error(f'f{f+1}_r{r//3}_{r%3}.tmx','exactly 72 room rectangles required; missing room')
        for obj in actors.findall('object'):
            x,y=float(obj.get('x',0)),float(obj.get('y',0))
            check(0 <= x < 1920 and 0 <= y < 1440, 'object outside floor rectangles', context)
            r=int(y//480)*3+int(x//640);name=f'f{f+1}_r{r//3}_{r%3}.tmx'
            check(obj.get('id') not in ids, 'duplicate object ID', name);ids.add(obj.get('id'))
            local=copy.deepcopy(obj)
            local.set('id',str(int(obj.get('id'))%100))
            local.set('x',format(x-r%3*640,'g'));local.set('y',format(y-r//3*480,'g'))
            rooms[r].find('objectgroup').append(local)
        for r, virtual in rooms.items():
            sources[f'f{f+1}_r{r//3}_{r%3}.tmx']=virtual
    return sources

def solid_tile_numbers():
    """Runtime tile-wall table, from the editable shared Tiled properties."""
    root = ElementTree.parse(PROJECT_ROOT / 'assets/zdungeon.tsx').getroot()
    return tuple(int(tile.get('id')) for tile in root.findall('tile')
                 if read_properties(tile).get('solid') == 'true')

def read_rooms():
    sources = dungeon_rooms()
    ts = ElementTree.parse(PROJECT_ROOT / 'assets/zdungeon.tsx').getroot()
    count = load()[0]['tile_count']
    tileprops = {int(t.attrib['id']): read_properties(t) for t in ts.findall('tile')}
    for tile in range(count):
        props = tileprops.get(tile, {})
        if 'solid' not in props and 'door' not in props:
            continue
        if any(props.get(k) not in ('true','false') for k in ('solid','door')) or (props.get('door') == 'true' and props.get('solid') == 'true'):
            room_error('f1_r0_0.tmx', f'shared solid/door properties invalid for tile {tile}')
    maps = {}
    for name, root in sorted(sources.items()):
        meta = read_properties(root)
        f = int(meta['floor'])
        r = int(meta['room'])
        data = root.find('layer/data')
        gids = [int(v) for v in data.text.split(',')]
        if any((v < 0 or v > count for v in gids)):
            room_error(name, 'tile outside shared tileset')
        for y in (0, 1, 29):
            if any(gids[y * 40:(y + 1) * 40]):
                room_error(name, f'tile in HUD row {y}')
        tiles = bytes((max(0, g - 1) for g in gids))
        solid = bytes((int(tileprops.get(gids[(y + 2) * 40 + x] - 1, {}).get('solid')
            == 'true') for y in range(25) for x in range(40)))
        objects = []
        for o in root.findall('objectgroup/object'):
            pr = read_properties(o)
            kind = pr.get('kind', o.attrib.get('type'))
            x = int(float(o.attrib['x']) / 16)
            y = int(float(o.attrib['y']) / 16) - 2
            if float(o.attrib['x']) % 16 or float(o.attrib['y']) % 16:
                room_error(name, 'objects must be on the 16x16 grid')
            if kind not in OBJECT_KINDS:
                room_error(name, f'unknown object type {kind}')
            if not (0 <= x < 40 and 0 <= y < 24):
                room_error(name, f'{kind} in HUD/outside playable rows')
            footprint = [(x, y), (x, y + 1)]
            if kind == 'chest':
                footprint += [(x + 1, y)]
            if kind == 'transport_site':
                footprint += [(xx, yy) for xx in range(x - 1, x + 2) for yy in range(y - 2, y + 2)]
            if any((not (0 <= xx < 40 and 0 <= yy < 25) for xx, yy in footprint)):
                room_error(name, f'{kind} extends into HUD/outside playable rows')
            if any((solid[yy * 40 + xx] for xx, yy in footprint)):
                room_error(name, f'{kind} sits in a wall')
            if kind == 'enemy' and (not 0 <= int(pr.get('type', 0)) < 8):
                room_error(name, 'enemy type must be 0..7')
            objects.append((OBJECT_KINDS[kind], x, y, int(pr.get('type', 0))))
        if len(objects) > 16:
            room_error(name, 'maximum 16 objects/sites per room')
        legal = {(x, y) for y in range(24) for x in range(40) if not solid[y * 40 + x
            ] and (not solid[(y + 1) * 40 + x])}
        if (18, 13) not in legal:
            room_error(name, 'central arrival cell is blocked')
        seen = {(18, 13)}
        queue = collections.deque(seen)
        while queue:
            x, y = queue.popleft()
            for dx, dy in [(1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)]:
                p = (x + dx, y + dy)
                if p in legal and p not in seen:
                    seen.add(p)
                    queue.append(p)
        if seen != legal:
            room_error(name, 'unreachable walkable region')
        for kind, x, y, param in objects:
            if (x, y) not in seen:
                room_error(name, f'object {kind} is unreachable')
        maps[f, r] = dict(name=name, tiles=tiles, empty=[g == 0 for g in gids], solid
            =solid, objects=objects)
    for (f, r), m in maps.items():
        for side, other in [('E', r + 1 if r % 3 < 2 else None), ('S', r + 3 if r < 6 else None)]:
            if other is None:
                continue
            n = maps[f, other]
            a = [not m['solid'][y * 40 + 39] for y in range(25)] if side == 'E' else [
                not m['solid'][24 * 40 + x] for x in range(40)]
            b = [not n['solid'][y * 40] for y in range(25)] if side == 'E' else [not
                n['solid'][x] for x in range(40)]
            if a != b:
                room_error(m['name'], f"{side} doorway does not match {n['name']}")
    for f in range(8):
        seen = {4}
        q = [4]
        while q:
            r = q.pop()
            m = maps[f, r]
            neighbours = []
            if r % 3:
                neighbours.append((r - 1, any((not m['solid'][y * 40] for y in range(25)))))
            if r % 3 < 2:
                neighbours.append((r + 1, any((not m['solid'][y * 40 + 39] for y in range(25)))))
            if r >= 3:
                neighbours.append((r - 3, any((not v for v in m['solid'][:40]))))
            if r < 6:
                neighbours.append((r + 3, any((not v for v in m['solid'][-40:]))))
            for n, open_ in neighbours:
                if open_ and n not in seen:
                    seen.add(n)
                    q.append(n)
        if len(seen) != 9:
            room_error(f'f{f + 1}_r0_0.tmx', 'unreachable room in floor graph')
    for f in range(8):
        pools = {kind: {r for (ff, r), m in maps.items() if ff == f and r != 4 and any
            ((o[0] == kind for o in m['objects']))} for kind in (1, 2, 3, 4)}

        def assign(k, used):
            if k == 5:
                return True
            return any((assign(k + 1, used | {r}) for r in pools[k] - used))
        if not assign(1, set()):
            room_error(f'f{f + 1}', 'key, pile, picture and transport need four distinct eligible rooms'
                )
    if not any((o[0] == 6 for (f, r), m in maps.items() if f == 7 for o in m['objects'])):
        room_error('f8', 'a final figure site is required')
    if not any((o[0] == 5 for (f, r), m in maps.items() if f == 7 for o in m['objects'])):
        room_error('f8', 'a chest is required to exchange carried piles')
    return maps

def read_splash():
    root, layers = map_layers(PROJECT_ROOT / 'assets/splash.tmx', ['Scene', 'Lettering'], 'zdungeon.tsx')
    records = []
    for obj in root.findall('objectgroup/object'):
        props = read_properties(obj)
        if props.get('kind') not in ('7', '8'):
            continue
        x, y, digits = float(obj.get('x')), float(obj.get('y')), int(props['digits'])
        require(x % 16 == y % 16 == 0 and 0 <= x < 640 and 0 <= y < 480 and 1 <= digits <= 6, 'Invalid title value record')
        records.append(bytes([int(props['kind']), int(x / 16), int(y / 16), digits]))
    require(len(records) == 2, 'dynamic-value placeholder removed: title requires two value records')
    return root, layers, records

# Shared image helpers used by asset checks.

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--art', action='store_true')
    args = parser.parse_args()
    validate()
    if not args.art:
        read_rooms()
        read_splash()
    print('Assets valid: saved indexed GIF, tile names and loadable Tiled data.')

if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, IndexError, TypeError, struct.error, ET.ParseError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
