#====================================================================================
#                          plates.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          板・殻・格子・断面材のファミリ
#====================================================================================
"""薄い部位 (板厚・バー幅) はすべて ctx.thick() で引く。穴・隙間は ctx.opening 以上。"""
import math

import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, bar_grid, box_at, profile_union, tray


def _max_divisions(span, bar_w, opening):
    """幅 span に幅 bar_w のバーを n+1 本置いて穴幅 >= opening を保てる最大 n。"""
    n = 0
    while True:
        gap = (span - (n + 2) * bar_w) / (n + 1)
        if gap < opening:
            return n
        n += 1


def plate_perforated(ctx):
    asm = Assembly(ctx)
    sy = ctx.u(0.4, 1.0)
    t = ctx.thick(1.0, 2.5)
    bar_w = ctx.thick(1.2, 3.0)
    nx = min(ctx.rng.int_range(1, 4), _max_divisions(1.0, bar_w, ctx.opening))
    ny = min(ctx.rng.int_range(1, 4), _max_divisions(sy, bar_w, ctx.opening))
    ctx.rec(sy=sy, t=t, bar_w=bar_w, nx=nx, ny=ny)
    if nx == 0 or ny == 0:
        box_at(asm, (1.0, sy, t), (0.0, 0.0, 0.0))
    else:
        bar_grid(asm, 1.0, sy, t, nx, ny, bar_w)
    return asm


def tray_open(ctx):
    asm = Assembly(ctx)
    sy = ctx.u(0.3, 0.9) * ctx.lean
    sz = ctx.u(0.1, 0.5) * ctx.lean
    wall = ctx.thick(1.0, 1.6)
    tray(asm, (1.0, sy, sz), wall, open_faces=("+z",))
    # 仕切り (x 方向を分ける板)。内側の区画幅が opening 以上になる本数だけ
    inner = 1.0 - 2.0 * wall
    n_div = ctx.rng.int_range(0, 3)
    n_div = min(n_div, _max_divisions(inner, wall, ctx.opening) )
    for i in range(n_div):
        x = -inner / 2 + (i + 1) * inner / (n_div + 1)
        box_at(asm, (wall, sy - wall, sz - wall * 0.5), (x, 0.0, wall * 0.25))
    ctx.rec(sy=sy, sz=sz, wall=wall, dividers=n_div)
    return asm


def tube_open(ctx):
    asm = Assembly(ctx)
    # 長さ/直径。肉厚 ≤ 0.8 r_out で最小厚みを保てる範囲に上限を置く
    ld_max = min(6.0, 0.5 / (ctx.feature / 0.8 * 1.05))
    ld = ctx.lu(0.8, ld_max)
    r_out = 0.5 / ld * ctx.lean
    wall = min(ctx.thick(1.0, 2.0), 0.8 * r_out)
    r_in = r_out - wall
    if r_in < 0.5 * ctx.opening:
        r_in = 0.5 * ctx.opening
        r_out = max(r_out, r_in + ctx.feature)
        wall = r_out - r_in
    length = 1.0
    asm.add(P.tube(r_in, r_out, length))
    n_flange = ctx.rng.int_range(0, 2)
    ft = ctx.thick(1.0, 1.5)
    fr = r_out * ctx.u(1.25, 1.8)
    for k in range(n_flange):
        z = (-length / 2 + ft / 2) if k == 0 else (length / 2 - ft / 2)
        asm.add(P.tube(r_in, fr, ft).posed(None, (0.0, 0.0, z)))
    ctx.rec(ld=ld, r_out=r_out, wall=wall, flanges=n_flange)
    return asm


def channel_profile(ctx):
    asm = Assembly(ctx)
    kind = ctx.rng.choice(["L", "U", "T", "I", "H", "Z"])
    tf = ctx.thick(1.0, 2.5)
    tw = ctx.thick(1.0, 2.5)
    width = max(ctx.u(0.25, 1.0) * ctx.lean, 2 * tw + ctx.opening)
    height = max(ctx.u(0.25, 1.0) * ctx.lean, 2 * tf + ctx.opening)
    depth = 1.0
    ctx.rec(kind=kind, width=width, height=height, tf=tf, tw=tw)
    profile_union(asm, kind, width, height, tf, tw, depth)
    return asm


