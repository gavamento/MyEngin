# sub-05: Server.exe の実運用ループ・Runtime の --net-connect・server_verify

- 依存: sub-04
- 状態: 未着手
- 往復: 0

## やること
sub-04 のプロトコルを実プロセスへ配線し、プロセス間で検証する。

1. Server.exe:
   - `ServerLoop` (src\Server\): 60Hz の実時間ペース (スリープ精度は winmm の timeBeginPeriod を使うなら終了時に戻す)。tick 境界の処理順は spec 4.1.4 のとおり固定。
   - `IHostingProvider` と `LocalHosting` (src\Server\Hosting\)。spec 4.1.9。
   - CLI (spec 4.1.10 の Server 分)。`--replay-record` で .rep v9 (システム入力つき・開始スナップショット埋め込み) を書く。確定 tick を .rep に書いてから送信する。
   - 終了: 全クライアントの Bye 後 / `--replay-ticks` 到達 / Hosting の Terminate。タイムアウトで必ず終わる (R2)。
   - NetRuntimeInfo 相当の統計をログへ (定期、sim の外)。
2. Runtime (と Editor の Play):
   - `--net-connect HOST:PORT [--player-session-id ID]`、`--allow-game-mismatch`。
   - EngineLoop のクライアント経路: 既存 P2P の `NetReady` / `BuildNetInputs` / `NetReconcile` / `NetCheckDesync` と**同じ場所**に、ClientSession 版を並べる (経路を増やすが RunOneTick は 1 本のまま)。SystemInputTick は verify / P2P と同じ「入力の置換」の場所で ctx へ入れる。
   - 途中参加 / 再同期: スナップショット受信完了 → tick 境界で Restore → `HashWorld` 照合 → 一致で再開。不一致は再要求 (最大 3 回)。
   - 時刻同期の係数は accumulator への加算だけに効かせる。
   - クライアントの `--replay-record` は**確定 tick だけ**を .rep v9 に書く (既存の NetCommitConfirmed と同じ規約)。途中参加したクライアントの .rep は参加 tick のスナップショットを埋め込んで始まる。
3. `tools\server_verify.bat` (新規、net_verify.bat の流儀: 自己呼び出しのバックグラウンド、`--rep-diff` で判定、ビルドはしない):
   - spec R1 の (a)(b)(c)。ケース: (A) Debug サーバ + Debug×2 / ロス 0、(B) Debug サーバ + Debug/Release 混在×3 / ロス 20% / 途中参加 1 / 切断→再接続 1 (`--allow-game-mismatch` を使うのはこのケースだけ)、(C) desync 注入 (`--net-poke-tick` 相当をクライアントに) → バンドル + 再同期 → 以降一致。
   - 各クライアントの .rep と、サーバ .rep の該当 tick 範囲が一致すること (途中参加者は参加 tick 以降)。比較の道具が足りなければ `--rep-diff` に tick 範囲指定を足す。
   - サーバ .rep を Debug / Release の `Server.exe --replay-verify` と `Runtime.exe --replay-verify --warp --no-audio` で再生して全 tick 一致。
4. CI: server_verify を既存 CI 定義に**任意ジョブ** (失敗しても全体を落とさない) として足すかは、既存の CI 定義の流儀を見て判断し SELF_EVAL に書く (spec R-3)。

## やらないこと (このサブでは)
- GameLift (sub-07)
- NetWindow の UI (sub-09)
- ABI (sub-06)

## 触る場所 (planner の見立て)
- `src\Server\ServerLoop.*`、`src\Server\Hosting\IHostingProvider.h`、`LocalHosting.*`、`src\Server\ServerMain.cpp`
- `src\Engine\Engine\Loop\EngineLoop.cpp` (クライアント経路。P2P の既存経路 :635-750, :1660-1900 の近く)
- `src\Engine\Engine\App\EngineCli.cpp`
- `src\Engine\Engine\Replay\Replay.*` (`--rep-diff` の tick 範囲が要れば)
- 新規 `tools\server_verify.bat`

## 受け入れ条件 (このサブ)
spec 5. の **R1, R2, R3** と **C1〜C7 全部**。

## 検証コマンド
- `tools\server_verify.bat` (全ケースの PASS 行と、各ケースのログの所在)
- `tools\replay_verify.bat`、`tools\net_verify.bat`、`pwsh -File tools\check_rules.ps1`
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest`
- R3: Release の Server.exe のログの tick 時間の行

## 実装メモ (coder が追記)

## フィードバック履歴
