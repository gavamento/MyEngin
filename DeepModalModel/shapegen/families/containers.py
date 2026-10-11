#====================================================================================
#                          containers.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          容器・小道具のファミリ (カップ・ボウル・瓶・樽・箱・蓋・積層)
#====================================================================================
"""中空の容器は必ず開口を持つ (閉じた殻はボクセライザが内部を埋める)。殻は
pieces.shell_profile で外形から肉厚ぶん内側へオフセットした断面を回転体にする。"""
import math

import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, box_at, chain_tube, tray
from .plates import _edge_bars


def _shell(ctx, outer_rz, wall, label):
    prof = P.shell_profile(outer_rz, wall, open_top=True)
    return P.Revolve(prof, feature=wall, label=label)


def cup_mug(ctx):
    asm = Assembly(ctx)
    tall = ctx.rng.chance(0.6)
    handle = ctx.rng.chance(0.75)
    rh = ctx.thick(0.5, 0.8)
    reach = (ctx.u(0.08, 0.2) + rh) if handle else 0.0
    H = 1.0 if tall else ctx.u(0.35, 0.7)
    # 取っ手を含めた幅が主寸法 1.0 を超えないように口径を決める (超えると厚み判定が落ちる)
    rt = (ctx.u(0.12, 0.38) if tall else 0.5 - reach / 2) * ctx.lean
    rb = rt * ctx.u(0.6, 1.0)
    wall = ctx.thick(1.0, 1.5)  # 大径の薄殻は面積で占有が嵩むので肉厚は控えめ
    if rt - wall < 0.5 * ctx.opening:
        rt = wall + 0.5 * ctx.opening
        rb = max(rb, rt * 0.6)
    outer = [(0.0, -H / 2), (rb, -H / 2), (rt, H / 2)]
    asm.add(_shell(ctx, outer, wall, "cup"))
    if handle:
        z0 = -H / 2 + H * ctx.u(0.2, 0.35)
        z1 = H / 2 - H * ctx.u(0.1, 0.25)
        pts = []
        n = 5
        for i in range(n):
            s = i / (n - 1)
            z = z0 + (z1 - z0) * s
            r_wall = rb + (rt - rb) * (z + H / 2) / H
            bulge = math.sin(math.pi * s)
            # 端点は壁の中央 (肉厚の中心) に置く = 壁に深さ wall/2 で埋まる
            x = (r_wall - wall / 2) + reach * bulge
            pts.append((x, 0.0, z))
        chain_tube(asm, pts, [rh] * n, extend=rh * 0.5)
        ctx.rec(handle_r=rh, reach=reach)
    if ctx.rng.chance(0.4):
        fr = ctx.thick(0.5, 0.8)
        asm.add(P.torus(max(rb - fr, fr * 1.5), fr).posed(None, (0.0, 0.0, -H / 2 + fr * 0.6)))
        ctx.rec(foot_r=fr)
    ctx.rec(tall=tall, H=H, rt=rt, rb=rb, wall=wall, handle=handle)
    return asm


def bowl_shell(ctx):
    asm = Assembly(ctx)
    open_deg = ctx.u(40.0, 170.0) * ctx.lean  # 下極からの開き角 (大きいほど深い)
    wall = ctx.thick(1.0, 2.0)
    ez = ctx.u(0.5, 1.0) * ctx.lean    # 楕円率 (z を潰す)
    R = 0.5
    steps = 10
    outer = [(0.0, -R * ez)]
    for i in range(1, steps + 1):
        phi = -0.5 * math.pi + math.radians(open_deg) * i / steps
        outer.append((R * math.cos(phi), R * ez * math.sin(phi)))
    if outer[-1][0] - wall < 0.5 * ctx.opening:
        open_deg = 90.0
        outer = [(0.0, -R * ez)] + [(R * math.cos(-0.5 * math.pi + 0.5 * math.pi * i / steps),
                                      R * ez * math.sin(-0.5 * math.pi + 0.5 * math.pi * i / steps))
                                     for i in range(1, steps + 1)]
    asm.add(_shell(ctx, outer, wall, "bowl"))
    if ctx.rng.chance(0.4):
        fr = ctx.thick(0.5, 0.8)
        asm.add(P.torus(R * ctx.u(0.3, 0.5), fr).posed(None, (0.0, 0.0, -R * ez + fr * 0.5)))
    ctx.rec(open_deg=open_deg, wall=wall, ez=ez)
    return asm


