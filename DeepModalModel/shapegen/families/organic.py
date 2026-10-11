#====================================================================================
#                          organic.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          キャラ・クリーチャー・岩・植物のファミリ
#====================================================================================
"""枝状体は attach_limb / chain_tube (円錐台 + 関節球) で組む。根元は親の内部点から
embed だけ食い込ませる。岩は楕円体を既存ピースのアンカー点に重ねて積む。"""
import math

import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, PlacementError, attach_limb, chain_tube


def _limb2(asm, ctx, start, d1, l1, d2, l2, r0, r1, r2, embed):
    """2 節の肢。d1/d2 は方向、l1/l2 は長さ。半径は最小厚みの半分を下回らせない。"""
    rmin = 0.5 * ctx.feature
    r0, r1, r2 = max(r0, rmin), max(r1, rmin), max(r2, rmin)
    mid = attach_limb(asm, start, d1, l1, r0, r1, embed)
    end = attach_limb(asm, mid, d2, l2, r1, r2, r1 * 0.5)
    return end


def _dir(yaw_deg, pitch_deg):
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    return np.array([math.cos(p) * math.cos(y), math.cos(p) * math.sin(y), math.sin(p)])


def biped_blocky(ctx):
    asm = Assembly(ctx)
    H = 1.0
    torso_h = H * ctx.u(0.28, 0.4)
    torso_w = H * ctx.u(0.18, 0.32)
    torso_d = max(ctx.feature, torso_w * ctx.u(0.4, 0.7))
    leg_len = H * ctx.u(0.3, 0.42)
    head_r = H * ctx.u(0.07, 0.12)
    z_torso = -H / 2 + leg_len + torso_h / 2
    blocky = ctx.rng.chance(0.5)
    if blocky:
        asm.add(P.Box(torso_w, torso_d, torso_h).posed(None, (0.0, 0.0, z_torso)))
    else:
        asm.add(P.capsule(min(torso_w, torso_d) * 0.5, torso_h - min(torso_w, torso_d)).posed(None, (0.0, 0.0, z_torso)))
    # 首と頭
    neck_r = ctx.thick(0.5, 0.8)
    top = np.array([0.0, 0.0, z_torso + torso_h / 2 - ctx.feature])
    neck_end = attach_limb(asm, top, (0.0, 0.0, 1.0), H * ctx.u(0.03, 0.08) + ctx.feature, neck_r, neck_r, ctx.feature,
                           joint_sphere=False)
    # 頭は首の先端に半径の半分だけ食い込ませる (先端に関節球は無いので深めに)
    if ctx.rng.chance(0.5):
        asm.add(P.sphere(head_r).posed(None, neck_end + np.array([0.0, 0.0, head_r * 0.5])))
    else:
        asm.add(P.Box(head_r * 1.8, head_r * 1.6, head_r * 2).posed(None, neck_end + np.array([0.0, 0.0, head_r * 0.5])))
    # 腕
    arm_r = ctx.thick(0.5, 0.8)
    arm_len = H * ctx.u(0.12, 0.2)
    for s in (-1.0, 1.0):
        shoulder = np.array([s * (torso_w / 2 - arm_r), 0.0, z_torso + torso_h / 2 - arm_r * 1.5])
        d1 = _dir(90.0 * s + ctx.u(-30.0, 30.0), ctx.u(-80.0, 10.0))
        d2 = rotation.axis_angle((1.0, 0.0, 0.0), ctx.u(-60.0, 60.0)) @ d1
        hand = _limb2(asm, ctx, shoulder, d1, arm_len, d2, arm_len, arm_r, arm_r * 0.9, arm_r * 0.8, arm_r * 1.5)
        if ctx.rng.chance(0.6):
            asm.add(P.Box(arm_r * 2.2, arm_r * 1.6, arm_r * 2.5).posed(None, hand))
    # 脚
    leg_r = ctx.thick(0.6, 1.0)
    for s in (-1.0, 1.0):
        hip = np.array([s * (torso_w / 2 - leg_r), 0.0, z_torso - torso_h / 2 + leg_r * 1.2])
        d1 = _dir(ctx.u(-20.0, 20.0) + (0.0 if s > 0 else 180.0), -90.0 + ctx.u(-25.0, 25.0))
        d2 = rotation.axis_angle((1.0, 0.0, 0.0), ctx.u(-40.0, 40.0)) @ d1
        foot = _limb2(asm, ctx, hip, d1, leg_len * 0.5, d2, leg_len * 0.5, leg_r, leg_r * 0.9, leg_r * 0.8, leg_r * 1.5)
        asm.add(P.Box(leg_r * 2.2, leg_r * 3.5, leg_r * 1.6).posed(None, foot + np.array([0.0, leg_r * 0.8, 0.0])))
    ctx.rec(torso=[torso_w, torso_d, torso_h], leg_len=leg_len, head_r=head_r, blocky=blocky)
    return asm


