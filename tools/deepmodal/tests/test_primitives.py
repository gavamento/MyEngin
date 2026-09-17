#====================================================================================
#                          test_primitives.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          primitives.py の形状生成器の不変量テスト
#====================================================================================
"""primitives.py の 7 ジェネレータ全部を検証するテストは無い (spec の pytest 一覧にも
無い) が、**ジェネレータが黙って壊れる** (中空箱が中実になる、穴あき板の穴が
塞がる) と sub-04 の overfit の門が前提にしている形状の多様性が消える。
ここでは実際の C++ ボクセライザ (Editor.exe --modal-voxelize) を通した不変量を
2 つだけ固定する。Python 側に第 2 のボクセライザを書かない (spec §2 #8) ので、
`@pytest.mark.editor` (Editor.exe 無ければ skip)。
"""
import tempfile
from pathlib import Path

import pytest

import meshio
import primitives
import voxelize


def _voxelize_one(tmp_path: Path, name: str, verts, faces, editor_exe):
    obj_path = tmp_path / f"{name}.obj"
    positions, indices = primitives.flatten(verts, faces)
    primitives.write_obj(obj_path, positions, indices)
    list_path = tmp_path / f"{name}_list.txt"
    list_path.write_text(str(obj_path) + "\n", encoding="utf-8")
    rc = voxelize.run_voxelize_list(list_path, tmp_path, editor_exe)
    assert rc == 0
    return meshio.read_mvox(tmp_path / f"{name}#mesh0#prim0.mvox")


@pytest.mark.editor
def test_hollow_box_interior_much_less_than_solid_box():
    """中空箱 (トレイ状、開口あり) は同じ外形寸法の中実箱よりも interior が
    はっきり少ない (開口から flood-fill が空洞まで届くため。§4.1 の
    「非 watertight は殻だけ」の裏返し — 完全に閉じた殻は必ず内部が埋まる)。"""
    editor_exe = voxelize.find_editor_exe()
    if not editor_exe.exists():
        pytest.skip(f"Editor.exe not found: {editor_exe}")

    # wall は voxel_size (h = longestEdge/28 ≈ 0.011) より薄くする — h より厚い壁は
    # 壁自体の「バルク」が (正しく) interior として埋まってしまい、この不変量の
    # 検算には向かない (実測で判明。0.05 だと 19683 → 11380 までしか減らなかった)
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        hollow = _voxelize_one(tmp_path, "hollow", *primitives.hollow_box_mesh(0.3, 0.3, 0.3, 0.01),
                                editor_exe)
        solid = _voxelize_one(tmp_path, "solid", *primitives.box_mesh(0.3, 0.3, 0.3), editor_exe)

    assert solid.interior_count > 5000  # 立方体に近い箱は大部分が内部で埋まる
    assert hollow.interior_count < solid.interior_count / 4


@pytest.mark.editor
def test_perforated_plate_has_hole():
    """穴あき板 (4 本バーの枠) の中心は非占有 (穴)、バー本体は占有 —
    枠の生成が壊れて穴が塞がる/バーが消えることを検知する。"""
    editor_exe = voxelize.find_editor_exe()
    if not editor_exe.exists():
        pytest.skip(f"Editor.exe not found: {editor_exe}")

    sx, sy, thickness, hole_w, hole_h = 0.4, 0.4, 0.05, 0.2, 0.2
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        grid = _voxelize_one(tmp_path, "perf",
                              *primitives.perforated_plate_mesh(sx, sy, thickness, hole_w, hole_h),
                              editor_exe)

    def voxel_of(point):
        idx = []
        for a in range(3):
            v = int((point[a] - grid.origin[a]) / grid.voxel_size)
            idx.append(max(0, min(31, v)))
        return tuple(idx)

    # 穴の中心 (原点。生成パラメータから hole_w/hole_h の内側)
    hole_idx = voxel_of((0.0, 0.0, 0.0))
    assert grid.occ[hole_idx] == 0

    # 上バーの中心 (Hy=0.2, hh=0.1 なので y=(0.2+0.1)/2=0.15 がバー本体)
    bar_idx = voxel_of((0.0, 0.15, 0.0))
    assert grid.occ[bar_idx] == 1
