#====================================================================================
#                          model.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          Deep-Modal の 3D U-Net (Conv3d/ConvTranspose3d/ReLU/Add のみ)
#====================================================================================
"""spec §4.2 の `.dmnet` op 表が持てる op 種は Conv3d(0) / ConvTranspose3d(1) /
ReLU(2) / Add(3) の 4 つだけ (sub-05 の C++ 推論バックエンドと 1:1)。BatchNorm は
学習を安定させるためだけに使い、export.py が畳み込みへ畳んでから書き出す
(この 4 op 種だけで表現できる形にするため)。

★グラフは「データとして 1 回だけ」定義する (`ModalUNet._build_ops`)。forward() も
export.py もこのグラフ (`self.ops`、`OpSpec` のリスト) を辿って実行するだけの
薄い実行器を通す — 学習時に実際に計算されるグラフと export される op 表が
別々に手書きされていると必ず乖離する (このリポジトリ全体の「規則は 1 本」原則)。

★ボクセル/cell の軸並び規約 (sub-05 が im2col・ループ順を合わせる契約):
入力テンソルは (N, C=1, D, H, W) で D=z, H=y, W=x (x が最内 = 最終軸)。
これは C++ の VoxelIndexOf(x,y,z) = x + n*(y+n*z) (x 最内) と一致させるための
意図的な選択で、出力テンソル (N, 192, 16,16,16) も同じ規約 (cz,cy,cx) を使う
(layout.cell_order() の cx 最内と対応)。meshio.read_mvox() が返す occ 配列は
occ[x,y,z] なので、モデルへ渡す前に `np.transpose(occ, (2,1,0))` で [z,y,x] へ
入れ替える (train.py / export.py の `voxel_to_tensor` が唯一の変換点)。
"""
from dataclasses import dataclass, field
from typing import Optional

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

import layout

# .dmnet ヘッダの paramCount 上限 (spec §4.2「予算 ≤ 2.0M パラメータ」)。
# export.py がこれを超えたら assert で止める
MAX_PARAM_COUNT = 2_000_000

OP_CONV3D = layout.OP_CONV3D
OP_CONVTRANSPOSE3D = layout.OP_CONVTRANSPOSE3D
OP_RELU = layout.OP_RELU
OP_ADD = layout.OP_ADD


@dataclass
class OpSpec:
    """.dmnet の op 表 1 エントリと同じ形の値オブジェクト (spec §4.2)。
    `module` は Conv3d/ConvTranspose3d のときだけ ConvBN3d を指す (ReLU/Add は None)。
    export.py はここから weightOffset/biasOffset だけ後付けする。"""
    type: int
    in0: int
    in1: int  # -1 = 未使用
    out: int
    cin: int
    cout: int
    k: int
    stride: int
    pad: int
    out_pad: int
    module: Optional[nn.Module] = field(default=None)


class ConvBN3d(nn.Module):
    """Conv3d/ConvTranspose3d + (任意) BatchNorm3d。学習中は BN で正規化するが、
    ディスクに書くのは `fold()` が返す「BN を畳み込んだ後」の (weight, bias) だけ
    (spec §4.2「BN は export で畳み込み済み」)。ReLU はここに含めない —
    グラフ実行器 (ModalUNet._exec_op) が別 op として挟む (op 種を Conv/ConvTranspose/
    ReLU/Add の 4 つだけに保つため)。"""

    def __init__(self, cin: int, cout: int, k: int, stride: int = 1, pad: int = 0,
                 transpose: bool = False, out_pad: int = 0, use_bn: bool = True):
        super().__init__()
        self.transpose = transpose
        self.use_bn = use_bn
        conv_cls = nn.ConvTranspose3d if transpose else nn.Conv3d
        kwargs = dict(kernel_size=k, stride=stride, padding=pad, bias=not use_bn)
        if transpose:
            kwargs["output_padding"] = out_pad
        self.conv = conv_cls(cin, cout, **kwargs)
        self.bn = nn.BatchNorm3d(cout) if use_bn else None

    def forward(self, x):
        x = self.conv(x)
        if self.bn is not None:
            x = self.bn(x)
        return x

    def fold(self):
        """BN を畳み込みへ畳んだ (weight, bias) を返す (fp32、export.py がこれを
        fp16 に丸める)。BN 無しの層 (head2) はそのまま conv の重みを返す。

        Conv3d の weight 形状は (cout,cin,k,k,k) = out 軸が axis0、
        ConvTranspose3d は (cin,cout,k,k,k) = out 軸が axis1 — reshape を間違えると
        チャンネルごとのスケールが別チャンネルへ掛かる静かなバグになるので、
        ここだけがこの区別を知っている (呼び出し側は意識しない)。"""
        weight = self.conv.weight.detach().clone()
        if self.bn is None:
            bias = self.conv.bias.detach().clone()
            return weight, bias
        gamma = self.bn.weight.detach()
        beta = self.bn.bias.detach()
        mean = self.bn.running_mean.detach()
        var = self.bn.running_var.detach()
        eps = self.bn.eps
        scale = gamma / torch.sqrt(var + eps)
        if self.transpose:
            folded_weight = weight * scale.view(1, -1, 1, 1, 1)
        else:
            folded_weight = weight * scale.view(-1, 1, 1, 1, 1)
        folded_bias = beta - mean * scale
        return folded_weight, folded_bias


