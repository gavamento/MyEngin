# sub-09: エディタ NetWindow と文書 (ADR-022 等)

- 依存: sub-05, sub-06
- 状態: 未着手
- 往復: 0

## やること
spec 4.3 / E1 / E2。

1. NetWindow (`src\Editor\Windows\Project\NetWindow.cpp`): クライアント接続欄 (HOST:PORT、player session ID、接続ボタン = エディタの Play をクライアントとして始める経路。既存の P2P 開始の流儀に合わせる) と、接続中の表示 (役割、自レーン、playerId、レーン 4 本の状態と playerId、確定 tick、先行量、到着余裕、再同期回数、desync 回数)。値は NetRuntimeInfo (拡張が要れば POD のまま足す) からだけ読む。文字列は `Tr()` + LocalizationTable の en/ja。
2. スクショ 2 枚 (Release): 未接続、ローカルの Server.exe へ接続中。撮り方は既存の一時プローブ + `--screenshot` の流儀。画像は `plans\m81-dedicated-server\shots\` に置く。目視はユーザー。
3. 文書:
   - `docs\adr\ADR-022-dedicated-server.md`: 背景 (P2P の限界)、決定 (入力確定型、状態配信型を採らない理由 = ADR-013 との整合)、決定論の原則 (DD §3 の要約)、ホスティング抽象、spec D3/D4/D5/D6/D11/D12 の裁定、却下案。
   - `engine_spec.md` §11 に §11.5 (専用サーバ: プロトコル、確定処理、途中参加、.rep v9、server_verify)、ABI v23、§13 の ADR 一覧に ADR-022。
   - `docs\engine-feature-guide.md` §13 (今「専用サーバ…は本機能に含めません」) を更新。
   - `docs\test_checklists.md` に server_verify と Anywhere 手動確認の項目。
   - 既知の制限 (多レーン UI 不可 = D13、コンピュート ABI の結果を sim に入れない = spec R-7、GPU パーティクル設定は全員一致が前提 = R-6、Server は cook キャッシュ無効 = R-8、トークン 15 分、Windows のみ) を明記。

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

## フィードバック履歴
