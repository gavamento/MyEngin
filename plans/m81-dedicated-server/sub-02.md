# sub-02: Session 型・システム入力・SessionLanes・.rep v9・規則 13

- 依存: sub-01
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 4.1.2 / 4.1.3 / 4.2 / D9 / D10 / D11 / D12 / D15 / D16 を、**ネット無し**で sim と記録に通す。プロトコルはまだ作らない。

1. `src\Engine\Engine\Session\` を新設し、POD 型と純関数を置く:
   - `SessionConfig`、`SimProvenance` (この段では engine/game/content は 0 のまま。値の算出は sub-03)、`SnapshotMeta`、`SystemEvent` (24 バイト)、`SystemInputTick` (200 バイト、`kMaxSystemEventsPerTick = 8`)、`SessionLanes` (レーン 4 本の状態 Empty/Connected/Reserved・playerId・lastEventSeq・この tick に適用したイベントの写し)。static_assert でサイズを固定。
   - 純関数: `ApplySystemInput(SessionLanes&, const SystemInputTick&)`、`AllocateLane(const SessionLanes&) → lane or none`、`SubstituteLateInput(const InputSnapshot& prev) → InputSnapshot` (消費型フィールド chars / charCount / mouseDeltaX / mouseDeltaY / wheelDelta を 0)、`DefaultLanesFor(playerCount)` (非サーバ構成の既定 = [0, playerCount) Connected、playerId 0)。
   - 不正イベント (存在しないレーンへの Leave 等) はエラーログ + 無視 (MYE_CHECK にしない)。
2. sim への配線:
   - `EngineContext` に「この tick の SystemInputTick」(呼び出し側が確定させる) と「セッションにシステム入力があるか」を足し、`RunOneTick` の**フェーズ 1 直後・`inputActions.Evaluate` の前**で `ApplySystemInput` を 1 回呼ぶ。システム入力が無い構成では何もしない (既存シーンのハッシュ列を 1 tick も変えない)。
   - `SessionLanes` の持ち主は Scene (sim 状態)。シーン遷移 (LoadScene) で**リセットしない** (セッションはシーンを跨ぐ)。この判断と理由をコメントに書く。
3. スナップショット: SimSnapshot に `SessionLanes` の節、`kSimSnapshotVersion` 24。`SimRefs` に参照を足す。
4. ハッシュ: WorldHasher に `SessionLanes` 節。**SessionConfig にシステム入力があるときだけ**畳む (D10)。`--hash-dump` のフィールド名つき出力にも出す。
5. .rep v9: spec 4.2 のとおり。`ReplayRecorder::Start` は SessionConfig / SimProvenance / 開始 SnapshotMeta を受け取り、`RecordTick` は SystemInputTick (有無はヘッダ flags) を受け取る。`ReplayPlayer` は v8 と v9 を読む。D9 の rngState/rngInc の検査 (埋め込みスナップショットがあるとき、ヘッダ値と blob の World RNG が一致しなければ Load 失敗)。`DiffReplayFiles` は新フィールドを項目名つきで比較。EngineLoop の verify 経路は SystemInputTick を `ctx` へ流す (レーン入力の置換と**同じ場所**)。CrashRing も v9 の形で書く (システム入力の有無を保つ)。
6. check_rules 規則 13 (spec D16 / DD §3「機械的に見張る」):
   - 13-a: 次のディレクトリのソースが `Engine/Engine/Net/`、`Engine/Platform/Net/`、`Server/`、`aws/gamelift` / `GameLiftServerAPI` を include したら違反: `src\Engine\Core\**`、`src\Engine\Engine\**` (ただし `Net\**`、`Loop\EngineLoop.cpp`、`App\**` は許可リスト — 各行に理由コメント)、`src\GameLogic\**`、`src\Shared\**`。
   - 13-b: `src\Engine\Engine\Session\**` は `Net/` `Platform/Net/` を include しない (sim 側の型の置き場が Net に依存しない)。
   - 13-c: GameLift SDK のヘッダ / lib 名 (`aws-cpp-sdk-gamelift-server`) は `build\Server.vcxproj` と `src\Server\**` 以外に現れない。
   - 既存で引っかかるファイルがあれば、許可リストに入れる前に「sim から到達するか」を調べて SELF_EVAL に書く。

## やらないこと (このサブでは)
- プロトコル・ソケット・Server.exe のネット機能 (sub-04/05)
- engine/game/content の値の算出 (sub-03。この段では 0)
- ABI (sub-06)

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Session\SessionTypes.h/.cpp`、`SessionSelfTest.cpp/.h`
- `src\Engine\Engine\Loop\TickRunner.cpp` (ApplySystemInput の 1 行)、`EngineLoop.h` (EngineContext)、`EngineLoop.cpp` (verify 経路・recorder.Start・CrashRing)
- `src\Engine\Engine\Replay\Replay.h/.cpp`、`SimSnapshot.h/.cpp`、`WorldHasher.cpp`、`CrashRing.cpp`
- `src\Engine\Engine\Scene\Scene.h/.cpp` (SessionLanes の所有)
- `tools\check_rules.ps1`、`tools\gen_project_files.ps1` の実行
- selftest の登録箇所 (既存 *SelfTest の登録表)

