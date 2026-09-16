#====================================================================================
#                          export.py
#  MyEngine/ 秋田蓮音                                                      09/16/2026
#                                          学習済み ModalUNet の .dmnet 書き出しと fixture 生成
#====================================================================================
"""BN を畳み込みへ畳み、fp16 へ丸めてから `.dmnet` (spec §4.2、layout.py の
DMNET_* が書式の正本) を書く。3 つの入口:

  --checkpoint PATH --out PATH   train.py の checkpoint (state_dict + widths +
                                  log_amp_min/max) を実運用サイズで書き出す
  --fixture --out DIR            sub-05 の C++ 推論を検査する fixture 4 点
                                  (widths=(4,8,8,8)、乱数重み、seed 固定)
  --random-full --out PATH       フルサイズ (基準構成) の乱数重み .dmnet
                                  (sub-05 の時間計測用。コミットしない)

★fp16 に丸めてから fp32 で期待値を計算する (C++ と同じ重みで、差が加算順だけに
なるように) — fixture_out.bin は `model.round_folded_to_fp16` を通した後の重みで
計算した値であること。丸める前の重みで計算すると C++ 側の再現対象にならない。
"""
import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import torch

import layout
import model
import meshio

# fixture / random-full の既定シード (再現性)
FIXTURE_SEED = 20260916
RANDOM_FULL_SEED = 20260917

# fixture/random-full はデータセット統計を持たないので、非退化なプレースホルダを書く
# (実運用の値は --checkpoint 経路が train.py の統計から埋める)。
# ★ampScale の本来の意味 (J=1 N·s の中央値ピークが -12dBFS になる値) は
# ModalSynthRender (C++、sub-05 未着手) を実際に鳴らして較正する必要があり、
# Python 単体では計算できない。1.0 は「未較正」のプレースホルダで、耳確認
# (spec §2 #12、M76f/g) で確定させる — [追加]、SELF_EVAL 参照
_PLACEHOLDER_LOG_AMP_MIN = -20.0
_PLACEHOLDER_LOG_AMP_MAX = 0.0
_PLACEHOLDER_AMP_SCALE = 1.0
_PLACEHOLDER_MASK_THRESHOLD = 0.5


def fnv1a64(data: bytes) -> int:
    """FNV-1a 64bit (spec §4.2 weightsHash)。標準の定数そのまま。"""
    h = 0xCBF29CE484222325
    prime = 0x100000001B3
    mask = (1 << 64) - 1
    for byte in data:
        h ^= byte
        h = (h * prime) & mask
    return h