class ModalUNet(nn.Module):
    """spec §4.2 / design-draft.md §F の U-Net (`widths` でチャンネル幅を一般化)。

    `widths = (stem, w1, w2, w3)`:
      stem: 入力側 2 層 (conv3(in→stem)+ReLU, conv3(stem→stem)) @32^3
      w1  : 1 段目ダウン先 (16^3)、デコーダ最終幅にも使う
      w2  : 2 段目ダウン先 (8^3)
      w3  : 3 段目ダウン先 = ボトルネック (4^3)
      head: conv3(w1→2*w1)+ReLU, conv1(2*w1→out_channels) (活性なし)

    基準構成 (spec §4.2) は widths=(16,32,64,96) (≈1.7M params)。
    `--fixture` は widths=(4,8,8,8) (sub-04.md 記載の「幅 4/8/8/8」)。
    2 つの用途が同じクラス/同じグラフ組み立てを通ることで、C++ 側 (sub-05) が
    実装すべき op 列がただ 1 通りに定まる (fixture 専用の別グラフを持たない)。
    """

    def __init__(self, widths=(16, 32, 64, 96), in_channels: int = 1,
                 out_channels: int = layout.CHANNELS, use_bn: bool = True):
        super().__init__()
        s, w1, w2, w3 = widths
        self.widths = widths
        self.in_channels = in_channels
        self.out_channels = out_channels

        # ★use_bn=False (既定は True): 小規模 overfit 実測で、BN のミニバッチ統計が
        # 記憶を邪魔することが分かった (spec/sub-04.md はネット構造の詳細を規定して
        # いないので、BN の有無は coder の実装判断。README の「overfit の実測」節を
        # 参照。BN 無しでも Conv3d/ConvTranspose3d/ReLU/Add の 4 op 種だけという
        # 制約は変わらない — BN は元々どのみち export で畳み込むので op 表には出ない)
        self.stem1 = ConvBN3d(in_channels, s, 3, 1, 1, use_bn=use_bn)
        self.stem2 = ConvBN3d(s, s, 3, 1, 1, use_bn=use_bn)
        self.down1 = ConvBN3d(s, w1, 4, 2, 1, use_bn=use_bn)
        self.res1 = ConvBN3d(w1, w1, 3, 1, 1, use_bn=use_bn)
        self.down2 = ConvBN3d(w1, w2, 4, 2, 1, use_bn=use_bn)
        self.res2 = ConvBN3d(w2, w2, 3, 1, 1, use_bn=use_bn)
        self.down3 = ConvBN3d(w2, w3, 4, 2, 1, use_bn=use_bn)
        self.res3 = ConvBN3d(w3, w3, 3, 1, 1, use_bn=use_bn)
        self.up1 = ConvBN3d(w3, w2, 4, 2, 1, transpose=True, use_bn=use_bn)
        self.res2d = ConvBN3d(w2, w2, 3, 1, 1, use_bn=use_bn)
        self.up2 = ConvBN3d(w2, w1, 4, 2, 1, transpose=True, use_bn=use_bn)
        self.res1d = ConvBN3d(w1, w1, 3, 1, 1, use_bn=use_bn)
        self.head1 = ConvBN3d(w1, w1 * 2, 3, 1, 1, use_bn=use_bn)
        self.head2 = ConvBN3d(w1 * 2, out_channels, 1, 1, 0, use_bn=False)

        self.ops = self._build_ops()

    def _build_ops(self):
        m = self
        s, w1, w2, w3 = self.widths
        cin, cout = self.in_channels, self.out_channels
        return [
            OpSpec(OP_CONV3D, 0, -1, 1, cin, s, 3, 1, 1, 0, m.stem1),
            OpSpec(OP_RELU, 1, -1, 1, s, s, 0, 0, 0, 0, None),
            OpSpec(OP_CONV3D, 1, -1, 2, s, s, 3, 1, 1, 0, m.stem2),
            OpSpec(OP_CONV3D, 2, -1, 3, s, w1, 4, 2, 1, 0, m.down1),
            OpSpec(OP_RELU, 3, -1, 3, w1, w1, 0, 0, 0, 0, None),
            OpSpec(OP_CONV3D, 3, -1, 4, w1, w1, 3, 1, 1, 0, m.res1),
            OpSpec(OP_RELU, 4, -1, 4, w1, w1, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 3, 4, 5, w1, w1, 0, 0, 0, 0, None),  # enc32 相当 (skip 保存)
            OpSpec(OP_CONV3D, 5, -1, 6, w1, w2, 4, 2, 1, 0, m.down2),
            OpSpec(OP_RELU, 6, -1, 6, w2, w2, 0, 0, 0, 0, None),
            OpSpec(OP_CONV3D, 6, -1, 7, w2, w2, 3, 1, 1, 0, m.res2),
            OpSpec(OP_RELU, 7, -1, 7, w2, w2, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 6, 7, 8, w2, w2, 0, 0, 0, 0, None),  # enc64 相当 (skip 保存)
            OpSpec(OP_CONV3D, 8, -1, 9, w2, w3, 4, 2, 1, 0, m.down3),
            OpSpec(OP_RELU, 9, -1, 9, w3, w3, 0, 0, 0, 0, None),
            OpSpec(OP_CONV3D, 9, -1, 10, w3, w3, 3, 1, 1, 0, m.res3),
            OpSpec(OP_RELU, 10, -1, 10, w3, w3, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 9, 10, 11, w3, w3, 0, 0, 0, 0, None),  # bottleneck
            OpSpec(OP_CONVTRANSPOSE3D, 11, -1, 12, w3, w2, 4, 2, 1, 0, m.up1),
            OpSpec(OP_RELU, 12, -1, 12, w2, w2, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 12, 8, 13, w2, w2, 0, 0, 0, 0, None),  # enc64 と合流
            OpSpec(OP_CONV3D, 13, -1, 14, w2, w2, 3, 1, 1, 0, m.res2d),
            OpSpec(OP_RELU, 14, -1, 14, w2, w2, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 13, 14, 15, w2, w2, 0, 0, 0, 0, None),  # dec64
            OpSpec(OP_CONVTRANSPOSE3D, 15, -1, 16, w2, w1, 4, 2, 1, 0, m.up2),
            OpSpec(OP_RELU, 16, -1, 16, w1, w1, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 16, 5, 17, w1, w1, 0, 0, 0, 0, None),  # enc32 と合流
            OpSpec(OP_CONV3D, 17, -1, 18, w1, w1, 3, 1, 1, 0, m.res1d),
            OpSpec(OP_RELU, 18, -1, 18, w1, w1, 0, 0, 0, 0, None),
            OpSpec(OP_ADD, 17, 18, 19, w1, w1, 0, 0, 0, 0, None),  # dec32
            OpSpec(OP_CONV3D, 19, -1, 20, w1, w1 * 2, 3, 1, 1, 0, m.head1),
            OpSpec(OP_RELU, 20, -1, 20, w1 * 2, w1 * 2, 0, 0, 0, 0, None),
            OpSpec(OP_CONV3D, 20, -1, 21, w1 * 2, cout, 1, 1, 0, 0, m.head2),
        ]

    @property
    def buffer_count(self) -> int:
        return max(max(op.out, op.in0, op.in1) for op in self.ops) + 1

    def forward(self, x):
        bufs = {0: x}
        for op in self.ops:
            bufs[op.out] = _exec_op(op, bufs, folded=None)
        return bufs[self.ops[-1].out]

    def param_count_folded(self) -> int:
        """BN を畳んだ後の実推論パラメータ数 (spec §4.2 の paramCount。
        BN の gamma/beta/running_* は畳み込み後は使わないので数えない)。"""
        total = 0
        for op in self.ops:
            if op.module is None:
                continue
            w, b = op.module.fold()
            total += w.numel() + b.numel()
        return total


