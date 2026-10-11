#====================================================================================
#                          estimate.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          占有 voxel 数の解析的な見積り (格子を持たない)
#====================================================================================
"""Σ 体積/h³ + Σ 表面積/(2h²) の素朴な和。重なりを無視するので上界寄り。
これは**ボクセライザではない** (格子も三角形も見ない)。用途は 2 つだけ:
  1. 明らかに範囲外の形状を Editor.exe に渡す前に落とす (緩い門、constants.EST_*_FACTOR)
  2. pilot で実測占有との log 回帰を stats に出し、ファミリの寸法調整の目安にする
"""


def estimate_occ(pieces, h: float) -> float:
    if h <= 0.0:
        return 0.0
    vol = sum(p.volume() for p in pieces)
    area = sum(p.surface_area() for p in pieces)
    return vol / (h ** 3) + area / (2.0 * h * h)
