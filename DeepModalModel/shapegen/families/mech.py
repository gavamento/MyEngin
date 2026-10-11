#====================================================================================
#                          mech.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          機械部品のファミリ (歯車・フランジ・ブラケット・管継手・ピストン・ばね・鎖・束)
#====================================================================================
import math

import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, bar_grid, box_at, chain_tube


def gear_toothed(ctx):
    asm = Assembly(ctx)
    t = ctx.thick(1.0, 3.0)
    tooth_len = ctx.u(0.05, 0.12)
    r_out = 0.5 - tooth_len
    r_in = max(0.5 * ctx.opening, r_out * ctx.u(0.1, 0.4))
    tooth_w = ctx.thick(1.0, 2.0)
    k = ctx.rng.int_range(6, 24)
    while k > 3 and k * (tooth_w + ctx.opening) > 2 * math.pi * r_out:
        k -= 1
    asm.add(P.tube(r_in, r_out, t))
    embed = ctx.feature
    for i in range(k):
        a = 2 * math.pi * i / k
        R = rotation.rot_z(math.degrees(a))
        c = R @ np.array([r_out + tooth_len / 2 - embed / 2, 0.0, 0.0])
        asm.add(P.Box(tooth_len + embed, tooth_w, t).posed(R, c))
    if ctx.rng.chance(0.5):
        hub_h = t + ctx.thick(1.0, 3.0)
        asm.add(P.tube(r_in, min(r_in + ctx.thick(1.5, 3.0), r_out * 0.7), hub_h))
        ctx.rec(hub_h=hub_h)
    ctx.rec(t=t, tooth_len=tooth_len, r_in=r_in, tooth_w=tooth_w, teeth=k)
    return asm


def flange_bolt(ctx):
    asm = Assembly(ctx)
    t = ctx.thick(1.0, 2.5)
    r_hole = max(0.5 * ctx.opening, ctx.u(0.08, 0.2))
    asm.add(P.tube(r_hole, 0.5, t))
    pipe_h = ctx.u(0.3, 1.0) * ctx.lean
    pipe_wall = ctx.thick(1.0, 2.0)
    asm.add(P.tube(r_hole, r_hole + pipe_wall, pipe_h).posed(None, (0.0, 0.0, pipe_h / 2 - t / 2)))
    k = ctx.rng.int_range(3, 8)
    br = ctx.thick(0.6, 1.0)
    bolt_circle = 0.5 * ctx.u(0.65, 0.85)
    while k > 2 and k * (2 * br + ctx.opening) > 2 * math.pi * bolt_circle:
        k -= 1
    bh = t + ctx.thick(1.0, 2.0)
    for i in range(k):
        a = 2 * math.pi * i / k
        asm.add(P.cylinder(br, bh).posed(None, (bolt_circle * math.cos(a), bolt_circle * math.sin(a), 0.0)))
    ctx.rec(t=t, r_hole=r_hole, pipe_h=pipe_h, bolts=k, bolt_r=br)
    return asm


