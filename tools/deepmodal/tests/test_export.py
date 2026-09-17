#====================================================================================
#                          test_export.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          .dmnet 書き出し (export.py) の単体テスト
#====================================================================================
import struct

import numpy as np
import pytest
import torch

import export
import layout
import model


def test_fnv1a64_known_vectors():
    """FNV-1a 64bit の既知テストベクタ (http://www.isthe.com/chongo/tech/comp/fnv/)。
    実装を疑うより先に typo を潰す (deep-reasoning の「簡単な説明から検証する」)。"""
    assert export.fnv1a64(b"") == 0xCBF29CE484222325
    assert export.fnv1a64(b"a") == 0xAF63DC4C8601EC8C


def test_header_bytes_is_256():
    assert layout.DMNET_HEADER_BYTES == 256
    assert struct.calcsize(layout.DMNET_HEADER_FMT) == 256


def test_random_full_roundtrip(tmp_path):
    """build_dmnet_bytes → 書き出し → read_dmnet で読み戻し、op 表と重みが
    (fp16 丸め後の値と) 一致することを確認する (sub-04.md 受け入れ条件 #2)。"""
    torch.manual_seed(123)
    net = model.ModalUNet(widths=(16, 32, 64, 96))
    net.eval()
    data = export.build_dmnet_bytes(net, log_amp_min=-10.0, log_amp_max=0.0,
                                     amp_scale=1.0, mask_threshold=0.5)
    path = tmp_path / "roundtrip.dmnet"
    path.write_bytes(data)

    header, ops_raw, raw = export.read_dmnet(path)
    assert header["magic"] == layout.DMNET_MAGIC
    assert header["version"] == layout.DMNET_VERSION
    assert header["op_count"] == len(net.ops)
    assert header["buffer_count"] == net.buffer_count
    assert header["in_n"] == layout.VOXEL_N
    assert header["out_n"] == layout.MAP_N
    assert header["bands"] == layout.MEL_BANDS
    assert header["channels"] == layout.CHANNELS
    assert header["param_count"] == net.param_count_folded()
    np.testing.assert_allclose(
        [header[f"band_center_{i}"] for i in range(layout.MEL_BANDS)],
        layout.mel_band_centers(), atol=1e-2)

    folded_expected = model.round_folded_to_fp16(model.fold_all(net.ops))
    folded_from_file = export.load_op_weights(header, ops_raw, raw)
    for op, expected, got in zip(net.ops, folded_expected, folded_from_file):
        if expected is None:
            assert got is None
            continue
        w_exp, b_exp = expected
        w_got, b_got = got
        assert w_got.shape == w_exp.shape
        assert b_got.shape == b_exp.shape
        # 重みは fp16 に丸めてから書いているので厳密一致 (ビット一致)
        assert torch.equal(w_got, w_exp)
        assert torch.equal(b_got, b_exp)

    # weightsHash は書き出したバイト列 (重み+バイアス blob) から計算した値と一致する
    ops_start = layout.DMNET_HEADER_BYTES
    ops_bytes_len = header["op_count"] * layout.DMNET_OP_BYTES
    blob = raw[ops_start + ops_bytes_len:]
    assert header["weights_hash"] == export.fnv1a64(blob)


def test_op_table_matches_model_graph(tmp_path):
    """op 表の type/in0/in1/cin/cout/k/stride/pad/outPad が model.py のグラフ定義と
    1 対 1 で一致する (エクスポートの取りこぼしがないことの検査)。"""
    torch.manual_seed(0)
    net = model.ModalUNet(widths=(4, 8, 8, 8))
    net.eval()
    data = export.build_dmnet_bytes(net, log_amp_min=-1.0, log_amp_max=1.0,
                                     amp_scale=1.0, mask_threshold=0.5)
    path = tmp_path / "small.dmnet"
    path.write_bytes(data)
    header, ops_raw, _ = export.read_dmnet(path)
    assert len(ops_raw) == len(net.ops)
    for expected, got in zip(net.ops, ops_raw):
        assert got["type"] == expected.type
        assert got["in0"] == expected.in0
        assert got["in1"] == expected.in1
        assert got["out"] == expected.out
        assert got["cin"] == expected.cin
        assert got["cout"] == expected.cout
        assert got["k"] == expected.k
        assert got["stride"] == expected.stride
        assert got["pad"] == expected.pad
        assert got["out_pad"] == expected.out_pad
        has_module = expected.module is not None
        assert (got["weight_offset"] != layout.DMNET_OFFSET_NONE) == has_module
        assert (got["bias_offset"] != layout.DMNET_OFFSET_NONE) == has_module