## 受け入れ条件 (このサブ)
spec 5. の **S1, S2, S3, S4** と **C1, C2, C3, C5, C6**。
- S3 の「sub-02 前に録った .rep を sub-02 後に verify」: 着手前に `Editor.exe --local-demo --local-players 2 --synth-input --replay-ticks 600 --replay-record cache\m81b_before.rep` 等で 1 本録り、実装後の Debug / Release で `--replay-verify` して全 tick 一致を示す (v8 .rep の読込経路の実地確認を兼ねる)。

## 検証コマンド
- `tools\replay_verify.bat`
- `pwsh -File tools\check_rules.ps1` (+ 規則 13 の意図的違反での exit 1 確認)
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- 上記 S3 の旧 .rep 照合
- `bin\x64\Release\Server.exe --replay-verify cache\golden_mp.rep` (sub-01 の経路を壊していない)

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
実装:
  - src\Engine\Engine\Session\SessionTypes.h/.cpp (新規) — SessionConfig(88B)/SimProvenance(48B)/SnapshotMeta(168B)/SystemEvent(24B)/SystemInputTick(200B)/SessionLanes(272B) の POD と static_assert、SessionRole、RoleHasSystemInput、ApplySystemInput/AllocateLane/SubstituteLateInput/DefaultLanesFor/NormalizeSystemInput/FirstDifferentSystemInputField。不正イベントはエラーログ + 無視(eventSeq 昇順の安定整列後に処理、<= lastEventSeq は重複として無視、無効でも新しい seq なら lastEventSeq は進める)
  - Scene.h:Lanes() — SessionLanes を Scene が所有。Clear/LoadScene で消さない(コメントに理由)
  - EngineLoop.h:EngineContext — systemInput / hasSystemInput を追加。TickRunner.cpp:RunOneTick — inputActions.Evaluate の直前で hasSystemInput のときだけ systemInput=1 を立てて ApplySystemInput を 1 回。RecordTick にも systemInput を渡す
  - SimSnapshot.cpp/.h — SES1 節(WriteSession/ReadSession、ACU 節の後・World 節の前)、kSimSnapshotVersion 24、PeekSimSnapshotWorldRng(World 節末尾 16B)。SimSourcesOf は lanes.systemInput != 0 のときだけ SessionLanes を渡す
  - WorldHasher.h/.cpp — SimSources::sessionLanes(末尾 append)、HashSessionLanes(UI 節の直後。--hash-dump に SessionLanes 行)
  - Replay.h/.cpp — kReplayFileVersion 9、MyeReplayHeader(368B: v8 の 56B + flags + SessionConfig + SimProvenance + 開始 SnapshotMeta)、MyeReplayHeaderV8、Recorder::Start/RecordTick の拡張、Player::Load は v8/v9 を読む(v8 の新項目は 0)・未知 flags/eventCount>8 を拒否・D9(ヘッダ RNG と blob の World RNG の不一致を拒否)、HasSystemInput/SystemInputForTick、DiffReplayFiles に新フィールド(項目名つき)と tick ごとの SystemInputTick 比較
  - CrashRing.h/.cpp — CrashRingConfig::session、role が Server/Client ならレコードに SystemInputTick、OnTickBegin に systemInput 引数、ヘッダへ session/startMeta
  - EngineLoop.cpp — sessionConfig(role=netRole 生値, playerCount, tickRate 60, inputDelay)、recorder.Start へ SessionConfig/SimProvenance(initialSnapshotHash)/開始 SnapshotMeta、verify 経路で hasSystemInput と tick ごとの systemInput 置換(レーン入力置換と同じ場所)、CrashRing へ session と systemInput
  - HeadlessSim.cpp — VerifyReplay にも同じ置換(Server.exe --replay-verify で v9 システム入力付き .rep を通せる)
  - AcousticAudioSelfTest.cpp — kSimSnapshotVersion の固定値 23 → 24
  - Session\SessionSelfTest.h/.cpp(新規)+ EditorMain.cpp 登録 — S1(レーン純関数)/S2(v9 往復・v8 読込・rep-diff)/S3(スナップショット往復・ハッシュのゲート・CrashRing)/D9
  - tools\check_rules.ps1 — 規則 13-a/13-b/13-c
