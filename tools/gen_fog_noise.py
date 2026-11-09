# 霧のノイズマップ (assets/textures/fog_noise.png) を作る。
#
# かき乱しフィールドの「マップのノイズ」(DisturbanceField.noiseMap) のデモ用。R を霧の濃さの倍率として読む
# (0.5 が平均、明るいほど濃い)。上下左右がつながる (タイルする) ので、真上から繰り返して敷ける。
# 数値のノイズ (シェーダの fBm) と見分けが付くよう、x 方向に引き伸ばした筋 (風に流された霧) にしてある。
# seed 固定 = 何度回しても同じ画像になる。
#
#   python tools/gen_fog_noise.py
import os

import numpy as np
from PIL import Image

SIZE = 256
SEED = 20261109


def periodic_value_noise(rng, cells_x, cells_y, size):
    """周期 (cells_x, cells_y) の格子の乱数を smoothstep で補間した [0,1] のノイズ (size x size、タイルする)"""
    lattice = rng.random((cells_y, cells_x))
    ys = np.arange(size) * cells_y / size
    xs = np.arange(size) * cells_x / size
    y0 = np.floor(ys).astype(int)
    x0 = np.floor(xs).astype(int)
    fy = ys - y0
    fx = xs - x0
    fy = fy * fy * (3.0 - 2.0 * fy)
    fx = fx * fx * (3.0 - 2.0 * fx)
    y1 = (y0 + 1) % cells_y
    x1 = (x0 + 1) % cells_x
    a = lattice[np.ix_(y0, x0)]
    b = lattice[np.ix_(y0, x1)]
    c = lattice[np.ix_(y1, x0)]
    d = lattice[np.ix_(y1, x1)]
    top = a + (b - a) * fx[None, :]
    bottom = c + (d - c) * fx[None, :]
    return top + (bottom - top) * fy[:, None]


def fbm(rng, base_x, base_y, octaves, size):
    total = np.zeros((size, size))
    amp = 0.5
    norm = 0.0
    for k in range(octaves):
        total += amp * periodic_value_noise(rng, base_x << k, base_y << k, size)
        norm += amp
        amp *= 0.5
    return total / norm


def sample_wrapped(img, ox, oy):
    """img を (ox, oy) 画素ずらして読む (双線形、端は折り返し)"""
    size = img.shape[0]
    ys, xs = np.mgrid[0:size, 0:size].astype(float)
    sx = (xs + ox) % size
    sy = (ys + oy) % size
    x0 = np.floor(sx).astype(int)
    y0 = np.floor(sy).astype(int)
    fx = sx - x0
    fy = sy - y0
    x1 = (x0 + 1) % size
    y1 = (y0 + 1) % size
    top = img[y0, x0] + (img[y0, x1] - img[y0, x0]) * fx
    bottom = img[y1, x0] + (img[y1, x1] - img[y1, x0]) * fx
    return top + (bottom - top) * fy


def main():
    rng = np.random.default_rng(SEED)
    # 筋: x 方向の格子を z の 4 倍粗くした (横に引き伸ばした) fBm を、別の fBm で少しうねらせる (domain warp)
    streak = fbm(rng, 2, 8, 5, SIZE)
    warp_x = fbm(rng, 4, 4, 3, SIZE)
    warp_y = fbm(rng, 4, 4, 3, SIZE)
    warped = sample_wrapped(streak, (warp_x - 0.5) * 48.0, (warp_y - 0.5) * 12.0)
    # 平均 0.5 へ寄せてからコントラストを上げる (霧の倍率 1 + s(2n - 1) の n。暗い所 = 霧の切れ目)
    n = warped - warped.mean() + 0.5
    n = np.clip(0.5 + (n - 0.5) * 2.2, 0.0, 1.0)
    img = (n * 255.0 + 0.5).astype(np.uint8)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, 'assets', 'textures', 'fog_noise.png')
    Image.fromarray(img, mode='L').save(out)
    print(f'wrote {out} ({SIZE}x{SIZE}, mean {n.mean():.3f})')


if __name__ == '__main__':
    main()
