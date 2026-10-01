# sub-05: Server.exe の実運用ループ・Runtime の --net-connect・server_verify

- 依存: sub-04
- 状態: OK (commit 9f0a196)
- 往復: 1

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

5. (sub-04 VERDICT から) **EngineLoop のクライアント経路は `Net\ClientSimRunner` のフック (runTick / worldHash / liveInput / onCommitted) で繋ぐ**。EngineLoop に P2P の lambda (NetReconcile 等) と同方式のクライアント版をもう 1 本書かない (二重実装を 3 本目にしない)。P2P の lambda はそのままでよい。サーバは `ServerSession.h` 冒頭の順序 (OnPacket* → TryConfirm → RunTick → OnTickRan → .rep 記録 → Pump) を守る。
6. (sub-04 VERDICT should、spec D12 の改定) クライアント予測の改善: 予測は「最新の確定値の繰り返し。ただし chars / charCount / wheelDelta は 0」にする (mouseDeltaX/Y は繰り返す)。サーバの代替入力 (SubstituteLateInput、消費型を全部 0) とは別の関数にする — 予測は確定値に入らず sim の正しさに関係しないので、当たりやすさだけで決めてよい。R1 のケースで巻き戻し回数・再シム tick 数を変更前後で SELF_EVAL に並べる。
7. (sub-04 申し送り) EngineLoop の NetResimFrom の SystemInputTick 差し替えは sub-04 では EngineLoop 上で実走していない。R1 のロスありケースでクライアントが巻き戻しを起こし、その区間に参加・離脱が入ることをログで示す。

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

### round 1 (coder)

