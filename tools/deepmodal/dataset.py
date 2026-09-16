#====================================================================================
#                          dataset.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          1 メッシュ -> npz のデータセット生成ドライバ
#====================================================================================
"""形状の列挙 → ボクセル化 (Editor.exe) → hex8 FEM 組み立て → 一般化固有値 →
接触励起 (16^3 cell) → Mel 圧縮 → npz、までを 1 メッシュぶんずつ行う。

★eigsh の時間リスク: 占有 voxel 数が非常に多いメッシュ (実測、満杯 29^3 立方体
≈ 81000 DOF) は shift-invert の完全 LU 分解が数 GB 級のメモリと長時間を要求する
(fill-in が要素数の 2 乗超で増える)。そのため **`MAX_OCCUPIED_EXACT` を超える
メッシュは `modal.solve_modes_lobpcg()` (不完全 LU 前処理の LOBPCG、メモリ有界) へ
回す** — ただし npz は exact と**同じ扱いで書く** (spec §4.1 ユーザー追加指示)。
採否は `method` ではなく `modal.compute_residuals()` の値で決める:
`r > modal.RESIDUAL_DROP` のモードは Σ|a| (= feat) から除外し、npz と stats.json の
両方に solver metadata (method/パラメータ/反復回数/収束フラグ) と convergence
quality (モードごとの残差 + 要約) を残す。

resumable: 出力 npz が既に存在するメッシュはスキップし、**npz 自身に埋め込んだ
統計スカラーを読み戻して stats.json の該当エントリを再構成する** (round 1 の
バグ: 再開時に `{"status":"cached"}` だけを積んで上書きし、既存の測定値が
`stats.json` から消えていた。npz が唯一の永続先になるようにして根本修正する)。
multiprocessing は 1 メッシュ 1 プロセスの Pool。OMP_NUM_THREADS 等は
Pool 生成前 (親プロセス) で 1 に固定してから spawn する。
"""
import argparse
import json
import multiprocessing
import os
import statistics
import time
from pathlib import Path

import numpy as np

import layout


# 実測 (box_1、15979 occ voxel = 54000 DOF) で shift-invert 単体が 300 s 級になった。
# primitives.py の寸法乱数はこの値を大きく下回る中央値になるよう調整済み (README 参照)。
# これを超えるメッシュ (満杯に近い立方体等) は完全 LU のメモリ要求が実行環境の
# 空き RAM を食い潰す恐れがあるため LOBPCG 経路へ回す
MAX_OCCUPIED_EXACT = 9000
DEFAULT_K = 150


def _cell_order():
    """16^3 cell を C++ の CellIndexOf と同じ順 (cx が最内) で列挙する。"""
    order = []
    for cz in range(layout.MAP_N):
        for cy in range(layout.MAP_N):
            for cx in range(layout.MAP_N):
                order.append((cx, cy, cz))
    return order


_CELL_ORDER = _cell_order()