def test_param_count_budget_assert():
    """paramCount が MAX_PARAM_COUNT を超えたら build_dmnet_bytes が拒否する。"""
    torch.manual_seed(0)
    # 基準構成の 4 倍近い幅にして予算超過を作為的に発生させる
    net = model.ModalUNet(widths=(32, 64, 128, 192))
    net.eval()
    assert net.param_count_folded() > model.MAX_PARAM_COUNT
    with pytest.raises(AssertionError):
        export.build_dmnet_bytes(net, log_amp_min=-1.0, log_amp_max=1.0,
                                  amp_scale=1.0, mask_threshold=0.5)


def test_random_full_within_budget(tmp_path):
    out_path = tmp_path / "full_random.dmnet"
    data = export.export_random_full(out_path, seed=1)
    assert out_path.exists()
    assert len(data) <= 4 * 1024 * 1024
    header, _, _ = export.read_dmnet(out_path)
    assert header["param_count"] <= model.MAX_PARAM_COUNT


def test_random_full_reproducible_bytes(tmp_path):
    """同じ seed から 2 回書き出すと bytes が一致する (--fixture と同じ再現性の要求を
    --random-full 側でも確認しておく — 乱数シードの固定漏れを検出する)。"""
    a = export.export_random_full(tmp_path / "a.dmnet", seed=42)
    b = export.export_random_full(tmp_path / "b.dmnet", seed=42)
    assert a == b


@pytest.mark.editor
def test_fixture_files_present_and_valid():
    """コミット済みの fixture 4 点が読めて、往復検査 (fixture.dmnet を読み戻して
    fixture_in.mvox を推論した結果が fixture_out.bin と一致) を通ることを確認する。
    Editor.exe が無い環境では生成し直せないので skip する (test_layout.py と同型)。"""
    import voxelize
    import meshio

    fixture_dir = __import__("pathlib").Path(__file__).resolve().parents[3] / "tests" / "deepmodal"
    dmnet_path = fixture_dir / "fixture.dmnet"
    mvox_path = fixture_dir / "fixture_in.mvox"
    out_path = fixture_dir / "fixture_out.bin"
    if not (dmnet_path.exists() and mvox_path.exists() and out_path.exists()):
        pytest.skip("fixture files not generated yet (run export.py --fixture)")
    if not voxelize.find_editor_exe().exists():
        pytest.skip("Editor.exe not found")

    header, ops_raw, raw = export.read_dmnet(dmnet_path)
    ops = export.ops_from_header(header, ops_raw)
    folded = export.load_op_weights(header, ops_raw, raw)
    grid = meshio.read_mvox(mvox_path)
    x = model.voxel_to_tensor(grid.occ)
    with torch.no_grad():
        out = model.run_graph(ops, folded, x)
    cell_grid = model.output_to_cell_grid(out)

    expected = out_path.read_bytes()
    assert len(expected) == 64 * (4 + layout.CHANNELS * 4)
    for i in range(64):
        off = i * (4 + layout.CHANNELS * 4)
        flat_index = struct.unpack_from("<i", expected, off)[0]
        vals = np.frombuffer(expected, dtype=np.float32, count=layout.CHANNELS, offset=off + 4)
        cz = flat_index // (layout.MAP_N * layout.MAP_N)
        rem = flat_index % (layout.MAP_N * layout.MAP_N)
        cy = rem // layout.MAP_N
        cx = rem % layout.MAP_N
        got = cell_grid[cx, cy, cz, :]
        max_diff = float(np.max(np.abs(got - vals)))
        assert max_diff < 1e-3, f"cell {i} (idx={flat_index}) max|delta|={max_diff}"
