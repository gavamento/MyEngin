# sub-08: デモと compute A/B (`--compute-force-cpu`)

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- 再編: 2026-10-12 (旧 sub-07。C# 版のデモを追加)

## やること
spec 4.3 (デモ)、D11 / D15、受け入れ 10 / 12。

1. `--compute-force-cpu` と `kCfgComputeForceCpu = 1u << 5` (`SessionTypes.h`。Net 側の別名も揃える)。
2. `--compute-demo` を作る:
   - C++ / State: 群れ (例: 512 体。前の状態を RO、次の状態を RW の 2 バッファで持つ)。可視エンティティの LocalTransform に書く
   - C++ / Fx: 火花 (例: 4096 粒)。GPU を優先し、`DebugDrawLine` で描く (spec R2。写らなければ NoHash の描画コンポーネントに替え、実装メモに書く)
   - C# 版: 同じカーネルを C# スクリプトから呼ぶ小さな群れ (C# の CPU 版が実際に動くことを見せる)。C# レーンは record / verify 中は止まるので、replay の照合には入らない
   デモのスクリプトには、使うカーネルを全部宣言する (`COMPUTE_KERNELS` / `[ComputeKernels]`。宣言なしは失敗する — U10)。
   既存のデモに足す場合は末尾に足す (memory `demo-entity-order-is-rng-stream.md`)。
3. `tools\replay_verify.bat` に、compute シーンの Debug / Release 照合と `computeab` を足す (Auto で録り、`--compute-force-cpu` で検証する。`jobsab` と同じ形)。ジョブの一覧、PASS の文言、`:diagnose` も更新する。
4. 漏れの検出が働くことを 1 回だけ確かめる (Fx の Read 結果を LocalTransform に書く一時的な改変で computeab が FAIL すること。確かめたら元に戻す)。

## 触る場所 (planner の見立て)
- `src/Engine/Engine/Session/SessionTypes.h:34-43`、`src/Engine/Engine/App/EngineCli.cpp`、`src/GameLogic/Scripts/`、C# のデモスクリプト、`assets/shaders/kernels/`
- `tools\replay_verify.bat` (:83 / :148 付近)

## 受け入れ条件 (このサブ)
1. spec 5-10
2. spec 5-12 (C# の群れも写る)。画像のパスを実装メモに書く
3. 漏れの検出を確かめた結果を実装メモに書く

## 検証コマンド
- Debug / Release ビルド、`tools\replay_verify.bat`、`--screenshot`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
