#====================================================================================
#                          train.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          npz データセットからの学習ドライバと overfit の門
#====================================================================================
"""dataset.py が書いた npz (vox/valid/feat + solver metadata + convergence quality +
Mel-band coverage) を読み、`ModalUNet` (model.py) を学習する。

★品質指標を読んで除外・重み付けできること (spec §4.1「品質指標」、ユーザー指示)。
判断軸は **(1) Mel-band coverage と (2) residual 品質の 2 つだけ** —
`modes_requested` / `f_top` / `mode_count` / `method` は診断・表示用のメタデータで、
これらでは分岐しない (README.md / dataset.py のコメントと同じ原則)。既定は全部 off
(= 全サンプル同じ重み) だが、off でも読めていることが要件 (sub-04.md)。

★overfit の門 (`--overfit N --epochs 300`): このリポジトリでは**ここを越えるまで
ModelNet の生成コマンドを実行しない** (README / spec §4.1)。**判定は
`mask acc > 99%` + `R² = 1 − MSE/Var(target)`** (spec §5 #9 round 2)。
旧 `amp MSE < 1e-3` は撤回済み (planner が根拠なく置いた値で、目標場の表現上の
下限を下回っていた — README「overfit の門」参照)。R² の閾値は未確定なので
`main()` は mask acc だけを assert し、R² は report only (自己判定しない)。
"""
import argparse
import statistics
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

import numpy as np
import torch
import torch.nn.functional as F

import layout
import model
import modal as modal_solver  # RESIDUAL_DROP (--quality-weight の重み計算に使う)

# feat の 192 チャンネルのうち mask/amp それぞれに属する index (layout.mask_ch/amp_ch と
# 同じ式の実体化。互いに素な集合で 192 = 96+96 をちょうど覆う)
MASK_CHANNELS = np.array([layout.mask_ch(j, i) for j in range(3) for i in range(layout.MEL_BANDS)],
                          dtype=np.int64)
AMP_CHANNELS = np.array([layout.amp_ch(j, i) for j in range(3) for i in range(layout.MEL_BANDS)],
                         dtype=np.int64)


@dataclass
class Sample:
    name: str
    vox: torch.Tensor      # (1,1,32,32,32)
    target: torch.Tensor   # (1,192,16,16,16) — mask ch は 0/1、amp ch は正規化済み ln
    valid: torch.Tensor    # (1,1,16,16,16) — 1.0 = 有効 cell
    weight: float
    residual_max: Optional[float]
    coverage_ratio: float
    coverage_high: float


@dataclass
class DatasetStats:
    log_amp_min: float
    log_amp_max: float


def densify(npz_data) -> tuple:
    """npz の valid/feat を (16,16,16) の bool と (16,16,16,192) の float32 へ展開する
    ([cx,cy,cz(,c)] 添字。layout.cell_order() が dataset.py と共有する唯一の順序)。"""
    valid = npz_data["valid"].astype(bool)
    feat = npz_data["feat"].astype(np.float32)
    dense = np.zeros((layout.MAP_N, layout.MAP_N, layout.MAP_N, layout.CHANNELS), dtype=np.float32)
    idx = 0
    for (cx, cy, cz) in layout.cell_order():
        if valid[cx, cy, cz]:
            dense[cx, cy, cz, :] = feat[idx]
            idx += 1
    if idx != feat.shape[0]:
        raise ValueError(f"valid cell count mismatch: consumed {idx}, feat has {feat.shape[0]} rows")
    return valid, dense


def compute_amp_stats(dense_list: List[np.ndarray], valid_list: List[np.ndarray]) -> DatasetStats:
    """訓練に使うデータセット全体から ln(amp) の範囲 (logAmpMin/Max) を決める
    (README.md: 「0..1 への正規化は sub-04 の export.py 側でデータセット全体の統計から
    決める」)。**mask=1 (= 実データ、空帯域補間で埋めた値ではない) の位置だけ**を見る
    — 補間値まで含めると、実際には励起されていない帯域の値が範囲を歪める。"""
    values = []
    for dense, valid in zip(dense_list, valid_list):
        for j in range(3):
            for i in range(layout.MEL_BANDS):
                mask_col = dense[..., layout.mask_ch(j, i)] > 0.5
                sel = mask_col & valid
                if np.any(sel):
                    values.append(dense[..., layout.amp_ch(j, i)][sel])
    if not values:
        # 全サンプルが無音 (mask 全 0) という異常系。0 除算を避けるためだけの
        # フォールバックで、この経路を通ったら学習結果は意味を持たない
        return DatasetStats(log_amp_min=-1.0, log_amp_max=1.0)
    all_vals = np.concatenate(values)
    lo = float(np.min(all_vals))
    hi = float(np.max(all_vals))
    if hi <= lo:
        hi = lo + 1.0
    return DatasetStats(log_amp_min=lo, log_amp_max=hi)