def bracket_angle(ctx):
    asm = Assembly(ctx)
    t = ctx.thick(1.0, 2.0)
    a = 1.0
    b = ctx.u(0.4, 1.0)
    w = ctx.u(0.3, 0.8)
    holes = ctx.rng.chance(0.5)
    bar_w = ctx.thick(1.5, 3.0)

    def plate(sx, sz, center, R):
        if holes and sx > 3 * bar_w + 2 * ctx.opening:
            nx = max(1, min(3, int((sx - bar_w) // (bar_w + ctx.opening))))
            bar_grid(asm, sx, w, t, nx, 1, bar_w, center=center, R=R)
        else:
            asm.add(P.Box(sx, w, t).posed(R, center))

    # 水平板 (xy)、垂直板 (yz)。角で体積が重なる
    plate(a, t, (0.0, 0.0, 0.0), np.eye(3))
    plate(b, t, (-a / 2 + t / 2, 0.0, b / 2 - t / 2), rotation.rot_y(90.0))
    n_g = ctx.rng.int_range(0, 2)
    gt = ctx.thick(1.0, 1.5)
    for i in range(n_g):
        y = (-w / 2 + gt / 2 + i * (w - gt)) if n_g == 2 else 0.0
        g = min(a, b) * ctx.u(0.3, 0.7)
        poly = [(-a / 2 + t * 0.5, t * 0.5), (-a / 2 + t * 0.5 + g, t * 0.5), (-a / 2 + t * 0.5, t * 0.5 + g)]
        asm.add(P.ConvexPrism(poly, gt, feature=gt).posed(rotation.rot_x(90.0), (0.0, y, 0.0)))
    ctx.rec(t=t, b=b, w=w, holes=holes, gussets=n_g)
    return asm


def pipe_elbow_tee(ctx):
    asm = Assembly(ctx)
    # 肉厚 ≤ 0.6 r_out で最小厚みを保てるよう、外径の下限を最小厚みから決める
    r_out = max(ctx.u(0.08, 0.15) * ctx.lean, ctx.feature / 0.6 * 1.05)
    wall = min(ctx.thick(1.0, 1.8), 0.6 * r_out)
    r_in = r_out - wall
    if r_in < 0.5 * ctx.opening:
        r_in = 0.5 * ctx.opening
        r_out = r_in + wall
    kind = ctx.rng.choice(["elbow", "elbow", "tee", "cross"])
    angle = 90.0 if ctx.rng.chance(0.6) else ctx.u(45.0, 135.0)
    L = 0.5
    # 主管: z 軸、[-L, 0]
    asm.add(P.tube(r_in, r_out, L).posed(None, (0.0, 0.0, -L / 2)))
    branches = [angle]
    if kind == "tee":
        branches = [90.0, 270.0]
        asm.add(P.tube(r_in, r_out, L).posed(None, (0.0, 0.0, L / 2)))  # 直管を延長
    elif kind == "cross":
        branches = [90.0, 270.0]
        asm.add(P.tube(r_in, r_out, L).posed(None, (0.0, 0.0, L / 2)))
        branches.append(0.0)  # 上方向にもう 1 本 (直管の先、重なる)
    for a in branches:
        # 分岐方向 (xz 平面)。分岐管は主管の壁の内側 (r_in) から始める = 壁と体積で重なる
        d = np.array([math.sin(math.radians(a)), 0.0, math.cos(math.radians(a))])
        if a == 0.0:
            continue
        # 分岐の先端は主管の端と同じ距離 L に揃える (全体の最長辺が 1 を超えると厚み判定が落ちる)
        start = d * r_in * 0.9
        end = d * L
        asm.add(P.tube(r_in, r_out, float(np.linalg.norm(end - start)))
                .posed(rotation.align_z_to(d), 0.5 * (start + end)))
    if ctx.rng.chance(0.4):
        ft = ctx.thick(1.0, 1.5)
        asm.add(P.tube(r_in, r_out * 1.5, ft).posed(None, (0.0, 0.0, -L + ft / 2)))
    ctx.rec(kind=kind, angle=angle, r_out=r_out, wall=wall)
    return asm


def piston_shaft(ctx):
    asm = Assembly(ctx)
    R = ctx.u(0.12, 0.25) * ctx.lean
    H = ctx.u(0.2, 0.4) * ctx.lean
    asm.add(P.cylinder(R, H).posed(None, (0.0, 0.0, 0.5 - H / 2)))
    rr = ctx.thick(0.6, 1.2)
    rod_len = 1.0 - H
    asm.add(P.cylinder(rr, rod_len + ctx.feature).posed(None, (0.0, 0.0, -0.5 + rod_len / 2)))
    pin_r = ctx.thick(0.5, 0.8)
    asm.add(P.cylinder(pin_r, R * 2.4).posed(rotation.rot_y(90.0), (0.0, 0.0, 0.5 - H / 2)))
    n_ring = ctx.rng.int_range(0, 2)
    for i in range(n_ring):
        tr = ctx.thick(0.5, 0.7)
        asm.add(P.torus(R, tr).posed(None, (0.0, 0.0, 0.5 - H * (0.25 + 0.5 * i))))
    if ctx.rng.chance(0.5):
        asm.add(P.sphere(rr * ctx.u(1.5, 2.5)).posed(None, (0.0, 0.0, -0.5 + rr * 1.5)))
    ctx.rec(R=R, H=H, rod_r=rr, rings=n_ring)
    return asm


def spring_coil(ctx):
    asm = Assembly(ctx)
    wr = ctx.thick(0.5, 0.75)
    turns = ctx.rng.int_range(2, 6)
    pitch = max(2 * wr + ctx.opening, ctx.u(0.1, 0.3))
    if turns * pitch > 1.0:
        turns = max(2, int(1.0 // pitch))
        pitch = 1.0 / turns
    Rh = 0.5 - wr
    per_turn = 10
    n = turns * per_turn + 1
    pts = []
    for i in range(n):
        s = i / per_turn
        a = 2 * math.pi * s
        pts.append((Rh * math.cos(a), Rh * math.sin(a), -turns * pitch / 2 + pitch * s))
    chain_tube(asm, pts, [wr] * n, extend=wr * 0.6, segments=12)
    ctx.rec(wire_r=wr, turns=turns, pitch=pitch)
    return asm


def chain_links(ctx):
    asm = Assembly(ctx)
    k = ctx.rng.int_range(2, 5)
    r = ctx.thick(0.5, 0.9)
    R = max(r * 1.8, (1.0 + 0.6 * r * (k - 1)) / (2 * k) if k > 0 else 0.3)
    # 隣接リンクは中心間距離 d = 2R - 0.6r で管同士が体積で交差する (接触だけだと非連結)
    d = 2 * R - 0.6 * r
    total = 2 * R + d * (k - 1)
    scale = 1.0 / total
    R *= scale
    r *= scale
    d *= scale
    r = max(r, 0.5 * ctx.feature)
    x = -0.5 + R
    for i in range(k):
        # 鎖は x 方向に伸びる: 偶数リンクは xy 平面 (軸 z)、奇数リンクは xz 平面 (軸 y)
        Rm = rotation.rot_x(90.0) if i % 2 else np.eye(3)
        asm.add(P.torus(R, r).posed(Rm, (x, 0.0, 0.0)))
        x += d
    ctx.rec(links=k, R=R, r=r)
    return asm


def rod_bundle(ctx):
    asm = Assembly(ctx)
    n = ctx.rng.int_range(3, 9)
    rr = ctx.thick(0.5, 0.9)
    Rb = max(rr * 1.5, (n * (2 * rr + ctx.opening * 0.5)) / (2 * math.pi))
    Rb = min(Rb, 0.5 - rr)
    for i in range(n):
        a = 2 * math.pi * i / n
        asm.add(P.cylinder(rr, 1.0).posed(None, (Rb * math.cos(a), Rb * math.sin(a), 0.0)))
    n_band = ctx.rng.int_range(1, 2)
    br = max(rr, 0.5 * ctx.feature)
    for i in range(n_band):
        z = -0.25 if n_band == 2 and i == 0 else (0.25 if n_band == 2 else 0.0)
        asm.add(P.torus(Rb, br * 1.1).posed(None, (0.0, 0.0, z)))
    ctx.rec(rods=n, rod_r=rr, Rb=Rb, bands=n_band)
    return asm
