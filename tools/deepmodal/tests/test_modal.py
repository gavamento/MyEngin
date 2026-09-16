#====================================================================================
#                          test_modal.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          一般化固有値解法のスケール則と残差のテスト
#====================================================================================
import numpy as np

import fem
import modal


def _solve(h, young, density, nu=0.33, k=40):
    occ = np.ones((2, 2, 2), dtype=np.uint8)
    K, M, *_ = fem.assemble_from_occupancy(occ, h, young, density, nu)
    result = modal.solve_modes(K, M, k=k, f_min=0.0, f_max=1.0e12)
    return result.freq, result.raw


def test_first_six_eigenvalues_are_rigid_body():
    """2x2x2 の自由-自由メッシュは剛体 6 モードぶんの固有値がほぼ 0 になり、
    7 本目以降は明確に大きい (数桁のオーダー差)。"""
    _, raw = _solve(0.01, 7.0e10, 2700.0)
    rigid = raw[:6]
    real = raw[6:12]
    assert np.max(np.abs(rigid)) < 1.0  # 実固有値 (1e8 台) に対して無視できる大きさ
    assert np.min(real) > 1.0e6


def test_young_modulus_scaling():
    """E を 4 倍すると周波数 (= sqrt(lambda)) は 2 倍になる (Ke ∝ E の直接の帰結)。"""
    freq1, _ = _solve(0.01, 7.0e10, 2700.0)
    freq4, _ = _solve(0.01, 7.0e10 * 4.0, 2700.0)
    ratio = freq4[:10] / freq1[:10]
    assert np.allclose(ratio, 2.0, rtol=1e-6)


def test_density_scaling():
    """rho を 4 倍すると周波数は半分になる (Me ∝ rho)。"""
    freq1, _ = _solve(0.01, 7.0e10, 2700.0)
    freq_rho, _ = _solve(0.01, 7.0e10, 2700.0 * 4.0)
    ratio = freq_rho[:10] / freq1[:10]
    assert np.allclose(ratio, 0.5, rtol=1e-6)


def test_size_scaling():
    """h を 2 倍すると周波数は半分になる (spec §4.1 のスケール則、式 12 の根拠)。"""
    freq1, _ = _solve(0.01, 7.0e10, 2700.0)
    freq_h, _ = _solve(0.02, 7.0e10, 2700.0)
    ratio = freq_h[:10] / freq1[:10]
    assert np.allclose(ratio, 0.5, rtol=1e-6)


def test_band_filter_and_rigid_removal_end_to_end():
    """solve_modes() の帯域フィルタが機能し、剛体モードが結果に混入しない。
    h=0.01 では最低次の実モードが約 57 kHz (帯域外) になる (h×10 で 1/10、
    帯域 [100,10000] Hz に収まるようにする — size scaling のテストで検算済みの式)。"""
    occ = np.ones((2, 2, 2), dtype=np.uint8)
    K, M, *_ = fem.assemble_from_occupancy(occ, 0.1, 7.0e10, 2700.0, 0.33)
    result = modal.solve_modes(K, M, k=40, f_min=100.0, f_max=10000.0)
    assert result.freq.shape[0] > 0
    assert np.all(result.freq >= 100.0)
    assert np.all(result.freq <= 10000.0)
    assert result.vecs.shape[1] == result.freq.shape[0]
    assert result.method == "exact"


def test_exact_residuals_are_tiny():
    """shift-invert (完全 LU) は数値的にほぼ厳密なので、残差は RESIDUAL_ACCEPT より
    何桁も小さい (退化していない普通のケースでの健全性チェック)。"""
    occ = np.ones((2, 2, 2), dtype=np.uint8)
    K, M, *_ = fem.assemble_from_occupancy(occ, 0.1, 7.0e10, 2700.0, 0.33)
    result = modal.solve_modes(K, M, k=40, f_min=100.0, f_max=10000.0)
    assert result.residuals.shape[0] == result.freq.shape[0]
    assert np.all(np.isfinite(result.residuals))
    assert np.max(result.residuals) < modal.RESIDUAL_ACCEPT


def test_solve_modes_is_deterministic_for_symmetric_mesh():
    """sub-04 round 2 で判明した欠陥の回帰テスト: `eigsh` は `v0` (Krylov 部分空間の
    初期ベクトル) を指定しないと ARPACK が乱数ベクトルを使う。対称形状 (この
    4x4x4 の立方体のような) は固有値が縮退するため、初期ベクトルが変わると
    縮退部分空間内の基底が変わり、同じ K/M でも固有ベクトルが実行のたびに
    変わってしまう (実測: dataset.py が生成する npz の `feat` がボクセル列
    バイト一致の重複入力間で最大 6.9 食い違っていた)。`solve_modes()` に
    `seed` 引数で固定 `v0` を渡すようにした修正が効いていることを、
    同一 K/M への 2 回の呼び出しがビット一致することで確認する。"""
    occ = np.ones((4, 4, 4), dtype=np.uint8)
    K, M, *_ = fem.assemble_from_occupancy(occ, 0.1, 7.0e10, 2700.0, 0.33)

    r1 = modal.solve_modes(K, M, k=30, f_min=0.0, f_max=1.0e12)
    r2 = modal.solve_modes(K, M, k=30, f_min=0.0, f_max=1.0e12)

    assert np.array_equal(r1.freq, r2.freq)
    assert np.array_equal(r1.vecs, r2.vecs)


def test_lobpcg_matches_exact_on_small_mesh():
    """小メッシュ (4x4x4、375 DOF、1 秒未満) で solve_modes と solve_modes_lobpcg の
    低次モードが周波数・残差ともに一致することを回帰的に固定する
    (spec §5 #21「小メッシュの pytest 1 本」に対応。calibrate.py の縮小版)。
    ★対称形状は固有値が縮退しうるので、比較は周波数 (昇順対応) だけで行い、
    生の固有ベクトルは比較しない (§4.1 の縮退モードの罠)。"""
    occ = np.ones((4, 4, 4), dtype=np.uint8)
    K, M, *_ = fem.assemble_from_occupancy(occ, 0.1, 7.0e10, 2700.0, 0.33)

    exact = modal.solve_modes(K, M, k=30, f_min=0.0, f_max=1.0e12)
    lob = modal.solve_modes_lobpcg(K, M, m=30, maxiter=200, f_min=0.0, f_max=1.0e12)

    assert exact.freq.shape[0] > 5
    assert lob.freq.shape[0] > 5
    n = min(6, exact.freq.shape[0], lob.freq.shape[0])
    rel_err = np.abs(lob.freq[:n] - exact.freq[:n]) / np.maximum(exact.freq[:n], 1e-9)
    assert np.all(rel_err < 1e-3)

    # 両方とも RESIDUAL_ACCEPT 級 (このメッシュ規模は LOBPCG も高精度に収束するはず)
    assert np.max(exact.residuals[:n]) < modal.RESIDUAL_ACCEPT
    assert np.max(lob.residuals[:n]) < modal.RESIDUAL_DROP
