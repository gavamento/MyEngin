#====================================================================================
#                          modal.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          一般化固有値問題 (K,M) の 2 経路の解法と品質指標
#====================================================================================
"""K x = lambda M x を解く 2 つの経路 (spec §4.1「固有値解法の 2 経路と品質指標」、
ユーザー追加指示 2026-09-16)。

- `solve_modes` (shift-invert eigsh、完全 LU): 占有 voxel 数が少ない (`MAX_OCCUPIED_EXACT`
  以下の) メッシュ用。正確だが、正則グリッド由来の hex8 メッシュは埋め込み (fill-in) が
  要素数の 2 乗超で増える (実測 54000 DOF で nnz(K)=4.0M → nnz(L+U)=230M = 57 倍)。
- `solve_modes_lobpcg` (不完全 LU 前処理の LOBPCG): 完全 LU が数 GB 級のメモリ・
  600 s 超を要求する大きいメッシュ (満杯 29^3 立方体 ≈ 81000 DOF 等) 用。

★**採否は solver 名 (`method`) ではなく数値 (残差) で決める** (ユーザー指示)。
`method` はメタデータとして残すが、モードを Σ|a| に入れるかどうかの判断には使わない。
判断材料は `compute_residuals()` が返す **固有対ごとの相対残差**
`r_i = ||K x̂_i - λ_i M x̂_i||₂ / ||λ_i M x̂_i||₂` (x̂ は M-正規化 `x̂ᵀMx̂=1`)。
残差は「その固有対が正確か」しか言わない — **モードの取りこぼし**は残差だけでは
検出できない (LOBPCG はブロック幅の都合でモードを丸ごと落としうる)。集合として
完全かどうかは基準形状での直接法との突き合わせでしか分からない (calibrate.py)。

しきい値 (暫定。基準形状の実測 (calibrate.py) で確定させる。planner が
12^3 ブロック 6591 DOF で実測: 直接法 max 1.6e-8 / median 1.5e-9、LOBPCG は
max 4.8e-6 / median 9.6e-8 で、周波数は相対 1e-11 で一致):
  - r <= RESIDUAL_ACCEPT (1e-5): そのまま採用
  - RESIDUAL_ACCEPT < r <= RESIDUAL_DROP (1e-3): 採用するが品質フラグ (marginal) を立てる
  - r > RESIDUAL_DROP: そのモードを Σ|a| から除外する (dataset.py が行う)

★縮退モードの罠: 対称形状 (立方体・球等) は固有値が縮退し (実測: 立方体で
10964 Hz が 2 重、14726 Hz が 3 重)、縮退した固有空間内では個々の固有ベクトルの
基底が任意なので、解法間で per-mode の値を直接比較してはいけない (基準形状の
突き合わせは周波数と帯域集計 Σ|a| で行う。calibrate.py 参照)。
"""
import warnings
from dataclasses import dataclass, field

import numpy as np
from scipy.sparse import diags
from scipy.sparse.linalg import ArpackNoConvergence, eigsh, spilu, LinearOperator, lobpcg

# 残差しきい値 (spec §4.1。暫定値、calibrate.py の実測で確定させる)
RESIDUAL_ACCEPT = 1e-5
RESIDUAL_DROP = 1e-3


@dataclass
class SolveResult:
    """固有値解法 1 回ぶんの結果 + solver metadata + 品質指標。

    freq       : ndarray (m,) Hz、昇順 (剛体除去 + 帯域フィルタ後)
    vecs       : ndarray (ndof,m) — freq と対応する生の固有ベクトル (質量正規化はしていない)
    raw        : ndarray (k_eff,) — 剛体除去前の全固有値 (昇順。テスト/診断用)
    residuals  : ndarray (m,) — freq と対応する固有対ごとの相対残差 (compute_residuals)
    method     : str — "exact" | "lobpcg" (メタデータ。採否の判断には使わない)
    params     : dict — solver パラメータ (k/shift または m/maxiter/drop_tol/fill_factor)
    iterations : int — 反復回数 (不明なら -1)
    converged  : bool — solver が正常収束したか
    """
    freq: np.ndarray
    vecs: np.ndarray
    raw: np.ndarray
    residuals: np.ndarray
    method: str
    params: dict = field(default_factory=dict)
    iterations: int = -1
    converged: bool = True


def pick_shift(K, M) -> float:
    """特異性回避用の微小シフト (負値)。K/M 対角成分の比から固有値のオーダーを見積もる。

    ★sigma=0 で厳密に殻を切ると (K - 0*M) = K は剛体 6 モードぶんちょうど特異
    (自由-自由境界条件の六面体メッシュは必ずランク落ち 6) になり、LU 分解が失敗する
    (スケールの問題ではなく構造的な特異性なので、E や rho をどう変えても解消しない)。
    回避として K/M のオーダーから決めたごく小さい負のシフトを使う"""
    k_scale = float(np.mean(K.diagonal()))
    m_scale = float(np.mean(M))
    if m_scale <= 0.0:
        return -1.0
    return -1e-8 * (k_scale / m_scale)