def build_target(dense: np.ndarray, stats: DatasetStats) -> np.ndarray:
    """amp チャンネルだけを [0,1] へ正規化する (mask チャンネルは 0/1 のまま)。"""
    target = dense.copy()
    scale = stats.log_amp_max - stats.log_amp_min
    for j in range(3):
        for i in range(layout.MEL_BANDS):
            ch = layout.amp_ch(j, i)
            target[..., ch] = np.clip((dense[..., ch] - stats.log_amp_min) / scale, 0.0, 1.0)
    return target


def _quality_weight(residual_max: Optional[float], coverage_ratio: float) -> float:
    """--quality-weight のときのサンプル重み (spec §4.1 / sub-04.md「重みを下げる」)。
    ★しきい値と同じく暫定 — 判断軸は residual_max と coverage_ratio の 2 つだけ
    (mode count / method では分岐しない、ユーザー指示)。
    残差が RESIDUAL_DROP に近いほど、coverage が低いほど重みを下げる。両方とも
    下限 0.1 で床を張る (完全に無視すると、その形状の情報が学習から消えてしまうため)。
    数式そのものは spec が定めていない coder の実装判断 — 実際のしきい値/式は
    M76h で分布を見て再調整してよい。"""
    w_residual = 1.0
    if residual_max is not None and residual_max > 0.0:
        w_residual = max(0.1, min(1.0, 1.0 - residual_max / modal_solver.RESIDUAL_DROP))
    w_coverage = max(0.1, min(1.0, coverage_ratio))
    return w_residual * w_coverage


def load_dataset(data_dirs: List[str],
                  quality_max_residual: Optional[float] = None,
                  quality_min_coverage: Optional[float] = None,
                  quality_min_high_coverage: Optional[float] = None,
                  quality_weight: bool = False) -> List[dict]:
    """--data の各ディレクトリから *.npz を集め、品質フィルタ (既定 off) を適用する。
    戻り値は npz のパスと品質メタデータ (フィルタ・重み計算・ログに使う) の dict リスト
    (実データはこの後 `load_samples` が読む — フィルタで落ちる分の I/O を避けるため
    ここでは軽量スカラーだけ読む)。"""
    paths = []
    for d in data_dirs:
        d = Path(d)
        if not d.exists():
            print(f"[train] WARNING: --data ディレクトリが見つかりません (skip): {d}")
            continue
        found = sorted(d.glob("*.npz"))
        if not found:
            print(f"[train] WARNING: npz が 1 件もありません: {d}")
        paths.extend(found)

    entries = []
    excluded = 0
    residual_maxes = []
    for p in paths:
        with np.load(p, allow_pickle=False) as data:
            raw_residual_max = float(data["residual_max"])
            residual_max = raw_residual_max if raw_residual_max >= 0.0 else None
            coverage_ratio = float(data["coverage_ratio"])
            coverage_high = float(data["coverage_high"])
        if residual_max is not None:
            residual_maxes.append(residual_max)

        drop = False
        if quality_max_residual is not None and residual_max is not None \
                and residual_max > quality_max_residual:
            drop = True
        if quality_min_coverage is not None and coverage_ratio < quality_min_coverage:
            drop = True
        if quality_min_high_coverage is not None and coverage_high < quality_min_high_coverage:
            drop = True
        if drop:
            excluded += 1
            continue

        weight = _quality_weight(residual_max, coverage_ratio) if quality_weight else 1.0
        entries.append({
            "path": p, "name": p.stem, "residual_max": residual_max,
            "coverage_ratio": coverage_ratio, "coverage_high": coverage_high,
            "weight": weight,
        })

    # ★学習ログに「除外した npz 数 / 残差の分布」を 1 行 (sub-04.md の要求。
    # 品質フィルタで門の判定が変わったのかを後から切り分けられるように)
    if residual_maxes:
        print(f"[train] loaded {len(entries)} npz (excluded {excluded} of {len(paths)} "
              f"by quality filter); residual_max: min={min(residual_maxes):.3e} "
              f"median={statistics.median(residual_maxes):.3e} max={max(residual_maxes):.3e}")
    else:
        print(f"[train] loaded {len(entries)} npz (excluded {excluded} of {len(paths)} "
              f"by quality filter); residual データなし")
    return entries


