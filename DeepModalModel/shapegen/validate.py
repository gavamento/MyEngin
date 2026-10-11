#====================================================================================
#                          validate.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          .mvox の受理検査 (占有数・6 連結・cell 数・糸状・重複)
#====================================================================================
"""有効の定義 (README「有効 / 有用」)。順に調べ、最初に引っかかった理由で却下する:
  vox_error → vox_anomaly → occ_low/occ_high → disconnected → cells_low → filament →
  dup_exact → dup_coarse
重複判定は呼び出し側がインデックス昇順で `DedupTable` に通す (並列度に依存しないため)。
"""
import hashlib
from dataclasses import dataclass, asdict

import numpy as np
from scipy import ndimage

import meshio

from . import constants as C

_STRUCT6 = ndimage.generate_binary_structure(3, 1)


@dataclass
class Verdict:
    ok: bool
    reason: str
    occ: int = 0
    surface: int = 0
    interior: int = 0
    cells: int = 0
    filament_ratio: float = 0.0
    components: int = 0
    max_component_ratio: float = 0.0
    extent: int = 0
    occ_hash: str = ""
    coarse_key: str = ""

    def to_dict(self):
        return asdict(self)


def occupied_cells(occ: np.ndarray) -> np.ndarray:
    """2x2x2 voxel ごとの 16^3 cell で、占有 voxel を 1 個以上含む cell (bool [16,16,16])。"""
    n = C.MAP_N
    return occ.reshape(n, 2, n, 2, n, 2).any(axis=(1, 3, 5))


def filament_ratio(occ: np.ndarray) -> float:
    """6 近傍の占有数が 1 以下の占有 voxel の比率。"""
    o = occ.astype(np.int32)
    total = int(o.sum())
    if total == 0:
        return 0.0
    nb = np.zeros_like(o)
    nb[1:, :, :] += o[:-1, :, :]
    nb[:-1, :, :] += o[1:, :, :]
    nb[:, 1:, :] += o[:, :-1, :]
    nb[:, :-1, :] += o[:, 1:, :]
    nb[:, :, 1:] += o[:, :, :-1]
    nb[:, :, :-1] += o[:, :, 1:]
    thin = int(np.sum((o > 0) & (nb <= 1)))
    return thin / total


def analyze_occ(occ: np.ndarray, surface_count: int, interior_count: int,
                occ_min: int = C.OCC_MIN, occ_max: int = C.OCC_MAX) -> Verdict:
    """重複判定を除く全検査。occ は uint8 [32,32,32]。"""
    occ = (np.asarray(occ) != 0)
    n = C.VOXEL_N
    total = int(occ.sum())
    v = Verdict(False, "", occ=total, surface=int(surface_count), interior=int(interior_count))
    if total == 0:
        v.reason = "vox_anomaly"
        return v
    if surface_count + interior_count != total:
        v.reason = "vox_anomaly"
        return v
    pad = int(occ[0].sum() + occ[-1].sum() + occ[:, 0].sum() + occ[:, -1].sum()
              + occ[:, :, 0].sum() + occ[:, :, -1].sum())
    if pad != 0:
        v.reason = "vox_anomaly"
        return v
    idx = np.argwhere(occ)
    extent = int(np.max(idx.max(axis=0) - idx.min(axis=0) + 1))
    v.extent = extent
    if extent < n - 2 * C.PAD - 3:  # 最長辺は 29 voxel になるはず (多少の揺れは許す)
        v.reason = "vox_anomaly"
        return v
    if total < occ_min:
        v.reason = "occ_low"
        return v
    if total > occ_max:
        v.reason = "occ_high"
        return v
    labels, ncomp = ndimage.label(occ, structure=_STRUCT6)
    v.components = int(ncomp)
    if ncomp > 0:
        sizes = ndimage.sum(occ, labels, index=np.arange(1, ncomp + 1))
        v.max_component_ratio = float(np.max(sizes) / total)
    if ncomp != 1:
        v.reason = "disconnected"
        return v
    cells = int(occupied_cells(occ).sum())
    v.cells = cells
    if cells < C.CELLS_MIN:
        v.reason = "cells_low"
        return v
    fr = filament_ratio(occ)
    v.filament_ratio = fr
    if fr > C.FILAMENT_RATIO_MAX:
        v.reason = "filament"
        return v
    packed = np.packbits(occ.astype(np.uint8).ravel())
    v.occ_hash = hashlib.blake2b(packed.tobytes(), digest_size=16).hexdigest()
    coarse = np.packbits(occupied_cells(occ).astype(np.uint8).ravel())
    v.coarse_key = hashlib.blake2b(coarse.tobytes(), digest_size=16).hexdigest()
    v.ok = True
    return v


def analyze_mvox(path, occ_min: int = C.OCC_MIN, occ_max: int = C.OCC_MAX) -> Verdict:
    try:
        grid = meshio.read_mvox(str(path))
    except Exception as exc:  # noqa: BLE001 - 読めない理由は manifest に残す
        return Verdict(False, "vox_error", occ=0)
    return analyze_occ(grid.occ, grid.surface_count, grid.interior_count, occ_min, occ_max)


class DedupTable:
    """受理済みの占有ハッシュと 16^3 キーの出現数。manifest の受理行から再構築できる。"""

    def __init__(self, coarse_limit: int = C.COARSE_DUP_LIMIT):
        self.exact = set()
        self.coarse = {}
        self.coarse_limit = coarse_limit

    def add_accepted(self, occ_hash: str, coarse_key: str):
        self.exact.add(occ_hash)
        self.coarse[coarse_key] = self.coarse.get(coarse_key, 0) + 1

    def check_and_add(self, v: Verdict) -> str:
        """重複なら理由を返し、受理なら登録して "" を返す。"""
        if v.occ_hash in self.exact:
            return "dup_exact"
        if self.coarse.get(v.coarse_key, 0) >= self.coarse_limit:
            return "dup_coarse"
        self.add_accepted(v.occ_hash, v.coarse_key)
        return ""
