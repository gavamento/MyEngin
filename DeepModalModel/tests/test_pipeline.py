#====================================================================================
#                          test_pipeline.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          スタブの voxelize_fn でラウンド進行・再開・重複・設定検査を固定
#====================================================================================
"""スタブは「直方体の占有を layout.MVOX_STRUCT_FMT で詰める」シリアライザであって
ボクセライザではない (OBJ を読まない)。配線 (manifest / progress / 再開 / dedup) だけを
検証する。Editor.exe を使う経路は test_editor_smoke.py。"""
import json
import struct
from pathlib import Path

import numpy as np
import pytest

from shapegen import constants as C  # shapegen が tools/deepmodal を sys.path に足す
from shapegen import manifest, pipeline, validate

import layout  # noqa: E402


def _write_fake_mvox(path: Path, occ: np.ndarray):
    """meshio.read_mvox が読める最小の .mvox (ヘッダ 72B + 32^3 B)。"""
    surface = int(occ.sum())
    header = struct.pack(
        layout.MVOX_STRUCT_FMT, layout.MVOX_MAGIC, layout.MVOX_VERSION, layout.VOXEL_N, 0,
        0.0, 0.0, 0.0, 1.0 / 28, -0.5, -0.5, -0.5, 0.5, 0.5, 0.5, 1.0, surface, 0, 0)
    # C++ の並びは x + 32*(y + 32*z) = z が最外 → (z,y,x) の順で書く
    body = np.ascontiguousarray(np.transpose(occ, (2, 1, 0))).astype(np.uint8).tobytes()
    path.write_bytes(header + body)