```
SELF_EVAL: sub-05 (round 1)
実装:
  - src/Server/Hosting/IHostingProvider.h, LocalHosting.h/.cpp (新規) — ホスティング抽象 (Init / NotifyReady / Poll / ValidatePlayer / PlayerLeft / NotifySessionEnded / Shutdown) と LocalHosting (準備完了の直後に StartSession を 1 回、認証は常に通す、Ctrl+C / Ctrl+Break / コンソールを閉じる操作を Terminate として返す。コンソールハンドラは atomic フラグを立てるだけで、処理は Poll = メインスレッド)。
  - src/Server/ServerLoop.h/.cpp (新規) — 実時間 60Hz の実運用ループ。UDP ソケット (宛先 -> 不透明な peer キーの表、Hello 以外の未知宛先は無視)、timeBeginPeriod(1) は終了時に戻す。1 周の順序は spec 4.1.4 固定: 受信 -> ホスティングの出来事 -> TryConfirm -> sim.RunTick -> OnTickRan -> .rep へ RecordTick -> Pump(送信)。開始スナップショット (tick 0) を埋め込んだ .rep v9 を --replay-record で記録 (Server ロール・SessionConfig・SimProvenance・開始 SnapshotMeta つき)。終了: --replay-ticks / --exit-when-empty (全員が出て 2 秒) / Hosting の Terminate / --server-timeout (exit 5)。5 秒ごとと終了時に統計 (tick 時間の平均・最大、遅延代替、再同期、パケット) を sim の外のログへ。4 クライアントで平均 > 4ms なら WARN。
  - src/Server/ServerMain.cpp — ライブサーバの CLI (--port / --hosting local / --max-players 1..4 / --net-deadline / --net-rejoin-timeout / --exit-when-empty / --server-timeout。--net-delay / --net-loss / --replay-record / --replay-ticks / --allow-game-mismatch / シーン指定は共通 CLI)。値の矛盾は exit 1。--hosting gamelift は「このビルドには無い」で exit 1 (sub-07)。H2 検査 (GPU / 音声 / nethost が未ロード) はライブ運転の後にも行う。
  - src/Engine/Engine/Loop/EngineLoop.cpp/.h — クライアント経路 (--net-connect)。ClientSimRunner のフック (runTick = ApplyConfirmedInputs + RunOneTick + TickEndHash、worldHash、liveInput、onCommitted = 確定 tick だけ .rep へ、onSnapshotApplied) だけで繋ぎ、P2P の NetReconcile 系 lambda と同方式のものは書いていない (tick 本体は RunOneTick 1 本)。UdpSocket + ClientSession.Start、毎フレーム ClientFrame (受信 -> runner.Update -> 終了条件)。ctx.hasSystemInput = true、tickServices.netLockstep = true、recorder は onCommitted が引き取る。参加 / 再同期のたびにスナップショット復元直後に .rep を開始 (再同期は <stem>.rs<N>.rep に切る)。NetRuntimeInfo をクライアント用に 1 か所で書く。netEnabled は P2P だけを指すようにし、クライアントは clientEnabled。tick 本体が読む設定の写し tickConfig を持ち、--net-poke-after / --net-drop-after を参加 tick 基準にした。
  - src/Engine/Engine/Net/ClientSimRunner.h/.cpp — onSnapshotApplied フック (復元 + ハッシュ照合の直後、リング開始の前)。ResimFrom が「イベントを含む区間を再シムした」ことをログ + 統計 (resimsAcrossEvents) に出す (やること 7)。
  - src/Engine/Engine/Net/ClientSession.cpp/.h + src/Engine/Engine/Session/SessionTypes.h/.cpp — PredictLaneInput (最新の確定値の繰り返し。chars / charCount / wheelDelta は 0、mouseDeltaX/Y は繰り返す)。ClientSession::PredictLane はこちらを使う。サーバの代替入力 SubstituteLateInput は消費型を全部 0 のまま (やること 6)。
  - src/Engine/Engine/Net/ServerSession.h/.cpp — ActivePeerCount() (Gone 以外の peer 数。--exit-when-empty 用)。TestSkipEventSeq は「selftest 専用・本番では呼ばない」が宣言から読めるコメントに (sub-04 nit)。
  - src/Engine/Engine/Replay/Replay.h/.cpp — DiffReplayFiles(a, b, overlapMinTicks)。0 = 従来の厳密比較 (ループは DiffTickRange に切り出して共有)。N > 0 = 開始 tick (startMeta.tick) が違う 2 本の「重なった区間」だけを世界の意味に効くヘッダ項目 + 入力・イベント・ハッシュで比べ、重なりが N tick 未満なら失敗。割れた tick はファイル内の番号ではなくセッションの tick で名指しする。
  - src/Engine/Engine/App/EngineCli.h/.cpp, EngineLoop.h (EngineConfig) — --net-connect HOST:PORT (netRole 4) / --player-session-id / --net-player-id / --net-drop-after N / --net-poke-after N / --rep-diff-overlap N。Editor / Runtime / Server の 3 Main の --rep-diff が overlap を渡す。src/Engine/Engine/Net/NetRollback.cpp — desync.txt の role に client。
  - tools/server_verify.bat (新規) — net_verify の流儀 (自己呼び出しのバックグラウンド、--rep-diff で判定、ビルドしない)。引数 [ticks] [cases]。ケース A (Debug サーバ + Debug x2、ロス 0) / B (Debug サーバ + Debug・Release・Release、ロス 20%、途中参加 1、Bye 無しの消失 + 同じ playerId・player session ID での再接続 1、--allow-game-mismatch はここだけ) / C (クライアントの sim を参加 100 tick 後に壊す -> DESYNC + バンドル + 再同期、壊した区間は壊した tick でサーバと割れ、再同期後の区間は一致) / D (Release サーバ + Release x4、R3)。各ケース: 終了コード、C++ スクリプト 0 本の検出、サーバ .rep と各クライアント .rep の重なり区間の全 tick 一致、サーバ .rep を Debug / Release の Server.exe --replay-verify と Debug / Release の Runtime.exe --replay-verify (窓あり) で再生して全 tick 一致。
  - src/Engine/Engine/Session/SessionSelfTest.cpp, src/Engine/Engine/App/EngineCliSelfTest.cpp — PredictLaneInput、--rep-diff-overlap (重なり区間の一致 / 最小 tick 未満の失敗 / 壊れた tick をセッションの tick で名指し / 重ならない 2 本は不一致)、新 CLI フラグの selftest。
  - build/Server.vcxproj(.filters) — gen_project_files.ps1 で再生成 (Server の 5 ファイル追加)。

仕様との差分:
  - [逸脱] 終了条件「全クライアントの Bye 後」は --exit-when-empty として「Bye でもタイムアウトでも、接続処理中を含む peer が 0 になって 2 秒」とした — Bye 無しで消えたクライアント (クラッシュ) も同じ Reserved になるため区別しない。加えて --server-timeout SEC (既定は無効、超えたら exit 5 で .rep を閉じる) で R2 の「必ず終わる」を保証する。
  - [逸脱] やること 2 の「SystemInputTick は verify / P2P と同じ入力の置換の場所で ctx へ入れる」 — クライアント経路は EngineLoop の while (tick ループ) を通らず ClientSimRunner が tick を回すので、置換は runTick フック内の ApplyConfirmedInputs (verify / P2P / HeadlessSim と同じ関数) で行う。置換の関数は 1 本のまま。
  - [逸脱] やること 7 の「EngineLoop の NetResimFrom の SystemInputTick 差し替えを実走」 — クライアントの再シムは NetResimFrom ではなく ClientSimRunner::ResimFrom (BuildInputs が tick ごとの確定 SystemInputTick を引き、runTick フックの ApplyConfirmedInputs で置換する) を EngineLoop 上で実走させた。NetResimFrom は P2P (hasSystemInput が偽) 専用のまま。ログ: 「[client] rollback to tick 6604 re-simulates 12 tick(s) across 1 system event(s) in 1 tick(s)」(ケース B)。
  - [追加] 検証用 CLI: クライアントの --net-player-id (再接続の主張)、--net-drop-after N (参加 tick から N tick 後に Bye 無しで終了)、--net-poke-after N (同 N tick 後に sim を 1 フィールド壊す)、共通の --rep-diff-overlap N、サーバの --exit-when-empty / --server-timeout / --net-rejoin-timeout。tick 番号を参加後の相対にしたのは、参加 tick が実行ごとに違い絶対値では指定できないため。
  - [追加] クライアントは再同期のたびに .rep を <stem>.rs<N>.rep に切る (再同期後の tick は連続しないため。--replay-ticks は区間をまたいだ累計)。
  - [追加] サーバの --replay-ticks はコマンドラインに書いたときだけ効く (EngineConfig.replayTicks の既定 600 は記録用なのでサーバには効かせない)。
  - [追加] サーバの configBits (synth / jobs / sim-cache / cook-cache) は起動オプションの申告のまま (サーバ自身は cook キャッシュを常に使わないが、申告は CLI 通り。クライアントの既定と一致して接続できる)。クライアントと食い違えば sub-04 の規則どおり ConfigBits で Reject され、クライアントは理由つきで exit 1 (実測: サーバ無し・クライアントだけ --synth-input -> 拒否、exit 1)。
  - [追加] ClientSimHooks::onSnapshotApplied / ClientSimRunnerStats::resimsAcrossEvents / ServerSession::ActivePeerCount は sub-04 の型への小さな追加。
  - [未実装] CI への任意ジョブ (やること 4): 足さなかった。判断: net_verify.bat は同じ事情 (UDP + 複数プロセス + 実時間) で ci.yml に意図的に載せておらず (bat 冒頭に理由)、その流儀に合わせた。論理の回帰は CI が回す Editor.exe --selftest の Server/client net self test (sub-04、偽トランスポート) が押さえている。入れるなら windows ジョブの selftest 後に continue-on-error: true の 1 ステップ (replay_verify が先に両構成をビルド済み) — 約 13 分追加。planner が欲しければ別途。
  - [未実装] --hosting gamelift (sub-07)。Editor の Play からの接続 (--net-connect は共通 CLI 表なので Editor も受け取るが、実走していない)。

検証:
  - MSBuild MyEngine.sln Debug|x64 / Release|x64 -> どちらも exit 0、エラー・C 警告なし (最終ソースでビルドし直し済み)。
  - bin\x64\Debug\Editor.exe --selftest -> exit 1。FAIL は基点から失敗している Source control self test の 2 項目 (external cherry-pick state closes the normal write gate / external revert state survives status refresh) のみ。その 2 項目以外 ALL PASS (Session self test / Server/client net self test / Engine CLI flag table を含む)。
  - bin\x64\Release\Editor.exe --selftest (Debug の後に直列) -> 同上、その 2 項目以外 ALL PASS。
  - tools\replay_verify.bat -> exit 0、「[parallel] all 14 jobs passed in 139.6s」「[PASS] replay consistency (Debug/Release, 9 scenes: ...)」、規則検査 0 error。
  - tools\net_verify.bat -> exit 0、「[PASS] net lockstep (4 cases x [host==joiner / == local 2P reference] + desync detection)」。
  - pwsh -File tools\check_rules.ps1 -> 0 error(s), 0 warning(s) (最終ソースで再実行)。
  - tools\server_verify.bat (全ケース、予測の変更前) -> exit 0、803 秒。A: c1/c2 とも重なり区間一致 (3000 / 600 tick)、B: c1 3600 / c2 242 (消失前) / c2b 600 (再接続) / c3 600 tick 一致、サーバ「joins 3, rejoins 1」、C: 壊した区間は tick 7207 でサーバと割れる (--rep-diff が名指し) / 再同期後 488 tick 一致 / 健全な c1 は DESYNC 無し・3000 tick 一致、D: 4 クライアント x 1800 tick 一致。全ケースでサーバ .rep を Debug / Release Server.exe と Debug / Release Runtime.exe (窓あり) で再生して全 tick 一致。
  - tools\server_verify.bat (全ケース、予測の変更後 = 最終) -> exit 0、800 秒、全ケース同様に PASS。さらに最終ソースの再ビルド後に ケース D だけ再実行 -> PASS。
  - R3 (Release の Server.exe、4 クライアント、local-demo = replay_verify の mp シーン): 「tick time: avg 0.059 ms, max 7.2 ms over 7781 ticks (peak 4 live client(s))」(変更前の実行 0.062 ms / max 14.3 ms、再ビルド後 0.059 ms / max 11.8 ms)。平均は目標 4 ms の 1/60 以下。max は参加時のスナップショット撮影 + .rep 記録と推定 (切り分けていない)。
  - R2: クライアント無しで --server-timeout 6 -> exit 5、6.95 秒で終了し .rep (364 tick) を閉じた。--replay-ticks 120 -> exit 0。--exit-when-empty は上の全ケースで実走。
  - 拒否の経路: サーバの configBits と食い違うクライアント (--synth-input だけ違う) -> 「[client] the server refused the connection: launch options (...)」、exit 1。
  - 予測改善 (やること 6) の巻き戻し回数 / 再シム tick 数 (変更前 -> 変更後。同じ server_verify、実時間依存で 1 回ずつの実測):
      A (ロス 0): c1 83 / 392 -> 64 / 174、c2 69 / 368 -> 45 / 141
      B (ロス 20%): c1 87 / 342 -> 115 / 414 (増えた)、c2 20 / 119 -> 15 / 50、c3 0 / 0 -> 3 / 10、c2b 0 / 0 -> 0 / 0
      C: c1 78 / 383 -> 73 / 212、c2 56 / 292 -> 36 / 125
    ロス 0 のケースでは再シム tick が約 半分 (A c1 392 -> 174)。B の c1 だけ増えた (ロス 20% で外れる要因が入力以外 = イベント・欠落に支配されるため、合成入力の mouseDelta の繰り返しは効かない / 実行ごとのばらつきの範囲か、は 1 回ずつの測定では切り分けていない)。ハッシュ・.rep は変更前後とも全ケースで一致。
  - やること 7: ケース B で「[client] rollback to tick 6604 re-simulates 12 tick(s) across 1 system event(s) ...」 (c1: 他クライアントの参加 / 離脱 / 再接続が予測区間に入った)、A でも 1〜2 回。その区間を含む .rep が全 tick サーバと一致。
  - 未実行: Ctrl+C による LocalHosting の Terminate (コンソールハンドラ経路)、Editor からの --net-connect、4 クライアントの Debug サーバ、GameLift。

自己採点 (1-5):
  仕様適合: 4 — R1 (a)(b)(c)・R2・R3・C1〜C7 の検証は通した。差分は上に全て列挙。CI の任意ジョブは判断して見送り、Editor / Ctrl+C は未実走。
  正しさ: 4 — 全ケースで確定 tick の .rep がサーバと一致、再生も全構成で一致、P2P (net_verify) と replay_verify は不変。B の c1 の巻き戻し増など、観測のばらつきの切り分けは未了。
  コード品質: 4 — クライアント経路はフック経由で二重実装なし、ホスティング抽象と Loop を分離、共通 CLI 表に追加。EngineLoop.cpp にクライアント用ブロック (約 190 行) が入った (while ループの外側の 1 か所に集約)。
  テスト: 4 — Session / CLI / overlap の selftest と、実プロセスの server_verify (4 ケース) で通した。ServerLoop / LocalHosting / EngineLoop のクライアント経路の自動 selftest は無い (プロセス間検証のみ)。
不安・質問:
  - [planner] spec D12 / 4.1.4 の改定 (予測 = PredictLaneInput) を spec に反映するか。sub-04 VERDICT 通り実装済み。
  - [planner] server_verify を CI の任意ジョブにするか (上の判断を覆すか)。
  - [planner] サーバの .rep はメモリに溜めて終了時 (正常終了 / Terminate / timeout) に書く。サーバが異常終了すると記録が残らない。GameLift 運用 (sub-07 / 08) の前に逐次書き出し (または CrashRing 相当) が要るか。
  - [planner] 4 クライアントの tick 時間の max が 7〜14 ms に跳ねる (参加時のスナップショット撮影が原因と推定、未切り分け)。60Hz の締め切り 3 tick (50ms) に収まるので sim には影響しないが、参加の多い運用では見直しが要る。
  - 初回 Editor.exe --package の exit 1 (sub-03 の未解明) は今回 package を回していないので再現確認なし。
触ったファイル:
  src/Server/ServerMain.cpp, src/Server/ServerLoop.h, src/Server/ServerLoop.cpp, src/Server/Hosting/IHostingProvider.h, src/Server/Hosting/LocalHosting.h, src/Server/Hosting/LocalHosting.cpp, build/Server.vcxproj, build/Server.vcxproj.filters, tools/server_verify.bat, src/Engine/Engine/Loop/EngineLoop.cpp, src/Engine/Engine/Loop/EngineLoop.h, src/Engine/Engine/App/EngineCli.cpp, src/Engine/Engine/App/EngineCli.h, src/Engine/Engine/App/EngineCliSelfTest.cpp, src/Engine/Engine/Net/ClientSession.cpp, src/Engine/Engine/Net/ClientSession.h, src/Engine/Engine/Net/ClientSimRunner.cpp, src/Engine/Engine/Net/ClientSimRunner.h, src/Engine/Engine/Net/ServerSession.cpp, src/Engine/Engine/Net/ServerSession.h, src/Engine/Engine/Net/NetRollback.cpp, src/Engine/Engine/Replay/Replay.cpp, src/Engine/Engine/Replay/Replay.h, src/Engine/Engine/Session/SessionTypes.cpp, src/Engine/Engine/Session/SessionTypes.h, src/Engine/Engine/Session/SessionSelfTest.cpp, src/Runtime/RuntimeMain.cpp, src/Editor/App/EditorMain.cpp
申し送り:
  - 次サブ (sub-07 GameLift): IHostingProvider の StartSession は HostingSessionRequest (deadlineTicks / rejoinTimeoutTicks の上書き。人数は sim 構築時に決まるので上書き不可) を運ぶ。SDK のコールバックは実装側でキューに積み Poll で返すこと。ValidatePlayer / PlayerLeft は ServerSession のフックから呼ばれる。
  - 検証の道具: tools\server_verify.bat [ticks] [cases]。例: 「tools\server_verify.bat 600 B」でケース B だけ。--rep-diff A B --rep-diff-overlap N が「開始 tick の違う .rep の重なり」を比べる。ログは cache\sv_*.log、サーバ .rep は cache\sv_<case>_server.rep。
  - 起動順の罠: --exit-when-empty は「1 人でも参加したあと全員が出たら」終わる。Debug のクライアントは起動が遅く、先に参加した Release だけで出ていくとサーバが終わる — server_verify は前のクライアントが参加し終えてから次を起動し、先に参加するクライアントを長く居させている。
  - bat の罠: if ( ... ) ブロックの中の echo に ( ) を含む表示名が展開されるとブロックが途中で閉じて壊れる (「did was unexpected」)。
  - 範囲外の観察: Editor.exe は GUI サブシステムなので PowerShell の & で呼ぶと待たない (Start-Process -Wait が要る)。サーバ起動時の scene (cache\local_players.scene.json) は全プロセスが同じファイルへ書く (--local-demo)。
```

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。R1 (a)(b)(c) は server_verify の 4 ケースで、R2 は timeout / replay-ticks / exit-when-empty で、R3 は avg 0.059ms で確認。C1〜C7 も確認。逸脱 3 件 (終了条件、置換の場所、再シム関数) は承認。CI の任意ジョブを見送ったことも承認 (net_verify と同じ流儀)。サーバ .rep の逐次書出しとクラッシュ時の記録は sub-07 へ。Editor からの --net-connect の実走は sub-09 へ。nit: ServerLoop.cpp を作り直したあと、server_verify 全ケースを通したのが作り直し前か後かが SELF_EVAL から読み切れない。司会はコミット前に `git diff --stat` が SELF_EVAL の最終状態と一致することだけ確認すること (reviewer が全ケースを再実行する)。
