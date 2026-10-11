#====================================================================================
#                          registry.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          ファミリ一覧 (順序固定の明示リスト。重みは % 単位)
#====================================================================================
"""デコレータ収集にしない: モジュールの import 順でリストが変わると、同じ seed でも
`pick_family` の結果が変わる。追加・削除・重み変更をしたら constants.FAMILY_SET_VERSION を
上げる。カテゴリ別の合計 (ユーザー決定「ゲーム小道具中心」):
  solid 8 / plate 23 / container 13 / weapon 10 / furniture+building 14 / mech 12 /
  creature+organic 15 / generic 5 = 100。"""
from . import containers, furniture, mech, misc, organic, plates, solids, weapons
from .base import Family

FAMILIES = [
    # ---- 中実 (8) ----
    Family("box_solid", "solid", 2.0, solids.box_solid),
    Family("prism_solid", "solid", 1.5, solids.prism_solid),
    Family("ellipsoid_solid", "solid", 1.5, solids.ellipsoid_solid),
    Family("capsule_solid", "solid", 1.0, solids.capsule_solid),
    Family("frustum_solid", "solid", 1.0, solids.frustum_solid),
    Family("torus_ring", "solid", 1.0, solids.torus_ring),
    # ---- 板・殻・格子 (23) ----
    Family("plate_perforated", "plate", 3.0, plates.plate_perforated),
    Family("tray_open", "container", 3.0, plates.tray_open),
    Family("tube_open", "plate", 2.5, plates.tube_open),
    Family("channel_profile", "building", 3.0, plates.channel_profile),
    Family("corrugated_plate", "building", 1.5, plates.corrugated_plate),
    Family("bent_plate", "plate", 2.0, plates.bent_plate),
    Family("grid_lattice", "building", 3.0, plates.grid_lattice),
    Family("frame_cage", "building", 2.0, plates.frame_cage),
    Family("washer_disc", "mech", 1.5, plates.washer_disc),
    Family("wedge_block", "building", 1.5, plates.wedge_block),
    # ---- 容器・小道具 (13) ----
    Family("cup_mug", "container", 3.0, containers.cup_mug),
    Family("bowl_shell", "container", 2.5, containers.bowl_shell),
    Family("bottle_vase", "container", 2.5, containers.bottle_vase),
    Family("barrel_hoops", "container", 1.5, containers.barrel_hoops),
    Family("crate_planks", "container", 1.5, containers.crate_planks),
    Family("lid_bell", "container", 1.0, containers.lid_bell),
    Family("stacked_discs", "prop", 1.0, containers.stacked_discs),
    # ---- 武器・工具 (10) ----
    Family("sword", "weapon", 2.0, weapons.sword),
    Family("axe_hammer", "weapon", 2.0, weapons.axe_hammer),
    Family("spear_staff", "weapon", 1.5, weapons.spear_staff),
    Family("shield", "weapon", 1.5, weapons.shield),
    Family("bow_arc", "weapon", 1.0, weapons.bow_arc),
    Family("wrench_tool", "weapon", 2.0, weapons.wrench_tool),
    # ---- 家具・建材 (14) ----
    Family("table", "furniture", 2.5, furniture.table),
    Family("chair_bench", "furniture", 2.5, furniture.chair_bench),
    Family("shelf_cabinet", "furniture", 2.0, furniture.shelf_cabinet),
    Family("stairs_steps", "building", 1.5, furniture.stairs_steps),
    Family("pillar_column", "building", 1.5, furniture.pillar_column),
    Family("arch_frame", "building", 1.5, furniture.arch_frame),
    Family("fence_railing", "building", 1.5, furniture.fence_railing),
    Family("roof_truss", "building", 1.0, furniture.roof_truss),
    # ---- 機械 (12) ----
    Family("gear_toothed", "mech", 2.0, mech.gear_toothed),
    Family("flange_bolt", "mech", 1.5, mech.flange_bolt),
    Family("bracket_angle", "mech", 1.5, mech.bracket_angle),
    Family("pipe_elbow_tee", "mech", 2.0, mech.pipe_elbow_tee),
    Family("piston_shaft", "mech", 1.5, mech.piston_shaft),
    Family("spring_coil", "mech", 1.0, mech.spring_coil),
    Family("chain_links", "mech", 1.0, mech.chain_links),
    Family("rod_bundle", "mech", 1.5, mech.rod_bundle),
    # ---- キャラ・有機 (15) ----
    Family("biped_blocky", "creature", 3.0, organic.biped_blocky),
    Family("quadruped", "creature", 3.0, organic.quadruped),
    Family("tentacle_creature", "creature", 2.0, organic.tentacle_creature),
    Family("tree_branching", "organic", 2.0, organic.tree_branching),
    Family("rock_blob", "organic", 3.0, organic.rock_blob),
    Family("crystal_cluster", "organic", 1.0, organic.crystal_cluster),
    Family("mushroom_plant", "organic", 1.0, organic.mushroom_plant),
    # ---- 汎用 (5) ----
    Family("random_union", "generic", 5.0, misc.random_union),
]

FAMILY_BY_NAME = {f.name: f for f in FAMILIES}


def total_weight() -> float:
    return float(sum(f.weight for f in FAMILIES))