def corrugated_plate(ctx):
    asm = Assembly(ctx)
    sy = ctx.u(0.5, 1.0) * ctx.lean
    t = ctx.thick(1.0, 1.6)
    rib_w = ctx.thick(1.0, 2.0)
    rib_h = ctx.thick(1.0, 3.0)
    along_x = ctx.rng.chance(0.5)
    span = sy if along_x else 1.0
    k = min(ctx.rng.int_range(3, 10), _max_divisions(span, rib_w, ctx.opening) + 1)
    k = max(k, 1)
    box_at(asm, (1.0, sy, t), (0.0, 0.0, 0.0))
    embed = 0.5 * t
    for i in range(k):
        c = -span / 2 + rib_w / 2 + (i + 0.5) * (span - rib_w) / k
        if along_x:
            box_at(asm, (1.0, rib_w, rib_h + embed), (0.0, c, t / 2 + rib_h / 2 - embed / 2))
        else:
            box_at(asm, (rib_w, sy, rib_h + embed), (c, 0.0, t / 2 + rib_h / 2 - embed / 2))
    ctx.rec(sy=sy, t=t, rib_w=rib_w, rib_h=rib_h, ribs=k, along_x=along_x)
    return asm


def bent_plate(ctx):
    """xz 平面で折れ曲がる板 (幅は y)。折れ目に板厚と同径のヒンジ円柱を置いて連結する。"""
    asm = Assembly(ctx)
    n_fold = ctx.rng.int_range(1, 4)
    t = ctx.thick(1.0, 1.6)
    width = ctx.u(0.25, 0.8) * ctx.lean  # 折ると AABB が縮んで h が小さくなるので幅は控えめ
    total = 1.0
    lengths = [ctx.u(0.5, 1.5) for _ in range(n_fold + 1)]
    s = sum(lengths)
    lengths = [v / s * total for v in lengths]
    angles = [ctx.u(20.0, 160.0) * ctx.rng.sign() for _ in range(n_fold)]
    p = np.array([0.0, 0.0, 0.0])
    heading = 0.0
    ext = t
    first = True
    for k, length in enumerate(lengths):
        d = np.array([math.cos(math.radians(heading)), 0.0, math.sin(math.radians(heading))])
        a = p - d * ext
        b = p + d * (length + ext)
        R = rotation.rot_y(-heading)
        asm.add(P.Box(length + 2 * ext, width, t).posed(R, 0.5 * (a + b)))
        if k < n_fold:
            joint = p + d * length
            if not first or True:
                asm.add(P.cylinder(0.9 * t, width).posed(rotation.rot_x(90.0), joint))
            heading += angles[k]
            p = joint
        first = False
    ctx.rec(n_fold=n_fold, t=t, width=width, lengths=lengths, angles=angles)
    return asm


def grid_lattice(ctx):
    asm = Assembly(ctx)
    bw = ctx.thick(1.0, 2.0)
    if ctx.rng.chance(0.65):
        sy = ctx.u(0.5, 1.0)
        t = ctx.thick(1.0, 2.0)
        nx = min(ctx.rng.int_range(2, 6), _max_divisions(1.0, bw, ctx.opening))
        ny = min(ctx.rng.int_range(2, 6), _max_divisions(sy, bw, ctx.opening))
        nx, ny = max(nx, 1), max(ny, 1)
        ctx.rec(mode="2d", bw=bw, t=t, nx=nx, ny=ny)
        bar_grid(asm, 1.0, sy, t, nx, ny, bw)
        return asm
    # 3D ケージ: 各軸方向のバーを n x n 本
    n = 2 if ctx.rng.chance(0.7) else 3
    bw = ctx.feature if n == 3 else bw
    size = np.array([1.0, ctx.u(0.5, 1.0) * ctx.lean, ctx.u(0.5, 1.0) * ctx.lean])
    ctx.rec(mode="3d", n=n, bw=bw, size=list(size))
    for axis in range(3):
        others = [a for a in range(3) if a != axis]
        for i in range(n):
            for j in range(n):
                c = np.zeros(3)
                c[others[0]] = -size[others[0]] / 2 + bw / 2 + i * (size[others[0]] - bw) / (n - 1)
                c[others[1]] = -size[others[1]] / 2 + bw / 2 + j * (size[others[1]] - bw) / (n - 1)
                s = np.full(3, bw)
                s[axis] = size[axis]
                box_at(asm, tuple(s), tuple(c))
    return asm


