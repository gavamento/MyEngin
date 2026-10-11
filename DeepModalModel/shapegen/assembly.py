#====================================================================================
#                          assembly.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          ピースの連結規則付き組み立てと、よく使う部品のヘルパ
#====================================================================================
"""連結の規則 (Python 側の推定。最終判定は .mvox の 6 連結検査):

  2 ピース P, Q が「重なる」= P の内部候補点 q (sdf_P(q) ≤ -m) で sdf_Q(q) ≤ -m のものがある、
  またはその逆 (双方向)。m = MIN_OVERLAP_VOXELS × h。
  形状が「連結」= 重なりを辺とするピースのグラフが 1 成分。

重なり領域の点 q を含む voxel は P の殻にも Q の殻にも触れる (または両方の内部) ので、
両ピースの占有に共通の voxel ができ、各ピースの占有がそれぞれ連結なら全体も連結になる。

`add` は積むだけで、連結は `finalize` でまとめて検査する (平行なバーを先に積む格子や枠を
順序に縛られず書けるようにするため)。配置の試行錯誤が要るファミリ (岩・ランダム結合) は
`try_add` で「既存のどれかと重なるか」を即時に確かめる。
AABB が交差しないピース対は sdf を評価しない (枝刈り)。

`ShapeContext` は設計空間の寸法基準を持つ。ファミリは最長辺 ≈ DESIGN_MAIN で組み、
厚みはすべて `ctx.thick()` 経由で引く (= 最小厚み規則の機械的な保証)。
"""
from dataclasses import dataclass

import numpy as np

from . import constants as C
from . import mesh as meshlib
from . import pieces as P
from . import rotation


class PlacementError(Exception):
    """配置できなかった (build_shape が同じ乱数列から組み直す)。"""


class ShapeContext:
    def __init__(self, rng, rot_mode: str, profile: str = "small"):
        self.rng = rng
        self.rot_mode = rot_mode
        self.profile = profile
        self.fill = C.PROFILES[profile]["fill_scale"]
        # 副寸法 (幅・奥行・半径など、主寸法以外の嵩) の倍率。推定占有が重いときの組み直しで
        # 厚み (fill) と一緒に縮める。ファミリは嵩を決める寸法に ctx.lean を掛ける
        self.lean = 1.0
        # 設計空間の voxel 幅 (回転で L が伸びる分と突起で伸びる分を見込む)
        self.h = C.DESIGN_MAIN / C.VOXEL_DIVISIONS * C.ROT_MARGIN[rot_mode] * C.DESIGN_OVERSHOOT
        self.feature = C.MIN_FEATURE_VOXELS * self.h   # 最小厚み
        self.opening = C.MIN_OPENING_VOXELS * self.h   # 最小開口幅
        self.overlap = C.MIN_OVERLAP_VOXELS * self.h   # 連結の重なり深さ
        self.params = {}                               # ファミリが記録する乱数パラメータ

    # ---- 乱数の糖衣 (すべて rng 経由 = 決定的) ----
    def thick(self, lo: float, hi: float) -> float:
        """最小厚みの lo..hi 倍。上限はプロファイル / 適応リトライの fill で縮む。"""
        hi_eff = max(lo, hi * self.fill)
        return self.feature * self.rng.uniform(lo, hi_eff)

    def u(self, lo, hi):
        return self.rng.uniform(lo, hi)

    def lu(self, lo, hi):
        return self.rng.log_uniform(lo, hi)

    def rec(self, **kv):
        """manifest 用にパラメータを記録する (丸めて JSON 可能に)。"""
        for k, v in kv.items():
            if isinstance(v, float):
                v = round(v, 5)
            elif isinstance(v, (list, tuple)):
                v = [round(float(x), 5) if isinstance(x, (float, np.floating)) else x for x in v]
            elif isinstance(v, (np.floating,)):
                v = round(float(v), 5)
            elif isinstance(v, (np.integer,)):
                v = int(v)
            elif isinstance(v, np.bool_):
                v = bool(v)
            self.params[k] = v


@dataclass
class FinalShape:
    mesh: meshlib.TriMesh
    L: float
    h: float
    min_feature_vox: float
    est_occ: float
    n_pieces: int
    n_tris: int
    piece_params: list
    reject_reason: str = ""  # 空なら事前検査合格
    rotation_mode: str = "identity"


class _Placed:
    """ピース + キャッシュ (メッシュ・AABB・アンカー)。"""
    __slots__ = ("piece", "mesh", "lo", "hi", "_anchors")

    def __init__(self, piece):
        self.piece = piece
        self.mesh = piece.mesh()
        self.lo, self.hi = self.mesh.aabb()
        self._anchors = {}

    def anchors(self, m: float):
        key = round(m, 9)
        if key not in self._anchors:
            self._anchors[key] = self.piece.anchor_points(m)
        return self._anchors[key]


def _aabb_touch(a: _Placed, b: _Placed, eps: float) -> bool:
    return bool(np.all(a.lo <= b.hi + eps) and np.all(b.lo <= a.hi + eps))


