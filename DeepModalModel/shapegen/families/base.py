#====================================================================================
#                          base.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          ファミリ定義と 1 形状の構築手順 (seed → OBJ 用メッシュ)
#====================================================================================
"""`Family` は名前・カテゴリ・重み・build 関数の組。`build_shape` が 1 形状ぶんの手順を
固定する: seed 導出 → ファミリ選択 (最初の random()) → 回転モード → 構築 (PlacementError は
PLACE_RETRY 回まで同じ乱数列から引き直す) → 回転と事前検査 (Assembly.finalize)。
ファミリの build は Assembly を返す (配置できなければ PlacementError を投げる)。
"""
from dataclasses import dataclass, field
from typing import Callable, Optional

from .. import constants as C
from .. import rotation
from ..assembly import Assembly, PlacementError, ShapeContext, FinalShape
from ..rng import ShapeRng, derive_seed


@dataclass(frozen=True)
class Family:
    name: str
    category: str
    weight: float
    build: Callable[[ShapeContext], Assembly]


@dataclass
class BuildResult:
    index: int
    name: str
    family: str
    category: str
    rot_mode: str
    seed: int
    status: str            # "built" | "pre_rejected"
    reason: str
    shape: Optional[FinalShape]
    params: dict = field(default_factory=dict)
    attempts: int = 1


def pick_family(rng: ShapeRng, families, only=None) -> Family:
    """重み付き選択。`only` (名前の集合) があればその中だけから (重みはそのまま)。
    ★rng の最初の 1 回をここで使う (ファミリの追加・重み変更で以降の列が変わるのは
    FAMILY_SET_VERSION で管理する)。"""
    cands = [f for f in families if (only is None or f.name in only)]
    if not cands:
        raise ValueError("no family matches the filter")
    return rng.weighted(cands, [f.weight for f in cands])


def rotation_mix_for(category: str, preset: str) -> dict:
    if preset == "default":
        return C.ROTATION_MIX_BY_CATEGORY.get(category, C.ROTATION_MIX["default"])
    return C.ROTATION_MIX[preset]


def build_shape(global_seed: int, index: int, families, profile: str = "small",
                rotation_preset: str = "default", only=None) -> BuildResult:
    seed = derive_seed(global_seed, index)
    rng = ShapeRng(seed)
    fam = pick_family(rng, families, only)
    rot_mode = rotation.pick_mode(rng, rotation_mix_for(fam.category, rotation_preset))
    name = f"{fam.name}_{index:06d}"
    ctx = ShapeContext(rng, rot_mode, profile)

    R = rotation.random_rotation(rng, rot_mode)
    attempts = 0
    place_failures = 0
    est_retries = 0
    shape = None
    reason = ""
    # 配置失敗は PLACE_RETRY 回まで、推定占有の範囲外は厚み倍率 (ctx.fill) を掛け直して
    # EST_RETRY 回まで、同じ乱数列を進めながら組み直す (決定的)
    while True:
        attempts += 1
        ctx.params = {}
        try:
            asm = fam.build(ctx)
        except PlacementError:
            place_failures += 1
            if place_failures >= C.PLACE_RETRY:
                reason = "pre_reject_placement"
                break
            continue
        try:
            shape = asm.finalize(R, rot_mode)
        except PlacementError:
            shape = None
            reason = "pre_reject_placement"
        else:
            reason = shape.reject_reason
        soft_high = (reason == "" and shape is not None and shape.est_occ > C.EST_SOFT_HIGH)
        if (reason in ("pre_reject_est_high", "pre_reject_est_low") or soft_high) \
                and est_retries < C.EST_RETRY:
            est_retries += 1
            if reason == "pre_reject_est_low":
                ctx.fill *= C.EST_LOW_FILL_STEP
                ctx.lean = min(1.0, ctx.lean / C.EST_LEAN_STEP)
            else:
                ctx.fill *= C.EST_HIGH_FILL_STEP
                ctx.lean *= C.EST_LEAN_STEP
            continue
        if reason == "pre_reject_placement":
            place_failures += 1
            if place_failures < C.PLACE_RETRY:
                continue
        break
    ctx.params["fill"] = round(ctx.fill, 4)
    ctx.params["lean"] = round(ctx.lean, 4)
    if reason:
        return BuildResult(index, name, fam.name, fam.category, rot_mode, seed, "pre_rejected",
                           reason, shape, dict(ctx.params), attempts)
    return BuildResult(index, name, fam.name, fam.category, rot_mode, seed, "built", "", shape,
                       dict(ctx.params), attempts)
