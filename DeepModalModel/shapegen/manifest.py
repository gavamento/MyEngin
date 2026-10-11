#====================================================================================
#                          manifest.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          manifest.jsonl / progress.json / run_config.json の読み書き
#====================================================================================
"""manifest.jsonl は 1 行 1 形状 (受理も却下も)。append + fsync で書き、progress.json は
tmp → os.replace で原子的に更新する。クラッシュ時は manifest の末尾に progress より先の
行が残りうるので、再開時に `prune_after` で切り詰める。"""
import json
import os
from pathlib import Path


def append_rows(path, rows):
    path = Path(path)
    with open(path, "a", encoding="utf-8") as f:
        for row in rows:
            f.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
        f.flush()
        os.fsync(f.fileno())


def read_rows(path):
    path = Path(path)
    if not path.exists():
        return []
    rows = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                # 途中で落ちた最終行 (書きかけ) は捨てる
                continue
    return rows


def prune_after(path, next_index: int):
    """index >= next_index の行を落として書き直す。戻り値 = 落とした行。"""
    rows = read_rows(path)
    keep = [r for r in rows if int(r["index"]) < next_index]
    dropped = [r for r in rows if int(r["index"]) >= next_index]
    if dropped:
        tmp = Path(str(path) + ".tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            for r in keep:
                f.write(json.dumps(r, ensure_ascii=False, separators=(",", ":")) + "\n")
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    return dropped


def write_json_atomic(path, obj):
    path = Path(path)
    tmp = Path(str(path) + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(obj, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def read_json(path):
    path = Path(path)
    if not path.exists():
        return None
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)