def build_dmnet_bytes(net: "model.ModalUNet", *, log_amp_min: float, log_amp_max: float,
                       amp_scale: float, mask_threshold: float) -> bytes:
    """net (学習済み/乱数いずれか) から .dmnet の生バイト列を組み立てる。
    BN 畳み込み → fp16 丸め (重みのみ、バイアスは fp32 のまま) → ヘッダ/op表/blob。"""
    ops = net.ops
    folded = model.fold_all(ops)
    folded = model.round_folded_to_fp16(folded)

    weight_chunks = []
    bias_chunks = []
    weight_offsets = []
    bias_offsets = []
    # ヘッダ + op 表のバイト数 (weightOffset/biasOffset はファイル先頭からの絶対値)
    header_and_ops_bytes = layout.DMNET_HEADER_BYTES + len(ops) * layout.DMNET_OP_BYTES
    weight_cursor = 0  # 重み blob 内オフセット (後でヘッダ分を足す)
    bias_cursor = 0
    for params in folded:
        if params is None:
            weight_offsets.append(layout.DMNET_OFFSET_NONE)
            bias_offsets.append(layout.DMNET_OFFSET_NONE)
            continue
        weight, bias = params
        w16 = weight.detach().cpu().numpy().astype(np.float16)
        b32 = bias.detach().cpu().numpy().astype(np.float32)
        weight_offsets.append(weight_cursor)
        weight_chunks.append(w16.tobytes())
        weight_cursor += w16.nbytes
        bias_offsets.append(bias_cursor)
        bias_chunks.append(b32.tobytes())
        bias_cursor += b32.nbytes

    weight_blob = b"".join(weight_chunks)
    bias_blob = b"".join(bias_chunks)
    # 絶対オフセットへ変換 (重み blob はヘッダ+op表の直後、バイアス blob はその後)
    weight_blob_start = header_and_ops_bytes
    bias_blob_start = header_and_ops_bytes + len(weight_blob)
    abs_weight_offsets = [layout.DMNET_OFFSET_NONE if o == layout.DMNET_OFFSET_NONE
                           else o + weight_blob_start for o in weight_offsets]
    abs_bias_offsets = [layout.DMNET_OFFSET_NONE if o == layout.DMNET_OFFSET_NONE
                         else o + bias_blob_start for o in bias_offsets]

    op_bytes = bytearray()
    for op, woff, boff in zip(ops, abs_weight_offsets, abs_bias_offsets):
        op_bytes += struct.pack(layout.DMNET_OP_FMT, op.type, op.in0, op.in1, op.out,
                                 op.cin, op.cout, op.k, op.stride, op.pad, op.out_pad,
                                 woff, boff)
    assert len(op_bytes) == len(ops) * layout.DMNET_OP_BYTES

    param_count = net.param_count_folded()
    if param_count > model.MAX_PARAM_COUNT:
        raise AssertionError(f"paramCount {param_count} exceeds budget {model.MAX_PARAM_COUNT}")

    band_centers = layout.mel_band_centers()
    weights_hash = fnv1a64(weight_blob + bias_blob)

    header_values = (
        layout.DMNET_MAGIC, layout.DMNET_VERSION, len(ops), net.buffer_count,
        layout.VOXEL_N, layout.MAP_N, layout.MEL_BANDS, layout.CHANNELS,
        layout.F_MIN, layout.F_MAX, log_amp_min, log_amp_max, amp_scale, mask_threshold,
        layout.REF_YOUNG, layout.REF_DENSITY, layout.REF_POISSON, layout.L_REF,
        layout.REF_ALPHA, layout.REF_BETA,
    ) + tuple(band_centers) + (weights_hash, param_count) + (0,) * 9
    header_bytes = struct.pack(layout.DMNET_HEADER_FMT, *header_values)
    assert len(header_bytes) == layout.DMNET_HEADER_BYTES

    return bytes(header_bytes) + bytes(op_bytes) + weight_blob + bias_blob


def read_dmnet(path) -> tuple:
    """.dmnet を読み戻す (test_export.py の往復検査、および fixture_out.bin の
    再現用)。戻り値: (header dict, ops (dict のリスト), 全バイト列)。"""
    data = Path(path).read_bytes()
    header_bytes = data[:layout.DMNET_HEADER_BYTES]
    header = dict(zip(layout.DMNET_HEADER_FIELDS,
                       struct.unpack(layout.DMNET_HEADER_FMT, header_bytes)))
    if header["magic"] != layout.DMNET_MAGIC:
        raise ValueError(f"bad .dmnet magic: {header['magic']:#x}")
    if header["version"] != layout.DMNET_VERSION:
        raise ValueError(f"unsupported .dmnet version: {header['version']}")

    op_count = header["op_count"]
    ops_start = layout.DMNET_HEADER_BYTES
    ops_bytes_len = op_count * layout.DMNET_OP_BYTES
    ops = []
    for i in range(op_count):
        chunk = data[ops_start + i * layout.DMNET_OP_BYTES: ops_start + (i + 1) * layout.DMNET_OP_BYTES]
        ops.append(dict(zip(layout.DMNET_OP_FIELDS, struct.unpack(layout.DMNET_OP_FMT, chunk))))
    return header, ops, data


