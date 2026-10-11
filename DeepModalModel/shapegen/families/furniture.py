#====================================================================================
#                          furniture.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          家具・建材のファミリ (机・椅子・棚・階段・柱・門・柵・トラス)
#====================================================================================
import math

import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, box_at, chain_tube


def _leg(asm, ctx, x, y, z_top, z_bottom, r, embed, round_leg):
    """z_top (天板の内部) から z_bottom まで脚を下ろす。上端を embed だけ食い込ませる。"""
    h = z_top - z_bottom + embed
    c = (x, y, z_bottom + h / 2)
    if round_leg:
        asm.add(P.cylinder(r, h).posed(None, c))
    else:
        asm.add(P.Box(2 * r, 2 * r, h).posed(None, c))


def table(ctx):
    asm = Assembly(ctx)
    sy = ctx.u(0.5, 1.0) * ctx.lean
    tt = ctx.thick(1.0, 2.0)
    leg_h = ctx.u(0.4, 0.9)
    r = ctx.thick(0.5, 1.2)
    inset = ctx.u(0.0, 0.1)
    round_leg = ctx.rng.chance(0.5)
    n_leg = 4 if ctx.rng.chance(0.8) else 3
    box_at(asm, (1.0, sy, tt), (0.0, 0.0, 0.0))
    embed = tt * 0.5
    z_top = tt / 2 - embed
    corners = [(sx, sy_) for sx in (-1.0, 1.0) for sy_ in (-1.0, 1.0)]
    if n_leg == 3:
        corners = [(-1.0, -1.0), (-1.0, 1.0), (1.0, 0.0)]
    legs = []
    for cx, cy in corners:
        x = cx * (0.5 - inset - r)
        y = cy * (sy / 2 - inset - r)
        _leg(asm, ctx, x, y, z_top + embed, -leg_h, r, embed, round_leg)
        legs.append((x, y))
    n_str = ctx.rng.int_range(0, 2) if n_leg == 4 else 0
    sr = ctx.thick(0.5, 0.9)
    for k in range(n_str):
        z = -leg_h + leg_h * ctx.u(0.15, 0.5)
        if k == 0:
            box_at(asm, (1.0 - 2 * inset, 2 * sr, 2 * sr), (0.0, legs[0][1], z))
            box_at(asm, (1.0 - 2 * inset, 2 * sr, 2 * sr), (0.0, legs[3][1], z))
        else:
            box_at(asm, (2 * sr, sy - 2 * inset, 2 * sr), (legs[0][0], 0.0, z))
            box_at(asm, (2 * sr, sy - 2 * inset, 2 * sr), (legs[3][0], 0.0, z))
    ctx.rec(sy=sy, tt=tt, leg_h=leg_h, leg_r=r, n_leg=n_leg, round_leg=round_leg, stretchers=n_str)
    return asm


def chair_bench(ctx):
    asm = Assembly(ctx)
    bench = ctx.rng.chance(0.3)
    sx = 1.0 if bench else ctx.u(0.4, 0.55)
    sy = ctx.u(0.4, 0.55) * ctx.lean
    st = ctx.thick(1.0, 2.0)
    leg_h = ctx.u(0.3, 0.5)
    r = ctx.thick(0.5, 1.0)
    box_at(asm, (sx, sy, st), (0.0, 0.0, 0.0))
    embed = st * 0.5
    for cx in (-1.0, 1.0):
        for cy in (-1.0, 1.0):
            _leg(asm, ctx, cx * (sx / 2 - r), cy * (sy / 2 - r), st / 2, -leg_h, r, embed, False)
    back_h = ctx.u(0.35, 0.5)
    tilt = ctx.u(0.0, 15.0)
    R = rotation.rot_x(-tilt)
    y_back = -sy / 2 + r
    # 背の支柱 2 本 (座面に食い込ませる)
    post = P.Box(2 * r, 2 * r, back_h + embed)
    for cx in (-1.0, 1.0):
        base = np.array([cx * (sx / 2 - r), y_back, st / 2 - embed])
        c = base + R @ np.array([0.0, 0.0, (back_h + embed) / 2])
        asm.add(P.Box(2 * r, 2 * r, back_h + embed).posed(R, c))
    slats = ctx.rng.int_range(1, 5)
    if slats == 1 and ctx.rng.chance(0.5):
        # 一枚板
        c = np.array([0.0, y_back, st / 2 - embed]) + R @ np.array([0.0, 0.0, (back_h + embed) / 2])
        asm.add(P.Box(sx - 2 * r + 0.5 * r, st, back_h + embed).posed(R, c))
    else:
        sw = ctx.thick(1.0, 2.0)
        for k in range(slats):
            zz = embed + back_h * (0.15 + 0.75 * (k + 0.5) / slats)
            c = np.array([0.0, y_back, st / 2 - embed]) + R @ np.array([0.0, 0.0, zz])
            asm.add(P.Box(sx - r, st, sw).posed(R, c))
    arms = ctx.rng.chance(0.3) and not bench
    if arms:
        ah = ctx.u(0.15, 0.25)
        for cx in (-1.0, 1.0):
            x = cx * (sx / 2 - r)
            asm.add(P.Box(2 * r, 2 * r, ah + embed).posed(None, (x, sy / 2 - r, st / 2 - embed + (ah + embed) / 2)))
            asm.add(P.Box(2 * r, sy - r, 2 * r).posed(None, (x, 0.0, st / 2 + ah)))
    ctx.rec(bench=bench, sx=sx, sy=sy, st=st, leg_h=leg_h, back_h=back_h, tilt=tilt, slats=slats, arms=arms)
    return asm