def process_mesh(mvox_path: str, out_path: str, allow_exceed_cap: bool = False,
                  k: int = DEFAULT_K):
    """1 メッシュを処理する。npz を書けたら {"status": "ok", ...}、
    occupancy cap を超えて (allow_exceed_cap=False のとき) スキップしたら
    {"status": "skipped_occupancy_cap", ...}、例外時は {"status": "error", ...} を返す。
    stats 用の全フィールドを 1 つの dict にまとめる (multiprocessing 越しに pickle するため
    numpy 配列は含めない — 大きい配列は npz 側にだけ書く)。
    """
    import compact
    import contact
    import fem
    import meshio
    import modal

    name = Path(mvox_path).stem
    result = {"name": name, "mvox": str(mvox_path)}
    try:
        grid = meshio.read_mvox(mvox_path)
    except Exception as exc:  # noqa: BLE001 - データ生成ツールなので広く拾って記録する
        result.update(status="error", error=f"read_mvox: {exc}")
        return result

    occ_count = int(grid.occ.sum())
    result["occupied_voxels"] = occ_count

    if occ_count > MAX_OCCUPIED_EXACT and not allow_exceed_cap:
        result.update(status="skipped_occupancy_cap")
        return result

    t0 = time.time()
    try:
        K, M, node_coords, dofs, voxels = fem.assemble_from_occupancy(
            grid.occ, grid.voxel_size, layout.REF_YOUNG, layout.REF_DENSITY,
            layout.REF_POISSON)
    except Exception as exc:  # noqa: BLE001
        result.update(status="error", error=f"assemble: {exc}")
        return result
    t1 = time.time()
    result["ndof"] = int(K.shape[0])
    result["assemble_seconds"] = t1 - t0

    # ★占有数で「exact/lobpcg のどちらを使うか」は決めるが、採否 (モードを
    # Σ|a| に入れるかどうか) はここでは決めない — 残差 (下) だけで決める
    use_lobpcg = occ_count > MAX_OCCUPIED_EXACT
    t0 = time.time()
    try:
        if not use_lobpcg:
            solved = modal.solve_modes(K, M, k=k, f_min=layout.F_MIN, f_max=layout.F_MAX)
        else:
            solved = modal.solve_modes_lobpcg(K, M, f_min=layout.F_MIN, f_max=layout.F_MAX)
    except Exception as exc:  # noqa: BLE001
        result.update(status="error", error=f"solve_modes: {exc}")
        return result
    t1 = time.time()
    result["eigsh_seconds"] = t1 - t0
    mode_count_raw = int(solved.freq.shape[0])
    result["mode_count"] = mode_count_raw
    result["method"] = solved.method
    result["solver_params"] = solved.params
    result["iterations"] = solved.iterations
    result["converged"] = bool(solved.converged)

    # ---- 診断値 (spec §4.1。採否・重み付けには使わない — 記録専用) ----
    # 「予算で頭打ちになったのか、そもそも帯域内にそれ以上モードが無いのか」を
    # 切り分けるための値 (round 2 の calibrate.py が両解法に違う予算を渡していた
    # ことが原因の誤診断だったので、以後は modal.diagnostics() の 1 か所で計算する)
    diag = modal.diagnostics(solved)
    result["modes_requested"] = diag["modes_requested"]
    result["f_top"] = diag["f_top"]
    result["spectrum_complete"] = diag["spectrum_complete"]

    # ---- 残差による採否 (spec §4.1「固有値解法の 2 経路と品質指標」) ----
    # r > RESIDUAL_DROP のモードは Σ|a| (feat) から除外する。method では分岐しない。
    residuals = solved.residuals
    finite = np.isfinite(residuals)
    keep = finite & (residuals <= modal.RESIDUAL_DROP)
    marginal = finite & (residuals > modal.RESIDUAL_ACCEPT) & (residuals <= modal.RESIDUAL_DROP)
    dropped_count = int(np.sum(~keep))
    marginal_count = int(np.sum(marginal))
    result["residual_max"] = float(np.max(residuals[finite])) if np.any(finite) else None
    result["residual_median"] = float(np.median(residuals[finite])) if np.any(finite) else None
    result["dropped_mode_count"] = dropped_count
    result["marginal_mode_count"] = marginal_count

    freq = solved.freq[keep]
    modes = solved.vecs[:, keep]

    # ---- 16^3 cell ごとの接触励起 → Mel 圧縮 ----
    # method=="lobpcg" のときも exact と同じ扱いで npz を書く (builtin 6 種を含む、
    # 受け入れ条件 7 の要求)。信頼性は method ではなく上で適用した残差フィルタが担保する
    valid = np.zeros((layout.MAP_N, layout.MAP_N, layout.MAP_N), dtype=np.uint8)
    feats = []
    cell_coverage_list = []
    band_coverage_sum = np.zeros(layout.MEL_BANDS)
    for (cx, cy, cz) in _CELL_ORDER:
        node_id, amps = contact.excitation_for_cell(cx, cy, cz, voxels, dofs, node_coords,
                                                      freq, modes)
        if node_id is None:
            continue
        valid[cx, cy, cz] = 1
        _, mask, ln_amp = compact.compact_cell(freq, amps)
        feat = np.zeros(layout.CHANNELS, dtype=np.float32)
        for j in range(3):
            for i in range(layout.MEL_BANDS):
                feat[layout.mask_ch(j, i)] = 1.0 if mask[j, i] else 0.0
                feat[layout.amp_ch(j, i)] = ln_amp[j, i]
        feats.append(feat)
        # ---- Mel-band coverage (spec §4.1、ユーザー追加指示) ----
        # 軸の畳み方は OR (compact.cell_band_coverage の docstring 参照 — ランタイムの
        # BuildModes が軸を Σ で足すことと整合させるため)
        coverage_bool = compact.cell_band_coverage(mask)
        band_coverage_sum += coverage_bool.astype(np.float64)
        cell_coverage_list.append(float(np.mean(coverage_bool)))

    feat_arr = np.zeros((len(feats), layout.CHANNELS), dtype=np.float16)
    for idx, f in enumerate(feats):
        feat_arr[idx] = f.astype(np.float16)

    valid_cells = int(valid.sum())
    band_coverage = (band_coverage_sum / valid_cells) if valid_cells > 0 \
        else np.zeros(layout.MEL_BANDS)
    coverage_ratio = float(np.mean(band_coverage))
    coverage_high = float(np.mean(band_coverage[layout.MEL_BANDS - 8:]))
    cell_coverage_arr = np.array(cell_coverage_list, dtype=np.float32)

    result["valid_cells"] = valid_cells
    result["coverage_ratio"] = coverage_ratio
    result["coverage_high"] = coverage_high
    result["band_coverage"] = band_coverage.tolist()  # 32 個、stats.json の分布集計用

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    # npz にも solver metadata + convergence quality + coverage を残す (round 1 の指摘:
    # stats.json だけに置くと再開実行のたびに消える。npz が唯一の永続先 = 消えようがない)
    residuals_to_store = np.where(finite, residuals, -1.0).astype(np.float32)
    np.savez_compressed(
        out_path,
        vox=grid.occ.astype(np.uint8), valid=valid, feat=feat_arr,
        method=np.array(solved.method), solver_params=np.array(json.dumps(solved.params)),
        iterations=np.array(solved.iterations), converged=np.array(bool(solved.converged)),
        occupied_voxels=np.array(occ_count), ndof=np.array(int(K.shape[0])),
        assemble_seconds=np.array(result["assemble_seconds"]),
        eigsh_seconds=np.array(result["eigsh_seconds"]),
        mode_count=np.array(result["mode_count"]),
        modes_requested=np.array(result["modes_requested"]),
        f_top=np.array(result["f_top"] if result["f_top"] is not None else -1.0),
        spectrum_complete=np.array(result["spectrum_complete"]),
        valid_cells=np.array(valid_cells),
        coverage_ratio=np.array(coverage_ratio), coverage_high=np.array(coverage_high),
        band_coverage=band_coverage.astype(np.float32), cell_coverage=cell_coverage_arr,
        residuals=residuals_to_store,
        residual_max=np.array(result["residual_max"] if result["residual_max"] is not None
                               else -1.0),
        residual_median=np.array(result["residual_median"]
                                  if result["residual_median"] is not None else -1.0),
        dropped_mode_count=np.array(dropped_count), marginal_mode_count=np.array(marginal_count),
    )

    result.update(status="ok", npz=str(out_path))
    return result


