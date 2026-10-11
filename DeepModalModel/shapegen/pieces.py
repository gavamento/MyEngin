#====================================================================================
#                          pieces.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          閉じたプリミティブ (箱・凸角柱・楕円体・回転体・環)
#====================================================================================
"""すべてのピースは「それ単体で閉じた殻」を返す。形状はピースを重ねた三角形スープで、
ボクセライザの flood-fill が外から入れない領域は中実になる (CSG は不要)。

各ピースが持つもの:
- `local_mesh()`   : 局所系の閉じた殻
- `local_sdf(p)`   : 局所系の符号付き距離 (**保守的** = 内側を実際より深く言わない)。
                     連結保証 (assembly.py) が「両ピースに深さ m 以上入った点」を探すのに使う
- `local_anchors()`: 内部点の候補 (assembly が sdf で絞る)
- `volume()` / `surface_area()` / `min_feature()`: 占有見積りと最小厚み検査
姿勢は `R` (3x3) と `t` (3,) で、world = R @ local + t。

種類は 5 つだけ: Box / ConvexPrism / Ellipsoid / Revolve (軸に接する断面 = 中実) /
RevolveRing (軸に接しない閉断面 = 環)。円柱・円錐台・球・カプセル・トーラス・管は
Revolve 系の工場関数で作る (メッシュ生成・sdf・体積の実装を共有するため)。
"""
import math

import numpy as np

from . import mesh as meshlib
from .mesh import TriMesh

# 分割数。三角形数と形状の忠実度の折り合い (voxel 幅 h = L/28 に対して十分細かい)
DEFAULT_SEGMENTS = 24
SPHERE_LAT_STEPS = 12
# アンカー候補 = 「粗い格子」+「境界から ANCHOR_DEPTH_FACTOR*m だけ内側の点」。後者が
# 無いと、薄い壁 (カップ) や細長いピースの端 (脚・枝) の重なりを格子が拾えない。
# 深さが足りない候補は assembly が sdf で落とす
ANCHOR_FRACTIONS = (-0.75, -0.5, -0.25, 0.0, 0.25, 0.5, 0.75)
# 境界からの深さ (m 単位) を何段か置く: 重なり領域が薄い (2m 程度) ときも中央を拾うため
ANCHOR_DEPTH_FACTORS = (1.1, 2.0, 3.5)
ANCHOR_EDGE_SAMPLES = 5
ANCHOR_PHASES = 8


def _axis_coords(half: float, m: float):
    """1 軸ぶんの候補座標: 格子 + 面から数段の深さだけ内側 (半辺が許す場合)。"""
    coords = [f * half for f in ANCHOR_FRACTIONS]
    for f in ANCHOR_DEPTH_FACTORS:
        d = f * m
        if half > d:
            coords += [half - d, -(half - d)]
    return sorted(set(round(c, 12) for c in coords))


def _box_anchors(half, m: float):
    xs = _axis_coords(half[0], m)
    ys = _axis_coords(half[1], m)
    zs = _axis_coords(half[2], m)
    return np.array([(x, y, z) for x in xs for y in ys for z in zs], dtype=np.float64)


def _polygon_anchors(poly, m: float):
    """2D 多角形の内部候補: 7x7 格子 + 各辺から 1.5m 内側の点 + 重心。polygon_sdf <= -m で絞る。"""
    poly = np.asarray(poly, dtype=np.float64)
    lo, hi = poly.min(axis=0), poly.max(axis=0)
    xs = np.linspace(lo[0], hi[0], 7)
    ys = np.linspace(lo[1], hi[1], 7)
    cand = [(x, y) for x in xs for y in ys]
    cand.append(meshlib.polygon_centroid(poly))
    sign = 1.0 if meshlib.polygon_area(poly) > 0 else -1.0
    n = poly.shape[0]
    for i in range(n):
        a, b = poly[i], poly[(i + 1) % n]
        e = b - a
        L = float(np.linalg.norm(e))
        if L < 1e-12:
            continue
        # CCW のとき内向き法線は (-ey, ex)
        nrm = sign * np.array([-e[1], e[0]]) / L
        for f in ANCHOR_DEPTH_FACTORS:
            d = f * m
            for k in range(ANCHOR_EDGE_SAMPLES):
                t = (k + 0.5) / ANCHOR_EDGE_SAMPLES
                p = a + e * t + nrm * d
                cand.append((float(p[0]), float(p[1])))
    cand = np.array(cand, dtype=np.float64)
    inside = meshlib.polygon_sdf(cand, poly) <= -m
    return cand[inside]