def _pair_overlaps(a: _Placed, b: _Placed, m: float) -> bool:
    for src, dst in ((a, b), (b, a)):
        anchors = src.anchors(m)
        if anchors.shape[0] == 0:
            continue
        cand = anchors[src.piece.sdf(anchors) <= -m]
        if cand.shape[0] and np.any(dst.piece.sdf(cand) <= -m):
            return True
    return False


class Assembly:
    def __init__(self, ctx: ShapeContext):
        self.ctx = ctx
        self._placed = []

    @property
    def pieces(self):
        return [p.piece for p in self._placed]

    def add(self, piece):
        """積むだけ (連結は finalize で検査)。"""
        self._placed.append(_Placed(piece))
        return piece

    def try_add(self, piece) -> bool:
        """既存のどれかと重なるときだけ積む (即時判定が要るファミリ用)。"""
        cand = _Placed(piece)
        m = self.ctx.overlap
        if not self._placed:
            self._placed.append(cand)
            return True
        for q in self._placed:
            if _aabb_touch(cand, q, m) and _pair_overlaps(cand, q, m):
                self._placed.append(cand)
                return True
        return False

    def is_connected(self, m: float) -> bool:
        """重なりグラフが 1 成分か (union-find、AABB で枝刈り)。"""
        n = len(self._placed)
        if n <= 1:
            return n == 1
        parent = list(range(n))

        def find(x):
            while parent[x] != x:
                parent[x] = parent[parent[x]]
                x = parent[x]
            return x

        comps = n
        for i in range(n):
            for j in range(i + 1, n):
                if find(i) == find(j):
                    continue
                if not _aabb_touch(self._placed[i], self._placed[j], m):
                    continue
                if _pair_overlaps(self._placed[i], self._placed[j], m):
                    parent[find(i)] = find(j)
                    comps -= 1
                    if comps == 1:
                        return True
        return comps == 1

    def finalize(self, R: np.ndarray, rot_mode: str) -> FinalShape:
        if not self._placed:
            raise PlacementError("empty assembly")
        merged = meshlib.concat([p.mesh for p in self._placed]).transformed(R, np.zeros(3))
        L = merged.longest_edge()
        n_tris = merged.tri_count
        params = [p.piece.params() for p in self._placed]
        n = len(self._placed)
        if not (L > 0.0):
            return FinalShape(merged, 0.0, 0.0, 0.0, 0.0, n, n_tris, params,
                              "pre_reject_degenerate", rot_mode)
        h = L / C.VOXEL_DIVISIONS
        min_feature = min(p.piece.min_feature() for p in self._placed)
        min_feature_vox = min_feature / h
        from . import estimate
        est = estimate.estimate_occ(self.pieces, h)
        reason = ""
        if n_tris > C.TRI_MAX:
            reason = "pre_reject_tris"
        elif min_feature_vox < C.MIN_FEATURE_VOXELS:
            reason = "pre_reject_thin"
        elif est < C.EST_LOW_FACTOR * C.OCC_MIN:
            reason = "pre_reject_est_low"
        elif est > C.EST_HIGH_FACTOR * C.OCC_MAX:
            reason = "pre_reject_est_high"
        elif not self.is_connected(C.MIN_OVERLAP_VOXELS * h):
            reason = "pre_reject_placement"
        return FinalShape(merged, L, h, min_feature_vox, est, n, n_tris, params, reason, rot_mode)


# ---- 部品のヘルパ ----

def box_at(asm, size, center, R=None):
    return asm.add(P.Box(*size).posed(R, center))