def load_samples(entries: List[dict]) -> tuple:
    """load_dataset() が選んだエントリを実際に読み、Sample のリストと
    データセット統計 (logAmpMin/Max) を組み立てる。"""
    dense_list = []
    valid_list = []
    for e in entries:
        with np.load(e["path"], allow_pickle=False) as data:
            valid, dense = densify(data)
        dense_list.append(dense)
        valid_list.append(valid)

    stats = compute_amp_stats(dense_list, valid_list)

    samples = []
    for e, dense, valid in zip(entries, dense_list, valid_list):
        target = build_target(dense, stats)
        samples.append(Sample(
            name=e["name"],
            vox=None,  # 呼び出し側 (main) が対応する .mvox 相当を持たないため、
                       # vox は npz 自身の "vox" フィールドから別途埋める (下記)
            target=model.cell_dense_to_tensor(target),
            valid=model.cell_dense_to_tensor(valid.astype(np.float32)),
            weight=e["weight"],
            residual_max=e["residual_max"],
            coverage_ratio=e["coverage_ratio"],
            coverage_high=e["coverage_high"],
        ))
    # vox は npz に "vox" (32,32,32 uint8、meshio.read_mvox と同じ [x,y,z] 添字) として
    # そのまま入っている (dataset.py が保存)。ここで詰め直す
    for s, e in zip(samples, entries):
        with np.load(e["path"], allow_pickle=False) as data:
            occ = data["vox"]
        s.vox = model.voxel_to_tensor(occ)
    return samples, stats


def select_overfit_subset(samples: List[Sample], n: int) -> List[Sample]:
    """`--overfit N` 用に N 形状を選ぶ。alphabetical 先頭 N だと似た名前 (box_0/box_2/...)
    に偏るため、sorted リスト全体へ等間隔にインデックスを取って形状の種類を散らす
    (決定論的 — 乱数は使わない)。"""
    if n >= len(samples):
        return list(samples)
    idxs = sorted(set(int(round(x)) for x in np.linspace(0, len(samples) - 1, n)))
    if len(idxs) < n:
        remaining = [i for i in range(len(samples)) if i not in idxs]
        idxs = sorted(idxs + remaining[: n - len(idxs)])
    return [samples[i] for i in idxs[:n]]


def compute_batch_losses(pred, target, valid, weights):
    """バッチ 1 回分の重み付き mse/bce/acc を返す ((loss, mse, bce, acc)、loss だけ
    勾配を持つ)。サンプルごとに**有効 cell 数で先に正規化**してからサンプル間を
    weights で重み付き平均する — 有効 cell 数が多い形状 (大きい/複雑な形状) に
    勾配が引っ張られるのを防ぐ。"""
    b = pred.shape[0]
    valid96 = valid.expand(-1, 96, -1, -1, -1)
    mask_pred = pred[:, MASK_CHANNELS]
    mask_target = target[:, MASK_CHANNELS]
    amp_pred = pred[:, AMP_CHANNELS]
    amp_target = target[:, AMP_CHANNELS]
    denom = valid96.reshape(b, -1).sum(dim=1).clamp(min=1.0)

    bce_raw = F.binary_cross_entropy_with_logits(mask_pred, mask_target, reduction="none")
    mse_raw = F.mse_loss(amp_pred, amp_target, reduction="none")
    bce_per_sample = (bce_raw * valid96).reshape(b, -1).sum(dim=1) / denom
    mse_per_sample = (mse_raw * valid96).reshape(b, -1).sum(dim=1) / denom

    with torch.no_grad():
        pred_bin = (torch.sigmoid(mask_pred) > 0.5).float()
        target_bin = (mask_target > 0.5).float()
        correct = ((pred_bin == target_bin).float() * valid96).reshape(b, -1).sum(dim=1)
        acc_per_sample = correct / denom

    w_sum = weights.sum().clamp(min=1e-8)
    w_norm = weights / w_sum
    mse = (mse_per_sample * w_norm).sum()
    bce = (bce_per_sample * w_norm).sum()
    acc = (acc_per_sample * w_norm).sum().detach()
    loss = mse + bce
    return loss, mse.detach(), bce.detach(), acc


