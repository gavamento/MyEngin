#====================================================================================
#                          calibrate.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          基準形状での直接法 vs LOBPCG の校正ジョブ
#====================================================================================
"""spec §5 #21「基準形状の校正」。直接法 (shift-invert eigsh) が回せる基準形状で
`solve_modes` と `solve_modes_lobpcg` を**両方**走らせ、`calibration.json` に

  (a) 各解法のモードごと残差の分布
  (b) 周波数の突き合わせ (昇順で最も近いものを貪欲対応付け、相対誤差)
  (c) 取りこぼしたモード数 (直接法にあって LOBPCG に対応が無い周波数)
  (d) 帯域集計 Σ|a| の相対差 (代表 cell 1 点。生の固有ベクトルは比較しない —
      対称形状は固有値が縮退し、縮退空間内の基底は解法間で一致しない)

を書く。この実測から `modal.RESIDUAL_ACCEPT` / `RESIDUAL_DROP` の暫定値が
妥当かを判断する。

★round 2 の欠陥 (planner が発見): (b)(c) の比較で直接法に `k=150`、LOBPCG に
既定の `m=40` (非剛体 34 本) を渡していた — **違う予算同士を比べて
「取りこぼし」と誤診断した** (capsule/sphere という規模の違う 2 形状で
missed=116 が完全一致 = 収束の質ではなく予算差の証拠)。
このファイルは **`budget_matched`** (両解法に同じ非剛体モード数を要求した比較。
本当の意味での取りこぼし/精度差はここで見る) と **`direct_reference_large_k`**
(直接法だけ大きい k で回し、`spectrum_complete` で「そもそも帯域内にあと何本
あるか分からない」ことを可視化する診断) を分けて出す。

対象形状: builtin capsule (cap 未満、直接法が速い) と builtin sphere (cap 超、
LOBPCG 経路の実地サンプル)。既定では capsule だけを回す — `--full` で sphere も追加する。
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np

import compact
import contact
import fem
import layout
import meshio
import modal
import voxelize


def match_frequencies(freq_a: np.ndarray, freq_b: np.ndarray, rel_tol: float = 0.05):
    """freq_a (基準側) の各周波数に freq_b の最も近い要素を貪欲に対応付ける
    (Hungarian 法ほど厳密ではないが、モード数が数十~百程度の診断には十分)。
    戻り値: matches [(i, j, rel_err)], missed_a_indices (freq_b に対応が無い freq_a の添字)。
    """
    used_b = set()
    matches = []
    missed = []
    for i, fa in enumerate(freq_a):
        best_j = -1
        best_err = None
        for j, fb in enumerate(freq_b):
            if j in used_b:
                continue
            err = abs(fa - fb) / max(fa, 1e-9)
            if best_err is None or err < best_err:
                best_err = err
                best_j = j
        if best_j >= 0 and best_err <= rel_tol:
            matches.append((i, best_j, best_err))
            used_b.add(best_j)
        else:
            missed.append(i)
    return matches, missed


def _pick_representative_cell(voxels, dofs, node_coords):
    """代表 cell (帯域集計比較用)。中心付近から順に試し、最初に見つかった有効 cell を使う。"""
    center = layout.MAP_N // 2
    candidates = [(center, center, center)] + [
        (cx, cy, cz)
        for cx in range(layout.MAP_N) for cy in range(layout.MAP_N) for cz in range(layout.MAP_N)
    ]
    for cx, cy, cz in candidates:
        if contact.pick_contact_node(cx, cy, cz, voxels, dofs, node_coords) is not None:
            return (cx, cy, cz)
    return None


def _residual_summary(result) -> dict:
    finite = np.isfinite(result.residuals)
    vals = result.residuals[finite]
    diag = modal.diagnostics(result)
    return {
        "mode_count": diag["mode_count"],
        "modes_requested": diag["modes_requested"],
        "f_top": diag["f_top"],
        "spectrum_complete": diag["spectrum_complete"],
        "residual_max": float(np.max(vals)) if vals.size else None,
        "residual_median": float(np.median(vals)) if vals.size else None,
        "method": result.method,
        "params": result.params,
        "converged": bool(result.converged),
    }


def _band_aggregate_diff(voxels, dofs, node_coords, a, b) -> dict:
    """代表 cell 1 点で Σ|a| (帯域集計) の相対差を見る。生の固有ベクトルは比較しない
    (縮退モードの罠。§4.1 参照)。"""
    cell = _pick_representative_cell(voxels, dofs, node_coords)
    out = {"cell": cell, "mean_rel_diff": None, "max_rel_diff": None}
    if cell is None:
        return out
    _, amps_a = contact.excitation_for_cell(*cell, voxels, dofs, node_coords, a.freq, a.vecs)
    _, amps_b = contact.excitation_for_cell(*cell, voxels, dofs, node_coords, b.freq, b.vecs)
    sum_a, _, _ = compact.compact_cell(a.freq, amps_a)
    sum_b, _, _ = compact.compact_cell(b.freq, amps_b)
    denom = np.maximum(sum_a, sum_b)
    significant = denom > (np.max(denom) * 1e-3 if np.max(denom) > 0 else 0.0)
    if np.any(significant):
        rel = np.abs(sum_a[significant] - sum_b[significant]) / denom[significant]
        out["mean_rel_diff"] = float(np.mean(rel))
        out["max_rel_diff"] = float(np.max(rel))
    return out


def calibrate_shape(name: str, mvox_path: Path, budget: int = 34,
                     reference_k: int = 150) -> dict:
    """budget: 両解法に**同じ**要求する非剛体モード数 (「本当の取りこぼし」を見るため
    予算を揃える — round 2 の誤診断の直接の修正)。reference_k: 直接法だけを大きい
    予算で回し、`spectrum_complete` の診断に使う (LOBPCG は同じ予算だと時間がかかり
    すぎるため、直接法側だけの参考値)。"""
    grid = meshio.read_mvox(mvox_path)
    occ_count = int(grid.occ.sum())
    K, M, node_coords, dofs, voxels = fem.assemble_from_occupancy(
        grid.occ, grid.voxel_size, layout.REF_YOUNG, layout.REF_DENSITY, layout.REF_POISSON)
    rigid_modes = 6

    # ---- 予算を揃えた比較 (本当の意味での取りこぼし/精度差はここで見る) ----
    direct_matched = modal.solve_modes(K, M, k=budget, f_min=layout.F_MIN, f_max=layout.F_MAX)
    lob_matched = modal.solve_modes_lobpcg(K, M, m=budget + rigid_modes,
                                            f_min=layout.F_MIN, f_max=layout.F_MAX)
    matches, missed = match_frequencies(direct_matched.freq, lob_matched.freq)
    rel_errs = [m[2] for m in matches]
    budget_matched = {
        "requested_non_rigid_modes": budget,
        "direct": _residual_summary(direct_matched),
        "lobpcg": _residual_summary(lob_matched),
        "frequency_match": {
            "matched_count": len(matches),
            "missed_count": len(missed),
            "rel_err_max": max(rel_errs) if rel_errs else None,
            "rel_err_median": float(np.median(rel_errs)) if rel_errs else None,
        },
        "band_aggregate_diff": _band_aggregate_diff(voxels, dofs, node_coords,
                                                      direct_matched, lob_matched),
    }

    # ---- 直接法だけ大きい予算で回す参考値 (spectrum_complete の診断) ----
    direct_large = modal.solve_modes(K, M, k=reference_k, f_min=layout.F_MIN, f_max=layout.F_MAX)
    direct_reference_large_k = _residual_summary(direct_large)

    return {
        "name": name,
        "occupied_voxels": occ_count,
        "ndof": int(K.shape[0]),
        "budget_matched": budget_matched,
        "direct_reference_large_k": direct_reference_large_k,
    }


def main():
    ap = argparse.ArgumentParser(description="基準形状での直接法 vs LOBPCG の校正")
    ap.add_argument("--out", default="data/calibration.json",
                     help="既定は data/ 配下 (gitignore 対象。生成物なので版管理しない)")
    ap.add_argument("--full", action="store_true",
                     help="cap 超の基準形状 (builtin sphere) も追加する (数分かかる)")
    ap.add_argument("--budget", type=int, default=34,
                     help="両解法に要求する非剛体モード数 (既定は dataset.py の LOBPCG "
                          "既定 m=40 に合わせて 34 = 40-6)")
    ap.add_argument("--reference-k", type=int, default=150, help="直接法だけの参考予算")
    ap.add_argument("--work", default=None, help="ボクセル化の作業ディレクトリ (既定は --out の隣)")
    args = ap.parse_args()

    out_path = Path(args.out)
    work_dir = Path(args.work) if args.work else out_path.parent / "_calibrate_work"
    work_dir.mkdir(parents=True, exist_ok=True)

    shape_names = ["capsule"] + (["sphere"] if args.full else [])
    lines = [f"builtin://{n}" for n in shape_names]
    rc = voxelize.voxelize_batch(lines, work_dir)
    if rc != 0:
        print("[calibrate] WARNING: voxelize_batch reported errors", file=sys.stderr)

    results = []
    for name in shape_names:
        mvox_path = work_dir / f"{name}#mesh0#prim0.mvox"
        if not mvox_path.exists():
            print(f"[calibrate] ERROR: missing {mvox_path}", file=sys.stderr)
            continue
        print(f"[calibrate] {name} ...", flush=True)
        results.append(calibrate_shape(name, mvox_path, budget=args.budget,
                                        reference_k=args.reference_k))

    assessment = {
        "RESIDUAL_ACCEPT": modal.RESIDUAL_ACCEPT,
        "RESIDUAL_DROP": modal.RESIDUAL_DROP,
        "direct_residual_max_observed": max(
            (r["budget_matched"]["direct"]["residual_max"] for r in results
             if r["budget_matched"]["direct"]["residual_max"]),
            default=None),
        "lobpcg_residual_max_observed": max(
            (r["budget_matched"]["lobpcg"]["residual_max"] for r in results
             if r["budget_matched"]["lobpcg"]["residual_max"]),
            default=None),
    }
    doc = {"shapes": results, "threshold_assessment": assessment}
    out_path.write_text(json.dumps(doc, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"[calibrate] wrote {out_path}")


if __name__ == "__main__":
    main()
