#====================================================================================
#                          test_contact.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          16^3 cell の接触節点選択と励起計算のテスト
#====================================================================================
import numpy as np

import contact
import fem
import modal


def _solve_small_box():
    """4x4x4 の全占有ボクセル (2x2x2 cell ぶん) を組み立てて固有値問題を解く。
    cell ごとに違う節点が選ばれることを確認するための最小構成。"""
    occ = np.ones((4, 4, 4), dtype=np.uint8)
    K, M, node_coords, dofs, voxels = fem.assemble_from_occupancy(occ, 0.1, 7.0e10, 2700.0,
                                                                    0.33)
    result = modal.solve_modes(K, M, k=30, f_min=0.0, f_max=1.0e12)
    return K, M, node_coords, dofs, voxels, result.freq, result.vecs


def test_pick_contact_node_uses_cell_local_voxels():
    K, M, node_coords, dofs, voxels = fem.assemble_from_occupancy(
        np.ones((4, 4, 4), dtype=np.uint8), 0.1, 7.0e10, 2700.0, 0.33)
    node_a = contact.pick_contact_node(0, 0, 0, voxels, dofs, node_coords)
    node_b = contact.pick_contact_node(1, 1, 1, voxels, dofs, node_coords)
    assert node_a is not None and node_b is not None
    assert node_a != node_b
    # 選ばれた節点は本当にそれぞれの cell の voxel 範囲内にある
    coord_a = node_coords[node_a]
    coord_b = node_coords[node_b]
    assert np.all(coord_a <= 2) and np.all(coord_a >= 0)
    assert np.all(coord_b >= 2) and np.all(coord_b <= 4)


def test_pick_contact_node_none_when_cell_empty():
    # 2x2x2 のグリッドでは cell (0,0,0) だけが存在し、他の cell 座標には
    # 対応する voxel が無い (16^3 の cell 空間全体の中の 1 個だけを使う想定)
    K, M, node_coords, dofs, voxels = fem.assemble_from_occupancy(
        np.ones((2, 2, 2), dtype=np.uint8), 0.1, 7.0e10, 2700.0, 0.33)
    assert contact.pick_contact_node(5, 5, 5, voxels, dofs, node_coords) is None


def test_excitation_changes_with_cell():
    """異なる cell (= 異なる接触節点) を選ぶと励起ベクトル a_ij が変わる
    (Checkpoint E の要求そのもの)。

    完全に対称な立方体 (4x4x4 の全占有) では、2x2x2 の cell 格子の全 8 cell が
    立方体の対称群で等価な「角」になり、対角の 2 cell (0,0,0)/(1,1,1) は
    |振幅| が偶然一致してしまう (実測で確認済み — 対称性の帰結であって
    excitation_for_cell のバグではない)。ここでは実際の固有ベクトルの代わりに
    節点ごとに異なる値を持つ合成モード行列を使い、「選ばれた節点が違えば
    抽出される振幅も違う」という pick→excite の合成そのものを検証する。"""
    _, _, node_coords, dofs, voxels, freq, _ = _solve_small_box()
    assert freq.shape[0] > 0
    n_nodes = node_coords.shape[0]
    rng = np.random.default_rng(0)
    synth_modes = rng.standard_normal((n_nodes * 3, freq.shape[0]))

    node_a, amps_a = contact.excitation_for_cell(0, 0, 0, voxels, dofs, node_coords, freq,
                                                  synth_modes)
    node_b, amps_b = contact.excitation_for_cell(1, 1, 1, voxels, dofs, node_coords, freq,
                                                  synth_modes)
    assert node_a is not None and node_b is not None
    assert node_a != node_b
    assert not np.allclose(amps_a, amps_b)


def test_excitation_amplitude_formula():
    """a_ij = |U[dof_j(node), i]| / omega_i が定義どおりであることを直接確認する。
    node_id=0 の 3 自由度が modes 行列の行 0,1,2 にそのまま対応する 3 dof 系。"""
    freqs = np.array([100.0, 200.0])
    modes = np.array([
        [1.0, 0.5],
        [2.0, 0.25],
        [0.0, 1.0],
    ])
    amps = contact.excitation_at_node(0, freqs, modes)
    omega = 2.0 * np.pi * freqs
    expected = np.stack([np.abs(modes[j, :]) / omega for j in range(3)], axis=1)
    assert np.allclose(amps, expected)
