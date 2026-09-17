#====================================================================================
#                          compact.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          モード列を Mel 32 帯域 x 3 軸へ圧縮する
#====================================================================================
"""接触励起 (contact.py) が計算した「1 cell あたりのモード列 (freq, a_ij)」を、
ネット出力と同じ Mel 32 帯域 x 3 力軸へ圧縮する。

手順 (論文 §3 + spec §4.1「Mel」を踏まえた実装):
  1. 各モードの周波数を Mel 等分の帯域 index へ割り当てる (band_of)
  2. 軸ごとに帯域内の |a| を合算する (Σ|a|)。ただし「その cell の中で最大振幅の
     1e-3 未満」の弱いモードは寄与ゼロ (未励起扱い、ノイズフロアの除去)
  3. 帯域に実データがあるかを mask (bool) に記録してから、Σ|a| の自然対数を取る
  4. 空帯域 (mask=False) は同じ軸内の最も近い非空帯域の ln 値で埋める
     (論文の trick。同値距離は index の小さい方 — LocalPointToCell 等と同じ規約)
"""
import numpy as np

import layout

_LN_FLOOR = 1e-12  # log(0) を避けるための床 (mask=False のときだけ意味を持つ)


def band_edges_mel() -> np.ndarray:
    """MEL_BANDS+1 個の帯域境界 (Hz)。帯域 i は [edges[i], edges[i+1]) をカバーする
    (最終帯域だけ edges[-1] を含む、band_of がクランプで吸収する)。"""
    mel_min = layout.mel_of(layout.F_MIN)
    mel_max = layout.mel_of(layout.F_MAX)
    edges = np.empty(layout.MEL_BANDS + 1)
    for i in range(layout.MEL_BANDS + 1):
        m = mel_min + (mel_max - mel_min) * (i / layout.MEL_BANDS)
        edges[i] = layout.inv_mel(m)
    return edges


def band_of(freq, edges: np.ndarray = None):
    """周波数 (Hz、スカラーまたは配列) が属する帯域 index。範囲外は最近接端にクランプする。"""
    if edges is None:
        edges = band_edges_mel()
    idx = np.searchsorted(edges, freq, side='right') - 1
    return np.clip(idx, 0, layout.MEL_BANDS - 1)


def fill_empty_bands(ln_amp: np.ndarray, mask: np.ndarray) -> np.ndarray:
    """mask=False の帯域を、同じ配列内で最も近い mask=True の帯域の値で埋める。
    同値距離 (両隣が等距離) は index の小さい方を採用する。全帯域が空なら変更しない。"""
    n = ln_amp.shape[0]
    filled = ln_amp.copy()
    valid_idx = np.flatnonzero(mask)
    if valid_idx.size == 0:
        return filled
    for i in range(n):
        if mask[i]:
            continue
        dist = np.abs(valid_idx - i)
        best = valid_idx[np.argmin(dist)]  # argmin は最初の最小値 (= 小さい index) を返す
        filled[i] = ln_amp[best]
    return filled


def compact_cell(freqs: np.ndarray, amps: np.ndarray, min_rel: float = 1e-3):
    """1 cell ぶんのモード列を Mel 32 帯域 x 3 軸へ圧縮する。

    freqs: (n,) Hz
    amps : (n,3) 軸ごとの a_ij (= |U[dof_j(node), i]| / omega_i、contact.py が計算)
    戻り値:
      sum_amp : (3,32) 閾値後の Σ|a| (生値、対数化前)
      mask    : (3,32) bool、閾値後に実データがある帯域
      ln_amp  : (3,32) log(sum_amp) を空帯域は最寄り非空値で埋めたもの
    """
    n_bands = layout.MEL_BANDS
    sum_amp = np.zeros((3, n_bands))
    mask = np.zeros((3, n_bands), dtype=bool)
    if freqs.shape[0] == 0:
        ln_amp = np.full((3, n_bands), np.log(_LN_FLOOR))
        return sum_amp, mask, ln_amp

    edges = band_edges_mel()
    bands = band_of(freqs, edges)  # (n,)
    max_amp = float(np.max(np.abs(amps))) if amps.size else 0.0
    if max_amp <= 0.0:
        # 全モードが振幅ゼロ (励起なし)。閾値を 0 のまま `a >= threshold` を使うと
        # ゼロ振幅そのものが「閾値以上」を満たしてしまう (0 >= 0) ので、ここで
        # 早期に「実データなし」を確定させる (mask は全部 False のまま)
        ln_amp = np.full((3, n_bands), np.log(_LN_FLOOR))
        return sum_amp, mask, ln_amp
    threshold = max_amp * min_rel

    for j in range(3):
        for i in range(n_bands):
            sel = (bands == i)
            if not np.any(sel):
                continue
            a = np.abs(amps[sel, j])
            a = a[a >= threshold]  # 未励起 (弱すぎるモード) は寄与しない
            if a.size == 0:
                continue
            sum_amp[j, i] = float(np.sum(a))
            mask[j, i] = True

    ln_amp = np.where(mask, np.log(np.maximum(sum_amp, _LN_FLOOR)), np.log(_LN_FLOOR))
    for j in range(3):
        ln_amp[j] = fill_empty_bands(ln_amp[j], mask[j])

    return sum_amp, mask, ln_amp


def cell_band_coverage(mask: np.ndarray) -> np.ndarray:
    """Mel-band coverage (spec §4.1、ユーザー追加指示)。1 cell の mask (3,32) から、
    帯域ごとに「3 軸のどれか 1 本でも実データがあるか」を OR で畳んだ (32,) bool を返す。

    ★軸の畳み方は OR でなければならない — ランタイムの BuildModes 手順 4 は
    `a_i = Σ_j |k_j|·H(mask_j,i)·amp_j,i` と**軸を足す**ので、1 軸でも mask が立てば
    その帯域は実際に鳴る。AND や平均を取ると「実際に鳴る帯域」とこの指標がずれる。"""
    return np.any(mask, axis=0)
