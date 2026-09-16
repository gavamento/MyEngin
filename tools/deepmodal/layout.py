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

# ボクセル化の分割数 (spec §4.1「L = max(aabb extent)、h = L/28」)。FEM は**必ず
# この分割数を参照サイズに適用した h_ref で組む** (sub-04 round 1 で確定した欠陥修正
# — メッシュ実寸の voxel_size を渡すと、スケール不変な入力ボクセルに対して
# スケール依存の教師値が付き、かつランタイムの σ3 と二重にサイズを掛けることになる。
# spec §4.1「FEM は参照サイズで組む」/ dataset.py 参照)
VOXEL_DIVISIONS = 28
H_REF = L_REF / VOXEL_DIVISIONS  # m (FEM の要素寸法。全メッシュ共通)


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


def cell_order():
    """16^3 cell を C++ の CellIndexOf と同じ順 (cx が最内) で列挙する。
    dataset.py (feat 行の書き込み順) と train.py (dense 復元) の両方がこの 1 本を
    使う (同じ規則の 2 本目を書くと必ずずれる、というこのリポジトリ全体の原則)。"""
    order = []
    for cz in range(MAP_N):
        for cy in range(MAP_N):
            for cx in range(MAP_N):
                order.append((cx, cy, cz))
    return order


# ---- .dmnet ヘッダ / op 表 (spec §4.2。sub-05 の C++ ローダ (DmNet.h、未着手) が
#      読む側の正本コードを持つ予定だが、書式そのものの正本はここ (export.py が書き、
#      sub-05 はこれに合わせて読む。2 本目の書式定義を作らないこと) ----
#
# ★spec §4.2 は「256 B ヘッダ」と書いているが、列挙されているフィールドをそのまま
#   4 B 単位で足すと 224 B にしかならない (magic/version/opCount/bufferCount 4x4 +
#   inN/outN/bands/channels 4x4 + fMinHz..maskThreshold 4x6 + refYoung..refBeta 4x6 +
#   bandCenterHz[32] 4x32 + weightsHash 8 + paramCount 4 + reserved(単数) 4 = 224)。
#   `.mvox` が「64 B」→ sub-02 round 1 で「72 B」に訂正された前例 (フィールド数え間違い)
#   とは違い、こちらは 256 という丸い数自体が意図的 (将来ヘッダを増やす伸び代) と判断し、
#   **coder 判断で reserved を 32 B 分の余白 (9 x uint32) に広げて 256 B ちょうどへ揃えた**
#   ([追加]。sub-05 実装時に planner/coder が確定させること — SELF_EVAL 参照)
DMNET_MAGIC = 0x544E4D44  # リトルエンディアンで読むと "DMNT" (ModalTypes.h の DmNetHeader::magic と同値)
DMNET_VERSION = 1
DMNET_HEADER_BYTES = 256

# フィールド単位で書く (Material の暗黙パディングの罠と同じ理由で struct をそのまま
# cast しない)。4I: magic,version,opCount,bufferCount / 4i: inN,outN,bands,channels /
# 6f: fMinHz,fMaxHz,logAmpMin,logAmpMax,ampScale,maskThreshold /
# 6f: refYoung,refDensity,refPoisson,refSizeL,refAlpha,refBeta /
# 32f: bandCenterHz / Q: weightsHash / I: paramCount / 9I: reserved (伸び代)
DMNET_HEADER_FMT = "<4I4i6f6f32fQI9I"
DMNET_HEADER_FIELDS = (
    ("magic", "version", "op_count", "buffer_count",
     "in_n", "out_n", "bands", "channels",
     "f_min_hz", "f_max_hz", "log_amp_min", "log_amp_max", "amp_scale", "mask_threshold",
     "ref_young", "ref_density", "ref_poisson", "ref_size_l", "ref_alpha", "ref_beta")
    + tuple(f"band_center_{i}" for i in range(MEL_BANDS))
    + ("weights_hash", "param_count")
    + tuple(f"reserved_{i}" for i in range(9))
)

# op 表 1 エントリ = 12 x 4B = 48 B (spec §4.2)。type/in0/in1/out/cin/cout/k/stride/
# pad/outPad は符号付き (in1=-1 は「未使用」のセンチネル、Add 以外は全部これ)。
# weightOffset/biasOffset は **ファイル先頭からの絶対バイトオフセット** (blob 境界を
# reader 側で cin/cout/k から逆算させない設計判断)。重み (fp16、Conv3d は
# (cout,cin,k,k,k)、ConvTranspose3d は (cin,cout,k,k,k) の素の並び) とバイアス
# (fp32) は、ヘッダ+op 表の直後に「重み blob 全部 → バイアス blob 全部」の順で
# 連続配置する。Conv3d/ConvTranspose3d 以外 (ReLU/Add) は両方 DMNET_OFFSET_NONE
# (offset=0 は先頭の重みの正当な位置なので「無し」に使えない)
DMNET_OP_BYTES = 48
DMNET_OP_FMT = "<10i2I"
DMNET_OP_FIELDS = (
    "type", "in0", "in1", "out", "cin", "cout", "k", "stride", "pad", "out_pad",
    "weight_offset", "bias_offset",
)
DMNET_OFFSET_NONE = 0xFFFFFFFF

# op 種別 (spec §4.2 / ModalTypes.h の想定と同じ値)
OP_CONV3D = 0
OP_CONVTRANSPOSE3D = 1
OP_RELU = 2
OP_ADD = 3
