#====================================================================================
#                          rng.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          形状ごとの決定的な乱数 (seed 導出と分布ヘルパ)
#====================================================================================
"""形状 i の乱数列は (global_seed, i) だけから決まる。並列度・再開・実行順序に依存させない。

- `derive_seed` は SplitMix64 のミキシングを明示的に書く (Python の `hash()` は
  PYTHONHASHSEED で変わるので使わない)。
- `ShapeRng` は `random.Random` の `random()` だけを使って各分布を組む。CPython は
  `random()` と seeding のストリーム互換を文書で保証している (numpy の Generator は
  NEP 19 で保証しない) ため、ゴールデン値のテストで変化を検知できる。
"""
import math
import random

_MASK64 = (1 << 64) - 1


def _splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & _MASK64
    z = x
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & _MASK64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & _MASK64
    return z ^ (z >> 31)


def derive_seed(global_seed: int, index: int) -> int:
    """(global_seed, index) → 64bit seed。隣接 index や隣接 seed で相関しない。"""
    a = _splitmix64((global_seed & _MASK64) ^ 0xA5A5A5A5A5A5A5A5)
    b = _splitmix64(((index & _MASK64) * 0x2545F4914F6CDD1D) & _MASK64)
    return _splitmix64(a ^ (b + 0x632BE59BD9B4E019)) & _MASK64


class ShapeRng:
    """1 形状ぶんの乱数源。すべて `random()` 由来 (ストリーム互換のため)。"""

    def __init__(self, seed: int):
        self._r = random.Random(seed)
        self.seed = seed

    def random(self) -> float:
        return self._r.random()

    def uniform(self, lo: float, hi: float) -> float:
        return lo + (hi - lo) * self._r.random()

    def log_uniform(self, lo: float, hi: float) -> float:
        """対数一様 (寸法比のように桁で散らしたい量に使う)。lo, hi > 0。"""
        return math.exp(self.uniform(math.log(lo), math.log(hi)))

    def int_range(self, lo: int, hi: int) -> int:
        """lo..hi (両端含む) の整数。"""
        return lo + int(self._r.random() * (hi - lo + 1)) if hi > lo else lo

    def chance(self, p: float) -> bool:
        return self._r.random() < p

    def choice(self, seq):
        return seq[int(self._r.random() * len(seq))]

    def weighted(self, seq, weights):
        """重み付き選択 (重みは正。合計で正規化)。"""
        total = float(sum(weights))
        u = self._r.random() * total
        acc = 0.0
        for item, w in zip(seq, weights):
            acc += w
            if u < acc:
                return item
        return seq[-1]

    def sign(self) -> float:
        return 1.0 if self._r.random() < 0.5 else -1.0

    def normal(self, mu: float = 0.0, sigma: float = 1.0) -> float:
        """Box-Muller (random() 2 回)。"""
        u1 = max(self._r.random(), 1e-12)
        u2 = self._r.random()
        return mu + sigma * math.sqrt(-2.0 * math.log(u1)) * math.cos(2.0 * math.pi * u2)

    def unit_vector(self):
        """単位球面上の一様な方向。"""
        z = self.uniform(-1.0, 1.0)
        phi = self.uniform(0.0, 2.0 * math.pi)
        s = math.sqrt(max(0.0, 1.0 - z * z))
        return (s * math.cos(phi), s * math.sin(phi), z)

    def shuffle_copy(self, seq):
        """Fisher-Yates (random() 由来)。元の列は変えない。"""
        out = list(seq)
        for i in range(len(out) - 1, 0, -1):
            j = int(self._r.random() * (i + 1))
            out[i], out[j] = out[j], out[i]
        return out
