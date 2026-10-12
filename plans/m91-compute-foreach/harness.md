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
| sub-02 | OK | 2 | f71dc9f | 検査器と C# への変換器 (依存 01) |
| sub-03 | OK | 2 | c206131 | ForEach のエンジン核 (依存 01) |
| sub-04 | OK | 1 | 42c6b89 | State のスナップショット・ハッシュ (依存 03、snapshot v49 目安) |
| sub-05 | OK | 2 | 92bde8a | カーネル表・COMPUTE_KERNELS・ABI (依存 02,04、ABI v29 目安) |
| sub-10 | OK | 1 | ff60f77 | 外部プロジェクトの C++ カーネル表 (依存 05、U12。sub-06 より先に着手) |
| sub-11 | OK | 2 | ce452a4 | Debug selftest の net 試験の不安定さの切り分け (sub-06 の前。M91e 退行の疑い) |
| sub-12 | OK | 1 | 3972b9f+ | リポジトリ全体の改行を .gitattributes で統一 (依存 11、U14) |
| sub-13 | OK | 1 | c0cbcd3 | エンジン・ツールが書くテキストの改行を規則に揃え、作業ツリーの見張りを有効化 (依存 12) |
| sub-14 | OK | 1 | 三校 f918c7b (未 push) | 外部プロジェクト (三校) にも同じ改行規則 (依存 13、U15。HAL Collector は git 無し = U16) |
| sub-15 | OK | 1 | 232609d | 14-c の文言に直し方 (U17) と三校 verify.bat FAIL の切り分け (依存 14、U18) |
| sub-06 | 実装中 | 0 | | C# 同形 API・C# CPU 版 (依存 05) |
| sub-07 | 未着手 | 0 | | スクリプトのインスペクター・project_settings (依存 06) |
| sub-08 | 未着手 | 0 | | デモと compute A/B (依存 06) |
| sub-09 | 未着手 | 0 | | ADR-030 と文書 (依存 07,08,10) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- (2026-10-11、対話で確定) design-draft §1 のとおり。
- (2026-10-12、司会が AskUserQuestion) U1 = C# 用の CPU 版も作る (planner 裁定の逆) / U2 = WARP は使わない / U3 = エンジン所有のストア / U4 = State は同期 / U5 = 読み戻し経由だけ / U6 = スクリプトのインスペクター (planner 裁定の逆) / U7 = HLSL は厳しめに絞る / 受け入れ条件は U1・U6 反映後で確定 / 未コミットの DeepModal 変更は司会が M91 の前に別コミット。
- (2026-10-12、司会が AskUserQuestion) U8 = 上書きはカーネル単位 / U9 = C# からは常に C# 版 / U10 = 宣言していないカーネルの呼び出しは失敗 (planner 裁定の逆)。
- (2026-10-12、司会が AskUserQuestion) U12 = 外部プロジェクト (HAL Collector / 三校) の kernels から C++ のカーネル表を生成する対応を M91 でやる (planner 裁定「後回し」の逆)。
- (2026-10-12、司会が AskUserQuestion) U11 = スクリプトのコールバック外 (BT タスク等) からの ForEach は失敗 (裁定どおり)。仕様確定。
- (2026-10-12、司会が AskUserQuestion) U13 = net の selftest の `cannot open` に、開けるまで数回リトライする処理を**いま足す** (planner 裁定「足さない」の逆)。sub-11 の追補コミットに入れる。
- (2026-10-12、司会が AskUserQuestion) U14 = 改行の統一は**リポジトリ全体** (planner 裁定「M91 の分だけ」の逆)。既存の golden・replay・アセット・シーン・.sln/.vcxproj/.bat の例外まで決める。
- (2026-10-12、司会が AskUserQuestion) U15 = 外部プロジェクト (HAL Collector / 三校) のリポジトリの改行も**揃える** (planner 裁定「範囲外」の逆)。
- (2026-10-12、司会が AskUserQuestion) U16 = HAL Collector (git 無し) は触らない (裁定どおり)。
- (2026-10-12、司会が AskUserQuestion) sub-12 の作業ツリーの書き直しは「コミットしてから書き直す」を承認 (自動判定が coder の書き直しを 2 回拒否したため)。
- (2026-10-12、司会が AskUserQuestion) U17 = check_rules 規則 14-c はエラーのまま (裁定どおり)。文言に直し方のコマンドを入れる追補を sub-14 の後に入れる。
- (2026-10-12、司会が AskUserQuestion) U18 = 三校 verify.bat の FAIL 2 件は M91 の退行かだけ sub-15 で切り分ける (裁定どおり)。三校の golden 撮り直し・コード変更はしない。

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
- (planner VERDICT sub-03) 申し送り: (1) ComputeService::ForEach の分割 (引数検証と実行を関数へ) は sub-05 で入口を触るときに範囲内で。(2) SelfTestUnused.kernel.hlsl.meta の guid は手付け → sub-07 の .meta 整備で揃える。HeadlessSim への配線は sub-04 へ移管 (sub-04.md 4b・受け入れ 3)。GPU バインドはリフレクションの名前引き (FXC は未使用資源を落として詰めることを実測) → sub-05 のカーネル表は引数の HLSL 変数名を出す。
- (planner VERDICT sub-04) snapshot v49 (CMP 節は BT の後・World の前)。申し送り: (1) State ハッシュのコスト (1MiB/16MiB の ms/tick) は sub-08 の 5 で計測、16MiB で tick の 10% 超なら planner 判断。(2) State 入り .rep の照合は sub-08 の 6 (Debug/Release と computeab)。(3) sub-05 の ABI CreateBuffer は実行中スクリプトのエンティティを owner に渡す。State/Fx は 1 スロット空間のまま (Fx の Status/Target/Read/ハンドルを sim 分岐に使わない規約を spec 4.1.4 に追記)。
- (planner VERDICT sub-05) ABI v29 = 174 (+10 本)。Kernels.gen.{h,cpp} は版管理 (spec 4.2 改訂)。U12 を受けて sub-10 を新設、コミット件名の文字を繰り下げ (sub-10 = M91f、sub-06 = M91g、07 = M91h、08 = M91i、09 = M91j)。申し送り: ScriptHost.cpp が長い (分割は後続)。R10 (複数 ScriptHost 同時生存時の construct dangling) は範囲外。Release selftest は司会がコミット前に再実行して PASS。memory の M75h は v30 へ更新済み。
- (planner VERDICT sub-10) 申し送り: (1) Debug selftest の net 試験 (V1 LoadPersist / LoadGame) が 1 回目に FAIL → sub-11 で HEAD と 42c6b89 の再現率を比べて切り分け。基点でも落ちる場合はユーザー判断。(2) [Rebuild Scripts] ボタンからの手動確認を sub-09 で test_checklists に足す。(3) 自動 Rebuild 後のホットリロードで unknown component が復活するかは範囲外 (記録のみ)。sub-07 に 1b (プロジェクトとエンジンの同名カーネルの優先順) を追加。
- (planner VERDICT sub-11) 申し送り: (1) `TestWaitUntilReadable` の上限 1500 ms は遅い CI で揺れうる → sub-12 の 7 で緩める。(2) Release selftest と replay_verify は sub-12 の受け入れ 4 で回す。比較側の CR 除去 (StripBanner) は sub-12 後も二重の守りとして残す。
- (planner、U15 反映) sub-14 新設。三校 `C:\HAL\三校` は push しない (共同制作のリポジトリ、時期はユーザー判断)。HAL Collector `C:\HAL\GameEngin_Demo` は git が無い (U16)。実行順: sub-12 → 13 → 14 → 06 → 07/08 → 09。
- (planner VERDICT sub-12) 申し送り: (1) `*.fbx` を binary にしたので ASCII FBX 2 件も改行変換されない旨を ADR-030 か docs/line-endings.md に 1 行 (sub-09)。(2) 3972b9f の件名の「作業中」は履歴を書き換えず残す。sub-13 の入力 (CRLF を書く ofstream 7 箇所と規則 14-c) は sub-13.md に追記済み。
- (planner VERDICT sub-13) 申し送り: (1) 受け入れ 3 の GUI 確認は sub-14 の 3-e (三校で保存・.meta 作り直し・Rebuild Scripts) と sub-09 の test_checklists 手動項目で補う。(2) AssetOps / NetRollback の書き込みは sub-14 の 3-b / 3-e で実経路確認。(3) selftest の LF 確認ラムダ 3 か所の重複は共通化しない。U17 (14-c の厳しさ) はユーザー確認。
- (planner VERDICT sub-14) 申し送り: 三校の origin に master が無い (origin/develop・origin/stage-transition のみ)。ローカル master の f918c7b の push 先はユーザー判断。3-e の GUI 操作 (シーン保存・actions/fxstack 書き出し・GUI の Rebuild Scripts) は sub-09 の test_checklists 手動項目へ。
- (planner VERDICT sub-15) 三校 verify.bat の FAIL 2 件は 7d28c31 と HEAD で同値 = M91 の退行ではない (三校側の別件、対応はユーザー判断)。.editorconfig は足さない。docs の VS メニュー名と三校の f918c7b の push 先はユーザー判断 (sub-09 の手動項目にも)。
