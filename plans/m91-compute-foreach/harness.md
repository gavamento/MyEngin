# harness 台帳: m91-compute-foreach

- 依頼原文:
  - 「C#/C++共通の計算用APIを使えるように / プログラマーがCPUとGPUを意識せずに処理を書けるAPIを目標 / いったん話をつめたい」
  - 「CPU/GPU自動選択、CPU/GPU強制指定、並列処理、バッファ管理、自動データ転送、非同期実行、実行結果の取得、GPU非対応時の代替実行、CPU/GPU負荷制限、自動SIMD最適化 などを `Compute.ForEach(particles, "ParticleUpdate", Execution.Auto);` こんな感じで呼び出せるAPI」
  - 「大前提決定論を維持するためにCPUとGPUで結果が変わる物はCPU側で計算するように / 判断はどちらでやっても整合性/決定論が壊れないかを確認するように / 重要なゲーム状態はCPUで計算するように / 1.制限を受け入れる 2.assets/shaders/kernels/に置く 3.ハッシュと不一致時の実行制限」
  - 「C++/C#は同じように呼び出せるようにしてほしい / HLSLはまだ話したい」
- 開始: 2026-10-11 / 基点コミット: 3766e33 (当初 7d28c31。M91 開始前に未コミットの DeepModal 変更を 3766e33 として切り出したため繰り下げ)
- フェーズ: 実装
- 事前調査: plans/m91-compute-foreach/design-draft.md

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 5e0b501 | 単一ソースのカーネル (互換ヘッダ・KERNEL 展開・C++ CPU 実行器・ハッシュ) |
| sub-02 | OK | 2 | (M91b) | 検査器と C# への変換器 (依存 01) |
| sub-03 | 未着手 | 0 | | ForEach のエンジン核 (依存 01) |
| sub-04 | 未着手 | 0 | | State のスナップショット・ハッシュ (依存 03、snapshot v49 目安) |
| sub-05 | 未着手 | 0 | | カーネル表・COMPUTE_KERNELS・ABI (依存 02,04、ABI v29 目安) |
| sub-06 | 未着手 | 0 | | C# 同形 API・C# CPU 版 (依存 05) |
| sub-07 | 未着手 | 0 | | スクリプトのインスペクター・project_settings (依存 06) |
| sub-08 | 未着手 | 0 | | デモと compute A/B (依存 06) |
| sub-09 | 未着手 | 0 | | ADR-030 と文書 (依存 07,08) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- (2026-10-11、対話で確定) design-draft §1 のとおり。
- (2026-10-12、司会が AskUserQuestion) U1 = C# 用の CPU 版も作る (planner 裁定の逆) / U2 = WARP は使わない / U3 = エンジン所有のストア / U4 = State は同期 / U5 = 読み戻し経由だけ / U6 = スクリプトのインスペクター (planner 裁定の逆) / U7 = HLSL は厳しめに絞る / 受け入れ条件は U1・U6 反映後で確定 / 未コミットの DeepModal 変更は司会が M91 の前に別コミット。
- (2026-10-12、司会が AskUserQuestion) U8 = 上書きはカーネル単位 / U9 = C# からは常に C# 版 / U10 = 宣言していないカーネルの呼び出しは失敗 (planner 裁定の逆)。
- (2026-10-12、司会が AskUserQuestion) U11 = スクリプトのコールバック外 (BT タスク等) からの ForEach は失敗 (裁定どおり)。仕様確定。

## 申し送り (セッション跨ぎ)
- (解消: 3766e33 に切り出し済み) 開始時点で作業ツリーに無関係の未コミット変更あり (`docs/test_checklists.md`、`tools/deepmodal/train.py`、`DeepModalModel/`、`docs/deepmodal/synthetic_shapes.md`、`tools/deepmodal/tests/test_train_axes.py`)。実装フェーズに入る前にユーザーへ扱いを確認する (`docs/test_checklists.md` は coder も触りうる)。
- (planner、2026-10-12) spec.md と sub-01〜08 を作成。AskUserQuestion が使えなかったため U1〜U7 は planner 裁定 (spec 7.1)。司会がユーザーへ確認し、確定確認 (受け入れ条件 5. の一覧) も代行すること。逆の選択が出たら planner を REVIEW_RESPONSE 相当で呼び直してサブを組み替える。
- (planner) 番号は着手時の末尾から取る: ABI は sub-04 着手時 (今なら v29。M75h は未着手なので後から着手した方が次を取る)、snapshot は sub-03 (今なら v49)、ADR は sub-08 (今なら ADR-030)。TypeId・kCookVersion は使わない。memory の M75h=v29 の記述は M91 が先に取ったら更新が要る。
- (planner、2026-10-12) ユーザーの回答 U1〜U7 を反映して spec を「確定」にした。U1 (C# の CPU 版を作る) と U6 (スクリプトのインスペクター) は planner 案の逆。これに合わせてサブを 9 本に組み直した: sub-02 (検査器と C# への変換器) を新設し、旧 sub-02〜08 を sub-03〜09 へ繰り下げた。番号の目安は、ABI = sub-05 (今なら v29)、snapshot = sub-04 (今なら v49)、ADR = sub-09 (今なら ADR-030)。U8〜U10 は planner の裁定を仮置きしている (spec 7.1)。
- (planner、2026-10-12) U8・U9 は裁定どおり、U10 は逆 (宣言していないカーネルの呼び出しは失敗) を反映した。spec に D28 (宣言の出所と時点) を足し、sub-03/05/06/07/08 を更新した。U11 (BT タスクなどスクリプトではない呼び出し元は失敗にする) は裁定を仮置きしている。
- ★(ユーザー指示 2026-10-12) 全サブ完了 (レビュー PASS まで) → Notion 活動記録へ記録 → PC をシャットダウン (`shutdown /s /t 60`)。上限超過などでユーザー判断が要る状態ではシャットダウンしない。
- (planner VERDICT sub-01) 申し送り: (1) kernels の新規 .meta 3 件が LF (既存は CRLF) → sub-07 の .meta 整備で揃える。(2) Kernel の selftest は自前の JobSystem で jobs 構成を回す (`--no-jobs` は効かない) → ADR-030 (sub-09) に 1 行。(3) GPU/CPU 差の実測は WARP のみ → sub-08 のデモ撮影時に実機 GPU で 1 度測り ADR-030 に追記。
- (sub-01 coder) 4 バイト params を SRV+UAV で作ると WARP debug layer がアクセス違反 (原因未特定)。読み取り専用バッファは SRV のみ (sub-03 必須事項)。
- (planner VERDICT sub-02) 申し送り: (1) GPU 側の FloatToInt/FloatToUint は FXC コンパイルのみ確認 → sub-08 の実機 GPU 計測時に 1 回確かめる。(2) ADR-030 (sub-09) の規則一覧は KernelLint.h (KL001〜KL028) から写す。環境変数 MYE_KERNEL_GOLDEN_DUMP / MYE_KERNEL_LINT_FILES、FloatToInt の仕様、互換層 3 か所 (kernel_common.hlsli / KernelCompat.h / KernelCompat.cs) を同式に保つことも ADR-030 へ。
