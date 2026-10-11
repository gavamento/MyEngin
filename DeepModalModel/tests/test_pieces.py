#====================================================================================
#                          test_pieces.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          全ピース × 境界パラメータで閉性・Euler 数・体積・sdf の保守性
#====================================================================================
import math

import numpy as np
import pytest

from shapegen import mesh as meshlib
from shapegen import pieces as P


def _all_pieces():
    return [
        ("box", P.Box(1.0, 0.05, 0.02), 1.0 * 0.05 * 0.02, 2),
        ("box_cube", P.Box(0.3, 0.3, 0.3), 0.027, 2),
        ("prism3", P.ConvexPrism(P.regular_polygon(3, 0.5), 0.2), None, 2),
        ("prism8_flat", P.ConvexPrism(P.regular_polygon(8, 0.5), 0.01), None, 2),
        ("cylinder", P.cylinder(0.25, 1.0), math.pi * 0.25 ** 2 * 1.0, 2),
        ("cylinder_thin", P.cylinder(0.02, 1.0, segments=8), math.pi * 0.02 ** 2, 2),
        ("frustum", P.frustum(0.3, 0.1, 0.8), math.pi * 0.8 / 3 * (0.09 + 0.03 + 0.01), 2),
        ("cone", P.frustum(0.3, 0.0, 0.8), math.pi * 0.8 / 3 * 0.09, 2),
        ("sphere", P.sphere(0.4), 4 / 3 * math.pi * 0.4 ** 3, 2),
        ("ellipsoid", P.Ellipsoid(0.5, 0.2, 0.1), 4 / 3 * math.pi * 0.5 * 0.2 * 0.1, 2),
        ("capsule", P.capsule(0.1, 0.6), math.pi * 0.01 * 0.6 + 4 / 3 * math.pi * 0.001, 2),
        ("capsule_min", P.capsule(0.05, 0.05, lat_steps=2, segments=6), None, 2),
        ("torus", P.torus(0.4, 0.1), 2 * math.pi ** 2 * 0.4 * 0.01, 0),
        ("tube", P.tube(0.2, 0.3, 0.5), math.pi * (0.09 - 0.04) * 0.5, 0),
        ("revolve_bottle", P.Revolve([(0, -0.5), (0.2, -0.5), (0.2, 0.0), (0.08, 0.2), (0.08, 0.5), (0, 0.5)],
                                     feature=0.16), None, 2),
    ]


@pytest.mark.parametrize("name,piece,volume,euler", _all_pieces(), ids=[p[0] for p in _all_pieces()])
def test_piece_closed_euler_volume(name, piece, volume, euler):
    m = piece.local_mesh()
    rep = meshlib.check_closed(m)
    assert rep.ok, f"{name}: {rep.reason}"
    assert rep.euler == euler
    if volume is not None:
        # Box/Ellipsoid/円柱/円錐台は解析式そのもの。断面を多角形で近似する回転体
        # (カプセルの半球、トーラスの管) は内接多角形ぶん小さい (5% 以内)
        tol = 0.05 if name in ("capsule", "torus") else 1e-6
        assert abs(piece.volume() - volume) / volume < tol
    # min_feature は正で、AABB 最小辺 (粗い多角形は内接で 15% 程度縮む) を大きく超えない
    lo, hi = m.aabb()
    assert 0 < piece.min_feature() <= float(np.min(hi - lo)) * 1.2 + 1e-9


@pytest.mark.parametrize("name,piece,volume,euler", _all_pieces(), ids=[p[0] for p in _all_pieces()])
def test_sdf_conservative_and_anchors_inside(name, piece, volume, euler):
    m = piece.local_mesh()
    # 表面の頂点では sdf ≈ 0 以下にはならない (保守的: 外を内と言わない)。
    # 多角形近似の頂点は厳密な表面上なので |sdf| は小さい
    d = piece.local_sdf(m.verts)
    assert np.all(d <= 1e-6), f"{name}: surface vertex reported outside by {d.max()}"
    assert np.all(d >= -0.05), f"{name}: surface vertex reported deep inside {d.min()}"
    # アンカー候補は (m を満たすものだけ残る契約ではないが) 全部が内部か境界近傍
    anchors = piece.local_anchors(0.005)
    assert anchors.shape[0] > 0
    da = piece.local_sdf(anchors)
    assert np.all(da <= 1e-9), f"{name}: anchor outside by {da.max()}"
    # 遠い点は外
    far = np.array([[10.0, 10.0, 10.0]])
    assert piece.local_sdf(far)[0] > 1.0


def test_pose_roundtrip_and_world_sdf():
    from shapegen import rotation
    b = P.Box(1.0, 0.5, 0.25).posed(rotation.rot_z(30.0), (1.0, 2.0, 3.0))
    center = np.array([[1.0, 2.0, 3.0]])
    assert abs(b.sdf(center)[0] + 0.125) < 1e-12
    world = b.mesh()
    rep = meshlib.check_closed(world)
    assert rep.ok
    lo, hi = world.aabb()
    assert np.allclose((lo + hi) / 2, center[0])


def test_ellipsoid_sdf_lower_bound():
    e = P.Ellipsoid(1.0, 1.0, 0.1)
    # 中心: 真の深さ 0.1 = rmin
    assert abs(e.local_sdf(np.array([[0.0, 0.0, 0.0]]))[0] + 0.1) < 1e-12
    # 長軸方向の内点: 真の深さは 0.1 より小さい (0.0436) が、下界はさらに小さい
    d = e.local_sdf(np.array([[0.9, 0.0, 0.0]]))[0]
    assert -0.05 < d < 0.0


def test_shell_profile_is_valid_revolve():
    outer = [(0.0, -0.5), (0.3, -0.5), (0.35, 0.5)]
    prof = P.shell_profile(outer, 0.05)
    piece = P.Revolve(prof, feature=0.05)
    rep = meshlib.check_closed(piece.local_mesh())
    assert rep.ok and rep.euler == 2
    # 軸上の口の中心は空 (開口)、壁の中は内部
    assert piece.local_sdf(np.array([[0.0, 0.0, 0.3]]))[0] > 0
    assert piece.local_sdf(np.array([[0.325, 0.0, 0.3]]))[0] < 0


def test_invalid_pieces_raise():
    with pytest.raises(ValueError):
        P.Box(1.0, 0.0, 1.0)
    with pytest.raises(ValueError):
        P.torus(0.1, 0.2)
    with pytest.raises(ValueError):
        P.tube(0.3, 0.2, 1.0)
    with pytest.raises(ValueError):
        P.Revolve([(0.1, 0.0), (0.2, 0.5), (0.0, 1.0)], feature=0.1)
