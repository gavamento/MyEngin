#!/usr/bin/env python
# gen_2d_assets.py — --2d-demo (M94) のアセットを assets\2d\ へ生成する。
# 使い方: python -I tools\gen_2d_assets.py
#
# 標準ライブラリだけで PNG を書く (zlib + struct)。乱数は seed 固定で、同じ入力から同じバイト列になる。
# GUID は固定値 (DemoContent.cpp の TwoD* 定数と対応)。スプライトの AssetID は
# fnv1a64("guid://<texture guid 16hex>#sprite<id>") (SpriteLibrary::SliceId と同じ式)。
import json
import math
import os
import random
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'assets', '2d')
SEED = 20261022

# ---- 固定 GUID (DemoContent.cpp の kTwoD* と同じ値) ----
G_TILES = 0x2D00000000000001
G_HERO = 0x2D00000000000002
G_CRATE = 0x2D00000000000003
G_BG_FAR = 0x2D00000000000004
G_BG_NEAR = 0x2D00000000000005
G_HERO_NORMAL = 0x2D00000000000006
G_HILL_TOP = 0x2D00000000000007
G_HILL_LEFT = 0x2D00000000000008
G_HILL_RIGHT = 0x2D00000000000009
G_HILL_FILL = 0x2D0000000000000A
G_ANIM_IDLE = 0x2D00000000000010
G_ANIM_WALK = 0x2D00000000000011
G_CONTROLLER = 0x2D00000000000012
G_PALETTE = 0x2D00000000000013
G_TILEMAP = 0x2D00000000000014
G_HILL_PROFILE = 0x2D00000000000015
G_BT = 0x2D00000000000016
G_BB = 0x2D00000000000017


def fnv1a64(text):
    h = 14695981039346656037
    for b in text.encode('utf-8'):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def slice_id(tex_guid, slice_no):
    return fnv1a64('guid://%016x#sprite%d' % (tex_guid, slice_no))