def compute_r2(pred, target, valid) -> tuple:
    """amp の説明率 `R² = 1 - MSE/Var(target)` (spec §5 #9 round 2、旧 `amp MSE<1e-3`
    の撤回後の指標)。**有効 cell の amp チャンネルだけをプールした単純平均/分散**で
    計算する (`compute_batch_losses` のサンプルごとに正規化してから重み平均する
    MSE とは定義を分けている — R² は「この母集団の分散をどれだけ説明できたか」を
    素朴に見るための指標なので、学習損失の重み付けを持ち込まない)。
    戻り値: (mse, var_target, r2)。"""
    valid96 = valid.expand(-1, 96, -1, -1, -1).bool()
    amp_pred = pred[:, AMP_CHANNELS]
    amp_target = target[:, AMP_CHANNELS]
    vals_pred = amp_pred[valid96]
    vals_target = amp_target[valid96]
    mse = torch.mean((vals_pred - vals_target) ** 2)
    var = torch.var(vals_target, unbiased=False)
    r2 = 1.0 - float(mse) / max(float(var), 1e-12)
    return float(mse), float(var), r2


def run_overfit_lbfgs(net, samples: List[Sample], epochs: int, lbfgs_max_iter: int,
                       device) -> tuple:
    """`--overfit` 専用の学習ループ (coder 判断、[逸脱] — sub-04.md は最適化手法を
    指定していない。README「overfit の実測」に実測ログを残す)。

    ★実測 (2026-09-16、16 形状、widths=(16,32,64,96)): Adam は学習率を 1e-4〜2e-2、
    lr 半減あり/なし、batch_size 1/4/16、モデル幅 1x/4x、head 幅 4x のどの組み合わせで
    試しても 300 step 前後で amp MSE が 0.002〜0.009 に頭打ちした (mask 側は
    どの設定でもほぼ 99%+ に収束する — 頭打ちは amp 回帰だけの問題)。LBFGS
    (2 次法、決定論的フルバッチ) に替えると同じ step 数でより低く/速く収束する。
    16 形状程度の小規模・決定論的な「記憶させたいだけ」の最適化には、確率的勾配法より
    LBFGS の方が素直に適合する。
    `--epochs` は LBFGS の外側ステップ数として扱う (1 外側ステップ = 内部で最大
    `lbfgs_max_iter` 回の line-search 付き反復)。
    ★round 2 (planner の切り分け): 旧 `amp MSE<1e-3` の頭打ちは実装の不備ではなく
    目標場自体の cell 単位ジッタ (畳み込みの並進同変性では区別できない受容野内成分)
    による**表現上の下限**だった。門は `mask acc>99%` + `R²=1-MSE/Var(target)` に
    組み替え済み (spec §5 #9)。戻り値に mse/var_target/r2/bce/acc を含める。"""
    net.train()
    vox = torch.cat([s.vox for s in samples]).to(device)
    target = torch.cat([s.target for s in samples]).to(device)
    valid = torch.cat([s.valid for s in samples]).to(device)
    weights = torch.tensor([s.weight for s in samples], dtype=torch.float32, device=device)

    opt = torch.optim.LBFGS(net.parameters(), lr=1.0, max_iter=lbfgs_max_iter,
                             history_size=10, line_search_fn="strong_wolfe")
    state = {}

    def closure():
        opt.zero_grad()
        net.train()
        pred = net(vox)
        loss, mse, bce, acc = compute_batch_losses(pred, target, valid, weights)
        loss.backward()
        state["mse"] = mse
        state["bce"] = bce
        state["acc"] = acc
        return loss

    for epoch in range(1, epochs + 1):
        opt.step(closure)
        if epoch % 20 == 0 or epoch in (1, epochs):
            print(f"[train] epoch {epoch}/{epochs} (LBFGS max_iter={lbfgs_max_iter}) "
                  f"amp_mse={float(state['mse']):.6f} mask_bce={float(state['bce']):.6f} "
                  f"mask_acc={float(state['acc']) * 100:.3f}%")

    net.eval()
    with torch.no_grad():
        pred = net(vox)
        _, mse_weighted, bce, acc = compute_batch_losses(pred, target, valid, weights)
        mse, var_target, r2 = compute_r2(pred, target, valid)
    print(f"[train] overfit report: amp_mse={mse:.6f} var_target={var_target:.6f} "
          f"R²={r2:.4f} mask_acc={float(acc) * 100:.3f}% "
          f"(参考: サンプル正規化 mse={float(mse_weighted):.6f})")
    return mse, var_target, r2, float(bce), float(acc)