仕様との差分:
  - [逸脱] SimRefs に SessionLanes への別参照を足さず、refs.scene->Lanes() で読み書きする — SessionLanes は Scene 所有で SimRefs.scene は必須。別参照だと配線漏れ(null のまま撮って lanes を落とす)が起こりうるため。SimSnapshot.h のコメントに明記
  - [追加] DiffReplayFiles は SessionConfig.role/inputDelay/deadlineTicks/rejoinTimeoutTicks と provenance の engine/game/protocol/api/replay の版を比較しない — net_verify が host(role1) と join(role2)・offline(role0) を --rep-diff で突き合わせるため。比較するのは flags/playerCount/tickRate/seed/configBits/referenceW,H/fontMetricsHash/schemaVersion/contentHash/initialSnapshotHash/startMeta の tick,worldHash,lastEventSeq
  - [追加] SessionLanes::systemInput (uint32) — 「システム入力を持つ記録」のハッシュ節ゲート(D10)を Scene の sim 状態として持たせた(RunOneTick が hasSystemInput のとき 1 にする。スナップショットに乗って引き継がれる)。SessionConfig 側は ctx.hasSystemInput(verify では .rep の flags.bit0)から来る
  - [追加] Join/Rejoin の SystemEvent.lane は入力では無視し、適用結果(SessionLanes::applied)にだけ割り当てレーンを入れる。Leave/Release は lane 指定+playerId 一致を要求。eventSeq が lastEventSeq 以下のイベントは重複として無視
  - [追加] 開始 SnapshotMeta は tick/lastEventSeq/config/provenance.initialSnapshotHash を常に、worldHash/blobHash はスナップショットを埋める記録(--rep-snapshot)のときだけ計算。CrashRing の startMeta は worldHash/blobHash=0(未計算)
  - [追加] 検証用の一時プローブ(MYE_M81B_PROBE で Server 役 + 合成イベントを注入)を EngineLoop.cpp に入れて実走確認し、**検証後に削除済み**(差分に残っていない: git diff に PROBE/getenv 無し)。本番には Server 役の記録経路がまだ無い(sub-04/05)
  - [未実装] SimProvenance の engine/protocol/api/schema/game/content と SessionConfig の seed/referenceW,H/fontMetricsHash/configBits/deadline/rejoinTimeout は 0 のまま(sub-03 以降)