def _stub_voxelize(dup_every: int = 0):
    """index に応じた大きさの直方体を書く。dup_every > 0 なら index%dup_every==0 の形状を同一にする。"""
    def fn(obj_paths, out_dir, log_path):
        out_dir = Path(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        for p in obj_paths:
            name = Path(p).stem
            index = int(name.rsplit("_", 1)[1])
            occ = np.zeros((C.VOXEL_N,) * 3, dtype=np.uint8)
            if dup_every and index % dup_every == 0:
                occ[2:31, 10:20, 10:18] = 1
            else:
                # index ごとに一意な直方体 (幅・奥行・位置を独立に変える。重複排除で
                # 受理が止まると run が終わらない)
                w = 4 + (index % 9)
                d = 4 + ((index // 9) % 9)
                y0 = 3 + ((index // 81) % 10)
                occ[2:31, y0:y0 + w, 8:8 + d] = 1
            _write_fake_mvox(out_dir / f"{name}#mesh0#prim0.mvox", occ)
        Path(log_path).write_text("stub\n", encoding="utf-8")
        return 0
    return fn


def _cfg(out, target=30, chunk=20, families=("box_solid",)):
    return pipeline.RunConfig(out=str(out), target=target, seed=5, jobs=1, editor_jobs=1,
                              chunk=chunk, batch=7, families=list(families))


def test_run_produces_manifest_list_and_stats(tmp_path):
    cfg = _cfg(tmp_path / "run")
    accepted = pipeline.run(cfg, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False)
    assert accepted >= cfg.target
    out = tmp_path / "run"
    rows = manifest.read_rows(out / "manifest.jsonl")
    assert len(rows) % cfg.chunk == 0
    ok = [r for r in rows if r["status"] == "ok"]
    assert len(ok) == accepted
    for r in ok:
        assert (out / "obj" / f"{r['name']}.obj").exists()
        assert (out / "vox" / f"{r['name']}#mesh0#prim0.mvox").exists()
        assert r["components"] == 1 and r["occ"] >= C.OCC_MIN
    lines = [l for l in (out / "list.txt").read_text(encoding="utf-8").splitlines() if not l.startswith("#")]
    assert len(lines) == cfg.target and all(Path(l).is_absolute() for l in lines)
    stats = json.loads((out / "stats.json").read_text(encoding="utf-8"))
    assert stats["accepted"] == accepted and "box_solid" in stats["families"]
    assert (out / "qa" / "sheet_random.png").exists()
    assert not (out / "work").exists() or not any((out / "work").iterdir())
    progress = manifest.read_json(out / "progress.json")
    assert progress["next_index"] == len(rows)


def test_resume_matches_uninterrupted_run(tmp_path):
    a = _cfg(tmp_path / "a", target=40, chunk=15)
    pipeline.run(a, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False)
    b = _cfg(tmp_path / "b", target=40, chunk=15)
    with pytest.raises(KeyboardInterrupt):
        pipeline.run(b, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False,
                     abort_after_rounds=1)
    # 中断後にコミット途中の行を模擬して混ぜる (再開時に捨てられるはず)
    manifest.append_rows(tmp_path / "b" / "manifest.jsonl",
                         [{"index": 999, "name": "box_solid_000999", "status": "ok", "family": "box_solid",
                           "occ_hash": "x", "coarse_key": "y", "occ": 1}])
    pipeline.run(b, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False)
    ra = manifest.read_rows(tmp_path / "a" / "manifest.jsonl")
    rb = manifest.read_rows(tmp_path / "b" / "manifest.jsonl")
    keys = ("index", "name", "family", "status", "reason", "occ", "occ_hash", "coarse_key", "rot_mode")
    assert [tuple(r.get(k) for k in keys) for r in ra] == [tuple(r.get(k) for k in keys) for r in rb]
    assert not (tmp_path / "b" / "obj" / "box_solid_000999.obj").exists()


def test_dedup_rejects_duplicates_in_index_order(tmp_path):
    cfg = _cfg(tmp_path / "d", target=20, chunk=20)
    pipeline.run(cfg, voxelize_fn=_stub_voxelize(dup_every=2), log=lambda s: None, use_pool=False)
    rows = manifest.read_rows(tmp_path / "d" / "manifest.jsonl")
    dup_rows = [r for r in rows if r["index"] % 2 == 0 and r.get("family") == "box_solid"
                and r["status"] in ("ok", "rejected") and r.get("occ_hash")]
    oks = [r for r in dup_rows if r["status"] == "ok"]
    rej = [r for r in dup_rows if r["status"] == "rejected" and r["reason"] == "dup_exact"]
    assert len(oks) == 1 and oks[0]["index"] == min(r["index"] for r in dup_rows)
    assert len(rej) == len(dup_rows) - 1


def test_config_mismatch_is_refused_unless_force(tmp_path):
    cfg = _cfg(tmp_path / "c", target=10, chunk=10)
    pipeline.run(cfg, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False)
    other = _cfg(tmp_path / "c", target=10, chunk=10)
    other.seed = 6
    with pytest.raises(pipeline.ConfigMismatch):
        pipeline.run(other, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False)
    pipeline.run(other, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False, force=True)
    cfg_on_disk = manifest.read_json(tmp_path / "c" / "run_config.json")
    assert cfg_on_disk["seed"] == 6 and cfg_on_disk["family_set_version"] == C.FAMILY_SET_VERSION


def test_family_filter_only_builds_requested(tmp_path):
    cfg = _cfg(tmp_path / "f", target=10, chunk=10, families=("sword", "table"))
    pipeline.run(cfg, voxelize_fn=_stub_voxelize(), log=lambda s: None, use_pool=False)
    rows = manifest.read_rows(tmp_path / "f" / "manifest.jsonl")
    assert rows and set(r["family"] for r in rows) <= {"sword", "table"}


def test_fake_mvox_is_readable_by_meshio(tmp_path):
    import meshio
    occ = np.zeros((C.VOXEL_N,) * 3, dtype=np.uint8)
    occ[3, 7, 11] = 1
    _write_fake_mvox(tmp_path / "t.mvox", occ)
    grid = meshio.read_mvox(str(tmp_path / "t.mvox"))
    assert grid.occ[3, 7, 11] == 1 and int(grid.occ.sum()) == 1
