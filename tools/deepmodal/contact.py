#====================================================================================
#                          contact.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          16^3 cell ごとの接触節点選択と励起ベクトル計算
#====================================================================================
"""cell (16^3、C++ の CellIndexOf/LocalPointToCell と同じ cell=voxel>>1 の畳み込み) ごとに
「占有 voxel の節点のうち中心に最も近いもの」を接触点の代表節点として選び、
その節点に単位力を加えたときの各モードの応答 a_ij = |U[dof_j(node), i]| / omega_i を返す。

節点の compact index は fem.assemble_from_occupancy() が返すもの (raw コーナー座標を
昇順に並べたときの順位) をそのまま使う。距離同値は raw コーナー座標ではなく
**compact index の小さい方** を採用する — compact index は raw の辞書式順序を保つ単調写像
なので、既存コードの「同値は index の小さい方」規約とも矛盾しない。
"""
import numpy as np

import layout


def cell_voxel_range(cx: int, cy: int, cz: int):
    """cell 座標 → 占有チェック対象の voxel 座標範囲 (各軸 2 個、[2c, 2c+1])。"""
    return (2 * cx, 2 * cx + 1), (2 * cy, 2 * cy + 1), (2 * cz, 2 * cz + 1)


def cell_center_node_coord(cx: int, cy: int, cz: int) -> np.ndarray:
    """cell の中心 (節点座標系、2 voxel ぶんの中点)。"""
    return np.array([2 * cx + 1, 2 * cy + 1, 2 * cz + 1], dtype=np.int64)


def pick_contact_node(cx: int, cy: int, cz: int, voxels: np.ndarray, dofs: np.ndarray,
                       node_coords: np.ndarray):
    """cell 内の占有 voxel の節点から、中心に最も近い節点の compact index を返す。
    該当する占有 voxel が無ければ None。距離同値は compact index の小さい方。"""
    (vx0, vx1), (vy0, vy1), (vz0, vz1) = cell_voxel_range(cx, cy, cz)
    sel = ((voxels[:, 0] >= vx0) & (voxels[:, 0] <= vx1)
           & (voxels[:, 1] >= vy0) & (voxels[:, 1] <= vy1)
           & (voxels[:, 2] >= vz0) & (voxels[:, 2] <= vz1))
    if not np.any(sel):
        return None
    cand_nodes = np.unique(dofs[sel] // 3)
    center = cell_center_node_coord(cx, cy, cz)
    coords = node_coords[cand_nodes]
    dist2 = np.sum((coords - center) ** 2, axis=1)
    order = np.lexsort((cand_nodes, dist2))  # 距離昇順、同値は node id 昇順
    return int(cand_nodes[order[0]])


def excitation_at_node(node_id: int, freqs: np.ndarray, modes: np.ndarray) -> np.ndarray:
    """節点 node_id に単位力 (x/y/z) を加えたときの各モードの応答振幅。
    戻り値 (n_modes, 3): a_ij = |U[dof_j(node), i]| / omega_i。"""
    if freqs.shape[0] == 0:
        return np.zeros((0, 3))
    omega = 2.0 * np.pi * freqs
    amps = np.zeros((freqs.shape[0], 3))
    for j in range(3):
        dof = node_id * 3 + j
        amps[:, j] = np.abs(modes[dof, :]) / omega
    return amps


def excitation_for_cell(cx: int, cy: int, cz: int, voxels: np.ndarray, dofs: np.ndarray,
                         node_coords: np.ndarray, freqs: np.ndarray, modes: np.ndarray):
    """cell (cx,cy,cz) の接触節点を選び、その励起ベクトルを返す。
    戻り値: (node_id または None, amps (n_modes,3))。node_id が None のときは
    その cell に占有 voxel が無い (= 特徴マップ側で cellSlot により最寄り有効 cell へ
    ラウティングされる想定、ここでは計算しない)。"""
    node_id = pick_contact_node(cx, cy, cz, voxels, dofs, node_coords)
    if node_id is None:
        return None, np.zeros((freqs.shape[0], 3))
    return node_id, excitation_at_node(node_id, freqs, modes)
