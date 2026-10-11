#====================================================================================
#                          weapons.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          武器・工具のファミリ (剣・斧/槌・槍・盾・弓・レンチ)
#====================================================================================
import math

import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, box_at, chain_tube


def sword(ctx):
    asm = Assembly(ctx)
    blade_len = ctx.u(0.55, 0.75)
    blade_w = ctx.u(0.06, 0.16)
    t = ctx.thick(1.2, 2.0)
    # 菱形/六角形断面 (xy) を z 方向に押し出す
    if ctx.rng.chance(0.5):
        poly = [(blade_w / 2, 0.0), (0.0, t / 2), (-blade_w / 2, 0.0), (0.0, -t / 2)]
    else:
        poly = [(blade_w / 2, -t / 4), (blade_w / 2, t / 4), (blade_w / 4, t / 2), (-blade_w / 4, t / 2),
                (-blade_w / 2, t / 4), (-blade_w / 2, -t / 4), (-blade_w / 4, -t / 2), (blade_w / 4, -t / 2)]
    # 全長 1: 上から刃 (blade_len)、鍔、柄、柄頭。刃の根元は鍔を貫通させる
    z_guard = 0.5 - blade_len
    blade = P.ConvexPrism(poly, blade_len + ctx.feature, feature=t * 0.9)
    asm.add(blade.posed(None, (0.0, 0.0, 0.5 - blade_len / 2 - ctx.feature / 2)))
    gw = ctx.u(0.15, 0.35)
    gt = ctx.thick(1.0, 1.8)
    if ctx.rng.chance(0.5):
        box_at(asm, (gw, gt, gt), (0.0, 0.0, z_guard))
    else:
        asm.add(P.cylinder(gt * 0.6, gw).posed(rotation.rot_y(90.0), (0.0, 0.0, z_guard)))
    pr = ctx.thick(0.6, 1.0)
    grip_r = ctx.thick(0.5, 0.9)
    grip_len = max(z_guard - (-0.5 + pr) + gt, 2 * grip_r)
    asm.add(P.cylinder(grip_r, grip_len).posed(None, (0.0, 0.0, z_guard + gt / 2 - grip_len / 2)))
    asm.add(P.sphere(pr).posed(None, (0.0, 0.0, -0.5 + pr)))
    ctx.rec(blade_len=blade_len, blade_w=blade_w, t=t, guard_w=gw, grip_r=grip_r)
    return asm


def axe_hammer(ctx):
    asm = Assembly(ctx)
    hr = ctx.thick(0.5, 0.9)
    asm.add(P.capsule(hr, 1.0 - 2 * hr))
    head_kind = ctx.rng.choice(["hammer", "axe", "double_axe", "pick"])
    hz = 0.5 - ctx.u(0.08, 0.18)
    head_t = ctx.thick(1.0, 2.5)
    if head_kind == "hammer":
        box_at(asm, (ctx.u(0.2, 0.4), head_t * ctx.u(1.0, 2.0), ctx.u(0.08, 0.18)), (0.0, 0.0, hz))
    elif head_kind in ("axe", "double_axe"):
        w = ctx.u(0.18, 0.3)
        h = ctx.u(0.15, 0.3)
        sides = (1.0,) if head_kind == "axe" else (1.0, -1.0)
        for s in sides:
            poly = [(0.0, -h * 0.35), (s * w, -h / 2), (s * w, h / 2), (0.0, h * 0.35)]
            if s < 0:
                poly = poly[::-1]
            asm.add(P.ConvexPrism(poly, head_t, feature=head_t).posed(rotation.rot_x(90.0), (0.0, 0.0, hz)))
    else:
        asm.add(P.frustum(head_t * 0.8, 0.0, ctx.u(0.25, 0.4)).posed(rotation.rot_y(90.0), (0.15, 0.0, hz)))
        box_at(asm, (0.2, head_t, head_t * 1.5), (0.0, 0.0, hz))
    ctx.rec(head=head_kind, hr=hr, head_t=head_t, hz=hz)
    return asm


def spear_staff(ctx):
    asm = Assembly(ctx)
    r = ctx.thick(0.5, 0.9)
    asm.add(P.cylinder(r, 1.0))
    kind = ctx.rng.choice(["spear", "staff", "trident"])
    if kind == "spear":
        hl = ctx.u(0.12, 0.25)
        head_r = max(ctx.feature, r * ctx.u(1.5, 2.5))  # 円錐は根元半径が最小厚み
        asm.add(P.frustum(head_r, 0.0, hl).posed(None, (0.0, 0.0, 0.5 - hl / 2 + r * 0.5)))
    elif kind == "trident":
        prong_r = max(ctx.feature, r)  # 円錐は根元半径が最小厚み
        bar = max(ctx.feature, 2 * r)
        box_at(asm, (r * 6 + bar, bar, bar), (0.0, 0.0, 0.5 - 0.15))
        for dx in (-1.0, 0.0, 1.0):
            hl = 0.15
            x = dx * r * 3.0
            asm.add(P.frustum(prong_r, 0.0, hl).posed(None, (x, 0.0, 0.5 - hl / 2 + bar * 0.3)))
    n_dec = ctx.rng.int_range(0, 3)
    for i in range(n_dec):
        z = ctx.u(-0.45, 0.3)
        if ctx.rng.chance(0.5):
            asm.add(P.sphere(r * ctx.u(1.3, 2.0)).posed(None, (0.0, 0.0, z)))
        else:
            tr = ctx.thick(0.5, 0.7)
            asm.add(P.torus(r + tr * 0.3, tr).posed(None, (0.0, 0.0, z)))
    ctx.rec(kind=kind, r=r, decorations=n_dec)
    return asm