# ---- 画像 (行優先の RGBA タプル列) ----
class Image:
    def __init__(self, w, h):
        self.w = w
        self.h = h
        self.px = [(0, 0, 0, 0)] * (w * h)

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = c

    def get(self, x, y):
        return self.px[y * self.w + x]

    def rect(self, x0, y0, w, h, c):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                self.put(x, y, c)

    def blit(self, other, ox, oy):
        for y in range(other.h):
            for x in range(other.w):
                self.put(ox + x, oy + y, other.get(x, y))

    def png(self):
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)
            for x in range(self.w):
                raw.extend(self.px[y * self.w + x])

        def chunk(tag, data):
            body = tag + data
            return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body) & 0xFFFFFFFF)

        return (b'\x89PNG\r\n\x1a\n'
                + chunk(b'IHDR', struct.pack('>IIBBBBB', self.w, self.h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
                + chunk(b'IEND', b''))


def rgb(r, g, b, a=255):
    return (r, g, b, a)


# ---- タイル (16 px) ----
GRASS = [rgb(112, 200, 72), rgb(150, 228, 92), rgb(78, 160, 56)]
DIRT = [rgb(122, 84, 52), rgb(108, 74, 46), rgb(86, 58, 38)]
OUTLINE = rgb(46, 30, 22)


def tile_dirt(rng):
    t = Image(16, 16)
    for y in range(16):
        for x in range(16):
            c = DIRT[0] if (x // 4 + y // 4) % 2 == 0 else DIRT[1]
            if rng.random() < 0.07:
                c = DIRT[2]
            t.put(x, y, c)
    return t


def tile_grass_top(rng, left=False, right=False):
    t = tile_dirt(rng)
    for x in range(16):
        for y in range(0, 5):
            c = GRASS[0] if y < 3 else GRASS[2]
            if y == 0 and (x + rng.randint(0, 2)) % 3 == 0:
                c = GRASS[1]
            t.put(x, y, c)
        if rng.random() < 0.5:
            t.put(x, 5, GRASS[2])
    if left:
        for y in range(16):
            t.put(0, y, OUTLINE if y > 1 else rgb(0, 0, 0, 0))
            if y > 1:
                t.put(1, y, DIRT[2])
        t.put(1, 0, rgb(0, 0, 0, 0))
    if right:
        for y in range(16):
            t.put(15, y, OUTLINE if y > 1 else rgb(0, 0, 0, 0))
            if y > 1:
                t.put(14, y, DIRT[2])
        t.put(14, 0, rgb(0, 0, 0, 0))
    return t


def tile_plank():
    t = Image(16, 16)
    for y in range(0, 6):
        for x in range(16):
            c = rgb(176, 124, 70) if y in (1, 2, 3) else rgb(120, 80, 46)
            if x % 8 == 7:
                c = rgb(96, 62, 36)
            t.put(x, y, c)
    for x in (2, 10):
        t.put(x, 2, rgb(70, 46, 28))
    return t


def tile_block():
    t = Image(16, 16)
    for y in range(16):
        for x in range(16):
            edge = x in (0, 15) or y in (0, 15)
            t.put(x, y, rgb(70, 74, 92) if edge else rgb(132, 138, 160) if (x + y) % 7 else rgb(118, 124, 146))
    for i in range(4, 12):
        t.put(i, i, rgb(60, 62, 78))
        t.put(15 - i, i, rgb(60, 62, 78))
    return t


def tile_bg(glow):
    t = Image(16, 16)
    base = rgb(44, 48, 74)
    mortar = rgb(30, 32, 52)
    for y in range(16):
        for x in range(16):
            shift = 8 if (y // 4) % 2 else 0
            c = mortar if (y % 4 == 3 or (x + shift) % 16 == 0) else base
            t.put(x, y, c)
    if glow:
        for y in range(5, 11):
            for x in range(5, 11):
                t.put(x, y, rgb(255, 214, 120) if 5 < x < 10 and 5 < y < 10 else rgb(190, 140, 60))
    return t


def tile_tuft(rng):
    t = Image(16, 16)
    for x in range(1, 15, 2):
        h = rng.randint(3, 6)
        for y in range(16 - h, 16):
            t.put(x, y, GRASS[0] if y > 16 - h else GRASS[1])
    return t


def make_tiles():
    rng = random.Random(SEED + 1)
    tiles = [
        tile_grass_top(rng),                  # 1 grass_top
        tile_dirt(rng),                       # 2 dirt
        tile_grass_top(rng, left=True),       # 3 grass_left
        tile_grass_top(rng, right=True),      # 4 grass_right
        tile_plank(),                         # 5 plank (一方通行)
        tile_block(),                         # 6 block (壊れる)
        tile_bg(False),                       # 7 bg_a
        tile_bg(True),                        # 8 bg_b
        tile_tuft(rng),                       # 9 tuft (前景)
    ]
    sheet = Image(16 * len(tiles), 16)
    for i, t in enumerate(tiles):
        sheet.blit(t, i * 16, 0)
    return sheet, ['grass_top', 'dirt', 'grass_left', 'grass_right', 'plank', 'block', 'bg_a', 'bg_b', 'tuft']


# ---- 主人公 (32 px、Idle 2 + Walk 4) ----
SKIN = rgb(244, 200, 160)
HAIR = rgb(90, 52, 36)
TUNIC = rgb(60, 110, 200)
TUNIC_D = rgb(40, 76, 150)
PANTS = rgb(52, 56, 84)
BOOT = rgb(40, 30, 28)


def hero_frame(kind, phase):
    f = Image(32, 32)
    bob = 1 if (kind == 'idle' and phase == 1) else 0
    # 足 (下端 y = 31)。walk は 4 コマで前後に開く
    legs = {0: (-2, 2), 1: (0, 0), 2: (2, -2), 3: (0, 0)}[phase] if kind == 'walk' else (0, 0)
    for dx, base in ((legs[0], 11), (legs[1], 18)):
        up = 2 if (kind == 'walk' and dx == 0) else 0
        f.rect(base + dx, 23, 4, 8 - up, PANTS)
        f.rect(base + dx - 1, 29 - up, 6, 3, BOOT)
    # 胴
    f.rect(10, 12 + bob, 12, 12, TUNIC)
    f.rect(10, 20 + bob, 12, 2, TUNIC_D)
    f.rect(8, 13 + bob, 2, 9, TUNIC_D)
    f.rect(22, 13 + bob, 2, 9, TUNIC_D)
    # 頭
    f.rect(11, 3 + bob, 10, 9, SKIN)
    f.rect(10, 1 + bob, 12, 4, HAIR)
    f.rect(10, 4 + bob, 2, 5, HAIR)
    f.rect(18, 7 + bob, 2, 2, rgb(30, 30, 40))
    # 輪郭 (左右・上・下)
    for y in range(32):
        for x in range(32):
            if f.get(x, y)[3] == 0:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < 32 and 0 <= ny < 32 and f.get(nx, ny)[3] == 255 and f.get(nx, ny) != OUTLINE:
                        f.put(x, y, OUTLINE)
                        break
    return f


def make_hero():
    frames = [hero_frame('idle', 0), hero_frame('idle', 1)] + [hero_frame('walk', i) for i in range(4)]
    sheet = Image(32 * len(frames), 32)
    for i, fr in enumerate(frames):
        sheet.blit(fr, i * 32, 0)
    return sheet, frames


def make_hero_normal(frames):
    # 不透明部の外形から法線を作る (緑 = 上、sRGB オフ)。α のぼかしの勾配が外向きの傾き
    sheet = Image(32 * len(frames), 32)
    radius = 3
    for i, fr in enumerate(frames):
        for y in range(32):
            for x in range(32):
                if fr.get(x, y)[3] == 0:
                    continue

                def a(px, py):
                    if 0 <= px < 32 and 0 <= py < 32:
                        return 1.0 if fr.get(px, py)[3] > 0 else 0.0
                    return 0.0

                gx = gy = 0.0
                for k in range(1, radius + 1):
                    gx += a(x + k, y) - a(x - k, y)
                    gy += a(x, y + k) - a(x, y - k)
                nx = -gx / radius
                ny = gy / radius       # 下が埋まっている = 上が空 = 法線は上
                nz = 0.6
                inv = 1.0 / math.sqrt(nx * nx + ny * ny + nz * nz)
                nx, ny, nz = nx * inv, ny * inv, nz * inv
                sheet.put(i * 32 + x, y, (int(round((nx * 0.5 + 0.5) * 255)), int(round((ny * 0.5 + 0.5) * 255)),
                                          int(round((nz * 0.5 + 0.5) * 255)), 255))
    return sheet


def make_crate():
    t = Image(16, 16)
    for y in range(16):
        for x in range(16):
            edge = x in (0, 15) or y in (0, 15)
            diag = abs(x - y) <= 1 or abs(x + y - 15) <= 1
            c = rgb(92, 58, 34) if edge else rgb(150, 100, 56) if not diag else rgb(112, 74, 42)
            t.put(x, y, c)
    return t


# ---- 背景 (横にタイルできる) ----
def make_bg_far(w, h):
    img = Image(w, h)
    rng = random.Random(SEED + 2)
    for y in range(h):
        t = y / (h - 1)
        sky = (int(96 + 80 * t), int(150 + 70 * t), int(224 + 20 * t))
        for x in range(w):
            img.put(x, y, rgb(*sky))
    # 遠い山: 周期が w を割り切る正弦の和 (継ぎ目が出ない)
    phases = [rng.random() * 6.283 for _ in range(3)]
    for x in range(w):
        m = (math.sin(2 * math.pi * x / w * 2 + phases[0]) * 0.5 + math.sin(2 * math.pi * x / w * 5 + phases[1]) * 0.3
             + math.sin(2 * math.pi * x / w * 11 + phases[2]) * 0.2)
        top = int(h * 0.55 - m * h * 0.14)
        for y in range(top, h):
            shade = 0.0 if y - top > 2 else 0.25
            img.put(x, y, rgb(int(100 + 30 * shade), int(120 + 40 * shade), int(176 + 40 * shade)))
    # 雲 (固定位置)
    for cx, cy in ((40, 24), (150, 40), (220, 18)):
        for dx in range(-10, 11):
            for dy in range(-3, 4):
                if (dx * dx) / 100.0 + (dy * dy) / 9.0 <= 1.0:
                    img.put((cx + dx) % w, cy + dy, rgb(240, 246, 255))
    return img


def make_bg_near(w, h):
    img = Image(w, h)
    rng = random.Random(SEED + 3)
    phases = [rng.random() * 6.283 for _ in range(3)]
    for x in range(w):
        m = (math.sin(2 * math.pi * x / w * 3 + phases[0]) * 0.5 + math.sin(2 * math.pi * x / w * 8 + phases[1]) * 0.3
             + math.sin(2 * math.pi * x / w * 16 + phases[2]) * 0.2)
        top = int(h * 0.45 - m * h * 0.16)
        for y in range(top, h):
            lighter = y - top < 3
            img.put(x, y, rgb(52, 112, 70) if lighter else rgb(36, 84, 56))
    # 木のシルエット
    for tx in (30, 92, 170, 228):
        for y in range(h // 4, h // 2 + 6):
            img.put(tx, y, rgb(30, 54, 40))
        for dy in range(-6, 7):
            for dx in range(-6, 7):
                if dx * dx + dy * dy <= 36:
                    img.put((tx + dx) % w, h // 4 + dy, rgb(40, 100, 64))
    return img


# ---- 丘 (SpriteShape の縁と塗り。PPU 32) ----
def make_hill_edges():
    rng = random.Random(SEED + 4)
    top = Image(32, 16)
    for y in range(16):
        for x in range(32):
            if y < 3:
                c = rgb(110, 200, 70) if (x + y) % 5 else rgb(150, 230, 90)
            elif y < 6:
                c = rgb(70, 160, 50)
            else:
                c = rgb(120, 84, 52) if (x // 4 + y // 3) % 2 else rgb(108, 74, 46)
            top.put(x, y, c)
    left = Image(32, 16)
    for y in range(16):
        for x in range(32):
            if y < 2:
                c = rgb(230, 200, 140)
            elif y < 5:
                c = rgb(190, 140, 90)
            else:
                c = rgb(150, 100, 64) if (x // 8) % 2 else rgb(140, 92, 58)
            left.put(x, y, c)
    right = Image(32, 16)
    for y in range(16):
        for x in range(32):
            if y < 2:
                c = rgb(170, 170, 180)
            elif y < 5:
                c = rgb(110, 80, 70)
            else:
                c = rgb(84, 58, 48) if (x // 8) % 2 else rgb(76, 52, 44)
            right.put(x, y, c)
    fill = Image(32, 32)
    for y in range(32):
        for x in range(32):
            base = rgb(104, 70, 44) if ((x // 16) + (y // 16)) % 2 == 0 else rgb(94, 62, 40)
            if rng.random() < 0.06:
                base = rgb(72, 48, 32)
            fill.put(x, y, base)
    return top, left, right, fill


# ---- 書き出し ----
def write_bytes(name, data):
    path = os.path.join(OUT, name)
    with open(path, 'wb') as f:
        f.write(data)


def write_json(name, obj):
    path = os.path.join(OUT, name)
    with open(path, 'w', newline='\n') as f:
        json.dump(obj, f, indent=2)
        f.write('\n')


def write_meta(name, guid, kind, tex=None):
    meta = {'guid': '%016x' % guid, 'type': kind, 'version': 2 if tex is not None else 1}
    if tex is not None:
        meta['tex'] = tex
    write_json(name + '.meta', meta)


def tex_meta(ppu, sprites=None, filter_mode=2):
    t = {'srgb': 0, 'generateMips': 0, 'compress': 0, 'filter': filter_mode, 'pixelsPerUnit': ppu}
    if sprites:
        t['sprites'] = sprites
    return t


def sprite_defs(names, size, pivot):
    return [{'id': i + 1, 'name': n, 'rect': [i * size, 0, size, size], 'pivot': list(pivot)}
            for i, n in enumerate(names)]


def main():
    os.makedirs(OUT, exist_ok=True)

    # タイル
    sheet, tile_names = make_tiles()
    write_bytes('tiles.png', sheet.png())
    write_meta('tiles.png', G_TILES, 'texture', tex_meta(16, sprite_defs(tile_names, 16, (0.5, 0.5))))

    # 主人公
    hero, frames = make_hero()
    hero_names = ['idle0', 'idle1', 'walk0', 'walk1', 'walk2', 'walk3']
    write_bytes('hero.png', hero.png())
    write_meta('hero.png', G_HERO, 'texture', tex_meta(16, sprite_defs(hero_names, 32, (0.5, 0.0))))
    write_bytes('hero_normal.png', make_hero_normal(frames).png())
    write_meta('hero_normal.png', G_HERO_NORMAL, 'texture', tex_meta(16))

    # 箱
    write_bytes('crate.png', make_crate().png())
    write_meta('crate.png', G_CRATE, 'texture', tex_meta(16))

    # 視差背景 (PPU 16: 256 px = 16 単位)
    write_bytes('bg_far.png', make_bg_far(256, 192).png())
    write_meta('bg_far.png', G_BG_FAR, 'texture', tex_meta(16))
    write_bytes('bg_near.png', make_bg_near(256, 96).png())
    write_meta('bg_near.png', G_BG_NEAR, 'texture', tex_meta(16))

    # 丘
    top, left, right, fill = make_hill_edges()
    for name, img, guid in (('hill_top.png', top, G_HILL_TOP), ('hill_left.png', left, G_HILL_LEFT),
                            ('hill_right.png', right, G_HILL_RIGHT), ('hill_fill.png', fill, G_HILL_FILL)):
        write_bytes(name, img.png())
        write_meta(name, guid, 'texture', tex_meta(32))
    write_json('hill_profile.spriteshape.json', {
        'engine': 'MyEngine', 'spriteshape': 1, 'name': 'Hill', 'fill': G_HILL_FILL, 'height': 0.5,
        'edges': [{'angleFrom': -45, 'angleTo': 45, 'sprite': G_HILL_TOP},
                  {'angleFrom': 45, 'angleTo': 135, 'sprite': G_HILL_LEFT},
                  {'angleFrom': -135, 'angleTo': -45, 'sprite': G_HILL_RIGHT}]})
    write_meta('hill_profile.spriteshape.json', G_HILL_PROFILE, 'spriteshape')

    # スプライト列アニメ
    hero_id = lambda n: slice_id(G_HERO, n)
    write_json('hero_idle.spriteanim.json', {
        'engine': 'MyEngine', 'spriteanim': 1, 'name': 'Idle',
        'frames': [{'sprite': hero_id(1), 'ticks': 30}, {'sprite': hero_id(2), 'ticks': 30}]})
    write_meta('hero_idle.spriteanim.json', G_ANIM_IDLE, 'spriteanim')
    write_json('hero_walk.spriteanim.json', {
        'engine': 'MyEngine', 'spriteanim': 1, 'name': 'Walk',
        'frames': [{'sprite': hero_id(3 + i), 'ticks': 8} for i in range(4)]})
    write_meta('hero_walk.spriteanim.json', G_ANIM_WALK, 'spriteanim')

    # コントローラ (v2)。speed (float) で Idle <-> Walk。Walk の tick 6 に足音 (既存の step_hard)
    write_json('hero.controller.json', {
        'engine': 'MyEngine', 'controller': 2, 'defaultState': 0,
        'parameters': [{'name': 'speed', 'type': 'float'}],
        'states': [
            {'name': 'Idle', 'clip': '', 'speed': 1, 'loop': 1, 'sprite': G_ANIM_IDLE},
            {'name': 'Walk', 'clip': '', 'speed': 1, 'loop': 1, 'sprite': G_ANIM_WALK},
        ],
        'clipEvents': {'Walk': [{'tick': 6, 'name': 'step', 'kind': 'sound', 'minWeight': 0.0,
                                 'sound': 'step_hard', 'volume': 0.5, 'pitch': 1.0}]},
        'transitions': [
            {'from': 0, 'to': 1, 'duration': 4, 'hasExitTime': 0,
             'conditions': [{'param': 0, 'op': 'gt', 'value': 0.1}]},
            {'from': 1, 'to': 0, 'duration': 4, 'hasExitTime': 0,
             'conditions': [{'param': 0, 'op': 'lt', 'value': 0.1}]},
        ]})
    write_meta('hero.controller.json', G_CONTROLLER, 'controller')

    # タイルパレット: 1 ground (ルール: 左上 / 右上 / 上 / 既定の 4 種) 2 plank 3 block 4 wall 5 window (アニメ) 6 tuft
    ts = lambda n: slice_id(G_TILES, n)
    write_json('level.tilepalette.json', {
        'engine': 'MyEngine', 'tilepalette': 1,
        'tiles': [
            {'id': 1, 'name': 'ground', 'sprite': ts(2), 'collider': 1, 'oneWay': False, 'hp': 0,
             'destroyedTile': 0, 'destroyEffect': 0,
             'rule': {'variants': [{'neighbors': '.o.o....', 'sprite': ts(3)},
                                   {'neighbors': '.o..o...', 'sprite': ts(4)},
                                   {'neighbors': '.o......', 'sprite': ts(1)}]}},
            {'id': 2, 'name': 'plank', 'sprite': ts(5), 'collider': 1, 'oneWay': True, 'hp': 0,
             'destroyedTile': 0, 'destroyEffect': 0},
            {'id': 3, 'name': 'block', 'sprite': ts(6), 'collider': 1, 'oneWay': False, 'hp': 3,
             'destroyedTile': 0, 'destroyEffect': 0},
            {'id': 4, 'name': 'wall', 'sprite': ts(7), 'collider': 0, 'oneWay': False, 'hp': 0,
             'destroyedTile': 0, 'destroyEffect': 0},
            {'id': 5, 'name': 'window', 'sprite': ts(7), 'collider': 0, 'oneWay': False, 'hp': 0,
             'destroyedTile': 0, 'destroyEffect': 0,
             'anim': {'ticksPerFrame': 30, 'frames': [ts(7), ts(8)]}},
            {'id': 6, 'name': 'tuft', 'sprite': ts(9), 'collider': 0, 'oneWay': False, 'hp': 0,
             'destroyedTile': 0, 'destroyEffect': 0},
        ]})
    write_meta('level.tilepalette.json', G_PALETTE, 'tilepalette')

    write_json('level.tilemap.json', make_tilemap())
    write_meta('level.tilemap.json', G_TILEMAP, 'tilemap')

    # 敵の BT (MoveTo 1 本。Blackboard の Entity キー target は BehaviorTree.bbEntityKey で主人公を入れる)
    write_json('chase.bb.json', {'engine': 'MyEngine', 'blackboard': 1, 'name': 'chase',
                                 'keys': [{'name': 'target', 'type': 'Entity'}]})
    write_meta('chase.bb.json', G_BB, 'blackboard')
    write_json('chase.bt.json', {
        'engine': 'MyEngine', 'behaviortree': 1, 'name': 'chase', 'blackboard': '%016x' % G_BB, 'root': 0,
        'nodes': [{'id': 0, 'type': 'MoveTo',
                   'params': {'acceptanceRadius': 0.8, 'failOnStuck': False},
                   'keys': {'target': 'target'}, 'decorators': [], 'children': [], 'pos': [0.0, 0.0]}]})
    write_meta('chase.bt.json', G_BT, 'behaviortree')

    print('done', OUT)


# ---- タイルマップ (60 x 20、3 レイヤー) ----
MAP_W = 60
MAP_H = 20


def make_tilemap():
    layers = {'Back': {}, 'Ground': {}, 'Front': {}}

    def put(layer, x, y, tile):
        layers[layer][(x, y)] = tile

    # Ground: 床 (y 0..1)、両端の壁 (y 2..13)、段差 (x 14..15、天面 y = 4)、
    # 一方通行板 (x 9..12、天面 y = 5)、壊れるブロック (x 7..8 を 2 列 3 段)
    for x in range(MAP_W):
        for y in (0, 1):
            put('Ground', x, y, 1)
    for y in range(2, 14):
        put('Ground', 0, y, 1)
        put('Ground', MAP_W - 1, y, 1)
    for x in range(14, 16):
        for y in (2, 3):
            put('Ground', x, y, 1)
    for x in range(9, 13):
        put('Ground', x, 4, 2)
    for x in (7, 8):
        for y in (2, 3, 4):
            put('Ground', x, y, 3)
    # Back: 低い壁と窓 (視差の空を隠しすぎない高さ)
    for x in list(range(1, 13)) + list(range(30, 59)):
        for y in (2, 3, 4):
            put('Back', x, y, 4)
    for x, y in ((2, 4), (11, 4), (33, 4), (41, 4), (50, 4), (56, 4)):
        put('Back', x, y, 5)
    # Front: 草 (床の上に決定的に散らす)
    rng = random.Random(SEED + 5)
    for x in range(1, MAP_W - 1):
        if rng.random() < 0.28 and (x, 2) not in layers['Ground']:
            put('Front', x, 2, 6)

    def chunks(cells):
        out = {}
        for (x, y), tile in cells.items():
            cx, cy = x // 16, y // 16
            ch = out.setdefault((cy, cx), [[0] * 16 for _ in range(16)])
            ch[y % 16][x % 16] = tile
        return [{'cx': cx, 'cy': cy, 'cells': rows} for (cy, cx), rows in sorted(out.items())]

    return {
        'engine': 'MyEngine', 'tilemap': 1, 'palette': G_PALETTE, 'cellSize': [1.0, 1.0],
        'layers': [
            {'name': 'Back', 'sortingLayerId': 0, 'orderInLayer': -20, 'collision': False, 'chunks': chunks(layers['Back'])},
            {'name': 'Ground', 'sortingLayerId': 0, 'orderInLayer': 0, 'collision': True, 'chunks': chunks(layers['Ground'])},
            {'name': 'Front', 'sortingLayerId': 0, 'orderInLayer': 30, 'collision': False, 'chunks': chunks(layers['Front'])},
        ]}


if __name__ == '__main__':
    sys.exit(main())
