# sub-14: 文書 (ADR-025) と全体検証

- 依存: sub-13
- 状態: 未着手
- 往復: 0

## やること
- `docs\adr\ADR-025-behavior-tree.md` を確定: 実行モデル (UE 方式 + ポーリングの監視、却下: 毎 tick 根から / 即時コールバック)、状態の置き場所 (システムの表 + BT 節、却下: 全部コンポーネント = ADR-024 との違いの理由)、イベントキュー (tick N+1 配送、順序、上限)、C++ タスクの状態の扱い、C# タスクは保証外、手数の上限、SubTree と BB の一致、`.bt.json` の位置と contentHash、性能の計測値、UE との違いの一覧、既知の限界。
- `engine_spec.md` (BT / BB / イベントキュー / PatrolRoute / ABI v27 の表 / snapshot v の履歴)、`docs\engine-feature-guide.md` (BT の使い方: 木を作る → BB → コンポーネント → デバッグ)、`docs\test_checklists.md` (BT の手動確認項目)、`docs\history\api-scripting-tools.md` (sub-11 で書いた分の見直し)、`plans\ai-roadmap-m83-m86.md` の進捗表 (M85 完了、M85 で計画から変えたこと)。AGENTS.md / CLAUDE.md に番号の記載があれば追従。
- 全体検証: AGENTS.md 7 章の広範な変更の一式。

- (sub-01〜sub-09 の VERDICT から) ADR-025 の既知の限界・未検証に必ず入れるもの: UE の規則は記憶ベースで未照合 (Loop / Cooldown の Abort 時を含む)、同距離タイブレークのテストの検出力、親付き Agent の RotateTo、遷移中の AnimatorPlay のポーズの飛び、SubTree の平らな展開 (1024 上限・根の LowerPriority は Self)、Patrol は入るたびに最近傍、ReloadHub が置き換えると登録のパス・名前が小文字になる、BB のキー改名・削除は編集中の木だけ追従 (ほかの木・Entity キー初期値は追従しない)、巻き戻し直後は Abort の矢印が空・SimpleParallel の Immediate の停止は矢印にしない・デバッグ線は再シム中は積まない、未保存編集中の窓では増えたノードがライブ強調されない。

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

## フィードバック履歴
