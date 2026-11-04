# sub-07: 文書 (engine_spec / ADR 索引 / dogfooding / test_checklists) + 最終統合確認

- 依存: sub-01〜06、Wave 2 の統合 (Wave 3。単独)
- 状態: 未着手
- 往復: 0

## 作業場所の注意

- 司会の指示した場所 (統合後の master、または worktree) で作業する。worktree なら絶対パスだけを使い `C:\HAL\MyEngin` へ書かない。最初と最後に `git status`

## やること

spec §5 の 29 / 30。実装済みの契約を文書に写し、統合後の全検証と付録 B の目視を最終確認する。

## やらないこと (このサブでは)

- コードの変更 (文書だけ。文書の整合や付録 B の目視で見つけた不具合は「不安・質問」で planner へ)

## 触る場所 (planner の見立て)

- `engine_spec.md`
  - §10 に新節 `### 10.14 Action combat foundations (M95, ADR-034)` (§10.13 の後、§11 の前): A〜F の契約、opt-in の既定、非対応 (速度倍率 / 混合ポーズの混ぜ直し / カプセルの掃引 / kinematic Rigidbody の縦 / 運動量交換)、ABI v32 の 5 本 (193 スロット)、snapshot v66〜v68、controller v3 の JSON 例 (`canInterrupt`、`skel.rootMotionY`、`clipEvents` の `time` / `frame` / `fps` 形)、selftest の `MYE_SELFTEST_ONLY` / `--selftest-list` (§11.3 か §2 の検証の節に 1 段落)
  - selftest の 3 層 (関連スイート / `--selftest` = fast / `--selftest-full`、`MYE_SELFTEST_ONLY`、`--selftest-list`、CI は full) を §11.3 の検証の節に 1 段落。`docs\test_checklists.md` の冒頭 (`:3`) の「自動化済みの検証」も `--selftest-full` に
  - §13 (`:3807` 付近) の ADR 一覧に ADR-034
  - §12.2 の実績に M95 の 1 行
- `docs\adr\ADR-034-*.md` は sub-02 で作成済み → 索引だけ (必要なら「影響」に sub-05 の縦成分を追記)
- `docs\dogfooding.md` 17 は sub-02 で解決済み → 表 (`:43`) と本文 (`:528`) の表現を最終確認
- `docs\test_checklists.md` 末尾に `## M95: アクション戦闘の土台` 節 (M94 節の体裁): 受け入れ 20 (遷移リストの「割り込み可」) / 28 (イベント帯の秒併記) / 実機の手動項目 (一時プローブのスクリプトでヒットストップ・部位ヒット・割り込み・CC 同士の体当たり・縦ルートモーションを目視、`--screenshot`)、回帰 (ST / RV / 三校 verify.bat、`MYE_SELFTEST_ONLY=m95` の使い方)
- `AGENTS.md` §7 の表に `MYE_SELFTEST_ONLY=<名前,...>` と `--selftest-list` の 1 行 (共通規則の更新先は AGENTS.md)
- `plans\m95-action-combat\harness.md` の申し送り欄は司会が書く (coder は触らない)

## 受け入れ条件 (このサブ)

- 29: 上記の文書の更新。参照先 (ファイル / 節 / 関数名 / スロット名) が実在する (`grep` で確認して SELF_EVAL に貼る)
- 30: 最終確認として、付録 B の 3. の目視 (構造体のメンバー一覧・ABI 表の末尾・`Interop.cs` の末尾・snapshot の定数と版コメント・selftest のマーカー) の結果と、ST (Debug / Release)、CR、RV 全ジョブ、三校 verify.bat の結果を SELF_EVAL に貼る (文書のみの変更なのでビルドは不要だが、M95 全体の最終確認をこのサブで行う)

## 検証コマンド

- `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full` (M95 全体の最終確認)
- `tools\check_rules.ps1`
- `tools\replay_verify.bat`
- `cmd /c C:\HAL\三校\tools\verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
