#====================================================================================
#                          constants.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          形状生成器の名前付き定数 (閾値・予算・版)
#====================================================================================
"""閾値はすべてここに置く。`validate.py` の受理条件と `families` の設計寸法が同じ値を
見ることで「設計側は 2 voxel 厚を狙ったのに検査側は 3 voxel を要求する」といった
食い違いを防ぐ。ボクセル格子の定数 (32 / 28 / 16) は tools/deepmodal/layout.py が正本で、
ここでは import して使う (2 本目の定義を作らない)。"""
import layout

# ---- ボクセル格子 (正本は layout.py / C++ の Voxelizer.h) ----
VOXEL_N = layout.VOXEL_N              # 32
VOXEL_DIVISIONS = layout.VOXEL_DIVISIONS  # 28 (最長辺を 28 分割、両端 1.5 voxel が余白)
MAP_N = layout.MAP_N                  # 16 (ネット出力 cell の一辺)
PAD = layout.VOXEL_PAD                # 1 (外周の空き層)

# ---- 受理条件 (validate.py) ----
# 占有 voxel 数。下限は「6〜24 voxel の極小パーツは常に無音」(modelnet40_eval_report §3) の
# 観察から十分離し、上限は ModelNet40 の 95% 点 (6100) に合わせる。6000 超は FEM が
# 1 件 4〜6 分かかり、少数でも総時間を 10% 以上押し上げる (data/modelnet40/stats.json)
OCC_MIN = 300
OCC_MAX = 3500
# 占有を含む 16^3 cell の下限 (学習の教師値が付く cell が少なすぎる形状を除く)
CELLS_MIN = 40
# 6 近傍の占有数が 1 以下の voxel の比率の上限 (1 voxel 幅の糸状部位の検出)
FILAMENT_RATIO_MAX = 0.05
# 16^3 max-pool キー (ネット出力解像度) が同じ形状の受理上限 (準重複)
COARSE_DUP_LIMIT = 3

# ---- 設計寸法 (families / assembly) ----
# 最小厚み (voxel 単位)。1 voxel 層は FEM で 1 要素厚 = 曲げ剛性が不正確なので
# 1.6 以上 (2.0 だと回転余裕と合わせて殻が 3 voxel 厚超になり、占有が ModelNet40 の
# 分布より系統的に重くなった — スモーク実測)
MIN_FEATURE_VOXELS = 1.6
# ピース間の重なり深さ (voxel 単位)。重なり領域の点 q を含む voxel は両ピースの
# 占有に入るので、理論上は 0 より大きければ連結する。0.3 は数値誤差の余裕
MIN_OVERLAP_VOXELS = 0.3
# 中空の開口・隙間・歯間の最小幅 (voxel 単位)。表面 voxel が両側 1 層ずつ食うため 3
MIN_OPENING_VOXELS = 3.0
# 回転で AABB 最長辺が伸びる分 (h が大きくなる分) の厚み余裕。free の理論上限は √3 だが
# 細長い形状では L がむしろ縮むので 1.35 とし、残りは pre_reject_thin で落とす
ROT_MARGIN = {"identity": 1.0, "octahedral": 1.0, "tilt": 1.15, "free": 1.35}
# 設計空間の主寸法 (ファミリは最長辺 ≈ 1.0 で組む)。突起で伸びる分の余裕 10%
DESIGN_MAIN = 1.0
DESIGN_OVERSHOOT = 1.1
# 推定占有が範囲外のとき、厚み倍率 (ctx.fill) を掛け直して組み直す回数と倍率。
# EST_SOFT_HIGH は「却下はしないが軽くしたい」しきい値 (pilot_a の実測: 推定/実測の比 0.93、
# 中央値 2414 では FEM が 11 並列で 67 時間級になるため、分布を ModelNet40 より軽くする)
EST_RETRY = 3
EST_HIGH_FILL_STEP = 0.7
EST_LOW_FILL_STEP = 1.3
EST_SOFT_HIGH = 1500
EST_LEAN_STEP = 0.75  # 副寸法 (ctx.lean) の縮小率
# 1 形状の三角形数上限 (OBJ サイズと Editor.exe の読み込み時間の抑制)
TRI_MAX = 20000
# ピースの配置を同じ RNG 列から試し直す回数 (ファミリが None を返す前)
PLACE_RETRY = 4

# ---- 事前見積り (estimate.py) の門 (推定は実測と log 回帰で slope 0.91 / R² 0.93) ----
EST_LOW_FACTOR = 0.5
EST_HIGH_FACTOR = 1.1

# ---- パイプライン ----
CHUNK_INDICES = 2000     # 1 ラウンドのインデックス数
MAX_EMPTY_ROUNDS = 5     # 受理 0 のラウンドがこれだけ続いたら中断 (重複や設定ミスで終わらない事故の防止)
EDITOR_BATCH = 500       # Editor.exe 1 起動あたりの OBJ 数 (OBJ 経路は RenderResources 不使用)
DEFAULT_JOBS = 10
DEFAULT_EDITOR_JOBS = 4

# ---- 版 (ファミリ定義・重み・閾値を変えたら上げる。再開時の整合検査に使う) ----
FAMILY_SET_VERSION = "1"

# ---- 回転モードの既定混合 (%) ----
ROTATION_MIX = {
    "default": {"identity": 25, "octahedral": 15, "tilt": 35, "free": 25},
    "aligned": {"identity": 70, "octahedral": 20, "tilt": 10, "free": 0},
    "free": {"identity": 10, "octahedral": 5, "tilt": 25, "free": 60},
}
# ファミリのカテゴリ別の上書き (家具・建材・容器は軸平行寄り、岩・汎用・生物は自由寄り)
ROTATION_MIX_BY_CATEGORY = {
    "furniture": {"identity": 40, "octahedral": 20, "tilt": 30, "free": 10},
    "building": {"identity": 40, "octahedral": 20, "tilt": 30, "free": 10},
    "container": {"identity": 40, "octahedral": 20, "tilt": 30, "free": 10},
    "organic": {"identity": 10, "octahedral": 5, "tilt": 25, "free": 60},
    "creature": {"identity": 10, "octahedral": 5, "tilt": 25, "free": 60},
    "generic": {"identity": 10, "octahedral": 5, "tilt": 25, "free": 60},
}

# ---- プロファイル (占有分布の制御)。fill_scale は厚み系の上限倍率 ----
PROFILES = {
    "small": {"fill_scale": 0.8},
    "modelnet": {"fill_scale": 1.0},
}
