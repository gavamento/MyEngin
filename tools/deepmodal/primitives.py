#====================================================================================
#                          primitives.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          寸法乱数による原始形状 (OBJ) 生成 (seed 固定)
#====================================================================================
"""データ段階 0 (Primitive) の形状生成。箱 / 板 / 円柱 / 球 / 中空箱 / L 字 /
穴あき板を寸法乱数 (seed 固定、再現可能) で作り、OBJ として書き出す。

三角形の頂点順序 (CCW/CW) はボクセライザ (TriBoxOverlap) が向きを見ないので
気にしなくてよい (TriangleSoup.h の docstring 参照)。ここで気にするのは
「境界を閉じているか」(watertight かどうかは flood-fill の内部充填に影響する) だけ。
"""
import argparse
import math
import random
from pathlib import Path


def _rect_corners(hx, hy, z, cx=0.0, cy=0.0):
    return [(cx - hx, cy - hy, z), (cx + hx, cy - hy, z), (cx + hx, cy + hy, z),
            (cx - hx, cy + hy, z)]


def _cap_tris(base):
    return [(base + 0, base + 1, base + 2), (base + 0, base + 2, base + 3)]


def _side_band(bottom, top, n):
    faces = []
    for i in range(n):
        j = (i + 1) % n
        faces.append((bottom[i], bottom[j], top[i]))
        faces.append((bottom[j], top[j], top[i]))
    return faces


def box_mesh(sx, sy, sz, center=(0.0, 0.0, 0.0)):
    """軸平行の箱。center を中心に sx x sy x sz。"""
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    cx, cy, cz = center
    bottom = [(cx + dx, cy + dy, cz - hz) for dx, dy in ((-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy))]
    top = [(cx + dx, cy + dy, cz + hz) for dx, dy in ((-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy))]
    verts = bottom + top
    faces = _cap_tris(0) + [(t + 4, t2 + 4, t3 + 4) for (t, t2, t3) in _cap_tris(0)]
    faces += _side_band(list(range(4)), list(range(4, 8)), 4)
    return verts, faces


def plate_mesh(sx, sy, thickness):
    """薄い板 (z 方向に thickness の箱)。"""
    return box_mesh(sx, sy, thickness)


def cylinder_mesh(radius, height, segments=16):
    hz = height / 2.0
    bottom = []
    top = []
    for i in range(segments):
        theta = 2.0 * math.pi * i / segments
        x, y = radius * math.cos(theta), radius * math.sin(theta)
        bottom.append((x, y, -hz))
        top.append((x, y, hz))
    verts = bottom + top
    bc = len(verts)
    verts.append((0.0, 0.0, -hz))
    tc = len(verts)
    verts.append((0.0, 0.0, hz))

    faces = _side_band(list(range(segments)), list(range(segments, 2 * segments)), segments)
    for i in range(segments):
        j = (i + 1) % segments
        faces.append((bc, i, j))
        faces.append((tc, segments + j, segments + i))
    return verts, faces


def sphere_mesh(radius, lat_segments=8, lon_segments=12):
    verts = []
    for iy in range(lat_segments + 1):
        phi = math.pi * iy / lat_segments  # 0..pi (北極->南極)
        for ix in range(lon_segments):
            theta = 2.0 * math.pi * ix / lon_segments
            x = radius * math.sin(phi) * math.cos(theta)
            y = radius * math.sin(phi) * math.sin(theta)
            z = radius * math.cos(phi)
            verts.append((x, y, z))
    faces = []
    for iy in range(lat_segments):
        for ix in range(lon_segments):
            ix2 = (ix + 1) % lon_segments
            a = iy * lon_segments + ix
            b = iy * lon_segments + ix2
            c = (iy + 1) * lon_segments + ix
            d = (iy + 1) * lon_segments + ix2
            faces.append((a, b, c))
            faces.append((b, d, c))
    return verts, faces


