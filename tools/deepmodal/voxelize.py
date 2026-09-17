#====================================================================================
#                          voxelize.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          Editor.exe --modal-voxelize の subprocess ラッパ
#====================================================================================
"""Python は自前のボクセライザを持たない (spec §2 #8)。ここは
`Editor.exe --modal-voxelize --list F --out DIR` を呼ぶだけ。

Editor.exe は Windows サブシステム (GUI) の exe なので、`cmd /c` を挟んで待つ
(CLAUDE.md 環境の罠。挟まないと exit code も出力も取れない)。1 プロセスに大量の
FBX/glTF を読ませると RenderResources が肥大し続けるため、`batch_size` 行ごとに
プロセスを再起動する。
"""
import os
import subprocess
import sys
from pathlib import Path

DEFAULT_BATCH_SIZE = 200


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def find_editor_exe() -> Path:
    """`MYE_EDITOR_EXE` があればそれを使う。無ければ Release → Debug の順で探す
    (README の既定は Release。dogfooding セッションでは Debug しか無いこともある)。"""
    env = os.environ.get("MYE_EDITOR_EXE")
    if env:
        return Path(env)
    root = repo_root()
    release = root / "bin" / "x64" / "Release" / "Editor.exe"
    if release.exists():
        return release
    return root / "bin" / "x64" / "Debug" / "Editor.exe"


def run_voxelize_list(list_path: Path, out_dir: Path, editor_exe: Path = None) -> int:
    """1 回の Editor.exe 起動で list_path の全行を処理する。戻り値は exit code (0/1)。"""
    editor_exe = Path(editor_exe) if editor_exe else find_editor_exe()
    if not editor_exe.exists():
        print(f"[voxelize] ERROR: Editor.exe not found: {editor_exe}", file=sys.stderr)
        return 1
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    args = ["cmd", "/c", str(editor_exe), "--modal-voxelize",
            "--list", str(list_path), "--out", str(out_dir)]
    completed = subprocess.run(args, capture_output=True, text=True)
    if completed.stdout:
        sys.stdout.write(completed.stdout)
    if completed.stderr:
        sys.stderr.write(completed.stderr)
    return completed.returncode


def voxelize_batch(input_lines, out_dir, editor_exe: Path = None,
                    batch_size: int = DEFAULT_BATCH_SIZE, work_dir: Path = None) -> int:
    """input_lines (builtin://name / .off / .obj / .fbx / .gltf のパス文字列のリスト) を
    batch_size 件ごとに分割し、Editor.exe を batch_size 件ずつ再起動しながら処理する。
    戻り値: いずれかのバッチが失敗していれば 1、全部成功なら 0。"""
    out_dir = Path(out_dir)
    work_dir = Path(work_dir) if work_dir else out_dir
    work_dir.mkdir(parents=True, exist_ok=True)
    editor_exe = Path(editor_exe) if editor_exe else find_editor_exe()

    any_error = False
    for batch_idx, start in enumerate(range(0, len(input_lines), batch_size)):
        batch = input_lines[start:start + batch_size]
        list_path = work_dir / f"_voxelize_batch_{batch_idx:04d}.txt"
        list_path.write_text("\n".join(batch) + "\n", encoding="utf-8")
        rc = run_voxelize_list(list_path, out_dir, editor_exe)
        if rc != 0:
            any_error = True
    return 1 if any_error else 0