def frame_cage(ctx):
    asm = Assembly(ctx)
    size = np.array([1.0, ctx.u(0.4, 1.0) * ctx.lean, ctx.u(0.3, 1.0) * ctx.lean])
    bw = ctx.thick(1.0, 2.5)
    _edge_bars(asm, size, bw)
    n_panel = ctx.rng.int_range(0, 3)
    pt = ctx.thick(1.0, 1.5)
    faces = ctx.rng.shuffle_copy(["-z", "+z", "-x", "+x", "-y", "+y"])[:n_panel]
    for f in faces:
        axis = "xyz".index(f[1])
        sgn = -1.0 if f[0] == "-" else 1.0
        s = size.copy() - bw  # パネルは枠の内側いっぱい + バーに食い込む
        s[axis] = pt
        c = np.zeros(3)
        c[axis] = sgn * (size[axis] / 2 - bw / 2)
        box_at(asm, tuple(s), tuple(c))
    ctx.rec(size=list(size), bw=bw, panels=faces)
    return asm


def _edge_bars(asm, size, bw):
    """箱の 12 辺にバーを置く (隅で体積が重なる)。"""
    sx, sy, sz = size
    for axis in range(3):
        others = [a for a in range(3) if a != axis]
        for si in (-1.0, 1.0):
            for sj in (-1.0, 1.0):
                c = np.zeros(3)
                c[others[0]] = si * (size[others[0]] / 2 - bw / 2)
                c[others[1]] = sj * (size[others[1]] / 2 - bw / 2)
                s = np.full(3, bw)
                s[axis] = size[axis]
                box_at(asm, tuple(s), tuple(c))


def washer_disc(ctx):
    asm = Assembly(ctx)
    r_out = 0.5
    r_in = max(0.5 * ctx.opening, r_out * ctx.u(0.15, 0.7))
    t = ctx.thick(1.0, 2.0)
    asm.add(P.tube(r_in, r_out, t))
    if ctx.rng.chance(0.5):
        boss_h = t + ctx.thick(1.0, 3.0)
        boss_r = min(r_in + ctx.thick(1.0, 2.5), r_out * 0.8)
        asm.add(P.tube(r_in, boss_r, boss_h))
        ctx.rec(boss_h=boss_h, boss_r=boss_r)
    ctx.rec(r_in=r_in, t=t)
    return asm


def wedge_block(ctx):
    asm = Assembly(ctx)
    base = 1.0
    top = base * ctx.u(0.0, 0.7)
    height = ctx.u(0.2, 0.6) * ctx.lean
    depth = max(ctx.feature, ctx.u(0.1, 0.5) * ctx.lean)
    if top < ctx.feature:
        poly = [(-base / 2, 0.0), (base / 2, 0.0), (base / 2 * ctx.u(-0.5, 0.5), height)]
    else:
        off = ctx.u(-0.3, 0.3) * (base - top) / 2
        poly = [(-base / 2, 0.0), (base / 2, 0.0), (off + top / 2, height), (off - top / 2, height)]
    asm.add(P.ConvexPrism(poly, depth, feature=min(depth, height * 0.5)))
    if ctx.rng.chance(0.5):
        # 台座 (くさびの底辺 y=0 に重ねる薄板。y 方向に bt、くさびと [0, bt] で体積が重なる)
        bt = ctx.thick(1.0, 2.0)
        box_at(asm, (base, bt, depth * 1.2), (0.0, bt * 0.4, 0.0))
        ctx.rec(base_plate=bt)
    ctx.rec(top=top, height=height, depth=depth)
    return asm
