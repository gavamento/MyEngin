#====================================================================================
#                          solids.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          中実の単体ファミリ (箱・角柱・楕円体・カプセル・円錐台・環)
#====================================================================================
"""中実の立方体は充填率 100% (24389 voxel) で必ず却下されるので、扁平・細長に限って
体積を主寸法³ の 0.22 以下に抑える。球は半径を変えても同じ占有になる (最長辺で正規化)
ため単体では作らない (楕円体は半径比で変わる)。"""
import math

from .. import pieces as P
from ..assembly import Assembly


def box_solid(ctx):
    asm = Assembly(ctx)
    sy = ctx.lu(0.12, 0.8)
    sz_hi = min(0.8, 0.22 / sy)
    sz = max(ctx.feature, ctx.lu(min(0.08, sz_hi), sz_hi))
    sy = max(sy, ctx.feature)
    ctx.rec(size=[1.0, sy, sz])
    asm.add(P.Box(1.0, sy, sz))
    return asm


def prism_solid(ctx):
    asm = Assembly(ctx)
    n = ctx.rng.choice([3, 5, 6, 8])
    tall = ctx.rng.chance(0.6)
    if tall:
        height = 1.0
        r = ctx.lu(0.08, 0.26)
    else:
        r = 0.5
        height = max(ctx.feature, ctx.lu(0.08, 0.3))
    phase = ctx.u(0.0, math.pi)
    ctx.rec(n=n, tall=tall, r=r, height=height)
    asm.add(P.ConvexPrism(P.regular_polygon(n, r, phase), height))
    return asm


def ellipsoid_solid(ctx):
    asm = Assembly(ctx)
    ry = 0.5 * ctx.u(0.25, 0.8)
    rz = max(0.5 * ctx.feature, 0.5 * ctx.u(0.2, 0.6))
    ctx.rec(radii=[0.5, ry, rz])
    asm.add(P.Ellipsoid(0.5, ry, rz))
    return asm


def capsule_solid(ctx):
    asm = Assembly(ctx)
    r = max(0.5 * ctx.feature, ctx.lu(0.06, 0.2))
    h = 1.0 - 2.0 * r
    ctx.rec(r=r, h=h)
    asm.add(P.capsule(r, h))
    return asm


def frustum_solid(ctx):
    asm = Assembly(ctx)
    r0 = ctx.u(0.12, 0.28)
    r1 = r0 * ctx.u(0.0, 0.9)
    if r1 < 0.5 * ctx.feature:
        r1 = 0.0  # 円錐 (先端は尖る)
    ctx.rec(r0=r0, r1=r1)
    asm.add(P.frustum(r0, r1, 1.0))
    return asm


def torus_ring(ctx):
    asm = Assembly(ctx)
    ratio = ctx.u(1.6, 6.0)
    r = 0.5 / (1.0 + ratio)
    r = max(r, 0.5 * ctx.feature)
    R = 0.5 - r
    ctx.rec(R=R, r=r)
    asm.add(P.torus(R, r))
    return asm
