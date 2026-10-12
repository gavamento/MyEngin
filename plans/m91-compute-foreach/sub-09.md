# sub-09: ADR-030 と文書

- 依存: sub-07, sub-08, sub-10
- 状態: OK (commit f1f0007)
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

### round 1 (SELF_EVAL)
実装:
- docs/adr/ADR-030-compute-foreach.md (新規): D1〜D28 (U1 / U6 / U10 の当初案と逆になった経緯を含む)、サブセット規則 KL001〜KL028、D24 の結果・JIT 6 通り・再実行手順・変異テスト、決定表、ABI v29 の 10 本、compute A/B と漏れ検出の実測、State のハッシュ方式と 8 MiB の根拠表 (Release / Debug × 1 / 8 / 16 MiB)、実機 GPU の差、速度表、Unity / UE の前例と違い、ホットリロード、外部プロジェクト (1b)、起動順と既知の制限、環境変数。
- engine_spec.md: 10.12 Compute.ForEach を新設 (API・サブセット・決定表・宣言・設定キー・版番号)。5 の replay_verify に compute A/B、ABI v29 の段落 (次の bump は v30)、12.2 に M91 の行、12.3 の Numbering、13 の ADR 一覧、Last Updated。
- docs/test_checklists.md: M91 節 (デモ、スクリプトのインスペクター、.kernel.hlsl 編集と版ずれ、外部プロジェクト、改行の GUI 確認、実機 GPU、回帰)。
- docs/line-endings.md: `*.fbx` は binary なので ASCII FBX も改行変換されない旨を 1 行 (sub-12 の申し送り)。
- README.md: 現行の MYE_API_VERSION を 27 (158) から 29 (174) へ。
仕様との差分:
- [追加] README.md の ABI 版の行を直した (27 のまま古かったため。sub-09 の 3 文書の外)。
- [逸脱] sub-06 の申し送り「ForEach のバッファ上限 4 本が C++ と揃っている」は、コードで確認できなかった。4 本のオーバーロードは C# の糖衣 (Compute.cs) だけで、C++ の `Compute::ForEach` は可変長で固定の上限が無い (ScriptCompute.h)。ADR / engine_spec には実際のとおり書いた。
- [未実装] 「1M 要素で jobs あり/なし」の比較は取っていない (コードを触らない指示。sub-06 の速度表は jobs 10 本のみ)。ADR に「未計測」と明記した。
- [未実装] docs/line-endings.md の VS メニュー名は確認していない。test_checklists の手動項目に載せた。
- [追加] test_checklists の項目の一部 (宣言を外して「失敗: 宣言なし」を見る、狭い幅の確認など) は、コードと LocalizationTable.inl のラベルから書いた手順で、実機で通していない。
検証:
- 受け入れ 1 (grep 照合): MYE_API_VERSION 29u (EngineAPI.h:54)、check_rules.ps1 の `29 = 174` (:743)、kSimSnapshotVersion = 49 (SimSnapshot.h:155)、`kCfgComputeForceCpu = 1u << 5` (SessionTypes.h:44)、ComputeConfig の既定 gpuMinElements 4096 / cpuMaxChunks 0 / fxUploadBytesPerTick 8388608 / fxDispatchPerTick 64 / stateTotalBytes 8388608 (ComputeService.h:39-43、ComputeSettings.h のコメントも同値)、ジョブ定数 1 / 2 / 0x100 (ComputeTypes.h:25-27)、上限 4194240 / 1024 / 2048 / 128 MiB (ComputeBufferStore.h:21-24)、ABI の 10 スロット名が EngineAPI.h / Interop.cs / ADR / engine_spec のすべてに存在、MyeScriptDesc.kernelNameCount と MyeScriptModule.kernelCount (ScriptTypes.h:74,123)。ADR / engine_spec / test_checklists の数値はこれらと一致。
- KL001〜KL028 の一覧を KernelLint.h と照合して ADR の表に写した。LocalizationTable.inl の Insp_Compute* / PrjSet_Compute* のラベルを test_checklists の文言と照合した。
- `tools\check_rules.ps1` → 0 error / 50 warning (既存の規則 7 のみ)。
- 改行: ADR-030 / engine_spec.md / test_checklists.md / line-endings.md はバイナリで CR 0 件 (LF)。`git ls-files --eol` は w/lf。
- 未実施: エンジンのビルド・selftest・replay_verify (文書のみの変更。コードは触っていない)、GUI での手順の実走。
自己採点 (1-5):
  仕様適合: 4 — 3 文書と grep 照合を実施。jobs あり/なしの 1M 比較が無い
  正しさ: 4 — 数値は実装メモとコードで照合したが、手順 (checklists) の GUI 実走はしていない
  コード品質: 4 — 文書のみ。ADR が長く、決定番号が spec とずれる節 (決定 4 / 7 / 11 …) がある
  テスト: 3 — grep と check_rules のみ。手動項目は未実走
不安・質問:
- 「1M 要素で jobs あり/なし」の計測 (sub-09 やること 1) を、コード変更なしの範囲では取れなかった。取るなら ComputeStateSelfTest か KernelSelfTest に計測を足す小さなコード変更が要る。ADR は「未計測」としたまま。planner の判断を仰ぎたい。
- sub-06 の申し送りの「バッファ上限 4 本が C++ と揃っている」はコード上は揃っていない (上記)。C++ に 4 本の static_assert を足して揃えるか、現状の記述 (C# だけが 4 本) のままにするか。コード変更なので sub-09 では触っていない。
触ったファイル:
- docs/adr/ADR-030-compute-foreach.md (新規)、engine_spec.md、docs/test_checklists.md、docs/line-endings.md、README.md、plans/m91-compute-foreach/sub-09.md (実装メモ節のみ)
申し送り:
- ユーザーの確認: VS のメニュー名 (line-endings.md)、実機 GPU の Compute 欄の目視、GUI の [Rebuild Scripts] / [Compile C# Scripts]、Play 中のカーネル保存 (すべて test_checklists の M91 節)。
- memory の更新候補: M91 は ABI v29 = 174 / snapshot v49 / ADR-030。M75h は v30。

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
- round 1: VERDICT OK (planner、2026-10-12)。
  - 1M 要素の jobs あり/なしの計測は「未計測」と明記することで受け入れた。
  - C# だけ 4 本・C++ は可変長という記述は、事実どおりなので受け入れた。
  - nit: ADR の「決定 N」に、対応する spec の D 番号を添える。
