#====================================================================================
#                          test_families.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          全ファミリ × seed: 成功率・閉性・厚み・決定性・重み
#====================================================================================
import numpy as np
import pytest

from shapegen import constants as C
from shapegen import mesh as meshlib
from shapegen.families import registry
from shapegen.families.base import build_shape, pick_family
from shapegen.rng import ShapeRng

SEEDS = list(range(10))


@pytest.mark.parametrize("family", [f.name for f in registry.FAMILIES])
def test_family_builds_closed_and_thick(family):
    built = 0
    for i in SEEDS:
        res = build_shape(123, i, registry.FAMILIES, "small", "default", only={family})
        assert res.family == family
        assert res.name == f"{family}_{i:06d}"
        if res.shape is not None:
            rep = meshlib.check_closed(res.shape.mesh)
            # 殻の和なので全体は閉じている (各ピースが閉じている ⇒ 辺が対で現れる)
            assert rep.ok, f"{family} seed {i}: {rep.reason}"
            assert res.shape.n_tris <= C.TRI_MAX or res.reason == "pre_reject_tris"
        if res.status == "built":
            built += 1
            assert res.shape.min_feature_vox >= C.MIN_FEATURE_VOXELS - 1e-9
            assert res.shape.reject_reason == ""
    # 回転余裕で thin になる分と、嵩のあるファミリの推定超過 (安価な事前却下) を見込んで 5/10 以上
    assert built >= 5, f"{family}: built {built}/10"


def test_build_is_deterministic():
    a = build_shape(99, 5, registry.FAMILIES, "small", "default")
    b = build_shape(99, 5, registry.FAMILIES, "small", "default")
    assert a.family == b.family and a.rot_mode == b.rot_mode and a.params == b.params
    if a.shape is not None:
        assert np.array_equal(a.shape.mesh.verts, b.shape.mesh.verts)
        assert np.array_equal(a.shape.mesh.faces, b.shape.mesh.faces)
    c = build_shape(99, 6, registry.FAMILIES, "small", "default")
    assert (c.family, c.params) != (a.family, a.params)


def test_family_weights_total_100_and_names_unique():
    assert abs(registry.total_weight() - 100.0) < 1e-9
    names = [f.name for f in registry.FAMILIES]
    assert len(names) == len(set(names))
    assert all(f.weight > 0 for f in registry.FAMILIES)
    assert "#" not in "".join(names)


def test_pick_family_filter_and_distribution():
    rng = ShapeRng(3)
    counts = {}
    for _ in range(3000):
        f = pick_family(rng, registry.FAMILIES)
        counts[f.name] = counts.get(f.name, 0) + 1
    ru = counts.get("random_union", 0) / 3000
    assert 0.03 < ru < 0.08  # 重み 5%
    only = pick_family(ShapeRng(1), registry.FAMILIES, only={"sword"})
    assert only.name == "sword"
    with pytest.raises(ValueError):
        pick_family(ShapeRng(1), registry.FAMILIES, only={"nope"})