def _revolve_anchors(rz_points, phases: int = ANCHOR_PHASES):
    """(r,z) の候補を z 軸まわりの phases 方向へ展開する (r=0 は 1 点)。"""
    out = []
    for r, z in rz_points:
        if r <= 1e-12:
            out.append((0.0, 0.0, z))
            continue
        for i in range(phases):
            th = 2.0 * np.pi * i / phases
            out.append((r * np.cos(th), r * np.sin(th), z))
    return np.array(out, dtype=np.float64) if out else np.zeros((0, 3))


class Piece:
    kind = "piece"

    def __init__(self):
        self.R = np.eye(3)
        self.t = np.zeros(3)

    # ---- 姿勢 ----
    def posed(self, R=None, t=None):
        """姿勢を設定して自分を返す (連鎖用)。"""
        if R is not None:
            self.R = np.asarray(R, dtype=np.float64)
        if t is not None:
            self.t = np.asarray(t, dtype=np.float64)
        return self

    def to_local(self, p):
        return (np.asarray(p, dtype=np.float64) - self.t) @ self.R

    def to_world(self, p):
        return np.asarray(p, dtype=np.float64) @ self.R.T + self.t

    # ---- 派生が実装する ----
    def local_mesh(self) -> TriMesh:
        raise NotImplementedError

    def local_sdf(self, p) -> np.ndarray:
        raise NotImplementedError

    def local_anchors(self, m: float) -> np.ndarray:
        """内部点の候補 (m = 要求される深さ。境界から 1.5m 内側の点を含める)。"""
        raise NotImplementedError

    def volume(self) -> float:
        raise NotImplementedError

    def surface_area(self) -> float:
        raise NotImplementedError

    def min_feature(self) -> float:
        raise NotImplementedError

    def params(self) -> dict:
        return {}

    # ---- 共通 ----
    def mesh(self) -> TriMesh:
        return self.local_mesh().transformed(self.R, self.t)

    def sdf(self, p) -> np.ndarray:
        return self.local_sdf(self.to_local(np.atleast_2d(p)))

    def anchor_points(self, m: float) -> np.ndarray:
        return self.to_world(self.local_anchors(m))

    def aabb(self):
        return self.mesh().aabb()


# ---- 箱 ----

class Box(Piece):
    kind = "box"

    def __init__(self, sx, sy, sz):
        super().__init__()
        self.size = np.array([sx, sy, sz], dtype=np.float64)
        if np.any(self.size <= 0):
            raise ValueError(f"box size must be positive: {self.size}")

    @property
    def half(self):
        return 0.5 * self.size

    def local_mesh(self) -> TriMesh:
        hx, hy, hz = self.half
        square = [(-hx, -hy), (hx, -hy), (hx, hy), (-hx, hy)]
        return _prism_mesh(square, hz)

    def local_sdf(self, p):
        q = np.abs(p) - self.half
        outside = np.linalg.norm(np.maximum(q, 0.0), axis=1)
        inside = np.minimum(np.max(q, axis=1), 0.0)
        return outside + inside

    def local_anchors(self, m: float):
        return _box_anchors(self.half, m)

    def volume(self):
        return float(np.prod(self.size))

    def surface_area(self):
        sx, sy, sz = self.size
        return 2.0 * float(sx * sy + sy * sz + sz * sx)

    def min_feature(self):
        return float(np.min(self.size))

    def params(self):
        return {"kind": self.kind, "size": [round(float(v), 5) for v in self.size]}