def load_op_weights(header: dict, ops: list, data: bytes):
    """read_dmnet() の戻り値から、model.OpSpec と同じ順の (weight, bias) 済み
    torch テンソルのリストを組み立てる (ReLU/Add は None)。test_export.py の
    往復検査、および fixture_out.bin の C++ 側再現に使う。"""
    folded = []
    for op in ops:
        if op["weight_offset"] == layout.DMNET_OFFSET_NONE:
            folded.append(None)
            continue
        numel = op["cin"] * op["cout"] * (op["k"] ** 3)
        w_bytes = data[op["weight_offset"]: op["weight_offset"] + numel * 2]
        weight = np.frombuffer(w_bytes, dtype=np.float16).astype(np.float32)
        if op["type"] == layout.OP_CONVTRANSPOSE3D:
            weight = weight.reshape(op["cin"], op["cout"], op["k"], op["k"], op["k"])
        else:
            weight = weight.reshape(op["cout"], op["cin"], op["k"], op["k"], op["k"])
        b_bytes = data[op["bias_offset"]: op["bias_offset"] + op["cout"] * 4]
        bias = np.frombuffer(b_bytes, dtype=np.float32).copy()
        folded.append((torch.from_numpy(weight.copy()), torch.from_numpy(bias)))
    return folded


def ops_from_header(header: dict, ops_raw: list):
    """read_dmnet() の op dict のリストを model.OpSpec (module=None) のリストへ
    変換する (model.run_graph はこの形を要求する)。"""
    return [model.OpSpec(o["type"], o["in0"], o["in1"], o["out"], o["cin"], o["cout"],
                          o["k"], o["stride"], o["pad"], o["out_pad"], None) for o in ops_raw]


def export_checkpoint(checkpoint_path: Path, out_path: Path, amp_scale: float = None,
                       mask_threshold: float = None):
    """`amp_scale`/`mask_threshold` を省略すると未較正のプレースホルダのまま書く。
    ★`ampScale` の本来の意味 (J=1 N·s の中央値ピークが -12dBFS になる値) は
    `ModalSynthRender` を実際に鳴らして較正するしかない (Python 単体では計算できない、
    上のモジュール docstring 参照) ため、耳確認 (M76h) は `--amp-scale` で明示的に
    書き込んだ値を検証する。この関数を 2 回目以降呼ぶたびに新しい .dmnet が born-again
    で書かれる (既存ファイルへの部分書き換えは行わない — ヘッダ以外にも weightsHash が
    絡むため、常に全体を書き直すのが安全)。"""
    ckpt = torch.load(checkpoint_path, map_location="cpu", weights_only=False)
    widths = tuple(ckpt["widths"])
    net = model.ModalUNet(widths=widths)
    net.load_state_dict(ckpt["state_dict"])
    net.eval()
    data = build_dmnet_bytes(
        net, log_amp_min=ckpt["log_amp_min"], log_amp_max=ckpt["log_amp_max"],
        amp_scale=amp_scale if amp_scale is not None else _PLACEHOLDER_AMP_SCALE,
        mask_threshold=mask_threshold if mask_threshold is not None else _PLACEHOLDER_MASK_THRESHOLD)
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)
    print(f"[export] wrote {out_path} ({len(data)} bytes, paramCount={net.param_count_folded()})")


def export_random_full(out_path: Path, seed: int = RANDOM_FULL_SEED):
    torch.manual_seed(seed)
    net = model.ModalUNet(widths=(16, 32, 64, 96))
    net.eval()
    data = build_dmnet_bytes(
        net, log_amp_min=_PLACEHOLDER_LOG_AMP_MIN, log_amp_max=_PLACEHOLDER_LOG_AMP_MAX,
        amp_scale=_PLACEHOLDER_AMP_SCALE, mask_threshold=_PLACEHOLDER_MASK_THRESHOLD)
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(data)
    size_mb = len(data) / (1024 * 1024)
    print(f"[export] wrote {out_path} ({len(data)} bytes = {size_mb:.2f} MiB, "
          f"paramCount={net.param_count_folded()})")
    return data