def _worker(args):
    mvox_path, out_path, allow_exceed_cap, k = args
    return process_mesh(mvox_path, out_path, allow_exceed_cap=allow_exceed_cap, k=k)


def _builtin_list_path() -> Path:
    return Path(__file__).resolve().parents[2] / "tests" / "deepmodal" / "list_builtin.txt"


def _npz_stats_entry(name: str, npz_path: Path) -> dict:
    """既に npz があるメッシュ (再開でスキップした分) の stats エントリを、
    npz に埋め込んだスカラーから再構成する (round 1 で消えていた統計の恒久修正)。
    読めない/古い形式なら {"status": "cached_unreadable"} を返す (壊れていても
    全体の再開処理は止めない)。"""
    try:
        with np.load(npz_path, allow_pickle=False) as data:
            residual_max = float(data["residual_max"])
            residual_median = float(data["residual_median"])
            f_top = float(data["f_top"])
            return {
                "name": name,
                "status": "ok",
                "npz": str(npz_path),
                "method": str(data["method"]),
                "solver_params": json.loads(str(data["solver_params"])),
                "iterations": int(data["iterations"]),
                "converged": bool(data["converged"]),
                "occupied_voxels": int(data["occupied_voxels"]),
                "ndof": int(data["ndof"]),
                "assemble_seconds": float(data["assemble_seconds"]),
                "eigsh_seconds": float(data["eigsh_seconds"]),
                "mode_count": int(data["mode_count"]),
                "modes_requested": int(data["modes_requested"]),
                "f_top": f_top if f_top >= 0.0 else None,
                "spectrum_complete": bool(data["spectrum_complete"]),
                "valid_cells": int(data["valid_cells"]),
                "coverage_ratio": float(data["coverage_ratio"]),
                "coverage_high": float(data["coverage_high"]),
                "band_coverage": data["band_coverage"].tolist(),
                "residual_max": residual_max if residual_max >= 0.0 else None,
                "residual_median": residual_median if residual_median >= 0.0 else None,
                "dropped_mode_count": int(data["dropped_mode_count"]),
                "marginal_mode_count": int(data["marginal_mode_count"]),
            }
    except Exception as exc:  # noqa: BLE001 - 壊れた/旧形式の npz でも処理を止めない
        return {"name": name, "status": "cached_unreadable", "error": str(exc)}