def shield(ctx):
    asm = Assembly(ctx)
    ey = ctx.u(0.6, 1.0)
    wall = ctx.thick(1.0, 2.0)
    if ctx.rng.chance(0.5):
        # 浅いドーム殻 (開口は裏面)
        steps = 8
        depth = ctx.u(0.08, 0.2)
        Rs = (0.25 + depth * depth) / (2 * depth)  # 球冠の半径
        phi0 = math.asin((Rs - depth) / Rs)
        outer = [(0.0, -Rs)]
        for i in range(1, steps + 1):
            phi = -0.5 * math.pi + (0.5 * math.pi - phi0) * i / steps
            outer.append((Rs * math.cos(phi), Rs * math.sin(phi)))
        prof = P.shell_profile(outer, wall, open_top=True)
        # 殻の下極 (z=-Rs) が盾の表、縁 (z=-Rs+depth) が裏 = 開口は +z 側。縁を z=0 に置く
        dome = P.Revolve(prof, feature=wall, label="shield_dome")
        dome.posed(None, (0.0, 0.0, Rs - depth))
        asm.add(dome)
        front_z = -depth
        back_z = -depth + wall  # 握りは殻の内側の天井 (空洞ではなく壁) に埋める
        ctx.rec(dome_depth=depth)
    else:
        n = 24
        poly = [(0.5 * math.cos(2 * math.pi * i / n), 0.5 * ey * math.sin(2 * math.pi * i / n)) for i in range(n)]
        asm.add(P.ConvexPrism(poly, wall, feature=wall))
        front_z = -wall / 2
        back_z = wall / 2
    br = ctx.thick(0.8, 1.5)
    asm.add(P.sphere(br).posed(None, (0.0, 0.0, front_z - br * 0.3)))
    gt = ctx.thick(1.0, 1.5)
    # 握り: 裏面に半分 (0.5 gt) 埋める
    box_at(asm, (ctx.u(0.2, 0.4), gt, gt * 2.5), (0.0, 0.0, back_z + gt * 0.75))
    ctx.rec(ey=ey, wall=wall, boss_r=br)
    return asm


def bow_arc(ctx):
    asm = Assembly(ctx)
    n = ctx.rng.int_range(8, 12)
    arc = ctx.u(60.0, 150.0)
    R = 0.5 / math.sin(math.radians(arc) / 2)
    r_mid = ctx.thick(0.6, 0.9)
    r_end = max(0.5 * ctx.feature, r_mid * ctx.u(0.6, 0.9))
    pts = []
    radii = []
    for i in range(n):
        s = i / (n - 1)
        a = math.radians(-arc / 2 + arc * s)
        pts.append((R * math.sin(a), 0.0, R * math.cos(a) - R))
        radii.append(r_end + (r_mid - r_end) * math.sin(math.pi * s))
    chain_tube(asm, pts, radii, extend=r_mid * 0.5)
    box_at(asm, (ctx.u(0.08, 0.15), r_mid * 2.2, r_mid * 2.2), (0.0, 0.0, 0.0))
    if ctx.rng.chance(0.7):
        sr = ctx.thick(0.5, 0.6)
        a, b = np.array(pts[0]), np.array(pts[-1])
        d = b - a
        L = float(np.linalg.norm(d))
        asm.add(P.cylinder(sr, L + 2 * r_end).posed(rotation.align_z_to(d), 0.5 * (a + b)))
        ctx.rec(string_r=sr)
    ctx.rec(n=n, arc=arc, r_mid=r_mid, r_end=r_end)
    return asm


def wrench_tool(ctx):
    asm = Assembly(ctx)
    t = ctx.thick(1.0, 2.0)
    w = ctx.u(0.08, 0.16)
    box_at(asm, (0.8, w, t), (0.0, 0.0, 0.0))
    jaw_gap = max(ctx.opening, ctx.u(0.08, 0.2))
    jaw_len = ctx.u(0.12, 0.2)
    jaw_w = ctx.thick(1.0, 2.0)
    # 開口端: U 字 (基部 + 2 本の顎)
    x0 = 0.4
    box_at(asm, (jaw_w * 1.5, jaw_gap + 2 * jaw_w, t), (x0, 0.0, 0.0))
    for s in (-1.0, 1.0):
        box_at(asm, (jaw_len, jaw_w, t), (x0 + jaw_len / 2, s * (jaw_gap / 2 + jaw_w / 2), 0.0))
    other = ctx.rng.choice(["ring", "box_end", "none"])
    if other == "ring":
        rr = max(ctx.opening * 0.5, ctx.u(0.05, 0.1))
        asm.add(P.tube(rr, rr + jaw_w, t).posed(None, (-0.4 - rr * 0.6, 0.0, 0.0)))
    elif other == "box_end":
        box_at(asm, (0.15, w * 1.8, t * 1.5), (-0.4, 0.0, 0.0))
    ctx.rec(t=t, w=w, jaw_gap=jaw_gap, jaw_len=jaw_len, other=other)
    return asm