def _exec_op(op: OpSpec, bufs: dict, folded):
    """op 1 つを実行して結果を返す。`folded` が None なら `op.module` (学習中の
    Conv+BN) をそのまま呼ぶ (ModalUNet.forward)。`folded` が (weight,bias) の
    リスト (op と同じ順) なら、それを使って生の F.conv3d/F.conv_transpose3d を
    叩く (export.py の fp16 丸め後再計算、test_export.py の往復検査)。
    **forward() と export の再計算が同じこの関数を通ることで、2 つの実行経路が
    黙って乖離するのを防ぐ**。"""
    if op.type == OP_RELU:
        return F.relu(bufs[op.in0])
    if op.type == OP_ADD:
        return bufs[op.in0] + bufs[op.in1]
    if folded is not None:
        weight, bias = folded
        if op.type == OP_CONV3D:
            return F.conv3d(bufs[op.in0], weight, bias, stride=op.stride, padding=op.pad)
        if op.type == OP_CONVTRANSPOSE3D:
            return F.conv_transpose3d(bufs[op.in0], weight, bias, stride=op.stride,
                                       padding=op.pad, output_padding=op.out_pad)
        raise ValueError(f"unknown op type {op.type}")
    return op.module(bufs[op.in0])


def fold_all(ops):
    """ops と同じ順で (weight,bias) または None (ReLU/Add) のリストを返す。"""
    return [None if op.module is None else op.module.fold() for op in ops]