def bottle_vase(ctx):
    asm = Assembly(ctx)
    H = 1.0
    r_body = ctx.u(0.12, 0.3) * ctx.lean
    r_neck = r_body * ctx.u(0.3, 0.8)
    wall = ctx.thick(1.0, 1.8)
    hollow = ctx.rng.chance(0.7)
    if hollow and r_neck - wall < 0.5 * ctx.opening:
        r_neck = wall + 0.5 * ctx.opening
        r_body = max(r_body, r_neck * 1.2)
    z_body_top = -H / 2 + H * ctx.u(0.45, 0.75)
    z_shoulder = z_body_top + H * ctx.u(0.03, 0.12)
    r_mouth = r_neck * ctx.u(1.0, 1.4)
    outer = [(0.0, -H / 2), (r_body * ctx.u(0.7, 1.0), -H / 2), (r_body, -H / 2 + H * 0.15),
             (r_body, z_body_top), (r_neck, min(z_shoulder, H / 2 - H * 0.08)),
             (r_neck, H / 2 - H * 0.05), (r_mouth, H / 2)]
    if hollow:
        asm.add(_shell(ctx, outer, wall, "bottle"))
    else:
        asm.add(P.Revolve(outer + [(0.0, H / 2)], feature=min(2 * r_neck, wall), label="bottle_solid"))
    ctx.rec(r_body=r_body, r_neck=r_neck, wall=wall, hollow=hollow)
    return asm


def barrel_hoops(ctx):
    asm = Assembly(ctx)
    H = 1.0
    r_end = ctx.u(0.18, 0.3) * ctx.lean
    bulge = ctx.u(1.1, 1.35)
    wall = ctx.thick(1.0, 1.8)
    outer = [(0.0, -H / 2), (r_end, -H / 2), (r_end * bulge, -H * 0.2), (r_end * bulge, H * 0.2),
             (r_end, H / 2)]
    hollow = ctx.rng.chance(0.6)
    if hollow:
        asm.add(_shell(ctx, outer, wall, "barrel"))
    else:
        asm.add(P.Revolve(outer + [(0.0, H / 2)], feature=2 * r_end, label="barrel_solid"))
    n_hoop = ctx.rng.int_range(2, 4)
    hr = ctx.thick(0.5, 0.9)
    for k in range(n_hoop):
        z = -H / 2 + H * (0.12 + 0.76 * k / max(n_hoop - 1, 1))
        # 外形半径を線形補間
        r_at = np.interp(z, [p[1] for p in outer[1:]], [p[0] for p in outer[1:]])
        asm.add(P.torus(r_at - 0.2 * hr, hr).posed(None, (0.0, 0.0, z)))
    ctx.rec(r_end=r_end, bulge=bulge, wall=wall, hollow=hollow, hoops=n_hoop, hoop_r=hr)
    return asm