検証:
  - Debug/Release ビルド(MSBuild) → 成功。警告は基点から有る ProjectComputeRunnerSelfTest.cpp の C4127 と LNK4204 のみ(/p:MyeWarnAsError=true は基点から失敗するので未実施)
  - tools\replay_verify.bat → "[parallel] all 14 jobs passed in 131.2s" / "[PASS] replay consistency (Debug/Release, 9 scenes ...) + snapshot round-trip + time travel + rule check"(9 シーンの Release Server.exe ヘッドレス照合 verified 600 ticks ×9 PASS を含む。ログ tmp\m81b_replay_verify.log)
  - pwsh -File tools\check_rules.ps1 → 0 error(s), 0 warning(s)
  - 規則 13 の意図的違反(一時ファイルを作って実行、終了後に削除): Scene\ から Engine/Engine/Net/NetSession.h を include → 13-a で exit 1 / Session\ から Engine/Platform/Net/UdpSocket.h → 13-a + 13-b で exit 1 / Core\ から <aws/gamelift/server/GameLiftServerAPI.h> → 13-a + 13-c で exit 1 / GameLogic\ から "Server/ServerMain.h" → 13-a で exit 1 / Scene\ のコードに "aws-cpp-sdk-gamelift-server.lib" → 13-c で exit 1。削除後は exit 0
  - 規則 13-a の既存ヒットの調査(許可リストに入れる前): EngineLoop.cpp(ネット組立の持ち主。sim へは確定入力としてだけ入る)/ HeadlessSim.cpp(既定値の NetRuntimeInfo を持つだけ)/ EngineApiTable.cpp(ABI v13 Net* が NetRuntimeInfo=表示専用 POD を読む)。**EngineApiTable.cpp は sim から到達する既存経路**(スクリプトが tick 中に Net* を呼べる。NetRuntime.h に「書き戻し禁止」の契約があり desync 検出が防波堤)。M81 では広げないが、許可リストに理由付きで載せた。App\ はヒット無しだが spec どおり許可リストに入れた
  - Debug Editor.exe --selftest → Session self test: ALL PASS、基点から失敗の 2 項目(external cherry-pick state closes the normal write gate / external revert state survives status refresh)以外 ALL PASS(exit 1 はその 2 項目のため)。Release も同じ(ログ tmp\m81b_selftest_{debug2,release}.*)。※ 最初の Debug 実行は GUI サブシステムの exe を & で呼んで並行起動してしまい weight-cache の 3 項目が一時的に失敗したが、単独で再実行すると消えた(同時実行の temp 衝突。コードの問題ではない)
  - S3 旧 .rep: sub-02 着手前に Debug Editor.exe --local-demo --local-players 2 --synth-input --replay-ticks 600 --replay-fast --warp --no-audio --replay-record cache\m81b_before.rep で録画(version 8, 139256B)。実装後に --local-demo --local-players 2 --replay-verify cache\m81b_before.rep: Debug Editor "VERIFY PASS: 600 ticks hash-identical" / Release Editor 同 / Release Server.exe "verified 600 ticks - VERIFY PASS: hash-identical"(いずれも "loaded ... (v8 ...)" と表示=v8 読込経路を通った)
  - 一時プローブでの v9 通し(システム入力付き): Debug で --rep-snapshot --local-players 4 を 100 tick 録画(71566B=368+5598+100×656 と一致)→ Debug Editor(--snapshot-stress 37 あり/なし)・Release Editor・Debug/Release Server.exe の 5 経路すべて VERIFY PASS。Release で録り直した .rep と Debug の .rep は 71566B 中 0 バイト差。tick 30 の Leave の lane を 1 バイト書き換えた .rep は Server.exe --replay-verify が tick 30 で HASH MISMATCH(exit 1)、--rep-diff が "tick 30: systemInput.events[0].lane differs"。--hash-dump に SessionLanes の項目名付き行が出る
  - tools\net_verify.bat → "[PASS] net lockstep (4 cases ...)"(host/join で role が違う .rep の --rep-diff が identical)
  - bin\x64\Release\Server.exe --local-demo --replay-verify cache\golden_mp.rep → verified 600 ticks - VERIFY PASS(spec のコマンドは --local-demo が必要。無いと既定シーンから始まって tick 0 で割れる)
  - 未実行: /p:MyeWarnAsError=true ビルド(基点から失敗)、CI(WARP)での実行
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 S1〜S4/C1〜C3/C5/C6 を満たす。SimRefs への別参照を足さなかった逸脱 1 件(理由は上記)と、SimProvenance 等を 0 のままにした(spec どおり)
  正しさ: 4 — replay_verify 14 ジョブ・旧 v8 .rep の 3 経路照合・プローブでの v9 通し(5 経路+Debug/Release バイト一致+改ざん検出)・net_verify を確認。TimeTravel/NetRollback のリングが SystemInputTick を持たない(hasSystemInput が真の構成では再シムが壊れる)点は未対応で、現状その構成を作る経路が無い
  コード品質: 4 — 純関数と POD を Session\ に分離し、規則 13 で依存方向を機械化。EngineLoop の SessionConfig は最小(role/playerCount/tickRate/inputDelay)
  テスト: 4 — SessionSelfTest(S1〜S3, D9, CrashRing)を追加。EngineLoop 経路(ctx 置換・RunOneTick の呼び出し)は selftest ではなく一時プローブで実走確認(恒久テストは無い)
