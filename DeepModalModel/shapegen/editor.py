#====================================================================================
#                          editor.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          Editor.exe --modal-voxelize の起動ラッパ (並列・ログ捕捉)
#====================================================================================
"""tools/deepmodal/voxelize.py と同じ `cmd /c` 手順 (Editor.exe は GUI サブシステムなので
挟まないと exit code も出力も取れない)。違いは stdout/stderr をラウンド別のログファイルへ
落とすこと (複数プロセスを同時起動したとき端末に混ざらないように)。
OBJ 経路は RenderResources を使わず完全ヘッドレス (ModalTools.cpp) なので、`--list` と
`--out` を分ければ同時起動できる。"""
import os
import subprocess
from pathlib import Path

import voxelize


def find_editor_exe(override=None) -> Path:
    if override:
        return Path(override)
    return voxelize.find_editor_exe()


def editor_fingerprint(exe: Path) -> dict:
    """ボクセライザのバイナリが変わると .mvox が変わりうるので run_config に残す。"""
    exe = Path(exe)
    if not exe.exists():
        return {"path": str(exe), "size": 0, "mtime": 0}
    st = exe.stat()
    return {"path": str(exe), "size": int(st.st_size), "mtime": int(st.st_mtime)}


def run_voxelize(editor_exe: Path, list_path: Path, out_dir: Path, log_path: Path) -> int:
    """1 回の Editor.exe 起動で list_path の全行を out_dir へボクセル化する。戻り値 = exit code。"""
    editor_exe = Path(editor_exe)
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    if not editor_exe.exists():
        Path(log_path).write_text(f"ERROR: Editor.exe not found: {editor_exe}\n", encoding="utf-8")
        return 1
    args = ["cmd", "/c", str(editor_exe), "--modal-voxelize", "--list", str(list_path),
            "--out", str(out_dir)]
    completed = subprocess.run(args, capture_output=True, text=True, encoding="utf-8",
                               errors="replace")
    with open(log_path, "w", encoding="utf-8") as f:
        f.write(f"$ {' '.join(args)}\n")
        f.write(f"exit code: {completed.returncode}\n")
        if completed.stdout:
            f.write(completed.stdout)
        if completed.stderr:
            f.write(completed.stderr)
    return completed.returncode


def voxelize_obj_files(obj_paths, out_dir: Path, log_path: Path, editor_exe: Path) -> int:
    """pipeline が注入する既定の voxelize_fn。list を書いて run_voxelize を呼ぶだけ。"""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    list_path = out_dir / "_list.txt"
    list_path.write_text("\n".join(str(Path(p).resolve()) for p in obj_paths) + "\n", encoding="utf-8")
    return run_voxelize(editor_exe, list_path, out_dir, log_path)
