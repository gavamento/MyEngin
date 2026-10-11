#====================================================================================
#                          generate.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          形状生成器の CLI (run / finalize / preview / stats / regen)
#====================================================================================
"""使い方は README.md。`run` は既存の --out があれば自動で再開する (設定不一致はエラー)。
multiprocessing (spawn) のため、import とパス設定はモジュール最上位で行う
(子プロセスが __mp_main__ としてこのファイルを再実行する)。"""
import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import shapegen  # noqa: E402  (tools/deepmodal を sys.path に足す)
from shapegen import constants as C  # noqa: E402
from shapegen import manifest, pipeline, stats as statsmod  # noqa: E402
from shapegen.families import registry  # noqa: E402


def _add_run_args(ap):
    ap.add_argument("--out", required=True)
    ap.add_argument("--target", type=int, default=0, help="受理したい件数 (run では必須)")
    ap.add_argument("--seed", type=int, default=20261011)
    ap.add_argument("--jobs", type=int, default=C.DEFAULT_JOBS)
    ap.add_argument("--editor-jobs", type=int, default=C.DEFAULT_EDITOR_JOBS)
    ap.add_argument("--chunk", type=int, default=C.CHUNK_INDICES)
    ap.add_argument("--batch", type=int, default=C.EDITOR_BATCH)
    ap.add_argument("--occ-min", type=int, default=C.OCC_MIN)
    ap.add_argument("--occ-max", type=int, default=C.OCC_MAX)
    ap.add_argument("--profile", choices=sorted(C.PROFILES.keys()), default="small")
    ap.add_argument("--rotation-mix", choices=sorted(C.ROTATION_MIX.keys()), default="default")
    ap.add_argument("--family", nargs="*", default=[], help="このファミリだけ (省略 = 全部)")
    ap.add_argument("--keep-rejected", action="store_true")
    ap.add_argument("--editor-exe", default="")
    ap.add_argument("--force", action="store_true", help="既存の --out を消して最初から")


def _cfg_from(args) -> pipeline.RunConfig:
    for name in args.family:
        if name not in registry.FAMILY_BY_NAME:
            raise SystemExit(f"unknown family: {name} (available: {', '.join(registry.FAMILY_BY_NAME)})")
    return pipeline.RunConfig(
        out=args.out, target=args.target, seed=args.seed, jobs=args.jobs,
        editor_jobs=args.editor_jobs, chunk=args.chunk, batch=args.batch,
        occ_min=args.occ_min, occ_max=args.occ_max, profile=args.profile,
        rotation_preset=args.rotation_mix, families=list(args.family),
        keep_rejected=args.keep_rejected, editor_exe=args.editor_exe)


def cmd_run(args):
    if args.target <= 0:
        raise SystemExit("run には --target (正の整数) が必要です")
    cfg = _cfg_from(args)
    accepted = pipeline.run(cfg, force=args.force)
    print(f"[run] done: accepted {accepted} (target {cfg.target}) -> {cfg.out}")


def cmd_preview(args):
    """1 ファミリの調整用: 小さく回して stats と qa を出す (run の薄い糖衣)。"""
    args.target = args.count
    args.out = args.out
    cfg = _cfg_from(args)
    cfg.chunk = max(args.count * 2, 16)
    pipeline.run(cfg, force=True)
    st = manifest.read_json(Path(args.out) / "stats.json")
    print(statsmod.format_stats(st))


def cmd_finalize(args):
    st = pipeline.finalize(Path(args.out), args.target)
    print(statsmod.format_stats(st))


def cmd_stats(args):
    rows = manifest.read_rows(Path(args.out) / "manifest.jsonl")
    cfg = manifest.read_json(Path(args.out) / "run_config.json") or {}
    st = statsmod.compute_stats(rows, int(cfg.get("target", 0)))
    print(statsmod.format_stats(st))


def cmd_regen(args):
    from shapegen.families.base import build_shape
    from shapegen.mesh import write_obj
    cfg = manifest.read_json(Path(args.out) / "run_config.json") or {}
    seed = int(cfg.get("seed", args.seed))
    res = build_shape(seed, args.index, registry.FAMILIES, cfg.get("profile", "small"),
                      cfg.get("rotation_preset", "default"),
                      only=set(cfg.get("families") or []) or None)
    print(f"index {args.index}: {res.name} status={res.status} reason={res.reason} rot={res.rot_mode}")
    if res.shape is not None:
        s = res.shape
        print(f"  L={s.L:.4f} h={s.h:.5f} min_feature_vox={s.min_feature_vox:.2f} est_occ={s.est_occ:.0f} "
              f"pieces={s.n_pieces} tris={s.n_tris} params={res.params}")
        if args.obj:
            write_obj(args.obj, s.mesh, [f"regen index={args.index} seed={seed}"])
            print(f"  wrote {args.obj}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="DeepModal 用の形状生成器")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_run = sub.add_parser("run", help="目標件数まで生成 (既存 --out は自動再開)")
    _add_run_args(p_run)
    p_run.set_defaults(fn=cmd_run)

    p_prev = sub.add_parser("preview", help="1 ファミリを小さく回して確認")
    _add_run_args(p_prev)
    p_prev.add_argument("--count", type=int, default=24)
    p_prev.set_defaults(fn=cmd_preview)

    p_fin = sub.add_parser("finalize", help="list.txt / stats.json / qa を作り直す")
    p_fin.add_argument("--out", required=True)
    p_fin.add_argument("--target", type=int, default=0)
    p_fin.set_defaults(fn=cmd_finalize)

    p_st = sub.add_parser("stats", help="統計を表示")
    p_st.add_argument("--out", required=True)
    p_st.set_defaults(fn=cmd_stats)

    p_re = sub.add_parser("regen", help="1 形状を再生成して OBJ を書く (デバッグ)")
    p_re.add_argument("--out", required=True)
    p_re.add_argument("--index", type=int, required=True)
    p_re.add_argument("--seed", type=int, default=20261011)
    p_re.add_argument("--obj", default="")
    p_re.set_defaults(fn=cmd_regen)

    args = ap.parse_args(argv)
    # preview は --target を持たないので補う
    if args.cmd == "preview":
        args.target = args.count
    args.fn(args)


if __name__ == "__main__":
    main()
