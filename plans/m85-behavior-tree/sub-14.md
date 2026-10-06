# sub-14: 文書 (ADR-025) と全体検証

- 依存: sub-13
- 状態: OK (M85n としてコミット、ハッシュは台帳)
- 往復: 1

## やること
- `docs\adr\ADR-025-behavior-tree.md` を確定: 実行モデル (UE 方式 + ポーリングの監視、却下: 毎 tick 根から / 即時コールバック)、状態の置き場所 (システムの表 + BT 節、却下: 全部コンポーネント = ADR-024 との違いの理由)、イベントキュー (tick N+1 配送、順序、上限)、C++ タスクの状態の扱い、C# タスクは保証外、手数の上限、SubTree と BB の一致、`.bt.json` の位置と contentHash、性能の計測値、UE との違いの一覧、既知の限界。
- `engine_spec.md` (BT / BB / イベントキュー / PatrolRoute / ABI v27 の表 / snapshot v の履歴)、`docs\engine-feature-guide.md` (BT の使い方: 木を作る → BB → コンポーネント → デバッグ)、`docs\test_checklists.md` (BT の手動確認項目)、`docs\history\api-scripting-tools.md` (sub-11 で書いた分の見直し)、`plans\ai-roadmap-m83-m86.md` の進捗表 (M85 完了、M85 で計画から変えたこと)。AGENTS.md / CLAUDE.md に番号の記載があれば追従。
- 全体検証: AGENTS.md 7 章の広範な変更の一式。

- (sub-01〜sub-09 の VERDICT から) ADR-025 の既知の限界・未検証に必ず入れるもの: UE の規則は記憶ベースで未照合 (Loop / Cooldown の Abort 時を含む)、同距離タイブレークのテストの検出力、親付き Agent の RotateTo、遷移中の AnimatorPlay のポーズの飛び、SubTree の平らな展開 (1024 上限・根の LowerPriority は Self)、Patrol は入るたびに最近傍、ReloadHub が置き換えると登録のパス・名前が小文字になる、BB のキー改名・削除は編集中の木だけ追従 (ほかの木・Entity キー初期値は追従しない)、巻き戻し直後は Abort の矢印が空・SimpleParallel の Immediate の停止は矢印にしない・デバッグ線は再シム中は積まない、未保存編集中の窓では増えたノードがライブ強調されない、C++ タスクの状態は 112 バイトまで・FIELDS 以外は保持されない、BB の ABI 書き込みはインスタンスが無い tick には効かない、AnimatorPlay の名前引きは先頭の同名、BtRestart を自分の木のタスクから呼ぶと返った後に Abort。AnimatorPlay の true 経路を C# から未確認・C# タスクのインスタンスは巻き戻しで戻らずリロード / シーン遷移で捨てられる・死んだエンティティ分の掃除は enter のときだけ・C# レーン停止の警告は GameLogic のホットリロードで再び 1 回出る、C# タスクの fields は enter / tick の毎回 JSON で渡しリロード中に動いていた C# タスクは次の tick に OnStart から (fields 付き) やり直す、MyeManagedVTable の BtTask / BtTaskCatalog は内部契約で外部プロジェクトの MyeScripting.dll は exe と同時ビルドが前提。engine_spec / docs	est_checklists.md (393 行付近の v26) を ABI v27 = 158 へ。`activeNodeId` は SubTree 展開後の実行木の id であることを engine_spec と feature guide に明記 (sub-10 / sub-11 VERDICT)。

## やらないこと (このサブでは)
- コードの変更 (検証で見つかった不具合は SELF_EVAL に書き、planner が差し戻し先を決める)

## 触る場所 (planner の見立て)
- `docs\adr\ADR-025-behavior-tree.md`、`engine_spec.md`、`docs\engine-feature-guide.md`、`docs\test_checklists.md`、`docs\history\api-scripting-tools.md`、`plans\ai-roadmap-m83-m86.md`

## 受け入れ条件 (このサブ)
1. (spec 17) 上の文書が実装と一致 (番号: TypeId・ABI 版とスロット数・snapshot 版を実コードから引いて書く)。
2. (spec 16) Debug / Release `/p:MyeWarnAsError=true` 0 警告、Editor `--selftest` 両構成・Server `--selftest` 新規 FAIL 0、`check_rules.ps1` 0、`replay_verify.bat` 全ジョブ PASS、`shot_verify.bat` (`nav` の既知 FAIL 以外 PASS)。結果を SELF_EVAL に。