def chain_tube(asm, points, radii, extend: float, segments=P.DEFAULT_SEGMENTS):
    """折れ線 points[k] を半径 radii[k] の円錐台でつなぎ、関節に球を置く。
    各区間は両端を少なくとも関節球の半径ぶん伸ばす (球の中心が区間の内部に入る)。"""
    pts = [np.asarray(p, dtype=np.float64) for p in points]
    lat = max(4, segments // 2)  # 関節球の分割は管の分割に合わせる (三角形数の抑制)
    added = [asm.add(P.sphere(radii[0], lat, segments).posed(None, pts[0]))]
    for k in range(len(pts) - 1):
        a, b = pts[k], pts[k + 1]
        d = b - a
        length = float(np.linalg.norm(d))
        if length < 1e-9:
            continue
        d = d / length
        R = rotation.align_z_to(d)
        ext = max(extend, radii[k], radii[k + 1])
        seg = P.frustum(radii[k], radii[k + 1], length + 2.0 * ext, segments=segments)
        added.append(asm.add(seg.posed(R, 0.5 * (a + b))))
        added.append(asm.add(P.sphere(radii[k + 1], lat, segments).posed(None, b)))
    return added


def tray(asm, size, wall, open_faces=("+z",), center=(0.0, 0.0, 0.0)):
    """箱型の殻を 6 枚の板で組む (open_faces に挙げた面は省く = 開口)。
    壁同士は隅で体積が重なる (x 壁は y 方向に全長、z 方向に全高)。"""
    sx, sy, sz = size
    cx, cy, cz = center
    plates = {
        "-z": ((sx, sy, wall), (cx, cy, cz - sz / 2 + wall / 2)),
        "+z": ((sx, sy, wall), (cx, cy, cz + sz / 2 - wall / 2)),
        "-x": ((wall, sy, sz), (cx - sx / 2 + wall / 2, cy, cz)),
        "+x": ((wall, sy, sz), (cx + sx / 2 - wall / 2, cy, cz)),
        "-y": ((sx, wall, sz), (cx, cy - sy / 2 + wall / 2, cz)),
        "+y": ((sx, wall, sz), (cx, cy + sy / 2 - wall / 2, cz)),
    }
    added = []
    for face in ("-z", "-x", "+x", "-y", "+y", "+z"):
        if face in open_faces:
            continue
        size_f, center_f = plates[face]
        added.append(box_at(asm, size_f, center_f))
    return added


def bar_grid(asm, sx, sy, thickness, nx, ny, bar_w, center=(0.0, 0.0, 0.0), R=None):
    """xy 平面の板を (nx x ny) の貫通穴が空いた格子として組む。y 方向のバー nx+1 本、
    x 方向のバー ny+1 本。バーが直交して体積で重なる。"""
    cx, cy, cz = center
    added = []
    xs = np.linspace(-sx / 2 + bar_w / 2, sx / 2 - bar_w / 2, nx + 1)
    ys = np.linspace(-sy / 2 + bar_w / 2, sy / 2 - bar_w / 2, ny + 1)
    local = []
    for x in xs:
        local.append(((bar_w, sy, thickness), (x, 0.0, 0.0)))
    for y in ys:
        local.append(((sx, bar_w, thickness), (0.0, y, 0.0)))
    Rm = np.eye(3) if R is None else np.asarray(R)
    for size, c in local:
        c_world = Rm @ np.asarray(c) + np.array([cx, cy, cz])
        added.append(asm.add(P.Box(*size).posed(Rm, c_world)))
    return added


def profile_union(asm, kind: str, width, height, tf, tw, depth, center=(0.0, 0.0, 0.0), R=None):
    """断面 (xy) が L/U/T/I/H/Z 形の矩形の和を z 方向に depth 押し出す。
    フランジ厚 tf、ウェブ厚 tw。ウェブはフランジを貫通する長さにして体積で重ねる。"""
    w, hh = width, height
    if kind == "L":
        rects = [(w, tf, 0.0, -hh / 2 + tf / 2), (tw, hh, -w / 2 + tw / 2, 0.0)]
    elif kind == "U":
        rects = [(w, tf, 0.0, -hh / 2 + tf / 2), (tw, hh, -w / 2 + tw / 2, 0.0),
                 (tw, hh, w / 2 - tw / 2, 0.0)]
    elif kind == "T":
        rects = [(w, tf, 0.0, hh / 2 - tf / 2), (tw, hh, 0.0, 0.0)]
    elif kind == "I":
        rects = [(w, tf, 0.0, hh / 2 - tf / 2), (w, tf, 0.0, -hh / 2 + tf / 2), (tw, hh, 0.0, 0.0)]
    elif kind == "H":
        rects = [(tf, hh, -w / 2 + tf / 2, 0.0), (tf, hh, w / 2 - tf / 2, 0.0), (w, tw, 0.0, 0.0)]
    elif kind == "Z":
        rects = [(w / 2 + tw / 2, tf, w / 4 - tw / 4, hh / 2 - tf / 2),
                 (w / 2 + tw / 2, tf, -w / 4 + tw / 4, -hh / 2 + tf / 2), (tw, hh, 0.0, 0.0)]
    else:
        raise ValueError(f"unknown profile kind: {kind}")
    Rm = np.eye(3) if R is None else np.asarray(R)
    added = []
    for sx, sy, cx, cy in rects:
        c_world = Rm @ np.array([cx, cy, 0.0]) + np.asarray(center, dtype=np.float64)
        added.append(asm.add(P.Box(sx, sy, depth).posed(Rm, c_world)))
    return added


def attach_limb(asm, start, direction, length, r0, r1, embed, joint_sphere=True,
                segments=P.DEFAULT_SEGMENTS):
    """start (親の内部点) から direction へ length の円錐台を生やす。根元は embed だけ
    親側へ食い込ませる。先端に半径 r1 の球を置く (関節)。戻り値は先端の座標。"""
    s = np.asarray(start, dtype=np.float64)
    d = np.asarray(direction, dtype=np.float64)
    d = d / max(np.linalg.norm(d), 1e-12)
    a = s - d * embed
    b = s + d * length
    R = rotation.align_z_to(d)
    seg = P.frustum(r0, r1, float(np.linalg.norm(b - a)), segments=segments)
    asm.add(seg.posed(R, 0.5 * (a + b)))
    if joint_sphere:
        asm.add(P.sphere(r1, max(4, segments // 2), segments).posed(None, b))
    return b