def run_primitives_stage(out_dir: Path, jobs: int, seed: int, variants: int):
    import primitives
    import voxelize

    out_dir = Path(out_dir)
    obj_dir = out_dir / "raw"
    vox_dir = out_dir / "vox"
    npz_dir = out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    obj_dir.mkdir(parents=True, exist_ok=True)

    shapes = primitives.generate_primitive_set(seed=seed, variants_per_type=variants)
    for name, positions, indices in shapes:
        obj_path = obj_dir / f"{name}.obj"
        if not obj_path.exists():
            primitives.write_obj(obj_path, positions, indices)

    builtin_lines = [line.strip() for line in _builtin_list_path().read_text().splitlines()
                      if line.strip()]
    obj_lines = [str((obj_dir / f"{name}.obj").resolve()) for name, _, _ in shapes]
    all_lines = builtin_lines + obj_lines

    # 既に .mvox がある入力は再ボクセル化しない (再開可能性。stem#mesh0#prim0 の規則は
    # ModalTools.cpp と同じ)
    def stem_of(line: str) -> str:
        if line.startswith("builtin://"):
            return line[len("builtin://"):]
        return Path(line).stem

    missing = [line for line in all_lines
               if not (vox_dir / f"{stem_of(line)}#mesh0#prim0.mvox").exists()]
    if missing:
        rc = voxelize.voxelize_batch(missing, vox_dir, work_dir=out_dir / "work")
        if rc != 0:
            print("[dataset] WARNING: voxelize_batch reported errors (see above)")

    builtin_stems = {stem_of(line) + "#mesh0#prim0" for line in builtin_lines}

    mvox_files = sorted(vox_dir.glob("*.mvox"))
    tasks = []
    cached_results = []
    for mvox_path in mvox_files:
        stem = mvox_path.stem
        out_path = npz_dir / f"{stem}.npz"
        if out_path.exists():
            # ★round 1 の bug: ここで {"status":"cached"} だけを積むと npz に既に
            # 書いてある統計が stats.json から消える。npz を読み戻して復元する
            cached_results.append(_npz_stats_entry(stem, out_path))
            continue
        # builtin 6 種は受け入れ条件 7 が「npz ≥ 20 本 + builtin 6 本」を要求している
        # ため、occupancy cap を超えても LOBPCG 経路 (modal.solve_modes_lobpcg) へ
        # 回して npz を書く (cube は満杯 29^3 立方体 = 実測できる最大占有、これが
        # 「満杯 30^3 立方体の eigsh < 600 s」の実地プローブになる)
        allow = stem in builtin_stems
        tasks.append((str(mvox_path), str(out_path), allow, DEFAULT_K))

    results = list(cached_results)
    if tasks:
        os.environ.setdefault("OMP_NUM_THREADS", "1")
        os.environ.setdefault("OPENBLAS_NUM_THREADS", "1")
        os.environ.setdefault("MKL_NUM_THREADS", "1")
        if jobs <= 1:
            results.extend(_worker(t) for t in tasks)
        else:
            with multiprocessing.Pool(processes=jobs) as pool:
                results.extend(pool.map(_worker, tasks))

    write_stats(out_dir / "stats.json", results)
    ok_count = sum(1 for r in results if r.get("status") == "ok")
    print(f"[dataset] processed {len(results)} mesh(es), {ok_count} npz ready "
          f"({len(cached_results)} restored from cache), out={out_dir}")
    return results


def _mean_band_coverage(rows):
    """rows (band_coverage を持つ per_mesh レコードのリスト) から、メッシュ横断の
    帯域ごとの平均 coverage (32,) を求める (指摘3: 高域が系統的に空かを見る用)。"""
    arrs = [r["band_coverage"] for r in rows if r.get("band_coverage")]
    if not arrs:
        return None
    return np.mean(np.array(arrs), axis=0).tolist()


