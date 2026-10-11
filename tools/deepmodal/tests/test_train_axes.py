#====================================================================================
#                          test_train_axes.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          train.py の入力ボクセルと教師の軸が揃っているか
#====================================================================================
"""npz の `vox` は occ[x,y,z]、モデル入力は (D=z,H=y,W=x) (model.voxel_to_tensor が唯一の
変換点)。f17ad63 で ModalNpzDataset / load_samples の入力側だけこの変換が落ち、教師
(z,y,x 順) と入力 (x,y,z 順) が食い違った状態で学習していた。ここでは非対称な占有を
npz に書き、両方の読み込み経路が voxel_to_tensor と同じテンソルを返すことを固定する。"""
import numpy as np
import torch

import layout
import model
import train


def _write_npz(path, occ):
    """valid 1 セル・mask 全 1 の最小 npz (train.py が読むフィールドだけ)。"""
    valid = np.zeros((layout.MAP_N,) * 3, dtype=np.uint8)
    valid[3, 5, 7] = 1
    feat = np.zeros((1, layout.CHANNELS), dtype=np.float16)
    for j in range(3):
        for i in range(layout.MEL_BANDS):
            feat[0, layout.mask_ch(j, i)] = 1.0
            feat[0, layout.amp_ch(j, i)] = -10.0 + i * 0.1
    np.savez_compressed(
        path, vox=occ, valid=valid, feat=feat,
        residual_max=np.array(1e-8), coverage_ratio=np.array(0.5), coverage_high=np.array(0.5))


def _asymmetric_occ():
    """x/y/z の入れ替えで必ず変わる占有 (各軸で違う長さの直方体)。"""
    occ = np.zeros((layout.VOXEL_N,) * 3, dtype=np.uint8)
    occ[2:5, 6:14, 10:30] = 1
    return occ


def test_npz_dataset_vox_matches_voxel_to_tensor(tmp_path):
    occ = _asymmetric_occ()
    npz = tmp_path / "asym#mesh0#prim0.npz"
    _write_npz(npz, occ)
    entries = train.load_dataset([str(tmp_path)])
    stats = train.compute_amp_stats_from_entries(entries)

    vox_t, _target, _valid, _w = train.ModalNpzDataset(entries, stats)[0]
    expected = model.voxel_to_tensor(occ)[0]
    assert vox_t.shape == expected.shape == (1, layout.VOXEL_N, layout.VOXEL_N, layout.VOXEL_N)
    assert torch.equal(vox_t, expected)
    # 転置が落ちると (D,H,W)=(x,y,z) になり、この非対称形状では必ず違う
    assert not torch.equal(vox_t, torch.from_numpy(occ.astype(np.float32)).unsqueeze(0))


def test_load_samples_vox_matches_voxel_to_tensor(tmp_path):
    occ = _asymmetric_occ()
    _write_npz(tmp_path / "asym#mesh0#prim0.npz", occ)
    entries = train.load_dataset([str(tmp_path)])
    samples, _stats = train.load_samples(entries)

    expected = model.voxel_to_tensor(occ).to(torch.uint8)
    assert samples[0].vox.dtype == torch.uint8
    assert torch.equal(samples[0].vox, expected)
