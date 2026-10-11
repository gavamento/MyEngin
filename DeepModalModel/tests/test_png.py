#====================================================================================
#                          test_png.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          最小 PNG ライタの往復とコンタクトシートの寸法
#====================================================================================
import numpy as np

from shapegen import constants as C
from shapegen import contact_sheet
from shapegen.png import read_png_gray, write_png_gray


def test_png_roundtrip(tmp_path):
    img = (np.arange(40 * 24) % 251).astype(np.uint8).reshape(40, 24)
    path = tmp_path / "a.png"
    write_png_gray(path, img)
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    back = read_png_gray(path)
    assert back.shape == img.shape and np.array_equal(back, img)


def test_contact_sheet_dimensions_and_projection(tmp_path):
    occ = np.zeros((C.VOXEL_N,) * 3, dtype=np.uint8)
    occ[5:10, 8:20, 12:14] = 1
    proj = contact_sheet.depth_projection(occ, 2)
    assert proj.shape == (C.VOXEL_N, C.VOXEL_N)
    assert proj[5:10, 8:20].min() > 0 and proj[0, 0] == 0
    tile = contact_sheet.render_shape(occ)
    assert tile.shape == (C.VOXEL_N, 3 * C.VOXEL_N)
    sheet = contact_sheet.make_sheet([occ] * 5, cols=4, scale=2, gap=2)
    assert sheet.shape == (2 * (64 + 2) + 2, 4 * (192 + 2) + 2)
    contact_sheet.write_sheet(tmp_path / "s.png", [occ, occ], ["a", "b"], cols=2, scale=1)
    assert (tmp_path / "s.png").exists()
    assert "row 0 col 1: b" in (tmp_path / "s.png.txt").read_text(encoding="utf-8")
