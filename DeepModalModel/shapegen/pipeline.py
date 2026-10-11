#====================================================================================
#                          pipeline.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          ラウンド駆動の生成 → ボクセル化 → 検査 → コミット (再開可能)
#====================================================================================
"""1 ラウンド = CHUNK 個のインデックス。
  1. 生成 (multiprocessing.Pool): build_shape → 事前検査 → OBJ を work/round/obj へ
  2. ボクセル化: EDITOR_BATCH 件ごとに list 化し、ThreadPool で Editor.exe を並列起動
     (出力ディレクトリはバッチごとに分ける)
  3. 検査: インデックス昇順に validate (重複表の更新もこの順 = 並列度に依存しない)
  4. コミット: 受理分を obj/ と vox/ へ移し、manifest に全行を追記、progress を原子更新
形状 i は (seed, i) の純関数なので、中断・再開・jobs の違いで結果は変わらない。
`voxelize_fn` はテストでスタブに差し替えられる (Editor.exe を起動せずに配線を検証する)。
"""
import multiprocessing
import os
import shutil
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, asdict, field
from pathlib import Path

import numpy as np

from . import constants as C
from . import contact_sheet, editor, manifest, stats as statsmod, validate
from .families import registry
from .families.base import build_shape
from .mesh import write_obj

CONFIG_KEYS = ("seed", "occ_min", "occ_max", "profile", "rotation_preset", "families",
               "family_set_version", "coarse_dup_limit")
LIST_PARTS = 10  # finalize が list.txt を分割する数 (run_dataset.bat が順に流す)


@dataclass
class RunConfig:
    out: str
    target: int
    seed: int
    jobs: int = C.DEFAULT_JOBS
    editor_jobs: int = C.DEFAULT_EDITOR_JOBS
    chunk: int = C.CHUNK_INDICES
    batch: int = C.EDITOR_BATCH
    occ_min: int = C.OCC_MIN
    occ_max: int = C.OCC_MAX
    profile: str = "small"
    rotation_preset: str = "default"
    families: list = field(default_factory=list)  # 空 = 全部
    keep_rejected: bool = False
    editor_exe: str = ""
    family_set_version: str = C.FAMILY_SET_VERSION
    coarse_dup_limit: int = C.COARSE_DUP_LIMIT

    def to_dict(self):
        return asdict(self)

    def identity(self) -> dict:
        return {k: getattr(self, k) for k in CONFIG_KEYS}


class ConfigMismatch(Exception):
    pass


# ---- 生成 (worker) ----

def build_one(args):
    """Pool の worker。戻り値は numpy を含まない dict (pickle 向け)。"""
    global_seed, index, profile, rotation_preset, only, obj_dir = args
    res = build_shape(global_seed, index, registry.FAMILIES, profile, rotation_preset,
                      only=set(only) if only else None)
    row = {
        "index": index, "name": res.name, "family": res.family, "category": res.category,
        "rot_mode": res.rot_mode, "seed": res.seed, "attempts": res.attempts, "params": res.params,
    }
    if res.shape is not None:
        s = res.shape
        row.update(est_occ=round(float(s.est_occ), 1), n_pieces=s.n_pieces, n_tris=s.n_tris,
                   L=round(float(s.L), 5), min_feature_vox=round(float(s.min_feature_vox), 3))
    if res.status != "built":
        row.update(status="rejected", reason=res.reason)
        return row
    obj_path = Path(obj_dir) / f"{res.name}.obj"
    header = [f"family={res.family} index={index} seed={res.seed} rot={res.rot_mode}",
              f"params={res.params}"]
    write_obj(obj_path, res.shape.mesh, header)
    row.update(status="built", reason="", obj=str(obj_path))
    return row


def _generate_rows(cfg: RunConfig, indices, obj_dir: Path, pool):
    tasks = [(cfg.seed, i, cfg.profile, cfg.rotation_preset, tuple(cfg.families), str(obj_dir))
             for i in indices]
    if pool is None:
        rows = [build_one(t) for t in tasks]
    else:
        rows = list(pool.imap(build_one, tasks, chunksize=25))
    rows.sort(key=lambda r: r["index"])
    return rows


