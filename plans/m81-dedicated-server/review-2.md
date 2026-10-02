# review-2 (m81-dedicated-server)

- 対象: 修正コミット c6ac1aa (M81j / sub-10)・18b53df (M81k / sub-11)・f8a5cba (M81l / sub-12)。差分は d16d77b..f8a5cba。sub-08 (G5) はユーザー作業待ちのため対象外
- 日付: 2026-10-02
- reviewer の検証ログ: `C:\HAL\MyEngin\tmp\m81r2_*`

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 の穴はすべて塞がった。サーバの sim がセーブを読む経路、D14、クライアント .rep とバンドルの再生、0 tick の偽 PASS、時刻同期の収束 (D18 の適応目標で、実プロセスでも目標に収束した)、エディタの Pause / Stop。残りは minor 2 件 (V15 の警告文、R5 判定の死角)
  機能性: 4 — C1〜C4・C6・C7 は再実行で PASS。C5 は補強された観点 (TickServices のゲート経由の外部 I/O) で差分を見直し、違反なし。V1〜V15 と R5 は selftest / server_verify / 実測で確認した。R5 の判定には死角が 1 つある (指摘 2)
  ビジュアルデザイン: 4 — shots\net_client_pause_disabled.png で Pause が無効表示、到着余裕 17.0ms を確認した。ツールチップの実表示と --lang ja は見ていない
  コード品質: 4 — netLockstep を NetLockstepBoundary 1 本から導き、TickGates / IsNetSessionGates で照合する形になった。0 tick の判定は JudgeReplayVerification に一本化され、EngineLoop と HeadlessSim で共有している。ReplayPlayer は「tick で引く」API と「通し番号で引く」API を分けた。時刻同期は追いつきと速度係数を到着余裕 1 つの誤差から導く。いずれも理由のコメント付き。EngineLoop 側のゲート照合は起動ログでの検査にとどまる (V2 の許容範囲内)
指摘:
  1. [minor] 宛先: coder — V15 の「cannot keep up」警告は、遅れの原因を往復時間と断定している。だがクライアント自身が止まっているだけ (同じ PC の WARP クライアントの CPU の奪い合い) の場合も同じ文面で出る。server_verify ケース B では「Round trip is about 1759 ms」と出たが、同じクライアントが自分で測った RTT は 70〜95ms だった。運用者が回線を疑って誤った対処をしうる — 根拠: cache\sv_B_server.log の WARN 行 (peer 1 lane 0、「Round trip is about 1759 ms」/ peer 3、「2920 ms」)。対応する cache\sv_B_c1.log の「[client] time sync: ... rtt 70.2 ms / 95.5 ms / 72.5 ms」。文面は ServerSession.cpp の WarnIfUnreachable — 期待: 推定値を「往復時間またはクライアント側の停止」と書くか、Confirmed で返っている echo から得たサーバ側の RTT 推定を併記して切り分けられるようにする
  2. [minor] 宛先: coder — R5 の late-subst 判定に死角がある。確定を一度も待たれなかったレーン (最初の有効な入力が一度も間に合わない = V15 の状態) は、サーバのレーン別ログが出ないので判定から黙って外れる。check_late_subst は「レーン行が 1 本以上ある」ことしか要求しないので、クライアント 1 台がずっと入力を落としていてもケース A は PASS しうる。V15 の WARN もケース A の合否には使われていない — 根拠: ServerLoop.cpp のレーン別ログ (`if (waited == 0) continue;`)。tools\server_verify.bat の :check_late_subst (LATESEEN だけを検査)。selftest の「V15 client that cannot keep up」では late-subst 0 / late-drop 1185 で、late-subst の割合には現れない (tmp\m81r2_selftest_Release.log) — 期待: ケース A で、参加したクライアントの数だけレーン行があること、かつ「cannot keep up」の WARN が 0 であることも合否に入れる
