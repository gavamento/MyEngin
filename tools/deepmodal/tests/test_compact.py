#====================================================================================
#                          test_compact.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          Mel 圧縮 (compact.py) の単体テスト
#====================================================================================
import numpy as np

import compact
import layout


def test_band_edges_endpoints_and_monotonic():
    """帯域境界は F_MIN で始まり F_MAX で終わり、厳密に単調増加する (端点 / 単調)。"""
    edges = compact.band_edges_mel()
    assert edges.shape[0] == layout.MEL_BANDS + 1
    assert np.isclose(edges[0], layout.F_MIN)
    assert np.isclose(edges[-1], layout.F_MAX)
    assert np.all(np.diff(edges) > 0.0)


def test_band_of_clamps_out_of_range():
    edges = compact.band_edges_mel()
    assert compact.band_of(layout.F_MIN - 50.0, edges) == 0
    assert compact.band_of(layout.F_MAX + 50.0, edges) == layout.MEL_BANDS - 1


def test_sum_amp_is_monotonic_in_contributions():
    """同じ帯域に落ちるモードを追加すると Σ|a| は単調に増える (減ることはない)。"""
    freqs = np.array([500.0])
    amps_small = np.array([[0.1, 0.0, 0.0]])
    amps_large = np.array([[0.1, 0.0, 0.0], [0.2, 0.0, 0.0]])
    freqs2 = np.array([500.0, 520.0])
    sum1, mask1, _ = compact.compact_cell(freqs, amps_small)
    sum2, mask2, _ = compact.compact_cell(freqs2, amps_large)
    band = compact.band_of(500.0)
    assert sum2[0, band] >= sum1[0, band]


def test_mask_reflects_threshold():
    """相対閾値 (min_rel) 未満のモードは寄与せず、mask が False のままになる。"""
    freqs = np.array([500.0, 5000.0])
    # 軸0: 500Hz に強い振幅、5000Hz に閾値未満の弱い振幅
    amps = np.array([[1.0, 0.0, 0.0], [1.0e-6, 0.0, 0.0]])
    sum_amp, mask, ln_amp = compact.compact_cell(freqs, amps, min_rel=1e-3)
    b_strong = compact.band_of(500.0)
    b_weak = compact.band_of(5000.0)
    assert mask[0, b_strong]
    assert not mask[0, b_weak]


def test_empty_band_filled_from_nearest():
    """空帯域 (mask=False) は同軸内で最も近い mask=True 帯域の ln 値で埋まる。"""
    freqs = np.array([200.0, 8000.0])
    amps = np.array([[1.0, 0.0, 0.0], [2.0, 0.0, 0.0]])
    sum_amp, mask, ln_amp = compact.compact_cell(freqs, amps)
    b_lo = compact.band_of(200.0)
    b_hi = compact.band_of(8000.0)
    # 2 帯域の中間にある空帯域は、両側のうち近い方の値と一致するはず
    mid = (b_lo + b_hi) // 2
    if mid != b_lo and mid != b_hi and not mask[0, mid]:
        dist_lo = abs(mid - b_lo)
        dist_hi = abs(mid - b_hi)
        expected = ln_amp[0, b_lo] if dist_lo <= dist_hi else ln_amp[0, b_hi]
        assert np.isclose(ln_amp[0, mid], expected)


def test_all_empty_axis_keeps_floor_value_and_no_crash():
    freqs = np.array([500.0])
    amps = np.array([[0.0, 0.0, 0.0]])  # 全軸ゼロ振幅
    sum_amp, mask, ln_amp = compact.compact_cell(freqs, amps)
    assert not np.any(mask)
    assert np.all(np.isfinite(ln_amp))


def test_no_modes_returns_all_empty():
    sum_amp, mask, ln_amp = compact.compact_cell(np.zeros(0), np.zeros((0, 3)))
    assert sum_amp.shape == (3, layout.MEL_BANDS)
    assert not np.any(mask)


def test_cell_band_coverage_is_or_over_axes():
    """coverage は軸の OR (1 本でも立てばその帯域は cover) — ランタイムの BuildModes が
    軸を足す (Σ_j) ことと整合させるため、AND や平均ではなく OR でなければならない。"""
    mask = np.zeros((3, layout.MEL_BANDS), dtype=bool)
    mask[0, 5] = True   # 軸 0 だけ帯域 5 が on
    mask[1, 5] = False
    mask[2, 5] = False
    mask[:, 10] = False  # どの軸も off
    mask[1, 20] = True
    mask[2, 20] = True  # 2 軸 on (3 軸目は off でも coverage は on のまま)

    coverage = compact.cell_band_coverage(mask)
    assert coverage.shape == (layout.MEL_BANDS,)
    assert coverage[5] == True  # noqa: E712 - bool 配列の意図を明示
    assert coverage[10] == False  # noqa: E712
    assert coverage[20] == True  # noqa: E712
