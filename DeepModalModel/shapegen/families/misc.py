#====================================================================================
#                          misc.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          汎用ファミリ (任意ピースのランダム結合)
#====================================================================================
"""カテゴリの見た目ではなく占有パターンの多様性を担う。種類均等でピースを引き、
既存ピースのアンカー点にずらして重ねる (連結規則は asm.try_add が検証)。"""
import numpy as np

from .. import pieces as P
from .. import rotation
from ..assembly import Assembly, PlacementError


def _random_piece(ctx):
    kind = ctx.rng.choice(["box", "cylinder", "frustum", "ellipsoid", "capsule", "torus", "prism"])
    # 長さ a に対して断面 b, c を細くする (ずんぐりしたピースは最長辺で正規化すると
    # 充填率が高く、1 個で上限超になる)
    a = ctx.lu(0.3, 0.9)
    b = max(ctx.feature, a * ctx.lu(0.1, 0.4) * ctx.lean)
    c = max(ctx.feature, a * ctx.lu(0.1, 0.4) * ctx.lean)
    if kind == "box":
        return P.Box(a, b, c)
    if kind == "cylinder":
        return P.cylinder(max(0.5 * ctx.feature, b / 2), a)
    if kind == "frustum":
        return P.frustum(max(0.5 * ctx.feature, b / 2), max(0.5 * ctx.feature, c / 2) * ctx.u(0.0, 1.0), a)
    if kind == "ellipsoid":
        return P.Ellipsoid(a / 2, max(0.5 * ctx.feature, b / 2), max(0.5 * ctx.feature, c / 2))
    if kind == "capsule":
        r = max(0.5 * ctx.feature, b / 2)
        return P.capsule(r, max(a - 2 * r, r))
    if kind == "torus":
        r = max(0.5 * ctx.feature, min(b, c) / 2)
        return P.torus(max(a / 2 - r, r * 1.2), r)
    n = ctx.rng.choice([3, 5, 6, 8])
    return P.ConvexPrism(P.regular_polygon(n, a / 2), b)


def random_union(ctx):
    asm = Assembly(ctx)
    n = ctx.rng.int_range(2, 5)
    asm.add(_random_piece(ctx).posed(rotation.random_rotation(ctx.rng, "octahedral"), (0.0, 0.0, 0.0)))
    placed = 1
    for i in range(1, n):
        piece = _random_piece(ctx)
        for _ in range(4):
            parent = asm.pieces[ctx.rng.int_range(0, len(asm.pieces) - 1)]
            anchors = parent.anchor_points(ctx.overlap)
            if anchors.shape[0] == 0:  # 薄すぎて深さ m の内部点が無いピース (親にできない)
                continue
            q = anchors[ctx.rng.int_range(0, anchors.shape[0] - 1)]
            mode = "free" if ctx.rng.chance(0.5) else "octahedral"
            R = rotation.random_rotation(ctx.rng, mode)
            lo, hi = piece.aabb()
            ext = float(np.max(hi - lo))
            off = np.array(ctx.rng.unit_vector()) * ext * ctx.u(0.2, 0.7)  # 重なりは浅めに (塊化を避ける)
            if asm.try_add(piece.posed(R, q + off)):
                placed += 1
                break
    if placed < 2:
        raise PlacementError("random_union placed too few pieces")
    ctx.rec(pieces=placed)
    return asm