def compute_residuals(K, M, vals: np.ndarray, vecs: np.ndarray) -> np.ndarray:
    """固有対ごとの相対残差 (spec §4.1 の式)。M-正規化してから計算する
    (正規化を固定しないと解法間で値が比較できない)。vals<=0 (剛体モード相当) は
    分母が退化するため NaN を返す (呼び出し側は剛体除去後の配列だけを渡すこと)。"""
    n = vals.shape[0]
    residuals = np.full(n, np.nan)
    for i in range(n):
        lam = vals[i]
        if lam <= 0.0:
            continue
        x = vecs[:, i]
        mx = M * x
        norm2 = float(x @ mx)
        if norm2 <= 0.0:
            continue
        x_hat = x / np.sqrt(norm2)
        mx_hat = M * x_hat
        r = K @ x_hat - lam * mx_hat
        den = np.linalg.norm(lam * mx_hat)
        residuals[i] = np.linalg.norm(r) / den if den > 0.0 else np.inf
    return residuals


def diagnostics(result: SolveResult) -> dict:
    """予算 (modes_requested) と f_top・spectrum_complete (spec §4.1 の診断値。
    ★採否・重み付けには使わない — 「予算で頭打ちになったのか、そもそも帯域内に
    それ以上モードが無いのか」を切り分けるためだけの値)。dataset.py と
    calibrate.py の両方がここを呼ぶ (定義を 2 か所に持つと round 2 の calibrate.py
    のように solver ごとに違う予算を渡す食い違いを生みやすい)。

    mode_count (帯域フィルタ後、残差フィルタ前) が要求予算ぴったりなら、予算の外に
    まだモードがあるかもしれない (spectrum_complete=False)。予算未満で終わっていれば
    それ以上は f_max を超えたということなので帯域内は拾い切っている
    (spectrum_complete=True)。"""
    if result.method == "exact":
        requested = result.params["k"]
    else:
        requested = result.params["m"] - result.params["rigid_modes"]
    mode_count = int(result.freq.shape[0])
    f_top = float(np.max(result.freq)) if mode_count > 0 else None
    return {
        "modes_requested": int(requested),
        "f_top": f_top,
        "spectrum_complete": bool(mode_count < requested),
        "mode_count": mode_count,
    }


def solve_modes(K, M, k: int = 256, f_min: float = 100.0, f_max: float = 10000.0,
                 rigid_modes: int = 6, shift: float = None, seed: int = 0) -> SolveResult:
    """shift-invert eigsh (完全 LU)。占有 voxel 数が少ないメッシュ用 (正確、`MAX_OCCUPIED_EXACT`
    以下を想定)。

    ★決定論のための `v0` 固定 (sub-04 round 2 で判明した欠陥修正): `eigsh` は
    `v0` を省略すると ARPACK が乱数の初期ベクトルを使う。対称形状 (円柱・球・立方体等)
    は固有値が縮退するため、初期ベクトルが変わると**縮退部分空間内の基底が変わり**、
    同じ K/M (= 同じ占有ボクセル) でも `vecs` (ひいては `contact.py` が読む
    per-node 振幅、npz の `feat`) が実行のたびに変わってしまう (実測:
    `cylinder_0/3/5` はボクセル列がバイト一致するのに `feat` が最大 6.9 食い違った
    — round 1 で「実寸を FEM に渡していたせい」と誤診断していたが、実寸を
    参照サイズに直した後も同じ食い違いが残ったため、**別の独立したバグ**と判明した)。
    `solve_modes_lobpcg` は既に `seed` 引数で `v0` を固定しているので、対称性を
    合わせてここにも固定 `v0` を渡す。"""
    ndof = K.shape[0]
    M_sparse = diags(M) if M.ndim == 1 else M
    k_eff = min(k + rigid_modes, ndof - 1)
    params = {"k": k, "shift": None, "rigid_modes": rigid_modes, "k_eff": k_eff, "seed": seed}
    if k_eff < 1:
        return SolveResult(np.zeros(0), np.zeros((ndof, 0)), np.zeros(0), np.zeros(0),
                            "exact", params, iterations=0, converged=True)
    sigma = shift if shift is not None else pick_shift(K, M)
    params["shift"] = sigma
    v0 = np.random.default_rng(seed).standard_normal(ndof)

    converged = True
    try:
        vals, vecs = eigsh(K, k=k_eff, M=M_sparse, sigma=sigma, which='LM', v0=v0)
    except ArpackNoConvergence as exc:
        # 収束しなかった分は捨て、収束した固有対だけを使う (fixture 生成等の
        # 極小メッシュで k_eff が DOF に対して大きすぎるときに起こりうる)
        vals = exc.eigenvalues
        vecs = exc.eigenvectors
        converged = False

    order = np.argsort(vals)
    vals = vals[order]
    vecs = vecs[:, order]
    raw = vals.copy()

    vals = vals[rigid_modes:]
    vecs = vecs[:, rigid_modes:]
    vals = np.clip(vals, 0.0, None)
    freq = np.sqrt(vals) / (2.0 * np.pi)

    band = (freq >= f_min) & (freq <= f_max)
    freq_band = freq[band]
    vecs_band = vecs[:, band]
    vals_band = vals[band]
    residuals = compute_residuals(K, M, vals_band, vecs_band)

    return SolveResult(freq_band, vecs_band, raw, residuals, "exact", params,
                        iterations=-1, converged=converged)


