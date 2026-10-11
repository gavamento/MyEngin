#====================================================================================
#                          test_estimate.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          占有見積りの式 (体積/h^3 + 表面積/(2h^2)) と上界性
#====================================================================================
from shapegen import estimate
from shapegen import pieces as P


def test_single_box_formula():
    b = P.Box(1.0, 0.5, 0.25)
    h = 1.0 / 28
    expect = 0.125 / h ** 3 + 2 * (0.5 + 0.125 + 0.25) / (2 * h * h)
    assert abs(estimate.estimate_occ([b], h) - expect) < 1e-6
    assert estimate.estimate_occ([b], 0.0) == 0.0


def test_overlap_is_upper_bound():
    """重ねたピースの和は、同じ体積の 1 ピースより大きい (重なりを引かない)。"""
    h = 1.0 / 28
    one = estimate.estimate_occ([P.Box(1.0, 0.2, 0.2)], h)
    two = estimate.estimate_occ([P.Box(0.6, 0.2, 0.2), P.Box(0.6, 0.2, 0.2).posed(None, (0.4, 0, 0))], h)
    assert two > one