def evaluate_pooled(net, samples: List[Sample], device, chunk: int = 16) -> tuple:
    """データセット全体をプールした (サンプル毎正規化を経由しない素朴な) mse/var/R²/mask_acc を
    返す (spec §5 #19、round 2 の must #1)。`compute_batch_losses` はサンプルごとに有効 cell 数で
    正規化してから重み平均するので、大きい/小さい形状の寄与を均等にする学習損失としては正しいが、
    「データセット全体の分散をどれだけ説明できたか」を読む指標としては別物 — この関数は
    `compute_r2` と同じプール定義を**データセット全体**に対して計算する (1 サンプルだけの
    `compute_r2` 呼び出しと同じ式を、全サンプルを 1 つの大きなバッチとして扱って適用するだけ。
    2 本目の式は書いていない)。GPU メモリを避けるため forward は `chunk` 件ずつに分けるが、
    集計は全チャンクの予測/目標を連結してから行う (チャンクごとの平均の平均ではない —
    チャンクごとにサンプル数が違うと歪むため)。"""
    net.eval()
    preds, targets, valids = [], [], []
    with torch.no_grad():
        for start in range(0, len(samples), chunk):
            group = samples[start:start + chunk]
            vox = torch.cat([s.vox for s in group]).to(device)
            pred = net(vox)
            preds.append(pred.cpu())
            targets.append(torch.cat([s.target for s in group]))
            valids.append(torch.cat([s.valid for s in group]))
    pred_all = torch.cat(preds)
    target_all = torch.cat(targets)
    valid_all = torch.cat(valids)
    mse, var, r2 = compute_r2(pred_all, target_all, valid_all)
    valid96 = valid_all.expand(-1, 96, -1, -1, -1).bool()
    mask_pred = pred_all[:, MASK_CHANNELS]
    mask_target = target_all[:, MASK_CHANNELS]
    pred_bin = (torch.sigmoid(mask_pred) > 0.5).float()
    target_bin = (mask_target > 0.5).float()
    acc = float(((pred_bin == target_bin).float())[valid96].mean())
    return mse, var, r2, acc


def run_epoch(net, samples: List[Sample], batch_size: int, opt, device, train: bool) -> tuple:
    n = len(samples)
    order = np.random.permutation(n) if train else np.arange(n)
    net.train(train)
    total_mse = total_bce = total_acc = total_n = 0.0
    for start in range(0, n, batch_size):
        idxs = order[start:start + batch_size]
        vox = torch.cat([samples[i].vox for i in idxs]).to(device)
        target = torch.cat([samples[i].target for i in idxs]).to(device)
        valid = torch.cat([samples[i].valid for i in idxs]).to(device)
        weights = torch.tensor([samples[i].weight for i in idxs], dtype=torch.float32, device=device)
        if train:
            opt.zero_grad()
            pred = net(vox)
            loss, mse, bce, acc = compute_batch_losses(pred, target, valid, weights)
            loss.backward()
            opt.step()
        else:
            with torch.no_grad():
                pred = net(vox)
                _, mse, bce, acc = compute_batch_losses(pred, target, valid, weights)
        bsz = len(idxs)
        total_mse += float(mse) * bsz
        total_bce += float(bce) * bsz
        total_acc += float(acc) * bsz
        total_n += bsz
    return total_mse / total_n, total_bce / total_n, total_acc / total_n


