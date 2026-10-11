#====================================================================================
#                          rotation.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          回転行列のヘルパと形状全体のランダム回転モード
#====================================================================================
"""ネットは回転同変ではないので、同じ形状でも姿勢が違えば別サンプルになる。
ボクセル化は AABB の最長辺で正規化するため、軸平行でない姿勢は階段状の占有パターンを
生む (実アセットも軸平行とは限らない)。4 つのモードを混ぜる:
  identity   : そのまま
  octahedral : 立方体の対称群 (24 要素) から一様 — 軸の入れ替えだけ
  tilt       : ランダム軸まわりに 3°〜20° — わずかな傾き
  free       : Shoemake の一様四元数 — 任意姿勢
"""
import math

import numpy as np

TILT_DEG_MIN = 3.0
TILT_DEG_MAX = 20.0


def rot_x(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=np.float64)


def rot_y(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=np.float64)


def rot_z(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=np.float64)


def axis_angle(axis, deg):
    a = np.asarray(axis, dtype=np.float64)
    n = np.linalg.norm(a)
    if n < 1e-12:
        return np.eye(3)
    a = a / n
    th = math.radians(deg)
    c, s = math.cos(th), math.sin(th)
    K = np.array([[0, -a[2], a[1]], [a[2], 0, -a[0]], [-a[1], a[0], 0]])
    return np.eye(3) + s * K + (1 - c) * (K @ K)


def align_z_to(direction):
    """+z を direction へ向ける回転 (最短弧)。"""
    d = np.asarray(direction, dtype=np.float64)
    n = np.linalg.norm(d)
    if n < 1e-12:
        return np.eye(3)
    d = d / n
    z = np.array([0.0, 0.0, 1.0])
    c = float(np.dot(z, d))
    if c > 1.0 - 1e-12:
        return np.eye(3)
    if c < -1.0 + 1e-12:
        return rot_x(180.0)
    axis = np.cross(z, d)
    return axis_angle(axis, math.degrees(math.acos(c)))


def quat_to_matrix(w, x, y, z):
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ], dtype=np.float64)


def _octahedral_group():
    """90° 回転の積で閉じる 24 要素 (決定的な順序)。"""
    gens = [rot_x(90.0), rot_y(90.0), rot_z(90.0)]
    found = [np.eye(3)]
    frontier = [np.eye(3)]
    while frontier:
        nxt = []
        for M in frontier:
            for G in gens:
                P = np.rint(G @ M).astype(np.float64)
                if not any(np.array_equal(P, Q) for Q in found):
                    found.append(P)
                    nxt.append(P)
        frontier = nxt
    assert len(found) == 24
    return found


OCTAHEDRAL = _octahedral_group()
MODES = ("identity", "octahedral", "tilt", "free")


def random_rotation(rng, mode: str) -> np.ndarray:
    if mode == "identity":
        return np.eye(3)
    if mode == "octahedral":
        return OCTAHEDRAL[rng.int_range(0, 23)]
    if mode == "tilt":
        axis = rng.unit_vector()
        return axis_angle(axis, rng.uniform(TILT_DEG_MIN, TILT_DEG_MAX))
    if mode == "free":
        u1, u2, u3 = rng.random(), rng.random(), rng.random()
        a, b = math.sqrt(1.0 - u1), math.sqrt(u1)
        w = a * math.sin(2 * math.pi * u2)
        x = a * math.cos(2 * math.pi * u2)
        y = b * math.sin(2 * math.pi * u3)
        z = b * math.cos(2 * math.pi * u3)
        return quat_to_matrix(w, x, y, z)
    raise ValueError(f"unknown rotation mode: {mode}")


def pick_mode(rng, mix: dict) -> str:
    names = list(MODES)
    return rng.weighted(names, [float(mix.get(n, 0)) for n in names])