def quadruped(ctx):
    asm = Assembly(ctx)
    L = 1.0
    body_len = L * ctx.u(0.45, 0.65)
    body_r = L * ctx.u(0.08, 0.16)
    leg_len = L * ctx.u(0.2, 0.35)
    z_body = -0.5 + leg_len + body_r
    if ctx.rng.chance(0.5):
        asm.add(P.Ellipsoid(body_len / 2, body_r, body_r * ctx.u(0.8, 1.2)).posed(None, (0.0, 0.0, z_body)))
    else:
        asm.add(P.capsule(body_r, body_len - 2 * body_r).posed(rotation.rot_y(90.0), (0.0, 0.0, z_body)))
    leg_r = ctx.thick(0.5, 0.9)
    for sx in (-1.0, 1.0):
        for sy in (-1.0, 1.0):
            hip = np.array([sx * (body_len / 2 - body_r * 0.8), sy * body_r * 0.5, z_body - body_r * 0.5])
            d1 = _dir(90.0 * sy + ctx.u(-20.0, 20.0), -90.0 + ctx.u(-30.0, 10.0))
            d2 = rotation.axis_angle((0.0, 1.0, 0.0), ctx.u(-35.0, 35.0)) @ d1
            _limb2(asm, ctx, hip, d1, leg_len * 0.5, d2, leg_len * 0.5, leg_r, leg_r * 0.85, leg_r * 0.75, leg_r * 1.5)
    # 首と頭
    neck_r = ctx.thick(0.5, 0.9)
    front = np.array([body_len / 2 - body_r * 0.5, 0.0, z_body + body_r * 0.3])
    d = _dir(0.0, ctx.u(10.0, 60.0))
    neck_end = attach_limb(asm, front, d, L * ctx.u(0.08, 0.2), neck_r, neck_r * 0.9, body_r * 0.6)
    head_r = L * ctx.u(0.05, 0.09)
    asm.add(P.Ellipsoid(head_r * 1.4, head_r, head_r).posed(None, neck_end + d * head_r * 0.6))
    # 尾
    if ctx.rng.chance(0.7):
        n_tail = ctx.rng.int_range(2, 5)
        back = np.array([-body_len / 2 + body_r * 0.5, 0.0, z_body + body_r * 0.2])
        pts = [back]
        r0 = ctx.thick(0.5, 0.8)
        radii = [r0]
        seg = L * ctx.u(0.05, 0.1)
        dirv = _dir(180.0 + ctx.u(-15.0, 15.0), ctx.u(-30.0, 45.0))
        for i in range(n_tail):
            dirv = rotation.axis_angle((0.0, 1.0, 0.0), ctx.u(-25.0, 25.0)) @ dirv
            pts.append(pts[-1] + dirv * seg)
            radii.append(max(0.5 * ctx.feature, r0 * (1.0 - 0.5 * (i + 1) / n_tail)))
        chain_tube(asm, pts, radii, extend=r0 * 0.5)
        ctx.rec(tail=n_tail)
    ctx.rec(body_len=body_len, body_r=body_r, leg_len=leg_len, head_r=head_r)
    return asm


def tentacle_creature(ctx):
    asm = Assembly(ctx)
    body = ctx.u(0.12, 0.25)
    asm.add(P.Ellipsoid(body, body * ctx.u(0.7, 1.0), body * ctx.u(0.6, 1.0)))
    n = ctx.rng.int_range(3, 8)
    r0 = ctx.thick(0.6, 1.0)
    for i in range(n):
        yaw = 360.0 * i / n + ctx.u(-20.0, 20.0)
        pitch = ctx.u(-70.0, 20.0)
        d = _dir(yaw, pitch)
        pts = [d * body * 0.6]
        radii = [r0]
        n_seg = ctx.rng.int_range(2, 5)
        seg = ctx.u(0.08, 0.16)
        for k in range(n_seg):
            d = rotation.axis_angle(ctx.rng.unit_vector(), ctx.u(0.0, 35.0)) @ d
            pts.append(pts[-1] + d * seg)
            radii.append(max(0.5 * ctx.feature, r0 * (1.0 - 0.55 * (k + 1) / n_seg)))
        chain_tube(asm, pts, radii, extend=r0 * 0.5, segments=16)
    ctx.rec(body=body, tentacles=n, r0=r0)
    return asm


