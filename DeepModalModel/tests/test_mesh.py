#====================================================================================
#                          test_mesh.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          閉性検査・OBJ 書式・2D 多角形 sdf
#====================================================================================
import numpy as np

from shapegen import mesh as meshlib
from shapegen import pieces as P


def test_check_closed_box_ok_and_broken_variants():
    box = P.Box(1.0, 0.5, 0.25).local_mesh()
    rep = meshlib.check_closed(box)
    assert rep.ok and rep.euler == 2 and rep.face_count == 12
    # 面を 1 枚抜く → 境界辺
    broken = meshlib.TriMesh(box.verts, box.faces[1:])
    assert not meshlib.check_closed(broken).ok
    # 1 枚だけ向きを反転 → 有向辺が重複
    flipped = box.faces.copy()
    flipped[0] = flipped[0][::-1]
    assert not meshlib.check_closed(meshlib.TriMesh(box.verts, flipped)).ok
    # 退化三角形
    degenerate = np.vstack([box.faces, [[0, 0, 1]]])
    assert not meshlib.check_closed(meshlib.TriMesh(box.verts, degenerate)).ok


def test_write_obj_format(tmp_path):
    box = P.Box(1.0, 1.0, 1.0).local_mesh()
    path = tmp_path / "b.obj"
    meshlib.write_obj(path, box, ["family=test index=0"])
    lines = path.read_text(encoding="utf-8").splitlines()
    assert lines[0] == "# family=test index=0"
    v = [l for l in lines if l.startswith("v ")]
    f = [l for l in lines if l.startswith("f ")]
    assert len(v) == 8 and len(f) == 12
    for line in f:
        idx = [int(t) for t in line.split()[1:]]
        assert len(idx) == 3 and min(idx) >= 1 and max(idx) <= 8
    assert all(l.startswith(("#", "v ", "f ")) for l in lines)


def test_polygon_sdf_square():
    sq = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
    pts = np.array([[0, 0], [0.5, 0], [2, 0], [1, 1], [-3, 0.5]])
    d = meshlib.polygon_sdf(pts, sq)
    assert abs(d[0] + 1.0) < 1e-12
    assert abs(d[1] + 0.5) < 1e-12
    assert abs(d[2] - 1.0) < 1e-12
    assert abs(d[3]) < 1e-12
    assert abs(d[4] - 2.0) < 1e-12
    # 向きを逆にしても同じ
    d2 = meshlib.polygon_sdf(pts, sq[::-1])
    assert np.allclose(d, d2)


def test_polygon_area_centroid():
    sq = [(0, 0), (2, 0), (2, 1), (0, 1)]
    assert abs(meshlib.polygon_area(sq) - 2.0) < 1e-12
    assert meshlib.polygon_area(sq[::-1]) < 0
    assert np.allclose(meshlib.polygon_centroid(sq), (1.0, 0.5))