def _prism_mesh(poly2d, hz) -> TriMesh:
    """CCW の凸多角形を z=±hz で押し出した閉じた殻 (上面 +z 向き、下面 −z 向き、側面外向き)。"""
    poly = np.asarray(poly2d, dtype=np.float64)
    n = poly.shape[0]
    bottom = np.column_stack([poly, np.full(n, -hz)])
    top = np.column_stack([poly, np.full(n, hz)])
    verts = np.vstack([bottom, top])
    faces = []
    faces += [(0, i + 1, i) for i in range(1, n - 1)]          # 下面 (反転)
    faces += [(n, n + i, n + i + 1) for i in range(1, n - 1)]  # 上面
    for i in range(n):
        j = (i + 1) % n
        faces.append((i, j, n + j))
        faces.append((i, n + j, n + i))
    return TriMesh(verts, np.array(faces, dtype=np.int64))


# ---- 凸角柱 ----

class ConvexPrism(Piece):
    kind = "prism"

    def __init__(self, poly2d, height, feature=None):
        """poly2d は CCW の凸多角形 (xy)。feature は最小厚み (省略時は高さと内接円径の小さい方)。"""
        super().__init__()
        self.poly = np.asarray(poly2d, dtype=np.float64)
        if meshlib.polygon_area(self.poly) <= 0:
            self.poly = self.poly[::-1].copy()
        self.height = float(height)
        self._feature = feature

    def local_mesh(self):
        return _prism_mesh(self.poly, 0.5 * self.height)

    def local_sdf(self, p):
        d2 = meshlib.polygon_sdf(p[:, :2], self.poly)
        dz = np.abs(p[:, 2]) - 0.5 * self.height
        q = np.column_stack([np.maximum(d2, 0.0), np.maximum(dz, 0.0)])
        return np.linalg.norm(q, axis=1) + np.minimum(np.maximum(d2, dz), 0.0)

    def local_anchors(self, m: float):
        pts2 = _polygon_anchors(self.poly, m)
        zs = _axis_coords(0.5 * self.height, m)
        out = [(x, y, z) for z in zs for x, y in pts2]
        return np.array(out, dtype=np.float64) if out else np.zeros((0, 3))

    def volume(self):
        return abs(meshlib.polygon_area(self.poly)) * self.height

    def surface_area(self):
        perim = float(np.sum(np.linalg.norm(np.roll(self.poly, -1, axis=0) - self.poly, axis=1)))
        return 2.0 * abs(meshlib.polygon_area(self.poly)) + perim * self.height

    def min_feature(self):
        if self._feature is not None:
            return float(self._feature)
        # 内接円径の近似: 重心から各辺への距離の最小 × 2
        cx, cy = meshlib.polygon_centroid(self.poly)
        d = -float(meshlib.polygon_sdf(np.array([[cx, cy]]), self.poly)[0])
        return float(min(self.height, 2.0 * d))

    def params(self):
        return {"kind": self.kind, "n": int(self.poly.shape[0]), "height": round(self.height, 5),
                "extent": [round(float(v), 5) for v in (self.poly.max(axis=0) - self.poly.min(axis=0))]}


def regular_polygon(n: int, radius: float, phase: float = 0.0):
    return [(radius * math.cos(phase + 2.0 * math.pi * i / n),
             radius * math.sin(phase + 2.0 * math.pi * i / n)) for i in range(n)]


# ---- 回転体 (中実) ----

