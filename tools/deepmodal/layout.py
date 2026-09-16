#====================================================================================
#                          layout.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          Deep-Modal の Python 側共有定数 (C++ と機械照合)
#====================================================================================
"""Deep-Modal のデータレイアウト定数。

正本は C++ 側の src/Engine/Engine/Modal/ModalTypes.h と
src/Engine/Engine/Modal/Voxelizer.h。ここにある VOXEL_N / MAP_N / MEL_BANDS /
CHANNELS の 4 定数は tools/check_rules.ps1 の $constGroups が ModalTypes.h の
kModalVoxelN 等と機械照合する。★1 行 1 整数 (`NAME = 数値`) の形を崩さないこと
(check_rules.ps1 の正規表現がこの形でしか拾えない)。
"""
import math

# ---- Deep-Modal 共有定数 (check_rules.ps1 の $constGroups が ModalTypes.h と照合) ----
VOXEL_N = 32    # ボクセルグリッドの一辺 (kModalVoxelN)
MAP_N = 16      # 特徴マップの一辺 (kModalMapN)
MEL_BANDS = 32  # Mel 帯域数 (kModalBands)
CHANNELS = 192  # ネット出力チャンネル数 = 力軸3 x (mask 32 + amp 32) (kModalChannels)

VOXEL_PAD = 1   # flood-fill 用の外周パディング層数 (kModalVoxelPad)

# ---- Mel 帯域の範囲 (spec §4.1「Mel」。DmNetHeader の既定 fMinHz/fMaxHz と同値) ----
F_MIN = 100.0
F_MAX = 10000.0

# ---- 参照材質 (spec §4.1「参照材質」。アルミ相当。ModalTypes.h の DmNetHeader 既定と同値。
#      stage0 の統計で確定させ、確定したらここと spec §8 の両方を更新する) ----
REF_YOUNG = 7.0e10     # Pa
REF_DENSITY = 2700.0   # kg/m^3
REF_POISSON = 0.33     # 無次元 (FEM 専用。ランタイムの BuildModes は読まない)
L_REF = 0.3            # m (基準サイズ。sub-03.md の表記に合わせて L_REF)
REF_ALPHA = 6.0        # 1/s (Rayleigh 減衰、質量項)
REF_BETA = 1.0e-7      # s (Rayleigh 減衰、剛性項)


def mask_ch(j: int, i: int) -> int:
    """チャンネル配置 (spec §4.1 BuildModes 手順 1)。j=力軸(0..2)、i=帯域(0..31)。
    C++ の MaskCh (ModalTypes.h) と同じ式の二重実装。"""
    return j * 64 + i


def amp_ch(j: int, i: int) -> int:
    """C++ の AmpCh と同じ式の二重実装。"""
    return j * 64 + 32 + i


def mel_of(f: float) -> float:
    return 2595.0 * math.log10(1.0 + f / 700.0)


def inv_mel(m: float) -> float:
    return 700.0 * (10.0 ** (m / 2595.0) - 1.0)


def mel_band_centers():
    """Mel(F_MIN..F_MAX) を MEL_BANDS 帯域に等分し、各帯域の中心 (区間中点の逆変換) を
    Hz のリストで返す。C++ の MelBandCenters (ModalTypes.h) と同じ式の二重実装 —
    test_layout がヘッダ値 (Editor.exe 経由) と 1e-2 Hz で照合してドリフトを検知する。
    ★実行時 (C++ ランタイム) はこの関数の値ではなく .dmnet ヘッダの bandCenterHz[32] を
    使う。この関数は「ヘッダを焼く側 (export.py)」と selftest 用の正本"""
    mel_min = mel_of(F_MIN)
    mel_max = mel_of(F_MAX)
    centers = []
    for i in range(MEL_BANDS):
        m0 = mel_min + (mel_max - mel_min) * (i / MEL_BANDS)
        m1 = mel_min + (mel_max - mel_min) * ((i + 1) / MEL_BANDS)
        centers.append(inv_mel(0.5 * (m0 + m1)))
    return centers


# ---- .mvox ヘッダ (spec §4.2。Voxelizer.h の SerializeVox/DeserializeVox が正本) ----
# 72 B = 18 フィールド x 4 B。フィールド単位で読む (struct をそのまま cast しない —
# Material の暗黙パディングの罠と同じ理由。Python 側は struct モジュールでこの並びどおりに
# unpack するだけなのでパディングの心配自体が無いが、順序はここでしか定義しない)
MVOX_MAGIC = 0x584F564D  # リトルエンディアンで読むと "MVOX"
MVOX_VERSION = 1
MVOX_HEADER_BYTES = 72
MVOX_OCC_BYTES = VOXEL_N * VOXEL_N * VOXEL_N
MVOX_TOTAL_BYTES = MVOX_HEADER_BYTES + MVOX_OCC_BYTES

# magic,version,n,pad (u32 x4) / origin[3],voxelSize,aabbMin[3],aabbMax[3],longestEdge (f32 x11) /
# surfaceCount,interiorCount,reserved (u32 x3) = 4*4 + 11*4 + 3*4 = 72 B
MVOX_STRUCT_FMT = "<4I11f3I"

MVOX_FIELDS = (
    "magic", "version", "n", "pad",
    "origin_x", "origin_y", "origin_z", "voxel_size",
    "aabb_min_x", "aabb_min_y", "aabb_min_z",
    "aabb_max_x", "aabb_max_y", "aabb_max_z",
    "longest_edge",
    "surface_count", "interior_count", "reserved",
)