def _voxelize_builtin_cube(work_dir: Path) -> Path:
    """builtin://cube を Editor.exe --modal-voxelize で焼く (sub-02/sub-03 と同じ
    `cmd /c` 経由の呼び出し)。**work_dir はコミット対象の外の一時ディレクトリ**
    (呼び出し側が tempfile で用意する — tests\\deepmodal 直下に作業ファイルを
    残さないため)。"""
    import voxelize

    editor_exe = voxelize.find_editor_exe()
    if not editor_exe.exists():
        raise FileNotFoundError(
            f"Editor.exe not found: {editor_exe} (--modal-voxelize を含むビルドが必要)")
    work_dir.mkdir(parents=True, exist_ok=True)
    list_path = work_dir / "list.txt"
    list_path.write_text("builtin://cube\n", encoding="utf-8")
    rc = voxelize.run_voxelize_list(list_path, work_dir, editor_exe)
    if rc != 0:
        raise RuntimeError("voxelize_batch failed for builtin://cube")
    produced = work_dir / "cube#mesh0#prim0.mvox"
    if not produced.exists():
        raise FileNotFoundError(f"expected voxelize output not found: {produced}")
    return produced


def _cell_valid_from_occupancy(occ: np.ndarray) -> np.ndarray:
    """cell (16^3) の有効性を **ボクセル占有だけ**から決める (dataset.py の FEM/接触
    経由の判定とは別物 — あちらは学習教師データの接触節点選びに要る計算で、
    ランタイムの baking はボクセル占有さえ分かれば十分。sub-05 の ModalSoundLibrary
    もこの規則を使う想定、と coder が仮置き — [追加]、SELF_EVAL 参照)。
    cell (cx,cy,cz) は 2x2x2 の voxel ブロックのどれか 1 つでも占有なら有効。"""
    valid = np.zeros((layout.MAP_N, layout.MAP_N, layout.MAP_N), dtype=bool)
    for cx in range(layout.MAP_N):
        for cy in range(layout.MAP_N):
            for cz in range(layout.MAP_N):
                block = occ[2 * cx:2 * cx + 2, 2 * cy:2 * cy + 2, 2 * cz:2 * cz + 2]
                valid[cx, cy, cz] = bool(np.any(block))
    return valid


