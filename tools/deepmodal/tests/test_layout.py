#====================================================================================
#                          test_layout.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          .mvox レイアウトと C++ ボクセライザとの照合
#====================================================================================
import tempfile
from pathlib import Path

import pytest

import layout
import meshio
import voxelize


def test_mvox_header_and_total_size():
    assert layout.MVOX_HEADER_BYTES == 72
    assert layout.MVOX_TOTAL_BYTES == 72 + 32 * 32 * 32
    assert layout.MVOX_TOTAL_BYTES == 32840


def test_mel_band_centers_monotonic_and_in_range():
    centers = layout.mel_band_centers()
    assert len(centers) == layout.MEL_BANDS
    assert all(layout.F_MIN <= c <= layout.F_MAX for c in centers)
    assert all(b > a for a, b in zip(centers, centers[1:]))


@pytest.mark.editor
def test_cpp_cube_matches_expected_occupancy():
    """C++ (Editor.exe --modal-voxelize) で builtin://cube を焼き、
    surface+interior が単位立方体の期待値 (29^3=24389) と一致することを確認する
    (学習側とランタイム側のボクセル化規則が同じであることの唯一の実地照合)。"""
    editor_exe = voxelize.find_editor_exe()
    if not editor_exe.exists():
        pytest.skip(f"Editor.exe not found: {editor_exe}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        list_path = tmp_path / "list.txt"
        list_path.write_text("builtin://cube\n", encoding="utf-8")
        rc = voxelize.run_voxelize_list(list_path, tmp_path, editor_exe)
        assert rc == 0
        mvox_path = tmp_path / "cube#mesh0#prim0.mvox"
        assert mvox_path.exists()
        grid = meshio.read_mvox(mvox_path)
        assert grid.surface_count + grid.interior_count == 24389
