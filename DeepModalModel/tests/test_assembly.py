#====================================================================================
#                          test_assembly.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          連結規則 (重なり / 離れ / 面接触) と部品ヘルパ
#====================================================================================
import numpy as np

from shapegen import constants as C
from shapegen import pieces as P
from shapegen.assembly import (Assembly, ShapeContext, bar_grid, chain_tube, profile_union, tray,
                               attach_limb)
from shapegen.rng import ShapeRng


def _ctx():
    return ShapeContext(ShapeRng(1), "identity", "small")


def test_overlapping_boxes_connected():
    asm = Assembly(_ctx())
    asm.add(P.Box(1.0, 0.2, 0.2))
    asm.add(P.Box(0.2, 1.0, 0.2).posed(None, (0.45, 0.4, 0.0)))  # 角で体積が重なる
    assert asm.is_connected(asm.ctx.overlap)


def test_separated_boxes_not_connected():
    asm = Assembly(_ctx())
    asm.add(P.Box(1.0, 0.2, 0.2))
    asm.add(P.Box(1.0, 0.2, 0.2).posed(None, (0.0, 0.5, 0.0)))
    assert not asm.is_connected(asm.ctx.overlap)
    assert not asm.try_add(P.Box(0.1, 0.1, 0.1).posed(None, (0.0, 5.0, 0.0)))


def test_face_contact_only_not_connected():
    """面で接するだけ (体積の重なり無し) は連結とみなさない。"""
    asm = Assembly(_ctx())
    asm.add(P.Box(1.0, 0.2, 0.2))
    asm.add(P.Box(1.0, 0.2, 0.2).posed(None, (0.0, 0.2, 0.0)))
    assert not asm.is_connected(asm.ctx.overlap)


def test_sphere_center_on_cylinder_axis_connected():
    """球の中心が円柱の端面より内側: 円柱側の候補点だけだと拾えない組合せ (双方向判定)。"""
    asm = Assembly(_ctx())
    asm.add(P.cylinder(0.05, 0.6))
    asm.add(P.sphere(0.06).posed(None, (0.0, 0.0, 0.28)))
    assert asm.is_connected(asm.ctx.overlap)


def test_helpers_produce_connected_assemblies():
    ctx = _ctx()
    m = ctx.overlap
    asm = Assembly(ctx)
    tray(asm, (1.0, 0.6, 0.3), 0.08)
    assert len(asm.pieces) == 5 and asm.is_connected(m)

    asm = Assembly(ctx)
    bar_grid(asm, 1.0, 0.8, 0.08, 3, 2, 0.1)
    assert len(asm.pieces) == 4 + 3 and asm.is_connected(m)

    for kind in ("L", "U", "T", "I", "H", "Z"):
        asm = Assembly(ctx)
        profile_union(asm, kind, 0.6, 0.5, 0.08, 0.08, 1.0)
        assert asm.is_connected(m), kind

    asm = Assembly(ctx)
    pts = [(0, 0, 0), (0.3, 0, 0.1), (0.5, 0.2, 0.3), (0.6, 0.5, 0.3)]
    chain_tube(asm, pts, [0.05, 0.045, 0.04, 0.035], extend=0.01)
    assert asm.is_connected(m)

    asm = Assembly(ctx)
    asm.add(P.Box(0.3, 0.3, 0.3))
    tip = attach_limb(asm, (0.0, 0.0, 0.14), (0.0, 0.0, 1.0), 0.4, 0.05, 0.04, 0.05)
    assert np.allclose(tip, (0.0, 0.0, 0.54))
    assert asm.is_connected(m)


def test_finalize_reports_pre_checks():
    ctx = _ctx()
    asm = Assembly(ctx)
    asm.add(P.Box(1.0, 0.3, 0.3))
    asm.add(P.Box(1.0, 0.3, 0.3).posed(None, (0.0, 2.0, 0.0)))
    shape = asm.finalize(np.eye(3), "identity")
    assert shape.reject_reason == "pre_reject_placement"

    asm = Assembly(ctx)
    asm.add(P.Box(1.0, 0.3, 0.001))  # 薄すぎる
    shape = asm.finalize(np.eye(3), "identity")
    assert shape.reject_reason == "pre_reject_thin"

    asm = Assembly(ctx)
    asm.add(P.Box(1.0, 1.0, 1.0))  # 満杯 → 推定が上限超
    shape = asm.finalize(np.eye(3), "identity")
    assert shape.reject_reason == "pre_reject_est_high"

    asm = Assembly(ctx)
    asm.add(P.Box(1.0, 0.3, 0.1))
    shape = asm.finalize(np.eye(3), "identity")
    assert shape.reject_reason == ""
    assert abs(shape.L - 1.0) < 1e-9 and shape.min_feature_vox > C.MIN_FEATURE_VOXELS


def test_context_thick_respects_minimum_and_fill():
    ctx = _ctx()
    for _ in range(200):
        t = ctx.thick(1.0, 3.0)
        assert ctx.feature - 1e-12 <= t <= 3.0 * ctx.feature * ctx.fill + 1e-12
    ctx.fill = 0.1
    for _ in range(50):
        assert abs(ctx.thick(1.0, 3.0) - ctx.feature) < 1e-12  # 上限が下限を下回ると下限に張り付く
