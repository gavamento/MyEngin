# sub-09: ADR-030 と文書

- 依存: sub-07, sub-08, sub-10
- 状態: 未着手
- 往復: 0
- 再編: 2026-10-12 (旧 sub-08。U1 / U6 の内容を追加)

## やること
spec 受け入れ 13。

1. `docs/adr/ADR-030-compute-foreach.md` (番号は着手した時点の末尾から取る)。書くこと:
   - D1〜D27 の決定と却下した案。特に、ユーザーが planner 案の逆を選んだ U1 (C# の CPU 版) と U6 (スクリプトのインスペクター) は、却下した当初案も含めて書く
   - サブセットの規則とその理由 (D23)
   - C# と C++ のバイト一致の結果 (D24)
   - 実測値 (GPU/CPU の最大誤差、1M 要素で jobs あり/なし、C++ と C# の比較)
   - Unity / UE の前例と、それとの違い
2. `engine_spec.md` に Compute.ForEach の節を足す (API、サブセット、決定表、宣言、設定キー、版番号)。
3. `docs/test_checklists.md` に手動確認の項目を足す。sub-13 の受け入れ 3 の GUI 手順 (シーンの保存、`.meta` の作り直し、Rebuild Scripts の後に `git status` で改行だけの差分が出ないこと) を含める。 (ホットリロードで不一致になる流れ、スクリプトのインスペクター、デモの見た目)。

## やらないこと (このサブでは)
- コードの変更

## 受け入れ条件 (このサブ)
1. spec 5-13: 版番号・スロット名・設定キー・既定値が実装と一致していることを grep で照合し、結果を実装メモに書く

## 検証コマンド
- grep による照合、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴

## 申し送り (2026-10-12、sub-15 から)
- `docs/line-endings.md` に書いた VS のメニュー名 (Edit > Advanced > Set Line Endings) は、実機で確かめていない。test_checklists に手動の項目として載せ、ユーザーの確認に回す。

## 申し送り (2026-10-12、sub-06 から。ADR-030 に書く事実)
- 生成する C# の `Range` の署名は、ABI の `MyeKernelRangeFn` (params, bufs, bufCount, begin, end) と同じ。
- collectible ALC の中の `[UnmanagedCallersOnly]` の関数ポインタは、`GetFunctionPointer()` で取れて、JobSystem のワーカーから呼べる。新しい表は、古い ALC を捨てる前に登録する。
- D24: ホワイトリストのすべての関数で、6 通りの JIT 設定 (既定 / TieredCompilation=0 / TieredPGO=0 / QuickJitForLoops=0 + OSR / AVX と AVX2 を無効 / ReadyToRun=0) のどれでも、C++ の CPU 版とバイト一致した。CI には常設しない。手動で再実行する手順を書く。
- 変異テストの手順: `KernelCompat.cs` の `dot(float3)` の加算の順を変えると、selftest の一致の確認が FAIL する。
- 速度: 1M 要素の Release で、Saxpy は C++ 0.21〜0.34 ms / C# 0.26〜0.60 ms、Mix は C++ 12.8〜22.4 ms / C# 15.1〜28.6 ms。
- `RWView<T>` を int で添字すると、C# では CS1503 になる。

## 申し送り (2026-10-12、sub-07 から)
- ADR-030 の既知の制限: ReloadHub が CompileScripts を同期で呼ぶので、Roslyn の数秒ぶんフレームが止まる。
- 起動順の事実: C# の起動時の登録は SetComputeService より後でなければ拒否される。HeadlessSim は順序が逆なので、selftest では見つけにくい。
- test_checklists に足すこと:
  - 日本語表示の Compute 欄
  - GameLogic を Rebuild した後に、C++ の行が「一致」に戻ること
  - Play 中にカーネルを保存したときの挙動
  - 実機 GPU での Compute 欄の数値 (sub-08 で確かめられなければ)

## 申し送り (2026-10-12、sub-08 から)
- ADR-030 に書くこと:
  - 実機 GPU (RTX 3060) の差: Saxpy は FMA の融合で最大 64 ULP、Mix は 6.2e-6、FloatToInt / FloatToUint は 18 値すべて一致した。WARP を前提にした許容差の 8 項目は、実機では FAIL する。
  - State のハッシュの方式: 8 バイト単位、4 レーン。
  - 既定 8 MiB の根拠の表: Release / Debug × 1 / 8 / 16 MiB。
  - compute A/B の仕組みと、漏れを入れると tick 0 で FAIL すること (実機 GPU と --warp の両方で確かめた)。
  - kCfgComputeForceCpu = 1u << 5。
- engine_spec の stateTotalBytes の既定値は 8388608。
- test_checklists に足すこと: 実機 GPU での Compute 欄の数値。Editor で ComputeCsSwarm / ComputeDemoSwarm / ComputeDemoSparks を選んで Compute 欄を見る手順。