class Revolve(Piece):
    """(r,z) の折れ線を z 軸まわりに回した中実体。先頭と末尾は r=0 (極)、途中は r>0。"""
    kind = "revolve"

    def __init__(self, profile_rz, feature, segments=DEFAULT_SEGMENTS, label="revolve"):
        super().__init__()
        prof = [(float(r), float(z)) for r, z in profile_rz]
        if len(prof) < 3:
            raise ValueError("profile needs >= 3 points")
        if abs(prof[0][0]) > 1e-12 or abs(prof[-1][0]) > 1e-12:
            raise ValueError("profile must start and end on the axis (r=0)")
        if any(r <= 0.0 for r, _ in prof[1:-1]):
            raise ValueError("intermediate profile points must have r > 0")
        self.profile = prof
        self.segments = int(segments)
        self._feature = float(feature)
        self.label = label

    def _poly(self):
        # 断面多角形: 折れ線 + 軸に沿って閉じる (末尾極 → 先頭極は r=0 上の辺)
        return np.array(self.profile, dtype=np.float64)

    def local_mesh(self):
        prof = self.profile
        rings = prof[1:-1]
        seg = self.segments
        theta = 2.0 * np.pi * np.arange(seg) / seg
        verts = [np.array([[0.0, 0.0, prof[0][1]]])]
        for r, z in rings:
            verts.append(np.column_stack([r * np.cos(theta), r * np.sin(theta), np.full(seg, z)]))
        verts.append(np.array([[0.0, 0.0, prof[-1][1]]]))
        verts = np.vstack(verts)
        p0 = 0
        base = 1
        m = len(rings)
        pn = 1 + m * seg
        faces = []
        for j in range(seg):
            jn = (j + 1) % seg
            faces.append((p0, base + jn, base + j))
        for k in range(m - 1):
            b0 = base + k * seg
            b1 = base + (k + 1) * seg
            for j in range(seg):
                jn = (j + 1) % seg
                faces.append((b0 + j, b0 + jn, b1 + jn))
                faces.append((b0 + j, b1 + jn, b1 + j))
        bl = base + (m - 1) * seg
        for j in range(seg):
            jn = (j + 1) % seg
            faces.append((pn, bl + j, bl + jn))
        return TriMesh(verts, np.array(faces, dtype=np.int64))

    def local_sdf(self, p):
        r = np.sqrt(p[:, 0] ** 2 + p[:, 1] ** 2)
        rz = np.column_stack([r, p[:, 2]])
        return meshlib.polygon_sdf(rz, self._poly())

    def local_anchors(self, m: float):
        return _revolve_anchors(_polygon_anchors(self._poly(), m))

    def volume(self):
        poly = self._poly()
        area = abs(meshlib.polygon_area(poly))
        cr, _ = meshlib.polygon_centroid(poly)
        return 2.0 * math.pi * abs(cr) * area  # Pappus

    def surface_area(self):
        total = 0.0
        for (r0, z0), (r1, z1) in zip(self.profile[:-1], self.profile[1:]):
            total += 2.0 * math.pi * 0.5 * (r0 + r1) * math.hypot(r1 - r0, z1 - z0)
        return total

    def min_feature(self):
        return self._feature

    def params(self):
        poly = self._poly()
        return {"kind": self.label, "rmax": round(float(poly[:, 0].max()), 5),
                "zspan": round(float(poly[:, 1].max() - poly[:, 1].min()), 5)}


# ---- 回転体 (環) ----

class RevolveRing(Piece):
    """軸に接しない閉じた (r,z) 多角形を回した環 (トーラス・管・ワッシャ)。"""
    kind = "ring"

    def __init__(self, loop_rz, feature, segments=DEFAULT_SEGMENTS, label="ring"):
        super().__init__()
        loop = np.array([(float(r), float(z)) for r, z in loop_rz], dtype=np.float64)
        if loop.shape[0] < 3 or np.any(loop[:, 0] <= 0.0):
            raise ValueError("ring loop needs >= 3 points with r > 0")
        if meshlib.polygon_area(loop) < 0:
            loop = loop[::-1].copy()
        self.loop = loop
        self.segments = int(segments)
        self._feature = float(feature)
        self.label = label

    def local_mesh(self):
        loop = self.loop
        L = loop.shape[0]
        seg = self.segments
        theta = 2.0 * np.pi * np.arange(seg) / seg
        verts = np.vstack([np.column_stack([r * np.cos(theta), r * np.sin(theta), np.full(seg, z)])
                           for r, z in loop])
        faces = []
        for l in range(L):
            ln = (l + 1) % L
            b0, b1 = l * seg, ln * seg
            for j in range(seg):
                jn = (j + 1) % seg
                faces.append((b0 + j, b0 + jn, b1 + jn))
                faces.append((b0 + j, b1 + jn, b1 + j))
        return TriMesh(verts, np.array(faces, dtype=np.int64))

    def local_sdf(self, p):
        r = np.sqrt(p[:, 0] ** 2 + p[:, 1] ** 2)
        return meshlib.polygon_sdf(np.column_stack([r, p[:, 2]]), self.loop)

    def local_anchors(self, m: float):
        return _revolve_anchors(_polygon_anchors(self.loop, m), phases=ANCHOR_PHASES * 2)

    def volume(self):
        area = abs(meshlib.polygon_area(self.loop))
        cr, _ = meshlib.polygon_centroid(self.loop)
        return 2.0 * math.pi * cr * area

    def surface_area(self):
        total = 0.0
        L = self.loop.shape[0]
        for i in range(L):
            r0, z0 = self.loop[i]
            r1, z1 = self.loop[(i + 1) % L]
            total += 2.0 * math.pi * 0.5 * (r0 + r1) * math.hypot(r1 - r0, z1 - z0)
        return total

    def min_feature(self):
        return self._feature

    def params(self):
        return {"kind": self.label, "r_in": round(float(self.loop[:, 0].min()), 5),
                "r_out": round(float(self.loop[:, 0].max()), 5),
                "zspan": round(float(self.loop[:, 1].max() - self.loop[:, 1].min()), 5)}