def round_folded_to_fp16(folded):
    """fold_all() の戻り値の重みだけ fp16 へ丸めて fp32 へ戻す (ディスク上は fp16、
    バイアスは fp32 のまま — spec §4.2「fp16 重み + fp32 バイアス」)。
    ★C++ 推論 (sub-05) が読むのは丸め後の値なので、期待値もこの丸めを経てから
    計算しないと「差が加算順だけ」にならない (sub-04.md の要求)。"""
    rounded = []
    for item in folded:
        if item is None:
            rounded.append(None)
            continue
        weight, bias = item
        weight_r = weight.half().float()
        rounded.append((weight_r, bias))
    return rounded


def run_graph(ops, folded, x):
    """folded 済み (weight,bias) を使って ops を実行する (export.py / test_export.py
    の共有経路)。folded[i] は ops[i] に対応 (None なら ReLU/Add)。"""
    bufs = {0: x}
    for op, params in zip(ops, folded):
        bufs[op.out] = _exec_op(op, bufs, folded=params)
    return bufs[ops[-1].out]


def voxel_to_tensor(occ: np.ndarray) -> torch.Tensor:
    """meshio.read_mvox() が返す occ[x,y,z] (uint8, 32^3) を、モデル入力の軸規約
    (N=1,C=1,D=z,H=y,W=x) の float32 テンソルへ変換する (モジュール docstring の
    「軸並び規約」を参照。train.py / export.py はここだけを通す)。"""
    zyx = np.transpose(occ, (2, 1, 0)).astype(np.float32)
    return torch.from_numpy(np.ascontiguousarray(zyx)).unsqueeze(0).unsqueeze(0)


def output_to_cell_grid(out: torch.Tensor) -> np.ndarray:
    """モデル出力 (1,192,16,16,16、軸は D=cz,H=cy,W=cx) を (16,16,16,192) の
    [cx,cy,cz,channel] レイアウトへ戻す (dataset.py の valid/feat と同じ添字順)。"""
    arr = out.detach().cpu().numpy()[0]  # (192,16,16,16) = [c,cz,cy,cx]
    return np.transpose(arr, (3, 2, 1, 0))  # -> [cx,cy,cz,c]


def cell_dense_to_tensor(dense) -> torch.Tensor:
    """[cx,cy,cz] または [cx,cy,cz,C] の numpy 配列 (dataset.py の valid/feat と
    同じ添字順) を、モデル出力と同じ軸規約 (D=cz,H=cy,W=cx、チャンネルは axis0) の
    バッチ次元 1 のテンソルへ変換する (`output_to_cell_grid` の逆変換。train.py の
    教師データ組み立てが唯一の呼び出し元)。"""
    dense = np.asarray(dense)
    if dense.ndim == 3:
        zyx = np.transpose(dense, (2, 1, 0))
        return torch.from_numpy(np.ascontiguousarray(zyx)).unsqueeze(0).unsqueeze(0).float()
    czyx_c = np.transpose(dense, (2, 1, 0, 3))  # [cz,cy,cx,c]
    c_first = np.moveaxis(czyx_c, -1, 0)  # [c,cz,cy,cx]
    return torch.from_numpy(np.ascontiguousarray(c_first)).unsqueeze(0).float()
