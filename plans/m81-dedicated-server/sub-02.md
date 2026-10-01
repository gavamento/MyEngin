# sub-02: Session 型・システム入力・SessionLanes・.rep v9・規則 13

- 依存: sub-01
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