不安・質問:
  - TimeTravel/NetRollback の投機リング・再シム(NetResimFrom/SeekTo)は tick 毎の SystemInputTick を保持しない。システム入力を持つ構成(sub-04 以降の Server/Client)で再シムすると ctx.systemInput が古いまま二重適用される。sub-04 で NetSpecTick に SystemInputTick を持たせ、再シム時に tick ごとに ctx.systemInput を差し替える必要がある(申し送り)
  - sub-03 で configBits/SimProvenance を埋めると、net_verify の offline(net_local) と host の --rep-diff が configBits 差で割れる可能性がある(現状は両方 0)。DiffReplayFiles が configBits を比較する点に注意
  - 規則 13-a の許可リストに EngineApiTable.cpp(Net* スロット→NetRuntimeInfo)を入れた判断(sim から到達する既存経路を許容)を planner に確認したい。代案は NetRuntime.h を Session/ 側へ移して Net/ の外に出すこと(本サブの範囲外と判断)
触ったファイル:
  - src\Engine\Engine\Session\SessionTypes.h / SessionTypes.cpp / SessionSelfTest.h / SessionSelfTest.cpp (新規)
  - src\Engine\Engine\Scene\Scene.h
  - src\Engine\Engine\Loop\EngineLoop.h / EngineLoop.cpp / TickRunner.cpp / HeadlessSim.cpp
  - src\Engine\Engine\Replay\Replay.h / Replay.cpp / SimSnapshot.h / SimSnapshot.cpp / WorldHasher.h / WorldHasher.cpp / CrashRing.h / CrashRing.cpp
  - src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp
  - src\Editor\App\EditorMain.cpp
  - tools\check_rules.ps1
  - build\Engine.vcxproj / build\Engine.vcxproj.filters (gen_project_files.ps1 の出力。新規 4 ファイルの登録なのでステージ対象)
申し送り:
  - ctx.systemInput の流れ: 記録は TickRunner(Recording)→RecordTick、再生は EngineLoop/HeadlessSim の verify 置換(レーン入力と同じ場所)。sub-04 のサーバは「確定させて ctx.hasSystemInput=true / ctx.systemInput=確定値」を RunOneTick の前に置けばよい
  - ApplySystemInput の挙動: eventSeq 昇順の安定整列、lastEventSeq 以下は重複で無視、無効イベントも新しい seq なら lastEventSeq を進める(欠番検出と食い違わないため)。この規則は sub-04 のクライアント予測と共有されること
  - MyeReplayHeader は 368B。CrashRing はヘッダを memcpy で書くので、ヘッダへ項目を足すときは static_assert(隙間なし)を維持すること
  - ReplayTickRecordBytes(playerCount, flags) が tick レコード長の正本(Load と CrashRing が使用)
  - gen_project_files.ps1 は Windows PowerShell 5.1 では構文エラーになる(pwsh で実行すること)

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。S1〜S4 / C1〜C3 / C5 / C6 / C7 を確認。逸脱 (SimRefs に別参照を足さず scene->Lanes()) は妥当として承認。DiffReplayFiles の比較項目の絞り込みも承認 (sub-03 に注意書き)。再シムでの SystemInputTick の差し替えは sub-04 へ。should: 規則 13-a の EngineApiTable.cpp の許可を「NetRuntime.h の include だけ」に絞る (ファイル単位の全面許可だと、将来 NetSession.h 等を include しても素通りする) — sub-03 で Net/ に触れるついでに直す。
