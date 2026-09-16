#====================================================================================
#                          test_dataset.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          dataset.py の --stage small (M76h) の単体テスト
#====================================================================================
"""Editor.exe (--modal-voxelize) や FEM/eigsh を実際に回さず、`--stage small` が
持つ「list ベース」の配線 (list 解析 / 既存 npz による再開 / --list 必須の CLI 検証) だけを
確認する。実地の焼き上げ (Editor.exe 経由) は `@pytest.mark.editor` の既存テスト
(test_layout.py) と、本サブの stage1 実行ログが担保する。"""
import json
import sys
from pathlib import Path

import numpy as np
import pytest

import dataset
import layout


def test_run_small_stage_resolves_and_filters_list_lines(tmp_path, monkeypatch):
    """コメント行 (#) と空行を無視し、builtin:// はそのまま、それ以外は絶対パスへ解決して
    _voxelize_missing (known_single_output=False) へ渡すことを確認する。FEM 段は
    _process_mvox_dir をモックして通過させるだけ (npz 生成は別途 pytest.mark.editor 相当の
    重いテストの領分ではないので、ここでは list → voxelize 呼び出しの配線だけを見る)。"""
    rel_obj = tmp_path / "shapes" / "thing.obj"
    rel_obj.parent.mkdir(parents=True)
    rel_obj.write_text("# dummy obj (中身は使わない)\n", encoding="utf-8")

    list_path = tmp_path / "list.txt"
    list_path.write_text(
        "# comment line\n"
        "\n"
        "builtin://cube\n"
        f"{rel_obj}\n",
        encoding="utf-8",
    )

    captured = {}

    def fake_voxelize_missing(lines, vox_dir, work_dir, known_single_output):
        captured["lines"] = lines
        captured["known_single_output"] = known_single_output

    def fake_process_mvox_dir(vox_dir, npz_dir, jobs, builtin_stems=frozenset(), k=dataset.DEFAULT_K):
        return []

    monkeypatch.setattr(dataset, "_voxelize_missing", fake_voxelize_missing)
    monkeypatch.setattr(dataset, "_process_mvox_dir", fake_process_mvox_dir)

    out_dir = tmp_path / "out"
    dataset.run_small_stage(list_path, out_dir, jobs=1)

    assert captured["known_single_output"] is False
    # コメント/空行は消え、builtin:// はそのまま、相対パスは絶対パスへ解決されている
    assert captured["lines"] == ["builtin://cube", str(rel_obj.resolve())]
    # stats.json は空リストでも書かれる (再開/集計の入れ物として常に存在する契約)
    stats = json.loads((out_dir / "stats.json").read_text(encoding="utf-8"))
    assert stats["count_total"] == 0


def test_stage_small_requires_list_argument(monkeypatch, tmp_path):
    """`--stage small` に `--list` を渡さないと argparse が exit する (ap.error)。"""
    monkeypatch.setattr(sys, "argv", ["dataset.py", "--stage", "small", "--out", str(tmp_path)])
    with pytest.raises(SystemExit):
        dataset.main()


def _write_fake_ok_npz(path: Path, name: str):
    """`_npz_stats_entry` が読む全キーを持つ最小の npz を作る (process_mesh の出力形式を
    模倣。実際の vox/feat の中身は再開経路のテストでは読まれないので省略)。"""
    np.savez_compressed(
        path,
        vox=np.zeros((32, 32, 32), dtype=np.uint8), valid=np.zeros((16, 16, 16), dtype=np.uint8),
        feat=np.zeros((0, layout.CHANNELS), dtype=np.float16),
        method=np.array("exact"), solver_params=np.array("{}"),
        iterations=np.array(1), converged=np.array(True),
        occupied_voxels=np.array(10), ndof=np.array(30),
        assemble_seconds=np.array(0.01), eigsh_seconds=np.array(0.02),
        mode_count=np.array(0), modes_requested=np.array(34), f_top=np.array(-1.0),
        spectrum_complete=np.array(True), valid_cells=np.array(0),
        coverage_ratio=np.array(0.0), coverage_high=np.array(0.0),
        band_coverage=np.zeros(layout.MEL_BANDS, dtype=np.float32),
        cell_coverage=np.zeros(0, dtype=np.float32),
        residuals=np.zeros(0, dtype=np.float32),
        residual_max=np.array(-1.0), residual_median=np.array(-1.0),
        dropped_mode_count=np.array(0), marginal_mode_count=np.array(0),
    )


def test_process_mvox_dir_resumes_from_existing_npz_without_reprocessing(tmp_path, monkeypatch):
    """既に npz があるメッシュは process_mesh を呼ばず、npz から stats を復元する
    (dataset.py 冒頭の docstring が警告する「round 1 のバグ」の回帰テスト、small ステージ
    でも primitives と同じ再開規則を共有していることの確認)。"""
    vox_dir = tmp_path / "vox"
    vox_dir.mkdir()
    npz_dir = tmp_path / "npz"
    npz_dir.mkdir()

    stem = "already_done#mesh0#prim0"
    (vox_dir / f"{stem}.mvox").write_bytes(b"\x00")  # 中身は読まれない (npz が先に見つかる)
    _write_fake_ok_npz(npz_dir / f"{stem}.npz", stem)

    called = {"count": 0}

    def fake_worker(args):
        called["count"] += 1
        raise AssertionError("既存 npz があるのに process_mesh が呼ばれた")

    monkeypatch.setattr(dataset, "_worker", fake_worker)

    results = dataset._process_mvox_dir(vox_dir, npz_dir, jobs=1)

    assert called["count"] == 0
    assert len(results) == 1
    assert results[0]["status"] == "ok"
    assert results[0]["name"] == stem