def export_fixture(out_dir: Path, seed: int = FIXTURE_SEED):
    """fixture 4 点 (spec §4.2、sub-04.md) を書く。
    ★fp16 丸め後の重みで fp32 期待値を計算する (harness からの明示要求。
    C++ の再計算と「差が加算順だけ」になるようにするため)。"""
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    torch.manual_seed(seed)
    net = model.ModalUNet(widths=(4, 8, 8, 8))
    net.eval()
    # BN の running stats は初期値 (mean=0,var=1) のままだと折り畳みが実質恒等になり
    # fold() の検査価値が薄いので、乱数で崩してから固定する (seed 固定なので毎回同じ)
    for m in net.modules():
        if isinstance(m, torch.nn.BatchNorm3d):
            m.running_mean.normal_(0.0, 1.0)
            m.running_var.uniform_(0.5, 2.0)

    dmnet_bytes = build_dmnet_bytes(
        net, log_amp_min=_PLACEHOLDER_LOG_AMP_MIN, log_amp_max=_PLACEHOLDER_LOG_AMP_MAX,
        amp_scale=_PLACEHOLDER_AMP_SCALE, mask_threshold=_PLACEHOLDER_MASK_THRESHOLD)
    dmnet_path = out_dir / "fixture.dmnet"
    dmnet_path.write_bytes(dmnet_bytes)
    print(f"[export] wrote {dmnet_path} ({len(dmnet_bytes)} bytes, "
          f"paramCount={net.param_count_folded()})")

    with tempfile.TemporaryDirectory(prefix="deepmodal_fixture_") as tmp:
        mvox_src = _voxelize_builtin_cube(Path(tmp))
        mvox_path = out_dir / "fixture_in.mvox"
        mvox_path.write_bytes(mvox_src.read_bytes())
    print(f"[export] wrote {mvox_path}")

    # fixture_out.bin: fixture.dmnet を読み戻し (ファイルそのものを正とする —
    # メモリ上の net を直接使うと「書き出しバイト列に何か取りこぼしがあった」場合の
    # 検出力が落ちる) → fp16 丸め済みの重みで推論 → 有効 64 cell を選ぶ
    header, ops_raw, data = read_dmnet(dmnet_path)
    ops = ops_from_header(header, ops_raw)
    folded = load_op_weights(header, ops_raw, data)

    grid = meshio.read_mvox(mvox_path)
    x = model.voxel_to_tensor(grid.occ)
    with torch.no_grad():
        out = model.run_graph(ops, folded, x)
    cell_grid = model.output_to_cell_grid(out)  # (16,16,16,192) [cx,cy,cz,c]

    valid = _cell_valid_from_occupancy(grid.occ)
    selected = [(cx, cy, cz) for (cx, cy, cz) in layout.cell_order() if valid[cx, cy, cz]][:64]
    if len(selected) < 64:
        raise RuntimeError(f"fixture_in.mvox has only {len(selected)} valid cells (need 64)")

    out_bin = bytearray()
    for (cx, cy, cz) in selected:
        flat_index = cx + layout.MAP_N * (cy + layout.MAP_N * cz)  # CellIndexOf と同じ式
        out_bin += struct.pack("<i", flat_index)
        out_bin += cell_grid[cx, cy, cz, :].astype(np.float32).tobytes()
    fixture_out_path = out_dir / "fixture_out.bin"
    fixture_out_path.write_bytes(bytes(out_bin))
    print(f"[export] wrote {fixture_out_path} ({len(out_bin)} bytes, 64 cells)")

    list_builtin_src = Path(__file__).resolve().parents[2] / "tests" / "deepmodal" / "list_builtin.txt"
    if not list_builtin_src.exists():
        raise FileNotFoundError(f"list_builtin.txt not found: {list_builtin_src}")
    list_builtin_dst = out_dir / "list_builtin.txt"
    if list_builtin_src.resolve() != list_builtin_dst.resolve():
        list_builtin_dst.write_text(list_builtin_src.read_text(encoding="utf-8"), encoding="utf-8")
    print(f"[export] fixture ready: {out_dir}")


def main():
    ap = argparse.ArgumentParser(description="Deep-Modal .dmnet エクスポート")
    ap.add_argument("--checkpoint", default=None)
    ap.add_argument("--fixture", action="store_true")
    ap.add_argument("--random-full", action="store_true")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--amp-scale", type=float, default=None,
                     help="--checkpoint 専用。省略時は未較正のプレースホルダ (1.0)。"
                          "耳確認 (M76h) で ModalSynthRender を実際に鳴らして決める")
    ap.add_argument("--mask-threshold", type=float, default=None,
                     help="--checkpoint 専用。省略時は既定 0.5")
    args = ap.parse_args()

    modes = sum([bool(args.checkpoint), args.fixture, args.random_full])
    if modes != 1:
        print("[export] ERROR: --checkpoint / --fixture / --random-full のどれか 1 つを指定",
              file=sys.stderr)
        sys.exit(1)

    if args.fixture:
        export_fixture(Path(args.out), seed=args.seed if args.seed is not None else FIXTURE_SEED)
    elif args.random_full:
        export_random_full(Path(args.out), seed=args.seed if args.seed is not None else RANDOM_FULL_SEED)
    else:
        export_checkpoint(Path(args.checkpoint), Path(args.out),
                           amp_scale=args.amp_scale, mask_threshold=args.mask_threshold)


if __name__ == "__main__":
    main()