def hollow_box_mesh(sx, sy, sz, wall):
    """上面が開いた中空箱 (トレイ状)。壁厚 wall。開口部があるので flood-fill が
    外部から内部空洞へ到達でき、空洞は interior に充填されない
    (完全に閉じた殻は「非 watertight は殻だけ」の逆で必ず内部が塗り潰される —
    §4.1 のコメント参照。中空を表すには開口が要る)。"""
    hx, hy, hz = sx / 2.0, sy / 2.0, sz / 2.0
    wall = min(wall, hx * 0.9, hy * 0.9, sz * 0.9)
    ihx, ihy = hx - wall, hy - wall
    iz_bottom = -hz + wall

    outer_bottom = _rect_corners(hx, hy, -hz)
    outer_top = _rect_corners(hx, hy, hz)
    inner_bottom = _rect_corners(ihx, ihy, iz_bottom)
    inner_top = _rect_corners(ihx, ihy, hz)  # 開口部 (蓋なし) の内側リム

    verts = outer_bottom + outer_top + inner_bottom + inner_top
    ob, ot, ib, it = range(0, 4), range(4, 8), range(8, 12), range(12, 16)

    faces = _cap_tris(0)  # outer bottom cap
    faces += _side_band(list(ob), list(ot), 4)  # outer side walls (top cap 無し = 開口)
    faces += [(t + 8, t2 + 8, t3 + 8) for (t, t2, t3) in _cap_tris(0)]  # inner bottom cap
    faces += _side_band(list(ib), list(it), 4)  # inner side walls (top cap 無し)
    # 天面のリム (幅 wall の額縁。外周と内周のリングをつなぐ)
    faces += [(ot[i], ot[(i + 1) % 4], it[i]) for i in range(4)]
    faces += [(ot[(i + 1) % 4], it[(i + 1) % 4], it[i]) for i in range(4)]
    return verts, faces


def l_shape_mesh(a, b, thickness_a, thickness_b, depth):
    """L 字断面 (幅 a x 高さ b、腕の太さ thickness_a/thickness_b) を depth だけ押し出す。
    断面は 2 枚の矩形 (水平バー [0,a]x[0,thickness_b]、垂直バー [0,thickness_a]x[thickness_b,b])
    に分解して三角形化する (凹多角形の一般 ear-clipping を避ける)。"""
    ta = min(thickness_a, a * 0.9)
    tb = min(thickness_b, b * 0.9)
    poly = [(0.0, 0.0), (a, 0.0), (a, tb), (ta, tb), (ta, b), (0.0, b)]
    cap_tris_2d = [(0, 1, 2), (0, 2, 3), (0, 3, 4), (0, 4, 5)]
    return _extrude_polygon(poly, cap_tris_2d, depth, center_xy=True)


def _extrude_polygon(poly2d, cap_tris_2d, depth, center_xy=False):
    if center_xy:
        cx = sum(p[0] for p in poly2d) / len(poly2d)
        cy = sum(p[1] for p in poly2d) / len(poly2d)
        poly2d = [(x - cx, y - cy) for x, y in poly2d]
    n = len(poly2d)
    hz = depth / 2.0
    front = [(x, y, -hz) for x, y in poly2d]
    back = [(x, y, hz) for x, y in poly2d]
    verts = front + back
    faces = list(cap_tris_2d)
    faces += [(i + n, j + n, k + n) for (i, j, k) in cap_tris_2d]
    faces += _side_band(list(range(n)), list(range(n, 2 * n)), n)
    return verts, faces


def perforated_plate_mesh(sx, sy, thickness, hole_w, hole_h):
    """矩形の穴が空いた板 (額縁形)。4 本の独立した矩形バーの和として組む —
    それぞれ単独で watertight な箱なので内部充填の心配が要らない。"""
    hx, hy = sx / 2.0, sy / 2.0
    hw, hh = min(hole_w / 2.0, hx * 0.8), min(hole_h / 2.0, hy * 0.8)
    bars = [
        # (center_x, center_y, size_x, size_y)
        (0.0, (hy + hh) / 2.0, sx, hy - hh),          # 上バー
        (0.0, -(hy + hh) / 2.0, sx, hy - hh),         # 下バー
        (-(hx + hw) / 2.0, 0.0, hx - hw, 2.0 * hh),   # 左バー
        ((hx + hw) / 2.0, 0.0, hx - hw, 2.0 * hh),    # 右バー
    ]
    verts = []
    faces = []
    for cx, cy, w, h in bars:
        bv, bf = box_mesh(w, h, thickness, center=(cx, cy, 0.0))
        base = len(verts)
        verts.extend(bv)
        faces.extend((i + base, j + base, k + base) for i, j, k in bf)
    return verts, faces


