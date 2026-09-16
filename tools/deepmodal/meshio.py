#====================================================================================
#                          meshio.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          .mvox の読み込みとモデルファイルの列挙
#====================================================================================
"""C++ 側 (Voxelizer.h の SerializeVox) が書いた .mvox をフィールド単位で読む。
Python は自前のボクセライザを持たない (spec §2 #8) — ここは読むだけ。"""
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np

import layout


@dataclass
class VoxelGrid:
    origin: np.ndarray       # (3,) float64
    voxel_size: float
    aabb_min: np.ndarray     # (3,)
    aabb_max: np.ndarray     # (3,)
    longest_edge: float
    surface_count: int
    interior_count: int
    occ: np.ndarray          # (32,32,32) uint8, occ[x,y,z] (VoxelIndexOf = x + 32*(y+32*z) と対応)


def read_mvox(path) -> VoxelGrid:
    """.mvox を読む。ヘッダの magic/version/n が食い違えば ValueError。"""
    data = Path(path).read_bytes()
    if len(data) != layout.MVOX_TOTAL_BYTES:
        raise ValueError(
            f"unexpected .mvox size: {len(data)} (expected {layout.MVOX_TOTAL_BYTES})")
    fields = struct.unpack_from(layout.MVOX_STRUCT_FMT, data, 0)
    values = dict(zip(layout.MVOX_FIELDS, fields))
    if values["magic"] != layout.MVOX_MAGIC:
        raise ValueError(f"bad .mvox magic: {values['magic']:#x}")
    if values["version"] != layout.MVOX_VERSION:
        raise ValueError(f"unsupported .mvox version: {values['version']}")
    if values["n"] != layout.VOXEL_N:
        raise ValueError(f"unexpected .mvox n: {values['n']} (expected {layout.VOXEL_N})")

    occ_bytes = data[layout.MVOX_HEADER_BYTES:layout.MVOX_TOTAL_BYTES]
    n = layout.VOXEL_N
    # C++ の VoxelIndexOf(x,y,z) = x + n*(y + n*z) は x が最内、z が最外。
    # numpy の C-order reshape((z,y,x)) はそのまま同じメモリレイアウトになる
    occ = np.frombuffer(occ_bytes, dtype=np.uint8).reshape((n, n, n))  # occ[z,y,x]
    occ = np.transpose(occ, (2, 1, 0))  # occ[x,y,z] に並べ替え (fem.py 側の慣習に合わせる)

    return VoxelGrid(
        origin=np.array([values["origin_x"], values["origin_y"], values["origin_z"]]),
        voxel_size=values["voxel_size"],
        aabb_min=np.array([values["aabb_min_x"], values["aabb_min_y"], values["aabb_min_z"]]),
        aabb_max=np.array([values["aabb_max_x"], values["aabb_max_y"], values["aabb_max_z"]]),
        longest_edge=values["longest_edge"],
        surface_count=values["surface_count"],
        interior_count=values["interior_count"],
        occ=occ,
    )


def list_mvox_files(directory) -> list:
    """directory 直下の .mvox をファイル名昇順で列挙する (決定論のため sorted 必須)。"""
    return sorted(Path(directory).glob("*.mvox"))