## 検証コマンド
- `msbuild` Debug / Release、`bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`、`bin\x64\Release\Server.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`tools\shot_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-14 (round 1)
- 実装: docs\adr\ADR-025-behavior-tree.md を確定 (決定 1〜10、UE との違いの表、既知の限界・未検証を sub-14.md の一覧どおり全部、ユーザー判断を反映、性能は今回の計測に差し替え)。engine_spec.md に 10.11 Behavior Tree、ABI v27 = 158 の段落、ADR 一覧へ ADR-024 / 025。docs\engine-feature-guide.md に 9.6。docs\test_checklists.md に M85 節 (--bt-demo の 5 段階と BT 窓のライブ表示を含む)、旧 v26 の行を v27 = 158 へ。docs\history\api-scripting-tools.md の v27 の「C# は位置ミラーだけ」を実態へ。plans\ai-roadmap-m83-m86.md の進捗表と「M85 で決めたこと」。AGENTS.md / CLAUDE.md に番号の記載は無く変更なし。
- 仕様との差分: [追加] engine_spec の ADR 一覧に抜けていた ADR-024 も足した。[逸脱] test_checklists.md の M84 節の v26 の行を v27 = 158 に書き換えた (台帳の指示。M84 の節としては履歴が混ざる)。M83 節の v25 の行は履歴のまま。
- 検証: Debug / Release msbuild /p:MyeWarnAsError=true → 0 警告 0 エラー (GameLogic 含む)。tools\build_managed.bat → exit 0。Editor --selftest Debug 1 回目 exit 0・FAIL 0 (既知 flake 0 件)、Release exit 0・FAIL 0、Server --selftest Release / Debug exit 0・FAIL 0 (ERROR 行 4 は既存テストの意図したログ)。check_rules.ps1 → 0 error 0 warning。replay_verify (MYE_EXTRA_ARGS=--no-audio) → 全 17 ジョブ PASS (198.7 s、起動時クラッシュ・無反応の再発なし)。shot_verify → 6 枚 FAIL (parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192、nav 214/21664 = 台帳の既存 FAIL と同数値)、ほかは PASS。golden bt は FAIL 6 枚に含まれない (数値が別) ので PASS と判断 (ログに名前が出ないため、個別の maxDiff の行は未確認)。--update はしていない。docs の整合: 番号 (TypeId 78 / 79、ABI v27 = 158、snapshot 39、定数 256 / 1024 / 8 / 112) は EngineAPI.h / Components / SimSnapshot.h / BehaviorTreeLibrary.h / check_rules.ps1 から引いた。
- 性能 (今回の Editor --selftest の [perf] 100 trees x 30 nodes、1 回): Release Update 37.5 µs / StateHash 121 µs、Debug 1502 / 1932 µs。sub-01 の ADR の値 (17 / 109 µs) から Update が約 2 倍で、原因は切り分けていない。基準 0.5 ms には十分余裕。
- 自己採点: 仕様適合 4 (ADR と文書は揃えた。UE 規則は記憶ベースのまま明記) / 正しさ 4 (数値は実コードから引いた。docs の画面操作の項目は目視未実施) / コード品質 4 (文書のみ) / テスト 4 (全体検証一式を回し、新規 FAIL 0。bt golden は間接判定)。
- 不安・質問: (1) 文書の「Window メニューからも開く」「Create メニューの名称」は spec に基づく記述で、画面では確認していない。(2) ADR の性能値は 1 回の計測 (ばらつく)。(3) test_checklists.md の M84 節の v26 の行を書き換えたが、履歴として v26 に戻すかは planner 判断。
- 触ったファイル: docs\adr\ADR-025-behavior-tree.md、engine_spec.md、docs\engine-feature-guide.md、docs\test_checklists.md、docs\history\api-scripting-tools.md、plans\ai-roadmap-m83-m86.md、plans\m85-behavior-tree\sub-14.md
- 申し送り: 別件 (範囲外) の状況は変わらず (golden 既存 FAIL 6 枚、replay_verify の起動時クラッシュは今回再発せず)。docs の 3 ファイルは作業ツリーが CRLF (git は LF 正規化) なので、diff は autocrlf 下で見ること。

## フィードバック履歴
- round 1: VERDICT OK (planner)。文書 6 本と全体検証一式 (selftest 3 構成・check_rules・replay_verify 17 PASS・shot_verify は既存 6 枚のみ FAIL) を確認。M84 節の v26→v27 書き換えは nit
