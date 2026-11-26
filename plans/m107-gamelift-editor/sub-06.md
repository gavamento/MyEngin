# sub-06: 文書・ADR-042・版 0.90.4

- 依存: sub-01〜sub-05
- 状態: 未着手
- 往復: 0

## やること

1. **ADR-042** `docs/adr/ADR-042-gamelift-editor.md` (番号は着手時に `docs/adr/` の最大 + 1 を確かめる。2026-11-27 時点の最大は ADR-041):
   決定 D3 (CLI を子プロセス、SDK を組み込まない)、D4 (`--hosting gamelift-managed` の明示)、D5 (vc_redist + install.bat)、D6 (出自を揃える組)、
   D8〜D9 (停止要求の一本化と stop イベント)、D12 (秘密の扱い)、D18 (確認ダイアログ・AI は AWS を触らない)、却下した案と理由。
2. **`docs/gamelift-editor.md`** (新規): 窓の使い方 (Anywhere / EC2)、前提 (aws CLI v2・`aws login`・リージョン)、
   必要な IAM 権限 (EC2 分: `CreateBuild` `RequestUploadCredentials` `DescribeBuild` `DeleteBuild` `CreateFleet` `DescribeFleetAttributes`
   `DescribeFleetCapacity` `DescribeFleetEvents` `UpdateFleetCapacity` `DeleteFleet` `CreateGameSession` `DescribeGameSessions`
   `CreatePlayerSession` `GetGameSessionLogUrl` 等。開発中は `gamelift:*` で可、と既存手順書の流儀に合わせる)、
   費用の注意 (EC2 は台数 > 0 の間課金。帯の意味。必ず台数 0 か削除)、窓を使わない CLI 手順 (sub-02 のチェックポイント手順を整えたもの)、
   うまくいかないとき (フリートイベントの読み方)、既知の制限 (spec 7. R5〜R7、`--exit-when-empty`、1 インスタンス 1 プロセス)。
3. **`docs/gamelift-anywhere.md`**: 8 章の停止の記述を実装 (sub-01) に合わせて直す。sub-01 で「Ctrl+C / `--stop-event` → ProcessEnding」が
   全ホスティング共通になったので、記述の主旨はそのまま正しくなった。直すのは「M81 時点では gamelift で効いていなかった (M107 で修正)」の注記、
   `--stop-event` の追記、「コンソールハンドラは hosting.Init (InitSDK 最大 30 秒) の後に入る = それ以前の Ctrl+C は即時終了」の注意。
   `--log-dir` を付けると引数の矛盾エラーもログファイルへ出る (元の stderr には出ない) ことも書く。冒頭に「エディタの GameLift 窓で同じことができる」と
   `docs/gamelift-editor.md` への案内。「EC2 フリートは対象外」の文を直す。11 章の未確定 2 点は手動確認 28 の結果待ちとして残す。
4. **`engine_spec.md`**: 該当章 (専用サーバ / オンライン / エディタの窓の一覧) に GameLift 窓と Server.exe の新しい引数。冒頭 Version 行。
5. **`docs/test_checklists.md`**: 手動確認 28 / 29 の項目。
6. **版 0.90.4**: `src/Engine/Engine/App/Project.h` の `kEngineVersion`、`engine_spec.md` 冒頭、`tools/collab_fixture.ps1` の `engineVersion`。
   着手時に master の最新版を確かめ、他の作業が先に 0.90.x を使っていたら次の番号にする (AGENTS.md 8 章)。
7. **最終検証**: `Editor.exe --selftest-full`、`Server.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
   (`MYE_REPLAY_JOBS=3` で並列にしてよい — メモリの M104 の注意)。

## やらないこと (このサブでは)

- コードの変更 (版の定数を除く)。不具合が見つかったら該当サブへの差し戻しとして SELF_EVAL の「不安・質問」に書く。

## 触る場所 (planner の見立て)

- `docs/adr/ADR-042-gamelift-editor.md` (新規)、`docs/gamelift-editor.md` (新規)、`docs/gamelift-anywhere.md`、`docs/adr/ADR-022-dedicated-server.md`
  (75 行「Linux / EC2 フリート: 後続マイルストーン」に Windows EC2 は M107 で、と追記するか判断)、`engine_spec.md`、`docs/test_checklists.md`、
  `src/Engine/Engine/App/Project.h`、`tools/collab_fixture.ps1`

## 受け入れ条件 (このサブ)

spec 5. の 25〜27。

1. 文書の記述が実装と一致する (引数名・ボタン名・ファイルの置き場所・既定値)。リンク切れが無い。[文書の照合]
2. 版 0.90.4 が 3 か所で揃う。[R + 目視]
3. 最終検証 4 本が通る (結果を SELF_EVAL に)。[E][S][R][replay]

## 検証コマンド

- `bin\x64\Debug\Editor.exe --selftest-full`
- `bin\x64\Debug\Server.exe --selftest`
- `tools\check_rules.ps1`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