# ---- ボクセル化 ----

def _default_voxelize_fn(cfg: RunConfig):
    exe = editor.find_editor_exe(cfg.editor_exe or None)

    def fn(obj_paths, out_dir, log_path):
        return editor.voxelize_obj_files(obj_paths, out_dir, log_path, exe)
    return fn


def _voxelize_round(built_rows, work: Path, cfg: RunConfig, voxelize_fn):
    """built_rows を cfg.batch 件ずつ分け、並列にボクセル化。row に mvox パスを付ける。"""
    batches = [built_rows[i:i + cfg.batch] for i in range(0, len(built_rows), cfg.batch)]
    jobs = []
    for k, batch in enumerate(batches):
        vox_dir = work / f"vox_{k:03d}"
        log_path = work / f"voxelize_{k:03d}.log"
        jobs.append((batch, vox_dir, log_path))
        for r in batch:
            r["mvox"] = str(vox_dir / f"{r['name']}#mesh0#prim0.mvox")
            r["vox_log"] = str(log_path)
    rcs = []
    with ThreadPoolExecutor(max_workers=max(1, cfg.editor_jobs)) as ex:
        futures = [ex.submit(voxelize_fn, [r["obj"] for r in batch], vox_dir, log_path)
                   for batch, vox_dir, log_path in jobs]
        for f in futures:
            rcs.append(f.result())
    return rcs


# ---- 検査とコミット ----

def _validate_round(rows, cfg: RunConfig, dedup: validate.DedupTable):
    for r in rows:
        if r.get("status") != "built":
            continue
        v = validate.analyze_mvox(r["mvox"], cfg.occ_min, cfg.occ_max)
        r.update({k: val for k, val in v.to_dict().items() if k not in ("ok", "reason")})
        r["filament_ratio"] = round(float(r["filament_ratio"]), 4)
        r["max_component_ratio"] = round(float(r["max_component_ratio"]), 4)
        if not v.ok:
            r["status"], r["reason"] = "rejected", v.reason
            continue
        dup = dedup.check_and_add(v)
        if dup:
            r["status"], r["reason"] = "rejected", dup
            continue
        r["status"], r["reason"] = "ok", ""


def _commit_round(rows, out: Path, cfg: RunConfig):
    obj_dir = out / "obj"
    vox_dir = out / "vox"
    rej_dir = out / "rejected"
    obj_dir.mkdir(exist_ok=True)
    vox_dir.mkdir(exist_ok=True)
    for r in rows:
        obj = r.pop("obj", None)
        mvox = r.pop("mvox", None)
        r.pop("vox_log", None)
        if r["status"] == "ok":
            os.replace(obj, obj_dir / Path(obj).name)
            os.replace(mvox, vox_dir / Path(mvox).name)
        elif cfg.keep_rejected and obj and Path(obj).exists():
            rej_dir.mkdir(exist_ok=True)
            os.replace(obj, rej_dir / Path(obj).name)
            if mvox and Path(mvox).exists():
                os.replace(mvox, rej_dir / Path(mvox).name)


def _remove_shape_files(out: Path, name: str):
    for p in (out / "obj" / f"{name}.obj", out / "vox" / f"{name}#mesh0#prim0.mvox",
              out / "rejected" / f"{name}.obj", out / "rejected" / f"{name}#mesh0#prim0.mvox"):
        if p.exists():
            p.unlink()


# ---- 再開 ----

