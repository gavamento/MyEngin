#====================================================================================
#                          test_editor_smoke.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          実際の Editor.exe --modal-voxelize を通す不変量 (無ければ skip)
#====================================================================================
"""Python 側に第 2 のボクセライザを書かない (spec §2 #8) ので、占有に関する不変量は
実物を起動して固定する。1 回の起動で全ファミリ × 2 seed をまとめて処理する。"""
import shutil
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
import pytest

from shapegen import constants as C  # shapegen が tools/deepmodal を sys.path に足す
from shapegen import editor, validate

import meshio  # noqa: E402
from shapegen.families import registry
from shapegen.families.base import build_shape
from shapegen.mesh import write_obj

pytestmark = pytest.mark.editor


@pytest.fixture(scope="module")
def editor_exe():
    exe = editor.find_editor_exe()
    if not exe.exists():
        pytest.skip(f"Editor.exe not found: {exe}")
    return exe


@pytest.fixture(scope="module")
def voxelized(editor_exe, tmp_path_factory):
    """全ファミリ × seed 0,1 を OBJ → .mvox にして {name: (BuildResult, Verdict)} を返す。"""
    root = tmp_path_factory.mktemp("smoke")
    obj_dir = root / "obj"
    obj_dir.mkdir()
    results = {}
    paths = []
    for fam in registry.FAMILIES:
        for i in (0, 1):
            res = build_shape(777, i, registry.FAMILIES, "small", "aligned", only={fam.name})
            if res.status != "built":
                continue
            p = obj_dir / f"{res.name}.obj"
            write_obj(p, res.shape.mesh, [f"family={fam.name}"])
            results[res.name] = res
            paths.append(p)
    rc = editor.voxelize_obj_files(paths, root / "vox", root / "vox.log", editor_exe)
    assert rc == 0, (root / "vox.log").read_text(encoding="utf-8")
    out = {}
    for name, res in results.items():
        mv = root / "vox" / f"{name}#mesh0#prim0.mvox"
        assert mv.exists(), name
        out[name] = (res, validate.analyze_mvox(mv, C.OCC_MIN, 10 ** 9), mv)
    return out


def test_acceptance_rate_and_connectivity(voxelized):
    reasons = {}
    for name, (res, v, _) in voxelized.items():
        if not v.ok:
            reasons.setdefault(v.reason, []).append(name)
    disconnected = reasons.get("disconnected", [])
    assert len(disconnected) <= max(1, len(voxelized) // 50), f"disconnected: {disconnected}"
    ok = sum(1 for _, (_, v, _) in voxelized.items() if v.ok)
    assert ok / len(voxelized) >= 0.85, reasons


def test_hollow_families_have_little_interior(voxelized):
    """中空容器は interior が占有の半分未満 (閉じた殻なら大半が interior になる)。"""
    for name, (res, v, _) in voxelized.items():
        if res.family in ("cup_mug", "bowl_shell", "tube_open", "tray_open") and v.ok:
            assert v.interior < 0.5 * v.occ, f"{name}: interior {v.interior}/{v.occ}"


def test_gear_has_center_hole(voxelized):
    found = False
    for name, (res, v, mv) in voxelized.items():
        if res.family == "gear_toothed" and v.ok and res.rot_mode == "identity":
            occ = meshio.read_mvox(str(mv)).occ
            # 軸 (x,y 中心) の列は穴 = 非占有
            assert occ[16, 16, :].sum() == 0, name
            found = True
    if not found:
        pytest.skip("no axis-aligned gear in the sample")


def test_estimate_tracks_actual(voxelized):
    ratios = [v.occ / res.shape.est_occ for _, (res, v, _) in voxelized.items() if v.ok and res.shape.est_occ > 0]
    assert ratios
    med = float(np.median(ratios))
    assert 0.6 < med < 1.6, med


def test_voxelize_is_deterministic_and_parallel_safe(editor_exe, tmp_path):
    res = build_shape(777, 0, registry.FAMILIES, "small", "aligned", only={"table"})
    assert res.status == "built"
    p = tmp_path / "t.obj"
    write_obj(p, res.shape.mesh, [])

    def run(k):
        d = tmp_path / f"out{k}"
        return editor.voxelize_obj_files([p], d, tmp_path / f"log{k}.txt", editor_exe), d

    with ThreadPoolExecutor(max_workers=2) as ex:
        rcs = list(ex.map(run, range(2)))
    assert all(rc == 0 for rc, _ in rcs)
    a = (rcs[0][1] / "t#mesh0#prim0.mvox").read_bytes()
    b = (rcs[1][1] / "t#mesh0#prim0.mvox").read_bytes()
    assert a == b