def tree_branching(ctx):
    asm = Assembly(ctx)
    trunk_r = ctx.thick(0.8, 1.6)
    trunk_h = ctx.u(0.3, 0.5)
    base = np.array([0.0, 0.0, -0.5])
    top = attach_limb(asm, base + np.array([0.0, 0.0, trunk_r]), (0.0, 0.0, 1.0), trunk_h, trunk_r, trunk_r * 0.8,
                      trunk_r, joint_sphere=True)
    levels = ctx.rng.int_range(2, 3)
    shrink = ctx.u(0.6, 0.8)
    n_branch = ctx.rng.int_range(2, 4)
    tips = []

    def grow(start, direction, length, r, level):
        if level > levels:
            tips.append(start)
            return
        for i in range(n_branch):
            yaw = 360.0 * i / n_branch + ctx.u(-30.0, 30.0)
            tilt = ctx.u(20.0, 60.0)
            d = rotation.axis_angle((0.0, 0.0, 1.0), yaw) @ (rotation.axis_angle((1.0, 0.0, 0.0), tilt) @ direction)
            rr = max(0.5 * ctx.feature, r * 0.75)
            end = attach_limb(asm, start, d, length, r, rr, r * 0.8, segments=12)  # 枝は多いので粗く
            grow(end, d, length * shrink, rr, level + 1)

    grow(top, np.array([0.0, 0.0, 1.0]), trunk_h * 0.7, trunk_r * 0.8, 1)
    n_canopy = ctx.rng.int_range(0, 3)
    for i in range(min(n_canopy, len(tips))):
        p = tips[ctx.rng.int_range(0, len(tips) - 1)]
        cr = ctx.u(0.06, 0.14)
        asm.add(P.Ellipsoid(cr, cr * ctx.u(0.8, 1.2), cr * ctx.u(0.6, 1.0)).posed(None, p))
    ctx.rec(trunk_r=trunk_r, trunk_h=trunk_h, levels=levels, branches=n_branch, canopy=n_canopy)
    return asm


def rock_blob(ctx):
    asm = Assembly(ctx)
    # 塊は相似形に縮めても占有が変わらない (最長辺で正規化される) ので、軽くするには
    # 個数を減らし、扁平で、互いに離して (重なりを浅く) 置く
    # 球に近い塊は充填率 ~50% (≈12,700 voxel) で必ず上限超になる。主塊を扁平 (板状の岩) にし、
    # 小塊を長軸方向に散らして全体を細長く保つ
    n = max(3, int(round(ctx.rng.int_range(3, 8) * ctx.lean)))
    r_main = 0.5
    asm.add(P.Ellipsoid(r_main, r_main * ctx.u(0.35, 0.7) * ctx.lean, r_main * ctx.u(0.15, 0.4) * ctx.lean))
    placed = 1
    for i in range(1, n):
        rr = max(ctx.feature, r_main * ctx.lu(0.15, 0.4))
        radii = (rr, rr * ctx.u(0.5, 1.0), rr * ctx.u(0.3, 0.6))
        ok = False
        for _ in range(4):
            parent = asm.pieces[ctx.rng.int_range(0, len(asm.pieces) - 1)]
            anchors = parent.anchor_points(ctx.overlap)
            if anchors.shape[0] == 0:
                continue
            q = anchors[ctx.rng.int_range(0, anchors.shape[0] - 1)]
            off = np.array(ctx.rng.unit_vector()) * rr * ctx.u(0.7, 1.3)
            R = rotation.random_rotation(ctx.rng, "free")
            if asm.try_add(P.Ellipsoid(*radii).posed(R, q + off)):
                ok = True
                placed += 1
                break
        if not ok:
            continue
    if placed < 3:
        raise PlacementError("rock_blob could not place enough lumps")
    ctx.rec(lumps=placed, r_main=r_main)
    return asm