def _load_or_init(cfg: RunConfig, out: Path, force: bool, log):
    config_path = out / "run_config.json"
    progress_path = out / "progress.json"
    existing = manifest.read_json(config_path)
    if existing is not None and not force:
        mismatch = {k: (existing.get(k), cfg.identity()[k]) for k in CONFIG_KEYS
                    if existing.get(k) != cfg.identity()[k]}
        if mismatch:
            raise ConfigMismatch(f"run_config.json と設定が食い違います (--force で作り直し): {mismatch}")
        progress = manifest.read_json(progress_path) or {"next_index": 0, "accepted": 0}
        next_index = int(progress["next_index"])
        dropped = manifest.prune_after(out / "manifest.jsonl", next_index)
        for r in dropped:
            _remove_shape_files(out, r["name"])
        if (out / "work").exists():
            shutil.rmtree(out / "work")
        rows = manifest.read_rows(out / "manifest.jsonl")
        accepted = sum(1 for r in rows if r.get("status") == "ok")
        dedup = validate.DedupTable(cfg.coarse_dup_limit)
        for r in rows:
            if r.get("status") == "ok":
                dedup.add_accepted(r["occ_hash"], r["coarse_key"])
        log(f"[resume] next_index={next_index} accepted={accepted} (dropped {len(dropped)} uncommitted rows)")
        return next_index, accepted, dedup
    if existing is not None and force:
        for sub in ("obj", "vox", "rejected", "work", "qa"):
            if (out / sub).exists():
                shutil.rmtree(out / sub)
        for f in ("manifest.jsonl", "progress.json", "list.txt", "stats.json"):
            if (out / f).exists():
                (out / f).unlink()
    exe = editor.find_editor_exe(cfg.editor_exe or None)
    import numpy, scipy, sys
    record = dict(cfg.to_dict())
    record.update(editor=editor.editor_fingerprint(exe), python=sys.version.split()[0],
                  numpy=numpy.__version__, scipy=scipy.__version__,
                  created=time.strftime("%Y-%m-%d %H:%M:%S"))
    manifest.write_json_atomic(config_path, record)
    manifest.write_json_atomic(progress_path, {"next_index": 0, "accepted": 0})
    return 0, 0, validate.DedupTable(cfg.coarse_dup_limit)


# ---- 本体 ----

def run(cfg: RunConfig, voxelize_fn=None, force: bool = False, log=print, use_pool: bool = True,
        abort_after_rounds: int = 0):
    """目標件数まで回す。戻り値 = 受理数。abort_after_rounds はテスト用 (中断の再現)。"""
    out = Path(cfg.out)
    out.mkdir(parents=True, exist_ok=True)
    next_index, accepted, dedup = _load_or_init(cfg, out, force, log)
    if voxelize_fn is None:
        voxelize_fn = _default_voxelize_fn(cfg)
    t_start = time.time()
    pool = None
    if use_pool and cfg.jobs > 1:
        ctx = multiprocessing.get_context("spawn")
        pool = ctx.Pool(processes=cfg.jobs)
    rounds = 0
    empty_rounds = 0
    try:
        while accepted < cfg.target:
            if empty_rounds >= C.MAX_EMPTY_ROUNDS:
                raise RuntimeError(f"受理 0 のラウンドが {empty_rounds} 回続いたので中断します "
                                   f"(重複排除・occ 範囲・ファミリ指定を確認)")
            rounds += 1
            t0 = time.time()
            indices = list(range(next_index, next_index + cfg.chunk))
            work = out / "work" / f"round_{next_index:08d}"
            obj_dir = work / "obj"
            obj_dir.mkdir(parents=True, exist_ok=True)
            rows = _generate_rows(cfg, indices, obj_dir, pool)
            t1 = time.time()
            built = [r for r in rows if r["status"] == "built"]
            _voxelize_round(built, work, cfg, voxelize_fn)
            t2 = time.time()
            _validate_round(rows, cfg, dedup)
            t3 = time.time()
            # 目標を超えた分も受理する (丸ごとラウンド単位で確定。切り詰めは finalize の list が target 件に絞る)
            _commit_round(rows, out, cfg)
            manifest.append_rows(out / "manifest.jsonl", rows)
            n_ok = sum(1 for r in rows if r["status"] == "ok")
            accepted += n_ok
            empty_rounds = 0 if n_ok > 0 else empty_rounds + 1
            next_index += cfg.chunk
            manifest.write_json_atomic(out / "progress.json",
                                       {"next_index": next_index, "accepted": accepted})
            shutil.rmtree(work, ignore_errors=True)
            reasons = {}
            for r in rows:
                if r["status"] != "ok":
                    reasons[r["reason"]] = reasons.get(r["reason"], 0) + 1
            occs = [r["occ"] for r in rows if r["status"] == "ok"]
            occ_med = float(np.median(occs)) if occs else 0.0
            elapsed = time.time() - t_start
            rate = accepted / elapsed if elapsed > 0 else 0.0
            eta = (cfg.target - accepted) / rate if rate > 0 and accepted < cfg.target else 0.0
            top = ", ".join(f"{k} {v / len(rows):.1%}" for k, v in sorted(reasons.items(), key=lambda kv: -kv[1])[:5])
            log(f"[round {rounds}] idx {indices[0]}-{indices[-1]} ok {n_ok}/{len(rows)} "
                f"accepted {accepted}/{cfg.target} occ_med {occ_med:.0f} | {top} | "
                f"gen {t1 - t0:.0f}s vox {t2 - t1:.0f}s chk {t3 - t2:.0f}s | elapsed {elapsed / 60:.1f}m eta {eta / 60:.1f}m")
            if abort_after_rounds and rounds >= abort_after_rounds:
                raise KeyboardInterrupt("abort_after_rounds")
    finally:
        if pool is not None:
            pool.close()
            pool.join()
    finalize(out, cfg.target, log)
    return accepted


