#====================================================================================
#                          test_fem.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          hex8 要素 (Ke/Me) の単体テスト
#====================================================================================
import numpy as np

import fem


def test_ke_unit_symmetric_and_psd():
    """Ke は対称、かつ半正定値 (剛体 6 モードぶんだけ 0 固有値、残りは正)。"""
    Ke = fem.hex8_ke_unit(0.33)
    assert np.allclose(Ke, Ke.T, atol=1e-10)
    eigvals = np.linalg.eigvalsh(Ke)
    eigvals_sorted = np.sort(eigvals)
    # 先頭 6 個は数値誤差レベルで 0 (剛体モード)、7 個目以降は明確に正
    assert np.all(np.abs(eigvals_sorted[:6]) < 1e-9)
    assert np.all(eigvals_sorted[6:] > 1e-6)


def _rigid_body_displacement(kind: str, coords: np.ndarray, theta: float = 1e-4) -> np.ndarray:
    """8 節点 (coords, (8,3)) に対する剛体変位場 (24,)。並進 3 + 微小回転 3。"""
    d = np.zeros((8, 3))
    if kind == "tx":
        d[:, 0] = 1.0
    elif kind == "ty":
        d[:, 1] = 1.0
    elif kind == "tz":
        d[:, 2] = 1.0
    elif kind == "rx":
        d[:, 1] = -theta * coords[:, 2]
        d[:, 2] = theta * coords[:, 1]
    elif kind == "ry":
        d[:, 2] = -theta * coords[:, 0]
        d[:, 0] = theta * coords[:, 2]
    elif kind == "rz":
        d[:, 0] = -theta * coords[:, 1]
        d[:, 1] = theta * coords[:, 0]
    else:
        raise ValueError(kind)
    return d.reshape(-1)


def test_ke_annihilates_rigid_body_modes():
    """剛体変位 (並進 3 + 微小回転 3) を入れると Ke @ r はほぼ 0 になる
    (物理的に歪みゼロの変位に対して内力が生じないことの検算)。"""
    Ke = fem.hex8_ke_unit(0.33)
    coords = fem._NATURAL * 0.5  # 単位立方体 (h=1) の物理コーナー座標
    scale = np.linalg.norm(Ke)
    for kind in ("tx", "ty", "tz", "rx", "ry", "rz"):
        r = _rigid_body_displacement(kind, coords)
        residual = Ke @ r
        # scale (行列ノルム) に対する相対誤差で判定 (小さい theta による回転成分の
        # 絶対値そのものは小さいため、絶対誤差だけでは閾値の選び方が恣意的になる)
        assert np.max(np.abs(residual)) < 1e-8 * max(scale, 1.0)


def test_me_lumped_mass_total():
    """集中質量の物理的総和 (= 24 自由度ぶんの対角和を 3 (1 節点あたりの自由度数) で
    割ったもの) が rho*h^3 に一致する。対角は 1 節点につき 3 自由度が同じ値を持つので、
    24 個の和は物理的な総質量の 3 倍になる (test docstring 参照)。"""
    h = 0.02
    rho = 2700.0
    me = fem.hex8_me_diag(h, rho)
    assert me.shape == (24,)
    assert np.isclose(me.sum() / 3.0, rho * h ** 3)
    assert np.allclose(me, me[0])  # 全自由度が均等 (集中質量は 8 節点へ均等配分)


def test_assemble_from_occupancy_2x2x2_shapes():
    """2x2x2 (8 voxel) の全占有グリッドは 3^3=27 節点、81 自由度になる。"""
    occ = np.ones((2, 2, 2), dtype=np.uint8)
    K, M, node_coords, dofs, voxels = fem.assemble_from_occupancy(
        occ, 0.01, 7.0e10, 2700.0, 0.33)
    assert K.shape == (81, 81)
    assert M.shape == (81,)
    assert node_coords.shape == (27, 3)
    assert voxels.shape == (8, 3)
    # K は対称 (組み立て誤りがあれば非対称になりやすい)
    diff = (K - K.T)
    assert np.max(np.abs(diff.toarray())) < 1e-6