def crate_planks(ctx):
    asm = Assembly(ctx)
    size = np.array([1.0, ctx.u(0.5, 1.0) * ctx.lean, ctx.u(0.4, 1.0) * ctx.lean])
    bw = ctx.thick(1.0, 2.0)
    _edge_bars(asm, size, bw)
    pt = ctx.thick(1.0, 1.5)
    plank_w = ctx.thick(1.5, 4.0)
    gap = max(ctx.opening, ctx.thick(1.0, 3.0))
    faces = ctx.rng.shuffle_copy(["-z", "-x", "+x", "-y", "+y"])[: ctx.rng.int_range(2, 5)]
    for f in faces:
        axis = "xyz".index(f[1])
        sgn = -1.0 if f[0] == "-" else 1.0
        others = [a for a in range(3) if a != axis]
        along, across = others[0], others[1]
        span = size[across] - bw
        n = max(1, int((span + gap) // (plank_w + gap)))
        for i in range(n):
            c = np.zeros(3)
            c[axis] = sgn * (size[axis] / 2 - pt / 2)
            c[across] = -span / 2 + plank_w / 2 + i * (plank_w + gap) if n > 1 else 0.0
            s = np.zeros(3)
            s[axis] = pt
            s[along] = size[along]
            s[across] = plank_w
            box_at(asm, tuple(s), tuple(c))
    ctx.rec(size=list(size), bw=bw, plank_w=plank_w, gap=gap, faces=faces)
    return asm


def lid_bell(ctx):
    """下面が開いたドーム (蓋・釣鐘)。天頂につまみ。"""
    asm = Assembly(ctx)
    wall = ctx.thick(1.0, 2.0)
    R = 0.5
    ez = ctx.u(0.4, 1.0) * ctx.lean
    steps = 10
    # 上極から縁へ下る外形 (開口は下)。shell_profile は「下極→上の口」を想定するので z を反転して組む
    outer = [(0.0, -R * ez)]
    for i in range(1, steps + 1):
        phi = -0.5 * math.pi + 0.5 * math.pi * i / steps
        outer.append((R * math.cos(phi), R * ez * math.sin(phi)))
    dome = _shell(ctx, outer, wall, "dome")
    dome.posed(rotation.rot_x(180.0), (0.0, 0.0, 0.0))  # 開口を下に
    asm.add(dome)
    knob_r = ctx.thick(1.0, 1.5)
    top_z = R * ez  # 反転後の天頂 (開口は z=0 の面)
    if ctx.rng.chance(0.5):
        asm.add(P.sphere(knob_r).posed(None, (0.0, 0.0, top_z + knob_r * 0.4)))
    else:
        asm.add(P.frustum(knob_r * 0.6, knob_r, knob_r * 2).posed(None, (0.0, 0.0, top_z + knob_r * 0.6)))
    if ctx.rng.chance(0.5):
        rr = ctx.thick(0.5, 0.8)
        asm.add(P.torus(R - rr * 0.5, rr).posed(None, (0.0, 0.0, rr * 0.3)))
    ctx.rec(wall=wall, ez=ez, knob_r=knob_r)
    return asm


def stacked_discs(ctx):
    asm = Assembly(ctx)
    # 円盤を密に積むと実質中実の軸対称体になり占有が重い (相似に縮めても変わらない)。
    # 全高 1 に対して半径を小さめにとり、紡錘・軸・コマのような細長い積層にする
    k = ctx.rng.int_range(3, 9)
    monotone = ctx.rng.chance(0.5)
    r_hi = ctx.u(0.12, 0.26) * ctx.lean
    radii = [max(0.5 * ctx.feature, ctx.u(0.3, 1.0) * r_hi) for _ in range(k)]
    if monotone:
        radii = sorted(radii, reverse=True)
    ts = [ctx.u(0.5, 2.0) for _ in range(k)]
    total = sum(ts)
    ts = [t / total for t in ts]  # 全高 1.0
    overlap = 0.4 * min(ts)
    z = -sum(ts) / 2
    for r, t in zip(radii, ts):
        asm.add(P.cylinder(r, t + overlap).posed(None, (0.0, 0.0, z + t / 2)))
        z += t
    if ctx.rng.chance(0.5):
        ar = ctx.thick(0.5, 1.0)
        asm.add(P.cylinder(ar, sum(ts) + 2 * ar))
        ctx.rec(axle_r=ar)
    ctx.rec(k=k, monotone=monotone, radii=radii, ts=ts)
    return asm
