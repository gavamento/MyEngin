#====================================================================================
#                          test_validate.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          合成した占有配列で受理検査の各規則を固定する
#====================================================================================
import numpy as np

from shapegen import constants as C
from shapegen import validate


def _grid():
    return np.zeros((C.VOXEL_N,) * 3, dtype=np.uint8)


def _cube(occ, x0, y0, z0, s):
    occ[x0:x0 + s, y0:y0 + s, z0:z0 + s] = 1


def _run(occ, occ_min=C.OCC_MIN, occ_max=C.OCC_MAX):
    total = int(occ.sum())
    return validate.analyze_occ(occ, surface_count=total, interior_count=0, occ_min=occ_min, occ_max=occ_max)


def test_single_block_ok_and_fields():
    occ = _grid()
    occ[2:31, 10:20, 10:18] = 1  # 最長軸 29、pad は空
    v = _run(occ)
    assert v.ok and v.reason == ""
    assert v.occ == 29 * 10 * 8 and v.components == 1 and v.extent == 29
    assert v.cells == validate.occupied_cells(occ).sum()
    assert v.filament_ratio == 0.0
    assert len(v.occ_hash) == 32 and len(v.coarse_key) == 32


def test_face_contact_is_one_component_edge_or_vertex_is_not():
    occ = _grid()
    occ[2:31, 10:14, 10:14] = 1
    occ[2:31, 14:18, 10:14] = 1  # 面で接する
    assert _run(occ).ok
    occ = _grid()
    occ[2:31, 10:14, 10:14] = 1
    occ[2:31, 14:18, 14:18] = 1  # 辺で接する
    v = _run(occ)
    assert not v.ok and v.reason == "disconnected" and v.components == 2
    assert 0.4 < v.max_component_ratio < 0.6
    occ = _grid()
    _cube(occ, 2, 2, 2, 10)
    _cube(occ, 12, 12, 12, 19)  # 頂点で接する (占有は上限超なので上限を外して連結だけ見る)
    v = _run(occ, occ_max=10 ** 6)
    assert v.reason == "disconnected"


def test_isolated_voxel_rejected():
    occ = _grid()
    occ[2:31, 10:20, 10:18] = 1
    occ[20, 25, 25] = 1
    assert _run(occ).reason == "disconnected"


def test_pad_violation_and_count_mismatch_are_anomalies():
    occ = _grid()
    occ[0:29, 10:20, 10:18] = 1  # pad 層 (index 0) を使っている
    assert _run(occ).reason == "vox_anomaly"
    occ = _grid()
    occ[2:31, 10:20, 10:18] = 1
    v = validate.analyze_occ(occ, surface_count=10, interior_count=0)
    assert v.reason == "vox_anomaly"
    assert validate.analyze_occ(_grid(), 0, 0).reason == "vox_anomaly"


def test_occ_bounds_and_cells():
    occ = _grid()
    occ[2:31, 10:12, 10:12] = 1  # 116 voxel、cell 数 15
    assert _run(occ).reason == "occ_low"
    assert _run(occ, occ_min=100).reason == "cells_low"
    occ = _grid()
    occ[2:31, 2:31, 2:31] = 1
    assert _run(occ).reason == "occ_high"
    occ = _grid()
    occ[2:31, 10:14, 10:14] = 1  # 464 voxel、cell 数 15*2*2=60
    v = _run(occ)
    assert v.ok and v.cells == 60


def test_filament_rejected():
    occ = _grid()
    occ[2:31, 10:20, 10:18] = 1
    # 1 voxel 幅の糸を 400 個ぶら下げる (6 近傍の占有 ≤ 1)
    for k in range(20):
        occ[2 + k, 20:30, 10 + (k % 8)] = 0
        occ[2 + k, 20, 10 + (k % 8)] = 1
    # 上の糸は根元が板に付いていて先端 1 個だけが「≤1 近傍」なので比率は小さい。
    # 本当の糸状: 長い線を 1 本の列として足す
    occ2 = _grid()
    occ2[2:31, 15, 15] = 1  # 29 voxel の線 → 全部 ≤ 2 近傍 … 端 2 個だけ ≤1
    occ2[2:4, 10:30, 10:30] = 1  # 本体 800 voxel
    v = _run(occ2)
    assert v.ok  # 線の内部は近傍 2 なので糸状とは数えない
    occ3 = _grid()
    occ3[2:31, 10:14, 10:18] = 1  # 薄い板 928 voxel
    cnt = 0
    for x in range(2, 31, 2):
        for z in range(10, 18, 2):
            occ3[x, 14, z] = 1  # 板 (y は 10..13) の上に孤立した 1 voxel の突起 (近傍 1) を 60 個
            cnt += 1
    v = _run(occ3)
    assert cnt / v.occ > C.FILAMENT_RATIO_MAX
    assert v.reason == "filament"


def test_dedup_table():
    occ = _grid()
    occ[2:31, 10:20, 10:18] = 1
    v = _run(occ)
    table = validate.DedupTable(coarse_limit=2)
    assert table.check_and_add(v) == ""
    assert table.check_and_add(v) == "dup_exact"
    # 16^3 キーが同じで占有が違う: 1 voxel だけ cell 内でずらす
    occ2 = occ.copy()
    occ2[30, 19, 17] = 0
    occ2[30, 19, 16] = 1
    v2 = _run(occ2)
    assert v2.coarse_key == v.coarse_key and v2.occ_hash != v.occ_hash
    assert table.check_and_add(v2) == ""
    occ3 = occ.copy()
    occ3[30, 18, 17] = 0
    occ3[30, 18, 16] = 1
    v3 = _run(occ3)
    assert table.check_and_add(v3) == "dup_coarse"