def write_stats(path: Path, results):
    ok = [r for r in results if r.get("status") == "ok"]
    # 中央値は exact 法だけで見る (受け入れ条件 7 の「exact 経路の中央値」)。
    # lobpcg 経路は cap を超えた少数の外れ値なので中央値には含めない
    exact_ok = [r for r in ok if r.get("method") == "exact"]
    lobpcg_ok = [r for r in ok if r.get("method") == "lobpcg"]
    eigsh_times = [r["eigsh_seconds"] for r in exact_ok if "eigsh_seconds" in r]
    mode_counts = [r["mode_count"] for r in ok if "mode_count" in r]
    coverage_ratios = [r["coverage_ratio"] for r in ok if "coverage_ratio" in r]
    coverage_highs = [r["coverage_high"] for r in ok if "coverage_high" in r]
    residual_maxes = [r["residual_max"] for r in ok if r.get("residual_max") is not None]
    dropped_total = sum(r.get("dropped_mode_count", 0) for r in ok)
    marginal_total = sum(r.get("marginal_mode_count", 0) for r in ok)
    spectrum_incomplete_total = sum(1 for r in ok if r.get("spectrum_complete") is False)

    stats = {
        "count_total": len(results),
        "count_ok": len(ok),
        "count_skipped_cap": sum(1 for r in results
                                  if r.get("status") == "skipped_occupancy_cap"),
        "count_error": sum(1 for r in results if r.get("status") == "error"),
        "eigsh_seconds": {
            "median": statistics.median(eigsh_times) if eigsh_times else None,
            "max": max(eigsh_times) if eigsh_times else None,
            "min": min(eigsh_times) if eigsh_times else None,
        },
        "mode_count": {
            "median": statistics.median(mode_counts) if mode_counts else None,
            "max": max(mode_counts) if mode_counts else None,
        },
        # ★診断値 (spec §4.1)。採否・重み付けには使わない — 予算で頭打ちになった
        # メッシュがどれだけあるかを可視化するだけ
        "spectrum": {
            "incomplete_count": spectrum_incomplete_total,
            "incomplete_ratio": (spectrum_incomplete_total / len(ok)) if ok else None,
        },
        # coverage_ratio は旧 band_occupancy_ratio と同じ役割を持つ「唯一の名前」
        # (round 2 の band_occupancy_ratio は定義そのものが OR ではなかったため置換した)
        "coverage": {
            "mean_ratio": (sum(coverage_ratios) / len(coverage_ratios))
                          if coverage_ratios else None,
            "mean_high": (sum(coverage_highs) / len(coverage_highs))
                         if coverage_highs else None,
            "band_distribution": _mean_band_coverage(ok),
            "band_distribution_by_method": {
                "exact": _mean_band_coverage(exact_ok),
                "lobpcg": _mean_band_coverage(lobpcg_ok),
            },
        },
        "residual": {
            "max_over_all_meshes": max(residual_maxes) if residual_maxes else None,
            "dropped_mode_count_total": dropped_total,
            "marginal_mode_count_total": marginal_total,
        },
        "full_occupancy_probe": [
            {"name": r["name"], "occupied_voxels": r.get("occupied_voxels"),
             "ndof": r.get("ndof"), "method": r.get("method"),
             "eigsh_seconds": r.get("eigsh_seconds"),
             "residual_max": r.get("residual_max"),
             "modes_requested": r.get("modes_requested"),
             "mode_count": r.get("mode_count"), "f_top": r.get("f_top"),
             "spectrum_complete": r.get("spectrum_complete")}
            for r in lobpcg_ok
        ],
        "per_mesh": results,
    }
    Path(path).write_text(json.dumps(stats, indent=2, ensure_ascii=False), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description="Deep-Modal データセット生成")
    ap.add_argument("--stage", required=True, choices=["primitives"],
                     help="modelnet10/40 は sub-08 の門を越えるまで実装しない (README 参照)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--seed", type=int, default=1234)
    ap.add_argument("--variants", type=int, default=6)
    args = ap.parse_args()

    if args.stage == "primitives":
        run_primitives_stage(Path(args.out), jobs=args.jobs, seed=args.seed,
                              variants=args.variants)


if __name__ == "__main__":
    main()