def solve_modes_lobpcg(K, M, m: int = 40, maxiter: int = 60, f_min: float = 100.0,
                        f_max: float = 10000.0, rigid_modes: int = 6, seed: int = 0,
                        ilu_drop_tol: float = 1e-3, ilu_fill_factor: float = 8.0) -> SolveResult:
    """占有 voxel 数が非常に多いメッシュ (実測 満杯 29^3 立方体 ≈ 81000 DOF) 用の代替解法。

    ★実測した risk (sub-03 不安・質問 #1 参照): solve_modes() の shift-invert は
    SuperLU の完全 LU 分解を使うが、正則グリッド由来の hex8 メッシュは埋め込み
    (fill-in) が要素数の 2 乗超で増える (54000 DOF で nnz(K)=4.0M → nnz(L+U)=230M,
    57 倍)。81000 DOF (満杯 29^3 立方体) では完全 LU が数 GB 級のメモリと 600 s を
    大きく超える時間を要求し、実行環境 (空き RAM 数 GB 級) では危険と判断した。

    代替として不完全 LU (spilu、fill_factor で埋め込みを打ち切る = メモリが
    有界) を前処理行列にした LOBPCG (Locally Optimal Block PCG) を使う。

    ★npz には **exact と同じ扱いで書く** (method で分岐しない)。採否は
    `compute_residuals()` の値を dataset.py が `RESIDUAL_ACCEPT`/`RESIDUAL_DROP` と
    比べて決める。低次モードは shift-invert と 1e-8 相対で一致することを box_1
    (54000 DOF) で確認済みだが、高次 (帯域上限付近) モードは収束が甘い場合がある —
    それを検出するのがまさに残差なので、`method` を見て一律に信用/不信用を
    決めないこと (縮退モード・取りこぼしの検出は calibrate.py の基準形状比較を使う)。
    """
    ndof = K.shape[0]
    M_sparse = diags(M) if M.ndim == 1 else M
    sigma = pick_shift(K, M)
    A = (K - sigma * M_sparse).tocsc()
    ilu = spilu(A, drop_tol=ilu_drop_tol, fill_factor=ilu_fill_factor)
    precond = LinearOperator(A.shape, matvec=ilu.solve)

    rng = np.random.default_rng(seed)
    X = rng.standard_normal((ndof, m))
    params = {"m": m, "maxiter": maxiter, "drop_tol": ilu_drop_tol,
              "fill_factor": ilu_fill_factor, "rigid_modes": rigid_modes}

    lambda_history = []
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter("always")
        vals, vecs = lobpcg(K, X, B=M_sparse, M=precond, largest=False, tol=1e-6,
                             maxiter=maxiter, retLambdaHistory=False)
        not_converged = any("not reaching the requested tolerance" in str(w.message)
                             for w in caught)
    converged = not not_converged
    iterations = maxiter  # scipy は反復回数を直接返さない。上限を打った回数として記録する

    order = np.argsort(vals)
    vals = vals[order]
    vecs = vecs[:, order]
    raw = vals.copy()

    vals = vals[rigid_modes:]
    vecs = vecs[:, rigid_modes:]
    vals = np.clip(vals, 0.0, None)
    freq = np.sqrt(vals) / (2.0 * np.pi)
    band = (freq >= f_min) & (freq <= f_max)
    freq_band = freq[band]
    vecs_band = vecs[:, band]
    vals_band = vals[band]
    residuals = compute_residuals(K, M, vals_band, vecs_band)

    return SolveResult(freq_band, vecs_band, raw, residuals, "lobpcg", params,
                        iterations=iterations, converged=converged)