# ---- 楕円体 (回転体ではないので別実装。球は radii が等しい特殊例) ----

class Ellipsoid(Piece):
    kind = "ellipsoid"

    def __init__(self, rx, ry, rz, lat_steps=SPHERE_LAT_STEPS, segments=DEFAULT_SEGMENTS):
        super().__init__()
        self.radii = np.array([rx, ry, rz], dtype=np.float64)
        if np.any(self.radii <= 0):
            raise ValueError("ellipsoid radii must be positive")
        self.lat_steps = int(lat_steps)
        self.segments = int(segments)

    def local_mesh(self):
        unit = Revolve(meshlib.circle_profile(1.0, self.lat_steps, half=True), feature=2.0,
                       segments=self.segments).local_mesh()
        return TriMesh(unit.verts * self.radii, unit.faces)

    def local_sdf(self, p):
        # 保守的な下界: 内側の点の深さは (1 - |p/r|) * min(r) 以上 (スケール球の包含で示せる)
        k = np.linalg.norm(p / self.radii, axis=1)
        return (k - 1.0) * float(np.min(self.radii))

    def local_anchors(self, m: float):
        # 格子 (半径の割合) + 表面から内側へ寄せた 26 方向の点。保守的な sdf
        # ((1-k)*rmin) で深さ m を満たすには k <= 1 - 1.5m/rmin まで寄せる
        rmin = float(np.min(self.radii))
        pts = [np.array((fx, fy, fz)) * self.radii * 0.5
               for fx in (-1.0, 0.0, 1.0) for fy in (-1.0, 0.0, 1.0) for fz in (-1.0, 0.0, 1.0)]
        for f in ANCHOR_DEPTH_FACTORS:
            k = 1.0 - f * m / rmin
            if k <= 0.0:
                continue
            for dx in (-1.0, 0.0, 1.0):
                for dy in (-1.0, 0.0, 1.0):
                    for dz in (-1.0, 0.0, 1.0):
                        d = np.array((dx, dy, dz))
                        n = np.linalg.norm(d)
                        if n < 1e-12:
                            continue
                        d = d / n
                        s = d / np.linalg.norm(d / self.radii)  # 表面上の点
                        pts.append(s * k)
        return np.array(pts, dtype=np.float64)

    def volume(self):
        return 4.0 / 3.0 * math.pi * float(np.prod(self.radii))

    def surface_area(self):
        # Knud Thomsen の近似 (p=1.6075、誤差 ≤ 1.1%)
        a, b, c = self.radii
        p = 1.6075
        return 4.0 * math.pi * ((a ** p * b ** p + a ** p * c ** p + b ** p * c ** p) / 3.0) ** (1.0 / p)

    def min_feature(self):
        return 2.0 * float(np.min(self.radii))

    def params(self):
        return {"kind": self.kind, "radii": [round(float(v), 5) for v in self.radii]}


# ---- 工場関数 (Revolve / RevolveRing の定型) ----

def sphere(r, lat_steps=SPHERE_LAT_STEPS, segments=DEFAULT_SEGMENTS):
    return Ellipsoid(r, r, r, lat_steps, segments)


def cylinder(r, h, segments=DEFAULT_SEGMENTS):
    return Revolve([(0.0, -h / 2), (r, -h / 2), (r, h / 2), (0.0, h / 2)],
                   feature=min(2.0 * r, h), segments=segments, label="cylinder")