def flatten(verts, faces):
    positions = [tuple(v) for v in verts]
    indices = []
    for tri in faces:
        indices.extend(tri)
    return positions, indices


def write_obj(path, positions, indices):
    lines = ["# Deep-Modal primitive (tools/deepmodal/primitives.py が生成、手編集しない)"]
    for x, y, z in positions:
        lines.append(f"v {x:.6f} {y:.6f} {z:.6f}")
    for i in range(0, len(indices), 3):
        a, b, c = indices[i] + 1, indices[i + 1] + 1, indices[i + 2] + 1
        lines.append(f"f {a} {b} {c}")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def generate_primitive_set(seed: int = 1234, variants_per_type: int = 4):
    """seed 固定の寸法乱数で全種類の形状を variants_per_type 個ずつ作る。
    戻り値: [(name, positions, indices), ...] (名前は "<type>_<k>" で決定論的)。"""
    rng = random.Random(seed)
    out = []

    def add(name, verts, faces):
        positions, indices = flatten(verts, faces)
        out.append((name, positions, indices))

    for k in range(variants_per_type):
        sx = rng.uniform(0.05, 0.6)
        sy = rng.uniform(0.05, 0.6)
        sz = rng.uniform(0.05, 0.6)
        add(f"box_{k}", *box_mesh(sx, sy, sz))

    for k in range(variants_per_type):
        sx = rng.uniform(0.1, 0.8)
        sy = rng.uniform(0.1, 0.8)
        thickness = rng.uniform(0.005, 0.03)
        add(f"plate_{k}", *plate_mesh(sx, sy, thickness))

    for k in range(variants_per_type):
        radius = rng.uniform(0.03, 0.3)
        height = rng.uniform(0.05, 0.6)
        add(f"cylinder_{k}", *cylinder_mesh(radius, height, segments=16))

    for k in range(variants_per_type):
        radius = rng.uniform(0.03, 0.35)
        add(f"sphere_{k}", *sphere_mesh(radius, lat_segments=10, lon_segments=14))

    for k in range(variants_per_type):
        sx = rng.uniform(0.15, 0.6)
        sy = rng.uniform(0.15, 0.6)
        sz = rng.uniform(0.1, 0.4)
        wall = rng.uniform(0.01, 0.05)
        add(f"hollowbox_{k}", *hollow_box_mesh(sx, sy, sz, wall))

    for k in range(variants_per_type):
        a = rng.uniform(0.2, 0.6)
        b = rng.uniform(0.2, 0.6)
        ta = rng.uniform(0.03, a * 0.4)
        tb = rng.uniform(0.03, b * 0.4)
        depth = rng.uniform(0.03, 0.2)
        add(f"lshape_{k}", *l_shape_mesh(a, b, ta, tb, depth))

    for k in range(variants_per_type):
        sx = rng.uniform(0.2, 0.6)
        sy = rng.uniform(0.2, 0.6)
        thickness = rng.uniform(0.01, 0.04)
        hole_w = rng.uniform(0.05, sx * 0.5)
        hole_h = rng.uniform(0.05, sy * 0.5)
        add(f"perfplate_{k}", *perforated_plate_mesh(sx, sy, thickness, hole_w, hole_h))

    return out


def main():
    ap = argparse.ArgumentParser(description="Deep-Modal primitive OBJ 生成")
    ap.add_argument("--out", required=True, help="OBJ を書き出すディレクトリ")
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--variants", type=int, default=4)
    args = ap.parse_args()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    shapes = generate_primitive_set(seed=args.seed, variants_per_type=args.variants)
    for name, positions, indices in shapes:
        write_obj(out_dir / f"{name}.obj", positions, indices)
    print(f"[primitives] wrote {len(shapes)} OBJ file(s) to {out_dir}")


if __name__ == "__main__":
    main()
