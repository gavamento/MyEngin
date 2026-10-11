#====================================================================================
#                          contact_sheet.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          占有の 3 軸深度投影を並べた QA 用コンタクトシート
#====================================================================================
"""1 形状 = 32x96 (x/y/z 方向から見た「最初に当たる深度」のグレースケール) を
scale 倍に拡大してタイル状に並べる。フォントを持たないので、どのタイルが何かは
同名の .txt (行 = タイル順の名前) で突き合わせる。"""
import numpy as np

from . import constants as C
from .png import write_png_gray


def depth_projection(occ: np.ndarray, axis: int) -> np.ndarray:
    """axis 方向から見て最初に占有に当たる深度を 0..255 (近いほど明るい、空は 0)。"""
    o = np.asarray(occ) != 0
    n = o.shape[axis]
    o_m = np.moveaxis(o, axis, 0)  # (depth, a, b)
    any_hit = o_m.any(axis=0)
    first = np.argmax(o_m, axis=0)  # 最初の True (無ければ 0)
    shade = (255.0 - first * (200.0 / n)).astype(np.uint8)
    return np.where(any_hit, shade, 0).astype(np.uint8)


def render_shape(occ: np.ndarray) -> np.ndarray:
    """(32, 96): [x 投影 | y 投影 | z 投影]。"""
    tiles = [depth_projection(occ, a) for a in range(3)]
    return np.hstack(tiles)


def make_sheet(occs, cols: int = 8, scale: int = 3, gap: int = 4) -> np.ndarray:
    n = C.VOXEL_N
    tile_h, tile_w = n * scale, 3 * n * scale
    rows = (len(occs) + cols - 1) // cols
    H = rows * (tile_h + gap) + gap
    W = cols * (tile_w + gap) + gap
    sheet = np.full((H, W), 40, dtype=np.uint8)
    for i, occ in enumerate(occs):
        r, c = divmod(i, cols)
        img = render_shape(occ)
        img = np.kron(img, np.ones((scale, scale), dtype=np.uint8))
        y0 = gap + r * (tile_h + gap)
        x0 = gap + c * (tile_w + gap)
        sheet[y0:y0 + tile_h, x0:x0 + tile_w] = img
    return sheet


def write_sheet(path, occs, names, cols: int = 8, scale: int = 3):
    write_png_gray(path, make_sheet(occs, cols, scale))
    with open(str(path) + ".txt", "w", encoding="utf-8") as f:
        for i, name in enumerate(names):
            r, c = divmod(i, cols)
            f.write(f"row {r} col {c}: {name}\n")