def shelf_cabinet(ctx):
    asm = Assembly(ctx)
    H = 1.0
    W = ctx.u(0.5, 1.0) * ctx.lean
    D = ctx.u(0.25, 0.5) * ctx.lean
    pt = ctx.thick(1.0, 2.0)
    n_shelf = min(ctx.rng.int_range(1, 4), int((H - 2 * pt) // (ctx.opening + pt)) - 1)
    n_shelf = max(n_shelf, 0)
    # 側板 2 枚 (yz 平面)、背板 (xz 平面、省略あり)、棚板 (xy 平面)
    box_at(asm, (pt, D, H), (-W / 2 + pt / 2, 0.0, 0.0))
    box_at(asm, (pt, D, H), (W / 2 - pt / 2, 0.0, 0.0))
    back = ctx.rng.chance(0.7)
    if back:
        box_at(asm, (W, pt, H), (0.0, -D / 2 + pt / 2, 0.0))
    for k in range(n_shelf + 2):
        z = -H / 2 + pt / 2 + k * (H - pt) / (n_shelf + 1)
        box_at(asm, (W - pt, D, pt), (0.0, 0.0, z))
    drawers = ctx.rng.chance(0.3)
    if drawers and n_shelf >= 1:
        ft = ctx.thick(1.0, 1.5)
        for k in range(n_shelf + 1):
            z = -H / 2 + pt / 2 + (k + 0.5) * (H - pt) / (n_shelf + 1)
            if ctx.rng.chance(0.5):
                box_at(asm, (W - 2 * pt + pt * 0.5, ft, (H - pt) / (n_shelf + 1) - ctx.opening * 0.5),
                       (0.0, D / 2 - ft / 2, z))
    ctx.rec(W=W, D=D, pt=pt, shelves=n_shelf, back=back, drawers=drawers)
    return asm


def stairs_steps(ctx):
    asm = Assembly(ctx)
    k = ctx.rng.int_range(3, 8)
    width = ctx.u(0.4, 1.0) * ctx.lean
    run = 1.0 / k
    rise = ctx.u(0.5, 1.2) * run
    total_h = rise * k
    scale = min(1.0, 1.0 / total_h)
    rise *= scale
    ov = ctx.feature * 0.8
    solid = ctx.rng.chance(0.5)
    if solid:
        width = ctx.u(0.15, 0.4) * ctx.lean  # 中実段は嵩むので幅を絞る (踏み台・ブロック階段)
    for i in range(k):
        x = -0.5 + (i + 0.5) * run
        if solid:
            # 各段は床まで届く柱状 (隣と側面で重なる)
            h = rise * (i + 1)
            box_at(asm, (run + ov, width, h), (x, 0.0, -total_h * scale / 2 + h / 2))
        else:
            # 踏面板 + 蹴上板
            t = ctx.thick(1.0, 1.6)
            z_top = -total_h * scale / 2 + rise * (i + 1)
            box_at(asm, (run + ov, width, t), (x, 0.0, z_top - t / 2))
            box_at(asm, (t, width, rise + t), (x - run / 2 + t / 2, 0.0, z_top - rise / 2 - t / 2 + t / 2))
    ctx.rec(k=k, width=width, rise=rise, solid=solid)
    return asm


def pillar_column(ctx):
    asm = Assembly(ctx)
    r = ctx.u(0.06, 0.16)
    H = 1.0
    # 柱は全高にして台座・柱頭の板を貫通させる (板厚ぶんだけの重なりだと検出限界)
    if ctx.rng.chance(0.6):
        asm.add(P.cylinder(r, H))
    else:
        asm.add(P.ConvexPrism(P.regular_polygon(ctx.rng.choice([4, 6, 8]), r), H))
    bt = ctx.thick(1.0, 2.0)
    bw = r * ctx.u(2.2, 3.5)
    box_at(asm, (bw, bw, bt), (0.0, 0.0, -H / 2 + bt / 2))
    if ctx.rng.chance(0.8):
        box_at(asm, (bw, bw, bt), (0.0, 0.0, H / 2 - bt / 2))
    n_ridge = ctx.rng.int_range(0, 8)
    rr = ctx.thick(0.5, 0.7)
    if n_ridge * 2 * rr * 1.5 > 2 * math.pi * r:
        n_ridge = 0
    for i in range(n_ridge):
        a = 2 * math.pi * i / n_ridge
        asm.add(P.cylinder(rr, H * 0.8).posed(None, (r * math.cos(a), r * math.sin(a), 0.0)))
    ctx.rec(r=r, base_w=bw, ridges=n_ridge)
    return asm


def arch_frame(ctx):
    asm = Assembly(ctx)
    W = 1.0
    H = ctx.u(0.6, 1.0)
    pw = ctx.thick(1.5, 3.5)
    depth = ctx.thick(1.0, 3.0)
    for s in (-1.0, 1.0):
        box_at(asm, (pw, depth, H), (s * (W / 2 - pw / 2), 0.0, 0.0))
    if ctx.rng.chance(0.5):
        box_at(asm, (W, depth, pw), (0.0, 0.0, H / 2 - pw / 2))
        ctx.rec(top="lintel")
    else:
        n = 9
        R = W / 2 - pw / 2
        pts = [(R * math.cos(math.pi * i / (n - 1)) * -1.0, 0.0, H / 2 - pw + R * math.sin(math.pi * i / (n - 1)))
               for i in range(n)]
        chain_tube(asm, pts, [pw * 0.55] * n, extend=pw * 0.3)
        ctx.rec(top="arc")
    ctx.rec(H=H, pw=pw, depth=depth)
    return asm


def fence_railing(ctx):
    asm = Assembly(ctx)
    n_post = ctx.rng.int_range(2, 5)
    H = ctx.u(0.4, 1.0)
    pw = ctx.thick(1.0, 2.0)
    rw = ctx.thick(1.0, 1.6)
    n_rail = ctx.rng.int_range(1, 3)
    for i in range(n_post):
        x = -0.5 + pw / 2 + i * (1.0 - pw) / max(n_post - 1, 1)
        box_at(asm, (pw, pw, H), (x, 0.0, 0.0))
    for k in range(n_rail):
        z = -H / 2 + H * (0.2 + 0.7 * k / max(n_rail - 1, 1)) if n_rail > 1 else H * 0.2
        box_at(asm, (1.0, rw, rw), (0.0, 0.0, z))
    picket_w = ctx.thick(1.0, 1.4)
    n_pick = ctx.rng.int_range(0, 12)
    pitch = picket_w + ctx.opening
    n_pick = min(n_pick, int((1.0 - 2 * pw) // pitch))
    for i in range(n_pick):
        x = -0.5 + pw + pitch * (i + 0.5)
        box_at(asm, (picket_w, picket_w, H * 0.9), (x, 0.0, -H * 0.05))
    ctx.rec(posts=n_post, H=H, rails=n_rail, pickets=n_pick)
    return asm


def roof_truss(ctx):
    asm = Assembly(ctx)
    span = 1.0
    H = ctx.u(0.25, 0.55)
    bw = ctx.thick(1.0, 2.0)
    depth = ctx.thick(1.0, 2.5)
    a = np.array([-span / 2, 0.0, -H / 2])
    b = np.array([span / 2, 0.0, -H / 2])
    c = np.array([0.0, 0.0, H / 2])
    ext = bw

    def member(p, q, w):
        d = q - p
        L = float(np.linalg.norm(d))
        asm.add(P.Box(L + 2 * ext, depth, w).posed(rotation.rot_y(-math.degrees(math.atan2(d[2], d[0]))),
                                                   0.5 * (p + q)))

    member(a, b, bw)
    member(a, c, bw)
    member(b, c, bw)
    n_web = ctx.rng.int_range(1, 4)
    for i in range(n_web):
        t = (i + 1) / (n_web + 1)
        base = a + (b - a) * t
        top = a + (c - a) * t if t < 0.5 else c + (b - c) * (t - 0.5) * 2
        member(base, top if t != 0.5 else c, bw)
    ctx.rec(H=H, bw=bw, depth=depth, web=n_web)
    return asm