def finalize(out, target: int = 0, log=print, qa_samples: int = 96, qa_seed: int = 7):
    """manifest から list.txt / stats.json / qa/ を作り直す。list は index 昇順で target 件まで
    (target=0 なら全部)。"""
    out = Path(out)
    rows = manifest.read_rows(out / "manifest.jsonl")
    ok = [r for r in rows if r.get("status") == "ok"]
    ok.sort(key=lambda r: r["index"])
    cfg = manifest.read_json(out / "run_config.json") or {}
    if target <= 0:
        target = int(cfg.get("target", 0)) or len(ok)
    listed = ok[:target] if target else ok
    lines = ["# DeepModalModel/shapegen が生成 (generate.py finalize)。1 行 1 OBJ (絶対パス)。",
             f"# accepted {len(ok)} / listed {len(listed)} / seed {cfg.get('seed')} / "
             f"family_set_version {cfg.get('family_set_version')}"]
    paths = [str((out / "obj" / f"{r['name']}.obj").resolve()) for r in listed]
    (out / "list.txt").write_text("\n".join(lines + paths) + "\n", encoding="utf-8")
    # FEM (dataset.py) は完走時にしか stats.json を書かないので、list を LIST_PARTS 分割して
    # 順に流せるようにする (5k ごとに統計が得られ、早期に学習実験へ回せる)
    per = (len(paths) + LIST_PARTS - 1) // LIST_PARTS if paths else 1
    for k in range(LIST_PARTS):
        part = paths[k * per:(k + 1) * per]
        (out / f"list_part{k:02d}.txt").write_text(
            "\n".join([f"# part {k}/{LIST_PARTS} of list.txt ({len(part)} lines)"] + part) + "\n",
            encoding="utf-8")
    st = statsmod.compute_stats(rows, target)
    manifest.write_json_atomic(out / "stats.json", st)
    _write_qa(out, ok, qa_samples, qa_seed, log)
    log(f"[finalize] list.txt {len(listed)} lines, stats.json, qa/ written to {out}")
    return st


def _write_qa(out: Path, ok_rows, qa_samples: int, qa_seed: int, log):
    import meshio
    qa = out / "qa"
    qa.mkdir(exist_ok=True)
    if not ok_rows:
        return
    rng = np.random.default_rng(qa_seed)  # 表示用の抽出だけ (生成の決定性には無関係)
    pick = rng.choice(len(ok_rows), size=min(qa_samples, len(ok_rows)), replace=False)
    sample = [ok_rows[i] for i in sorted(pick)]

    def occ_of(r):
        p = out / "vox" / f"{r['name']}#mesh0#prim0.mvox"
        return meshio.read_mvox(str(p)).occ

    contact_sheet.write_sheet(qa / "sheet_random.png", [occ_of(r) for r in sample],
                              [r["name"] for r in sample])
    by_family = {}
    for r in ok_rows:
        by_family.setdefault(r["family"], []).append(r)
    for fam, rows in sorted(by_family.items()):
        sub = rows[:24]
        contact_sheet.write_sheet(qa / f"sheet_{fam}.png", [occ_of(r) for r in sub],
                                  [r["name"] for r in sub])