検証した手段:
  - tools\replay_verify.bat → exit 0 (tmp\m81r2_replay_verify.log)
  - Editor.exe --selftest。Debug → Release を直列、CWD = リポジトリ直下 → FAIL は既知の Source control 2 項目だけ。Session self test / Server/client net self test は ALL PASS (V1〜V4・V7・V11・V12・V13・V15 のシナリオを含む。時刻同期の窓平均は tmp\m81r2_selftest_Release.log: V7 は 1 秒窓で 28 → 17ms、V13 は σ 3.5 / 6.1ms で 22 / 28ms、late-subst 0%)
  - Server.exe --selftest Debug / Release → exit 0 (V10 の 7 項目を含む)
  - check_rules → 0 error (規則 13-a の HeadlessSim は OnlyInclude になった)
  - net_verify → exit 0
  - server_verify ABCD → exit 0、815 秒 (tmp\m81r2_server_verify.log)
    - ケース A: late-subst 0.33% / 0.17%、強制再同期 0、クライアントが要求した再同期 0、クライアント .rep の単独再生は 4800 / 1800 tick で PASS
    - ケース C: バンドルの local.rep を単独で再生すると、壊した tick 6403 の直前まで一致した。サーバ .rep の dump との --hash-diff が LocalTransform を名指しした
    - 観察 (合否外): ケース B は late-subst 44% / 26%。Debug WARP のクライアントが CPU の奪い合いで数秒止まった (c1 の到着余裕が最悪 -2614ms) ことによる。ケース D は負荷試験の扱いで、86〜99%、強制再同期 2
  - round 1 で「0 tick で PASS」だった .rep 2 本を新ビルドで再検証した
    - scratchpad に退避した sv_A_c2.rep → verified 600 ticks PASS
    - bin\x64\Debug\crash\desync_6768_p1\local.rep → verified 100 ticks で、壊した tick 6764 で FAIL (exit 1)。期待どおり
  - R-12 / V7 の実測: Release Server.exe + Release Editor クライアント (ハードウェア GPU) を約 1 分回した。[client] time sync のログで、到着余裕は目標に追従した (目標 54.2 → 51.9ms、余裕 54.5 → 51.7ms、σ 約 18ms。D18 の適応目標どおり)。catch-up は参加直後の 14 tick 以後増えず、late-subst 3.23%、強制再同期 0、tick gates は netLockstep=1 resim=0 recorder=0 player=1 (tmp\m81r2_r12_editor.log / tmp\m81r2_r12_server.log)。round 1 で見えた「0.98 の減速と 2% の追いつきの打ち消し合い」は解消している
  - 画像: plans\m81-dedicated-server\shots\net_client_pause_disabled.png
  - 差分の全読: TickRunner.h / HeadlessSim / EngineLoop / Replay / ClientSession / ClientSimRunner / ServerSession / ServerLoop / ServerMain / Editor (EditorApp・PlayModeController・EditorToolbar)・LocalizationTable / NetInfoProbe・SaveLoadProbe / server_verify.bat / check_rules / spec の変更
  - C5 の見直し: TickRunner のゲートを全部洗った (Recording / Verifying / Networked / resim)。外部 I/O は LoadGame (:732) と LoadPersist (:758) の 2 か所で、どちらもライブサーバ・クライアント・オフライン再生で no-op にそろった。σ / 到着余裕 / netLeaveRequested は sim の外にある (tick を回す時刻とセッションの離脱だけに効く)
  - C6: 差分を grep した (unordered_ / rand / random_device / _DEBUG / NDEBUG の追加なし)
  - 実行後、ユーザーの imgui.ini のハッシュが不変であることを確認した
前回指摘の消込:
  1. 解消 — NetLockstepBoundary を HeadlessSim (セッション構成) と EngineLoop の両方で使う。V1 の selftest (実在するセーブを置いた LoadPersist / LoadGame) が PASS
  2. 解消 — HeadlessSim::Init がセッション構成で NetRuntimeInfo を D14 の値に埋める。オフライン再生も同じ値 (V12)。selftest PASS
  3. 解消 — ReplayPlayer::StartTick 基点で引くようになった。クライアント .rep とバンドルの単独再生を、server_verify A / C と reviewer の再検証で確認した。desync.txt の手順はサーバ .rep を使う形に改められた
  4. 解消 — JudgeReplayVerification。0 tick・範囲外は FAIL (exit 1) になる。EngineLoop / HeadlessSim で共有
  5. 解消 — 追いつきと速度係数を到着余裕から導く。目標は D18 の適応目標。偽トランスポートと実プロセスの両方で収束を確認した
  6. 解消 — R5 として server_verify に組み込まれた (ケース A で判定、D は負荷試験として表示のみ)。判定の死角は新規の minor 2
  7. 解消 — クライアント接続中の Pause / Step / Play は無効、Stop は Bye で抜ける、simulateScripts は常に true。スクショで確認した
  8. 解消 — PeerTable::Sweep で宛先を回収し、満杯時は ERROR を 1 回出す。キーは再利用しない。V10 の selftest PASS
  9. 解消 — 規則 13-a は OnlyInclude で NetRuntime.h / NetSession.h だけ。check_rules は 0 error
  10. 解消 — pending_ の Leave を見て二重に積まない。V11 の selftest PASS
  新規 minor 1・2 を round 1 で書かなかった理由: どちらも round 1 の後に入ったコード (V15 の警告、R5 の判定) に対する指摘で、前回の検査の穴ではない
```