def main():
    ap = argparse.ArgumentParser(description="Deep-Modal 学習ドライバ")
    ap.add_argument("--data", nargs="+", default=["data/stage0"])
    # ★round 2 の must #1: 旧既定 (epochs=100, lr_halve_every=20, 下限なし) は
    # 124 サンプル/batch16 = 8 step/epoch だと 800 step にしかならず、しかも 100 epoch 時点で
    # lr=3.1e-5 まで落ちて Adam が実質止まっていた (pooled R²=-0.25、定数モデルより悪い)。
    # epochs=1500 / lr_halve_every=150 / lr_min=5e-5 で実測 pooled R²=0.63 まで伸びることを
    # 確認したので、これを既定にする (stage0+stage1、124 サンプルでの実測。ModelNet10 は
    # サンプル数が 2 桁大きいので epoch あたりの step 数が増える — 既定のまま回してよいが、
    # `--eval-every` の pooled R² ログを必ず見て、正で頭打ちになっているかを確認すること。
    # 増やしても伸びなければ学習曲線を添えて報告する、閾値は固定しない)
    ap.add_argument("--epochs", type=int, default=1500)
    ap.add_argument("--lr", type=float, default=1e-3, help="既定 1e-3。論文値 0.02 は --lr 0.02")
    ap.add_argument("--lr-halve-every", type=int, default=150)
    ap.add_argument("--lr-min", type=float, default=5e-5,
                     help="半減の下限 (round 2 の must #1: 旧既定は下限が無く underfit していた)")
    ap.add_argument("--batch-size", type=int, default=16)
    ap.add_argument("--eval-every", type=int, default=50,
                     help="pooled R² (spec §5 #19) を測って学習曲線へ足す間隔 (epoch)")
    ap.add_argument("--overfit", type=int, default=None,
                     help="N 形状へ過学習させ、amp MSE<1e-3 & mask acc>99% を assert する (大規模生成の門)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--quality-max-residual", type=float, default=None)
    ap.add_argument("--quality-min-coverage", type=float, default=None)
    ap.add_argument("--quality-min-high-coverage", type=float, default=None)
    ap.add_argument("--quality-weight", action="store_true")
    ap.add_argument("--widths", type=int, nargs=4, default=[16, 32, 64, 96],
                     metavar=("STEM", "W1", "W2", "W3"))
    ap.add_argument("--no-bn", action="store_true",
                     help="BatchNorm3d を使わない (README「overfit の実測」参照。既定は使う)")
    ap.add_argument("--lbfgs-max-iter", type=int, default=20,
                     help="--overfit 時の LBFGS 1 outer step あたりの内部反復上限")
    ap.add_argument("--out", default="runs/checkpoint.pt")
    ap.add_argument("--device", default=None)
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)
    device = args.device or ("cuda" if torch.cuda.is_available() else "cpu")
    print(f"[train] device={device}")

    entries = load_dataset(args.data, quality_max_residual=args.quality_max_residual,
                            quality_min_coverage=args.quality_min_coverage,
                            quality_min_high_coverage=args.quality_min_high_coverage,
                            quality_weight=args.quality_weight)
    if not entries:
        print("[train] ERROR: 読み込める npz が 0 件です", file=sys.stderr)
        sys.exit(1)
    samples, stats = load_samples(entries)
    print(f"[train] log_amp_min={stats.log_amp_min:.4f} log_amp_max={stats.log_amp_max:.4f}")

    if args.overfit is not None:
        samples = select_overfit_subset(samples, args.overfit)
        print(f"[train] --overfit {args.overfit}: {[s.name for s in samples]}")

    net = model.ModalUNet(widths=tuple(args.widths), use_bn=not args.no_bn).to(device)
    print(f"[train] param_count_folded={net.param_count_folded()}")

    var_target = None
    r2 = None
    if args.overfit is not None:
        # --overfit は Adam ではなく LBFGS で回す (run_overfit_lbfgs の docstring に
        # 実測根拠。coder 判断、[逸脱])
        mse, var_target, r2, bce, acc = run_overfit_lbfgs(
            net, samples, args.epochs, args.lbfgs_max_iter, device)
    else:
        # ★round 2 の must #1: 124 サンプル/batch16 = 8 step/epoch だと 100 epoch でも
        # 800 step にしかならず、旧既定 (20 epoch ごと半減、下限なし) は終盤 lr=3.1e-5 まで
        # 落ちて Adam が実質止まっていた (= underfit)。`--lr-min` で下限を設け、
        # `--eval-every` で pooled R² (spec §5 #19、compute_r2 と同じプール定義を
        # データセット全体へ適用したもの、evaluate_pooled) を学習曲線に足して
        # 「サンプル毎正規化の amp_mse が下がっていても pooled R² が伸びているとは限らない」
        # という round 1 の見落としを再発させない
        opt = torch.optim.Adam(net.parameters(), lr=args.lr)
        for epoch in range(1, args.epochs + 1):
            if epoch > 1 and (epoch - 1) % args.lr_halve_every == 0:
                for g in opt.param_groups:
                    g["lr"] = max(g["lr"] * 0.5, args.lr_min)
            mse, bce, acc = run_epoch(net, samples, args.batch_size, opt, device, train=True)
            if epoch % 20 == 0 or epoch in (1, args.epochs):
                lr_now = opt.param_groups[0]["lr"]
                print(f"[train] epoch {epoch}/{args.epochs} lr={lr_now:.2e} "
                      f"amp_mse={mse:.6f} mask_bce={bce:.6f} mask_acc={acc * 100:.3f}%")
            if epoch % args.eval_every == 0 or epoch == args.epochs:
                p_mse, p_var, p_r2, p_acc = evaluate_pooled(net, samples, device)
                print(f"[train] epoch {epoch}/{args.epochs} pooled: amp_mse={p_mse:.6f} "
                      f"var_target={p_var:.6f} R²={p_r2:.4f} mask_acc={p_acc * 100:.3f}%")

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    torch.save({
        "state_dict": net.state_dict(),
        "widths": list(net.widths),
        "log_amp_min": stats.log_amp_min,
        "log_amp_max": stats.log_amp_max,
    }, out_path)
    print(f"[train] checkpoint saved: {out_path}")

    if args.overfit is not None:
        # ★spec §5 #9 round 2: 旧 `amp MSE<1e-3` は撤回済み (planner が根拠なく
        # 置いた値で、目標場の表現上の下限を下回っていた)。閾値はサイズ漏れ修正後の
        # 再計測で planner が確定する — ここでは R² を自己判定の合否に使わず、
        # 観測値をそのまま報告するだけにとどめる (司会指示: 「閾値の自己判定で
        # 合否を出さなくてよい」)。mask acc だけは確定済みの基準なので assert する
        print(f"[train] overfit report: amp_mse={mse:.6f} var_target={var_target:.6f} "
              f"R²={r2:.4f} mask_acc={acc * 100:.3f}% "
              f"(--overfit {args.overfit}, epochs={args.epochs}, "
              f"lbfgs_max_iter={args.lbfgs_max_iter})")
        if not (acc > 0.99):
            print(f"[train] overfit gate FAILED: mask acc {acc * 100:.3f}% <= 99%", file=sys.stderr)
            sys.exit(1)
        print("[train] mask acc 条件 (>99%) は満たした。amp R² は閾値未確定のため "
              "report only (planner が §8 で確定するまで自己判定しない)")
    else:
        # spec §5 #19 (round 2 の must #1): 本学習でも pooled R² を必ず報告する。
        # `run_epoch` が返す amp_mse はサンプル毎正規化 + 重み平均 (compute_batch_losses) で、
        # 「データセット全体の分散をどれだけ説明できたか」を読む指標ではない —
        # ここは必ず `evaluate_pooled` (compute_r2 と同じプール定義) を使う
        p_mse, p_var, p_r2, p_acc = evaluate_pooled(net, samples, device)
        print(f"[train] final report: pooled amp_mse={p_mse:.6f} var_target={p_var:.6f} "
              f"R²={p_r2:.4f} mask_acc={p_acc * 100:.3f}% (epochs={args.epochs}, "
              f"lr={args.lr}, lr_halve_every={args.lr_halve_every}, lr_min={args.lr_min}, "
              f"batch_size={args.batch_size}, n_samples={len(samples)})")
        if p_r2 <= 0.0:
            print(f"[train] WARNING: pooled R²={p_r2:.4f} <= 0 -- この .dmnet は「平均を返すだけの"
                  "定数モデル」より悪い。エンジン既定の資産としてコミットしないこと "
                  "(spec §5 #19)。学習曲線 (上の pooled ログ) を添えて報告し、"
                  "planner の裁定を仰ぐこと", file=sys.stderr)


if __name__ == "__main__":
    main()