def crystal_cluster(ctx):
    asm = Assembly(ctx)
    base_r = ctx.u(0.15, 0.3) * ctx.lean
    asm.add(P.Ellipsoid(base_r, base_r * ctx.u(0.6, 1.0), base_r * 0.5).posed(None, (0.0, 0.0, -0.4)))
    n = max(2, int(round(ctx.rng.int_range(2, 7) * ctx.lean)))
    for i in range(n):
        r = ctx.thick(1.0, 1.6)  # 円錐の先端は尖るので、根元半径そのものを最小厚み以上にする
        L = ctx.u(0.3, 0.8)
        d = _dir(ctx.u(0.0, 360.0), ctx.u(40.0, 90.0))
        start = np.array([ctx.u(-0.5, 0.5) * base_r, ctx.u(-0.5, 0.5) * base_r * 0.6, -0.4])
        R = rotation.align_z_to(d)
        body = P.ConvexPrism(P.regular_polygon(6, r), L)
        asm.add(body.posed(R, start + d * (L / 2 - base_r * 0.3)))
        # 先端 (円錐) は柱の上端に 0.6r 食い込ませる
        tip = P.frustum(r, 0.0, r * 1.5)
        asm.add(tip.posed(R, start + d * (L - base_r * 0.3 + r * 0.15)))
    ctx.rec(base_r=base_r, crystals=n)
    return asm


def mushroom_plant(ctx):
    asm = Assembly(ctx)
    if ctx.rng.chance(0.6):
        # きのこ: 柄 + 傘 (下面開口の殻)
        sr = ctx.thick(0.7, 1.4)
        sh = ctx.u(0.4, 0.7)
        asm.add(P.cylinder(sr, sh).posed(None, (0.0, 0.0, -0.5 + sh / 2)))
        cap_r = 0.5
        ez = ctx.u(0.35, 0.8) * ctx.lean
        wall = ctx.thick(1.0, 2.0)
        steps = 8
        outer = [(0.0, -cap_r * ez)] + [(cap_r * math.cos(-0.5 * math.pi + 0.5 * math.pi * i / steps),
                                          cap_r * ez * math.sin(-0.5 * math.pi + 0.5 * math.pi * i / steps))
                                         for i in range(1, steps + 1)]
        prof = P.shell_profile(outer, wall, open_top=True)
        # 反転後の傘は z ∈ [0, cap_r*ez] (縁が z=0、天頂が上)。柄の上端が天頂の壁の中
        # (内側の天井 + 0.5 wall) に来るように傘を置く — 空洞で止まると連結しない
        stem_top = -0.5 + sh
        cap_z = stem_top + 0.5 * wall - cap_r * ez
        cap = P.Revolve(prof, feature=wall, label="cap").posed(rotation.rot_x(180.0), (0.0, 0.0, cap_z))
        asm.add(cap)
        ctx.rec(kind="mushroom", sr=sr, sh=sh, ez=ez, wall=wall)
    else:
        # 鉢植え: トレイ + 茎 + 葉 (楕円体)
        from ..assembly import tray
        pw = ctx.u(0.3, 0.5)
        ph = ctx.u(0.2, 0.35)
        wall = ctx.thick(1.0, 1.8)
        tray(asm, (pw, pw, ph), wall, open_faces=("+z",), center=(0.0, 0.0, -0.5 + ph / 2))
        n = ctx.rng.int_range(1, 4)
        for i in range(n):
            x0 = ctx.u(-0.3, 0.3) * pw
            y0 = ctx.u(-0.3, 0.3) * pw
            r = ctx.thick(0.5, 0.7)
            pts = [np.array([x0, y0, -0.5 + wall * 0.5])]
            d = np.array([0.0, 0.0, 1.0])
            for k in range(ctx.rng.int_range(2, 4)):
                d = rotation.axis_angle(ctx.rng.unit_vector(), ctx.u(0.0, 30.0)) @ d
                pts.append(pts[-1] + d * ctx.u(0.12, 0.25))
            chain_tube(asm, pts, [r] * len(pts), extend=r * 0.5, segments=12)
            lr = ctx.u(0.05, 0.12)
            asm.add(P.Ellipsoid(lr * 1.5, lr, max(0.5 * ctx.feature, lr * 0.3)).posed(
                rotation.random_rotation(ctx.rng, "free"), pts[-1]))
        ctx.rec(kind="pot", pw=pw, ph=ph, stems=n)
    return asm
