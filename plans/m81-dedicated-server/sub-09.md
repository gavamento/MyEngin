# sub-09: エディタ NetWindow と文書 (ADR-022 等)

- 依存: sub-05, sub-06
- 状態: OK (commit d16d77b)
- 往復: 1

## やること
spec 4.3 / E1 / E2。

1. NetWindow (`src\Editor\Windows\Project\NetWindow.cpp`): クライアント接続欄 (HOST:PORT、player session ID、接続ボタン = エディタの Play をクライアントとして始める経路。既存の P2P 開始の流儀に合わせる) と、接続中の表示 (役割、自レーン、playerId、レーン 4 本の状態と playerId、確定 tick、先行量、到着余裕、再同期回数、desync 回数)。値は NetRuntimeInfo (拡張が要れば POD のまま足す) からだけ読む。文字列は `Tr()` + LocalizationTable の en/ja。
2. Editor の Play から `--net-connect` 相当でローカルの Server.exe に繋ぎ、確定 tick が進むことをログで示す (sub-05 では Runtime だけ実走した)。スクショ 2 枚 (Release): 未接続、ローカルの Server.exe へ接続中。撮り方は既存の一時プローブ + `--screenshot` の流儀。画像は `plans\m81-dedicated-server\shots\` に置く。目視はユーザー。
3. 文書:
   - `docs\adr\ADR-022-dedicated-server.md`: 背景 (P2P の限界)、決定 (入力確定型、状態配信型を採らない理由 = ADR-013 との整合)、決定論の原則 (DD §3 の要約)、ホスティング抽象、spec D3/D4/D5/D6/D11/D12 の裁定、却下案。
   - ABI v23 (126 → 131)、D3 (役割スロットを足さない)、v13 (表示専用・機種依存) と v23 (確定入力から導く sim 値) の違い、**外部プロジェクト (三校 / HAL Collector) の GameLogic.dll は apiVersion 22 のままだと拒否されるので再ビルドが必須**、NetEventProbe (検証用、自動では付かない) を書く。
   - `engine_spec.md` §11 に §11.5 (専用サーバ: プロトコル、確定処理、途中参加、.rep v9、server_verify)、ABI v23、§13 の ADR 一覧に ADR-022。
   - `docs\engine-feature-guide.md` §13 (今「専用サーバ…は本機能に含めません」) を更新。
   - `docs\test_checklists.md` に server_verify と Anywhere 手動確認の項目。
   - 既知の制限 (多レーン UI 不可 = D13、コンピュート ABI の結果を sim に入れない = spec R-7、GPU パーティクル設定は全員一致が前提 = R-6、Server は cook キャッシュ無効 = R-8、contentHash の対象 (拡張子の除外 + その種類の .meta + manifest 自身・`scripts/Generated/` のパス除外、R-9、デモが書くシーンファイルで揺れうる)、トークン 15 分、Windows のみ) を明記。

4. (sub-07 から) Server のクラッシュバンドルの crash.txt の再現手順を Server 向けに直す (crash.rep ではなく逐次 .rep を `Server.exe --replay-verify` にかける手順)。文書には次も書く: .rep の逐次書出しと異常終了時の救済、`Server.exe --selftest`、ゲームプロパティ `myeDeadlineTicks` / `myeRejoinTimeoutTicks`、SDK の TLS は証明書を検証しないこと (暗号化は M81 の対象外)、R-11。docs	est_checklists.md に `Server.exe --selftest` を追加する。

## やらないこと (このサブでは)
- 新しいネット機能

## 触る場所 (planner の見立て)
- `src\Editor\Windows\Project\NetWindow.cpp`、LocalizationTable.inl
- `src\Engine\Engine\Net\NetRuntime.h` (表示項目が足りなければ)
- `docs\adr\ADR-022-dedicated-server.md`、`engine_spec.md`、`docs\engine-feature-guide.md`、`docs\test_checklists.md`

## 受け入れ条件 (このサブ)
spec 5. の **E1, E2** と **C1, C2, C3**。文書は参照先・内容の整合を確認する (AGENTS.md §7)。

## 検証コマンド
- `pwsh -File tools\check_rules.ps1` (規則 10)
- `bin\x64\Release\Editor.exe --selftest`
- スクショ 2 枚
- `tools\replay_verify.bat` (NetWindow / NetRuntimeInfo に触れたため)

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL 写し)
- NetRuntime.h に専用サーバ構成の表示欄 (playerId / laneState[4] / lanePlayerId[4] / arrivalMarginMs / resyncs / desyncs) を POD のまま追加。EngineLoop のクライアント節 (書く場所は 1 か所のまま) が clientSession / clientRunner / scene.Lanes() から埋める。
- NetWindow: 未接続時に HOST:PORT・player session ID・接続ボタン。接続ボタンは `--net-connect` 付きでエディタをもう 1 つ起動する (P2P と同じく「セッションは起動時に張る」流儀。窓自身のセッションは変えない)。接続中 (role 4) は役割 / 自レーン / playerId / レーン 4 本 / 確定 tick / 先行 / 到着余裕 / ping / 再同期 / desync / パケット。LocalizationTable に en/ja 両方 (check_rules 規則 10 PASS)。
- EditorApp.openNet: --net-host / --net-join / --net-connect のとき Network 窓を自動で開く。
- CrashHandler: `streamedReplay` フラグ。Server の crash.txt は「逐次 .rep を Server.exe --replay-verify にかける」手順 (sub-07 nit)。実クラッシュ (--crash-test av) で文面と、その .rep の verify を確認。
- 文書: docs\adr\ADR-022-dedicated-server.md、engine_spec.md §11.5 / §12.2 行 / §13、docs\engine-feature-guide.md §13 と検証入口表、docs\test_checklists.md の M81 節。
- スクショ (Release): plans\m81-dedicated-server\shots\net_unconnected.png / net_connected.png。後者は Release の Server.exe + Runtime 1 台 + Release の Editor (--net-connect、--autoplay) の実接続で撮った。
## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。E1 は NetWindow の追加、en/ja、規則 10、スクショ 2 枚 (planner が net_connected.png を読んで確認)、Editor からの実接続で確認。E2 は ADR-022、engine_spec §11.5、feature-guide §13、test_checklists で確認。C1〜C4・C7 も PASS。接続ボタンを「別のエディタを起動する」実装にしたことは承認。should: スクショの到着余裕が 230ms で、目標の 1 tick (約 17ms) から遠い。参加直後の追いつき中に撮った値の可能性があるが、時刻同期 (4.1.6) が収束していない可能性もある。reviewer と sub-08 で、定常状態の到着余裕をログで確かめる (spec R-12)。