def frustum(r0, r1, h, segments=DEFAULT_SEGMENTS):
    """z=-h/2 で半径 r0、z=+h/2 で半径 r1。r1=0 なら円錐。"""
    prof = [(0.0, -h / 2), (r0, -h / 2)]
    if r1 > 0.0:
        prof.append((r1, h / 2))
    prof.append((0.0, h / 2))
    return Revolve(prof, feature=min(2.0 * min(r0, max(r1, r0 * 0.5)), h), segments=segments,
                   label="frustum")


def capsule(r, h, lat_steps=6, segments=DEFAULT_SEGMENTS):
    """円筒部の長さ h、半径 r。全長 h + 2r。"""
    prof = [(0.0, -h / 2 - r)]
    for i in range(1, lat_steps):
        phi = -0.5 * math.pi + 0.5 * math.pi * i / lat_steps
        prof.append((r * math.cos(phi), -h / 2 + r * math.sin(phi)))
    prof.append((r, -h / 2))
    prof.append((r, h / 2))
    for i in range(1, lat_steps):
        phi = 0.5 * math.pi * i / lat_steps
        prof.append((r * math.cos(phi), h / 2 + r * math.sin(phi)))
    prof.append((0.0, h / 2 + r))
    return Revolve(prof, feature=2.0 * r, segments=segments, label="capsule")


def torus(R, r, tube_steps=12, segments=DEFAULT_SEGMENTS):
    if R <= r:
        raise ValueError("torus needs R > r")
    loop = [(R + r * math.cos(2 * math.pi * i / tube_steps), r * math.sin(2 * math.pi * i / tube_steps))
            for i in range(tube_steps)]
    return RevolveRing(loop, feature=2.0 * r, segments=segments, label="torus")


def tube(r_in, r_out, h, segments=DEFAULT_SEGMENTS):
    """両端が開いた管 (環の押し出し)。"""
    if not (0.0 < r_in < r_out):
        raise ValueError("tube needs 0 < r_in < r_out")
    return RevolveRing([(r_in, -h / 2), (r_out, -h / 2), (r_out, h / 2), (r_in, h / 2)],
                       feature=min(r_out - r_in, h), segments=segments, label="tube")


def shell_profile(outer_rz, wall, open_top=True):
    """中空容器の (r,z) 断面を作る: 外形の折れ線 (下極 (0,z0) から口 (r_top,z_top) まで、
    上端が開口) に肉厚 wall の内側の折れ線を足して閉じる。戻り値は Revolve 用 profile
    (先頭・末尾が r=0)。開口は flood-fill が中へ入るために必須 (閉じた殻は埋まる)。"""
    pts = [(float(r), float(z)) for r, z in outer_rz]
    if abs(pts[0][0]) > 1e-12:
        raise ValueError("outer profile must start on the axis")
    outer = pts
    # 内側: 外形を内側へ wall だけオフセット (法線近似: 各頂点で隣接辺の二等分方向)
    inner = []
    n = len(outer)
    for i in range(n):
        r, z = outer[i]
        if i == 0:
            inner.append((0.0, z + wall))
            continue
        pr, pz = outer[i - 1]
        nr, nz = outer[i + 1] if i + 1 < n else (r, z + 1.0)  # 口の先は上方向
        d1 = np.array([r - pr, z - pz])
        d2 = np.array([nr - r, nz - z])
        d1 /= max(np.linalg.norm(d1), 1e-12)
        d2 /= max(np.linalg.norm(d2), 1e-12)
        # 2D の外向き法線 (回転体の外側 = +r 側) は (dz, -dr)
        n1 = np.array([d1[1], -d1[0]])
        n2 = np.array([d2[1], -d2[0]])
        nn = n1 + n2
        nn /= max(np.linalg.norm(nn), 1e-12)
        ir, iz = r - wall * nn[0], z - wall * nn[1]
        inner.append((max(ir, wall * 0.25), iz))
    if open_top:
        # 口: 外側上端 → 内側上端 (同じ高さに揃える) → 内側を下る → 軸へ
        top_z = outer[-1][1]
        inner[-1] = (inner[-1][0], top_z)
        profile = outer + [inner[i] for i in range(n - 1, -1, -1)]
        # 末尾 inner[0] は (0, z0 + wall) で r=0 → 極として閉じる
        return profile
    raise ValueError("closed shells are always filled by the voxelizer; use open_top=True")
