# sub-04: サーバ/クライアントのプロトコルと 1 プロセス内検証

- 依存: sub-03
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 4.1.1 / 4.1.4〜4.1.8 / 4.2 (プロトコル) / D4 / D11 / D12 を、**トランスポートとクロックを注入できる**形で実装し、1 プロセス内の決定的な偽トランスポートで検証する。プロセス間の配線 (Server.exe のループ、EngineLoop のクライアント経路) は sub-05。

1. `src\Engine\Engine\Net\` に:
   - `ServerSession`: Hello 受付 (ValidatePlayer はコールバックで注入)、eventSeq 採番、レーン割り当て (Session の純関数を呼ぶ)、締め切り判定 (現在時刻 ms は引数で受け取る)、確定入力 (レーン入力 + SystemInputTick) の生成と冗長配信、checkpoint ハッシュ、到着余裕、スナップショット送付 (チャンク ≤ 1024B、Ack ビットマップ、フレームあたり送信上限)、再接続、予約タイムアウト → Release、ResyncRequest への応答。
   - `ClientSession`: Hello/Welcome/Reject、自レーン入力の送信 (冗長 kNetRedundancy)、Confirmed の受信とリング、eventSeq 欠番検出 → ResyncRequest、スナップショット受信と組み立て、到着余裕からの tick 速度係数 (±2% 上限)、checkpoint 照合 → 不一致の報告。
   - パケットは既存 `NetPacketHeader` を流用し `NetMsg` に追加 (spec 4.2、既存値は動かさない)。
   - **トランスポート**: 送受信を `std::function` か小さな POD キュー経由にして、実 `UdpSocket` と偽トランスポートを差し替え可能にする (仮想関数の抽象クラスを増やさない方が既存の流儀に合う — coder が既存コードを見て選ぶ。選んだ理由を SELF_EVAL に)。
   - **クロック**: `NowMs()` を内部で読まない。呼び出し側が渡す。
2. ロールバック: `NetRollback` のリング容量をコンパイル時最大 (16+4) にし、上限を `Begin` の引数で受ける。P2P は 8 (現状と同じ挙動)、サーバ構成のクライアントは 12。投機記録 (`NetSpecTick`) に SystemInputTick を足し、予測が外れたとき (イベントが来た) も巻き戻す。**再シム (NetResimFrom) は tick ごとに `ctx.systemInput` / `ctx.hasSystemInput` をその tick の記録値へ差し替えてから RunOneTick を呼ぶ** (sub-02 で判明: 差し替えないと復元後の lastEventSeq に対して古い systemInput が別の tick で適用される)。TimeTravel のリング (SeekTo の再シム) も同じ: SystemInputTick をリングに持たせるか、`hasSystemInput` の構成ではタイムトラベルのリングを無効にするかを選び、選んだ理由を SELF_EVAL に書く (planner の推奨は後者 — サーバ/クライアント構成でのタイムトラベルは M81 の範囲外)。selftest に「システムイベントを含む区間をまたぐロールバック」を入れる。
3. 1 プロセス内の検証ハーネス (selftest):
   - 偽トランスポート: seed 付き `Pcg32` で遅延 (固定 + ジッタ)・ロス・並べ替え・重複を作る。時刻は仮想時刻 (ms) をテストが進める。
   - sim: sub-01 のヘッドレス sim ホストを N+1 個 (サーバ 1 + クライアント 3) 立てる。GameLogic のスクリプトを複数インスタンスで共有できない場合 (spec R-4) は、スクリプト無しのコード構築シーン (PlayerInput のミラー + 物理) で行い、その判断を SELF_EVAL に書く。
   - シナリオは spec N1〜N4。

4. (sub-01 VERDICT から) 確定入力の差し込み口は HeadlessSim の verify ループの `ctx.inputs[p] = ...` を一般化して作る。EngineLoop の TickServices 組み立てと HeadlessSim::Impl::BuildTickServices の重複は sub-01 で許容したが、**ここで入力の置換 (レーン入力 + SystemInputTick) を足すときは EngineLoop と HeadlessSim で同じ関数を呼ぶ**こと (置換の規則が 2 か所に分かれると、サーバとクライアントで消費する列が食い違う)。複数インスタンスを交互に回すときは tick 直前に `Activate()` を呼ぶ (sub-01 申し送り)。

## やらないこと (このサブでは)
- Server.exe の実時間ループ、UDP の実配線、Runtime の `--net-connect` (sub-05)
- ホスティング (sub-05 で LocalHosting、sub-07 で GameLift)
- ABI (sub-06)

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Net\ServerSession.*`、`ClientSession.*`、`NetProtocol.h` (メッセージ型)、`ServerNetSelfTest.cpp/.h`
- `src\Engine\Engine\Net\NetRollback.h/.cpp` (容量と上限、SystemInputTick)
- `src\Engine\Engine\Net\NetSession.h` (NetMsg / NetRole の追加)
- sub-01 のヘッドレス sim ホスト (複数インスタンス化に必要なら)

## 受け入れ条件 (このサブ)
spec 5. の **N1, N2, N3, N4** と **C1, C2, C3, C5, C6, C7**。
- C5 の観点: サーバの確定処理の中で、実時間・到着順が「どの tick に何を入れたか」以外の形で sim へ入っていないこと。SELF_EVAL に「sim へ入る値の一覧と、それぞれが記録される場所」を書く。

## 検証コマンド
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest` (新スイートの PASS 行を貼る)
- `tools\replay_verify.bat`、`tools\net_verify.bat`、`pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (coder)

```
SELF_EVAL: sub-04 (round 1)
実装:
  - src/Engine/Engine/Net/NetProtocol.h/.cpp — 新メッセージ本体 (Hello / Reject / Welcome / SnapshotChunk / SnapshotAck / ResyncRequest / Confirmed / tick レコード) と組み立て・検査。トランスポート型 `NetSendFn` (std::function)。ヘッダは既存 NetPacketHeader を流用
  - src/Engine/Engine/Net/ServerSession.h/.cpp — 入力確定型サーバ。Hello 受付 (provenance / configBits / 基準解像度 / フォント表の照合 + ValidatePlayer フック)、eventSeq 採番、レーン割り当て (sim と同じ ApplySystemInput をミラーに掛ける)、締め切り判定 (nowMs は引数)、確定入力 (レーン入力 + SystemInputTick) の生成と履歴、スナップショット (≤1024B チャンク、256 チャンク窓の ack ビットマップ、Pump あたり上限 32)、Confirmed の冗長配信 (新規 tick + 直前 2 tick、ack が止まれば未 ack の最古から撒き直し)、checkpoint ハッシュ配信、到着余裕、再接続 (Leave+Rejoin の乗っ取り含む)、予約のタイムアウト → Release、ResyncRequest への応答
  - src/Engine/Engine/Net/ClientSession.h/.cpp — Hello/Welcome/Reject、スナップショット受信と組み立て (blob ハッシュ検査、復元失敗は 3 回まで再要求)、自レーン入力の冗長送信 (一度決めた tick の値は変えない)、Confirmed の受信リング (連続した tick だけ確定として公開)、eventSeq 欠番検出 → ResyncRequest、到着余裕 EMA から速度係数 (±2%)、追いつき tick 数 (CatchUpTicks)
  - src/Engine/Engine/Net/ClientSimRunner.h/.cpp — クライアント側の予測・ロールバック・再同期の駆動。EngineLoop の NetReconcile / NetResimFrom / NetCommitConfirmed / NetCheckDesync と同じ方式を、sim 注入のフック (runTick / worldHash / liveInput / onCommitted) で再構成。システム入力も tick ごとに予測 (イベント無し) し、再シムは tick ごとの記録値で差し替える。desync を見つけたら WriteNetDesyncBundle で診断バンドルを出して再同期
  - src/Engine/Engine/Net/NetRollback.h/.cpp — リング配列を 16+4 のコンパイル時最大にし、上限は Begin(refs, tick, maxSpeculation) の引数 (P2P の既定 8 は従来と同じ剰余の法 = 同じ挙動)。NetSpecTick に SystemInputTick (sys/hasSys)、OnTickEnd に sys 引数 (既定 null)、SystemMatch()、MaxSpeculation()。kNetMaxSpeculationClient = 12
  - src/Engine/Engine/Net/NetSession.h/.cpp — NetRole に Server=3/Client=4、NetMsg に 6〜12、NetReject に ServerFull/PlayerRejected/UnknownPlayer (全部末尾追加、既存値は動かしていない)、NetRejectFromProvenance() (CompareNetIdentity の switch を共有関数へ抜き出した。挙動は同じ)。kNetProtoVersion は 6 のまま (P2P のパケットは 1 バイトも変えていない)
  - src/Engine/Engine/Loop/TickInputs.h (新規) — 確定入力 (レーン入力 + システム入力) を ctx へ置換する唯一の関数 ApplyConfirmedInputs / ApplyReplayInputs。EngineLoop (verify・P2P ロックステップ) と HeadlessSim (verify・RunTick) が同じ関数を呼ぶ (sub-01 申し送り・やること 4)
  - src/Engine/Engine/Loop/EngineLoop.cpp — verify と P2P ロックステップの置換を上の関数へ。NetResimFrom は hasSystemInput のとき tick ごとに記録値の SystemInputTick へ差し替え (終了後に退避値を戻す)。通常 tick の netRb.OnTickEnd にも sys を渡す。予測上限は netRb.MaxSpeculation()。タイムトラベルのリングは hasSystemInput では起こさない。P2P の挙動は不変
  - src/Engine/Engine/Loop/HeadlessSim.h/.cpp — RunTick(lanes, sys, resim) / WorldHash() / Refs() / PlayerCount() / SetPokeTick()、HeadlessSimSetup::systemInput。verify ループも ApplyReplayInputs へ
  - src/Engine/Engine/Net/ServerNetSelfTest.h/.cpp + src/Editor/App/EditorMain.cpp — 新スイート RunServerNetSelfTest (selftest 末尾に登録)。build/Engine.vcxproj(.filters) は tools\gen_project_files.ps1 で再生成

トランスポートの選択: std::function (NetSendFn / ClientSendFn) + push の OnPacket。理由: NetSession が既に std::function のフック (WaitUntilReady の pump) を使っていて抽象クラスを増やさずに済むこと、受信を pull にすると「受信キュー → 確定 → sim → 記録 → 送信」の順序 (spec 4.1.4) を呼び出し側が握れなくなること、偽トランスポートの配達順をテストが完全に決められること。時計は内部で読まず nowMs を引数で受ける。

TimeTravel の扱い: 「hasSystemInput の構成ではタイムトラベルのリングを起こさない」(planner 推奨) を選んだ。理由: リングの再シムは tick ごとの SystemInputTick を持たず、シークすると lanes が食い違う。持たせる案は TimeTravel のリング・分岐・ゴースト全部に SystemInputTick を通すことになり、M81 の範囲外のサーバ構成タイムトラベルのために変更面積が大きい。

sim へ入る値の一覧と記録先 (C5):
  サーバ: HeadlessSim::RunTick(ct.inputs, &ct.sys) の NetConfirmedTick だけ。
   (1) inputs[lane] = 届いた ClientInput (先着・一度決めたら変えない) / 締め切り超過は SubstituteLateInput(直前の確定入力) / Connected 以外はゼロ。記録先: ServerSession の履歴リング (history_[tick])。呼び出し側 (sub-05) はこれを .rep へ書いてから送る
   (2) sys.events = eventSeq (単調増加カウンタ)・playerId・kind・lane。出どころは Hello (Join/Rejoin)、peer のタイムアウト / Bye / 乗っ取り (Leave)、予約期間の経過 (Release。判定は実時間でなく tick 数)。記録先: 同じ history_[tick]
   (3) スナップショット = サーバ自身の sim 状態の撮影 (外から値は入らない)
   実時間・到着順・Hosting の都合が影響するのは「どの tick に何を入れたか」(締め切り判定、peerTimeoutMs、pacing) だけで、値としては sim に入らない。走査順は peers_ (挿入順 vector) とレーン番号順で、unordered コンテナは使っていない
  クライアント: runTick に渡す lanes は確定 tick があればその値 (サーバと同一)、無ければ自レーン = LocalInput(t)、他レーン = PredictLane (確定済み最新値から消費型を落としたもの)、システム入力 = イベント無し。記録先: NetRollback の NetSpecTick と CrashRing。予測で走った tick は確定入力と食い違えば巻き戻して置き換わり、onCommitted (= .rep の記録先) と checkpoint ハッシュに載るのは確定した tick だけ

R-4 (GameLogic.dll を 1 プロセスの複数 sim で共有できるか) の実測: 解決。HeadlessSim 4 インスタンスが GameLogic.dll (LocalPlayerDemo スクリプトを付けた --local-demo) を共有し、tick を交互に回し (サーバ 1 + クライアント 3、ロールバックの再シム込み) ても、全クライアントの確定ハッシュがサーバと一致した。N1〜N4 はスクリプト込みのシーンで実行している (スクリプト無しへの退避は不要だった)。

仕様との差分:
  - [追加] ClientSimRunner (Net/ClientSimRunner.*)。sub-04 の「触る場所」に無いが、N1〜N4 の「クライアントが予測して巻き戻す」を 1 プロセスで検証するのに必要で、sub-05 の EngineLoop も同じ部品を使える (EngineLoop の NetReconcile 等は lambda でフックできないため、同方式をフック注入で作り直した。P2P 側の lambda は触っていない = 重複は残る。sub-05 で EngineLoop のクライアント経路をこの部品へ寄せるか、lambda を足すかを選ぶ)
  - [追加] サーバは「入力を実際に届けた (有効な tick の入力が 1 本届いた) Live の peer」だけを待つ。スナップショット受信中・復元中・追いつき中の参加者は待たず、代替入力 (ゼロ / 直前値) で確定する (Welcome 前後で全員の tick が締め切りまで止まるのを避けるため)。spec 4.1.4 の「接続中の全レーンの入力が届いた」の例外。レーン状態 (Connected) は sim と同じ判定のまま
  - [追加] サーバは予定時刻の inputDelay tick 前より早く tick を確定しない (空のサーバや全員の入力が先行して届いたときの暴走防止)。TickDueMs はテストが渡す startMs 基準の整数演算
  - [追加] クライアントの追いつき (CatchUpTicks): 参加・再同期の直後に速度係数 ±2% では埋まらない遅れを、余分な tick で埋める (サーバのフロンティア + RTT 見込みとの差。許容 2 tick)。「いつ tick が回るか」だけで sim には入らない
  - [追加] ServerSession::TestSkipEventSeq (eventSeq を n 欠番にする試験用ミューテータ)。N3 の欠番検出を本物のプロトコル経路で起こすための口。本番では呼ばない
  - [追加] checkpoint 照合はクライアント側では ClientSession でなく ClientSimRunner::CheckDesync で行う (確定ハッシュの持ち主 NetRollback を持つ側)。ClientSession は ServerCheckpointHash の保持と、自分の確定 checkpoint を全パケットへ載せる (サーバが不一致を数える) ところまで
  - [追加] kServerDefaultDeadlineTicks (3 = 50ms) / kServerDefaultRejoinTimeoutTicks (1800 = 30 秒) と DefaultServerSessionConfig() — sub-03 申し送りの「SessionConfig.deadlineTicks / rejoinTimeoutTicks を埋める」。値はこのサブの試験で動かして決めたものではなく暫定。sub-05 の CLI (--net-deadline) で上書きされる
  - [追加] NetReject に ServerFull / PlayerRejected / UnknownPlayer、NetRole に Server / Client、HeadlessSim の RunTick / WorldHash / Refs / PlayerCount / SetPokeTick / systemInput (上記の実装欄)
  - [追加] 再接続は Leave → Rejoin を同じ tick に積む「乗っ取り」を許す (古い接続が生きている扱いのクライアントが落ちて即再起動した場合)。再接続の認証は playerId + player session ID の一致
  - [追加] 試験の一時ディレクトリ: bin\x64\<Config>\cache\server_net_selftest\crash\ (desync バンドルの出力先。ビルド成果物の下)
  - [未実装] 実 UdpSocket との配線、Server.exe のループ、Hosting イベント (StartSession / Terminate)、Runtime の --net-connect は sub-05。ServerSession.ValidatePlayer / PlayerLeft は std::function フックのみ (LocalHosting / GameLift は後続)
  - [未実装] ClientSession を EngineLoop に繋ぐ経路 (sub-05)。このため NetResimFrom の SystemInputTick 差し替え (EngineLoop 側) は hasSystemInput の経路がまだ無く EngineLoop では実走していない。同じ規則は ClientSimRunner::ResimFrom と TestRollbackAcrossSystemEvents で実走・検証済み

検証:
  - MSBuild MyEngine.sln (Debug|x64 / Release|x64) → どちらも exit 0。新規ファイルに警告なし
  - bin\x64\Debug\Editor.exe --selftest → exit 1。FAIL は基点から失敗している Source control self test の 2 項目 (external cherry-pick state closes the normal write gate / external revert state survives status refresh) のみ。その 2 項目以外 ALL PASS。新スイート「==== Server/client net self test: ALL PASS ====」(Debug 実測 約 50 秒: sim 4 台の起動 約 13 秒 + シナリオ)
  - bin\x64\Release\Editor.exe --selftest → exit 1。同じく Source control の 2 項目のみ。「==== Server/client net self test: ALL PASS ====」(Release 約 5 秒)。※Debug と Release を同時に走らせると M79 の surface material / deferred / water surface が FAIL する (シェーダキャッシュ置き場の共有による干渉。単独実行で解消、本件の変更とは無関係) — 単独で再実行した結果を上に書いている
  - tools\replay_verify.bat → exit 0、「[parallel] all 14 jobs passed in 137.1s」「[PASS] replay consistency (Debug/Release, 9 scenes: ...)」。Release の Server.exe --replay-verify (ヘッドレス照合) も含み「VERIFY PASS: hash-identical」9 件
  - tools\net_verify.bat → exit 0、「[PASS] net lockstep (4 cases x [host==joiner / == local 2P reference] + desync detection)」。P2P の予測ロールバック (上限 8) は従来どおり (case C: 19 rollbacks / max depth 8)
  - pwsh -File tools\check_rules.ps1 → 「0 error(s), 0 warning(s)」
  - N1: 偽トランスポート (遅延 25±15ms・ロス 20%・並べ替え 10%・重複 5%) で、クライアント 1・2 を起動 → 3 が tick 約 180 で途中参加 → 2 が突然消える → 保持中に同じ playerId で再接続 (同じレーン) → 1 が消えたままで予約が Release。joins 3 / rejoins 1 / leaves 2 / releases 1。全クライアントの確定ハッシュ = サーバのハッシュ列、サーバの SessionLanes ミラー = sim の SessionLanes。サーバの確定入力ログを初期スナップショットから別経路で再生してもハッシュ列が一致。締め切り超過: 遅延スパイク (片道 250ms) で代替入力 161 件、遅いレーンの非本人入力は全部 SubstituteLateInput(直前) 規則どおり、回復後はまた確定を進める
  - N2: 到着順・遅延が違う 3 パターン (10/15/20ms、30/40/50ms + ジッタ + 並べ替え + 重複、60/75/90ms + 同上) → サーバの確定入力ログが全 tick 同一 (≥400 tick)、ハッシュ列も同一。各実行の中で全クライアントがサーバと一致。確定入力が変わるシナリオ (N1 の各種) も実行内で一致
  - N3: eventSeq を 3 欠番にして参加を起こすと全クライアントが欠番を検出して再同期 (eventGaps 1 / resyncs 1 / snapshots 2)、以降一致。クライアント 2 の sim だけ 1 フィールドを壊す (--net-poke-tick と同じ変異) と tick 280 の checkpoint で検出 → crash\desync_280_p1\ (desync.txt + local.rep) を出力 → 再同期 → 以降一致、他クライアントは desync 0、サーバはクライアントの不一致報告を記録 (サーバは止まらない)
  - N4: 片道 75ms (RTT 150ms 相当) × 予測上限 12 → 3 クライアントとも stall 0 (最大巻き戻し深さ 8)。片道 150ms (RTT 300ms) → stall あり (各 約 500 回)、確定ハッシュは一致
  - 単体: tick レコードの往復 (バイト一致)、出自 (GameVersion) / 起動オプション / UI 基準解像度 / フォント表 / 満員 / ValidatePlayer / 誤った player session ID の拒否、--allow-game-mismatch、Bye → Reserved → Release → 次の Join が最小の空きレーン、スナップショット 400KB を 30% ロスで完成 (1 Pump の送信チャンクは上限以内、ack ウィンドウ 256 超え)、復元失敗の 3 回再要求
  - システムイベントをまたぐロールバック: 予測 (イベント無し) で 16 tick 走った区間に Join ×2 / Leave ×1 の確定入力が届いた想定で、tick 5 の直前から tick ごとの記録値で再シムすると、最初から確定入力で走った sim とハッシュ・SessionLanes がビット一致。差し替えを忘れた再シム (ネガティブコントロール) は一致しない
  - 未実行: 実 UDP / 複数プロセスでの通し (sub-05)、Release ビルドでの tick 時間計測 (R3 は sub-05)

自己採点 (1-5):
  仕様適合: 4 — N1〜N4 と C1〜C3/C5〜C7 を満たした。ただし EngineLoop 側のクライアント経路 (sub-05) が未配線で、EngineLoop の lambda と ClientSimRunner が同方式の二重実装になっている (差分欄)
  正しさ: 4 — 全シナリオでサーバ/クライアントのハッシュ列が一致し、ログ再生・ネガティブコントロールでも確認。実 UDP・長時間・4 人満員のロスは未検証
  コード品質: 3 — 責務は分けたが ServerSession.cpp / ServerNetSelfTest.cpp が大きく、EngineLoop の P2P ロジックとの二重実装が残る。予測上限 12 でのロールバック頻度 (合成入力のマウスデルタを SubstituteLateInput が 0 にするため毎 tick 外れる) の最適化は未検討
  テスト: 4 — N1〜N4 の全項目と負のコントロールを自動化。Editor.exe --selftest (Debug 約 50 秒 / Release 約 5 秒) に登録済み。実時間系 (pacing / 速度係数の収束) は統計の確認までで、収束値を assert していない
不安・質問:
  1. サーバが「届けていない peer を待たない」(差分欄) のは spec 4.1.4 の文言の例外。参加中の peer を待たない方針で良いか、待つ (締め切りまで) 方針に戻すかの判断をください。待つ方針だと、参加のたびに全員の tick が数十〜数百 ms 止まります
  2. 既定の締め切り 3 tick (50ms) / 予約期間 30 秒 は暫定 (差分欄)。GameLift のユースケースでの目安があれば指示ください
  3. 予測の外れ: 合成入力 (SynthLaneInput) はマウスデルタが非 0 の値を何 tick も繰り返すので、消費型を 0 にする予測 (D12 と同じ規則) が毎 tick 外れ、RTT 150ms で 1 クライアントが 530 tick 中 520 回巻き戻した (再シムは延べ 7 倍)。実マウスは tick ごとに値が変わるので差は小さいが、負荷が気になるなら「消費型も繰り返す」予測に変える手があります (D12 はサーバの代替入力の規則で、クライアント予測には義務ではない)。今回は D12 と揃えたまま
触ったファイル:
  - build/Engine.vcxproj
  - build/Engine.vcxproj.filters
  - src/Editor/App/EditorMain.cpp
  - src/Engine/Engine/Loop/EngineLoop.cpp
  - src/Engine/Engine/Loop/HeadlessSim.cpp
  - src/Engine/Engine/Loop/HeadlessSim.h
  - src/Engine/Engine/Loop/TickInputs.h (新規)
  - src/Engine/Engine/Net/ClientSession.cpp (新規)
  - src/Engine/Engine/Net/ClientSession.h (新規)
  - src/Engine/Engine/Net/ClientSimRunner.cpp (新規)
  - src/Engine/Engine/Net/ClientSimRunner.h (新規)
  - src/Engine/Engine/Net/NetProtocol.cpp (新規)
  - src/Engine/Engine/Net/NetProtocol.h (新規)
  - src/Engine/Engine/Net/NetRollback.cpp
  - src/Engine/Engine/Net/NetRollback.h
  - src/Engine/Engine/Net/NetSession.cpp
  - src/Engine/Engine/Net/NetSession.h
  - src/Engine/Engine/Net/ServerNetSelfTest.cpp (新規)
  - src/Engine/Engine/Net/ServerNetSelfTest.h (新規)
  - src/Engine/Engine/Net/ServerSession.cpp (新規)
  - src/Engine/Engine/Net/ServerSession.h (新規)
  - plans/m81-dedicated-server/sub-04.md (この実装メモ)
申し送り:
  - sub-05: ServerSession の使い方は Net/ServerSession.h 冒頭 (OnPacket* → TryConfirm → RunTick → OnTickRan → .rep 記録 → Pump)。HeadlessSim::RunTick が確定 tick の差し込み口。Hosting イベントは ServerSession の外 (tick 境界で処理) — StartSession で SessionConfig を上書きする場合は Init 前に決める
  - sub-05: クライアントは Net/ClientSimRunner のフック (runTick / worldHash / liveInput / onCommitted) で EngineLoop に繋げる。EngineLoop 側で hasSystemInput の経路を足すとき、tick の置換は ApplyConfirmedInputs、再シムは NetResimFrom (SystemInputTick の差し替え済み)、予測上限は Begin(refs, tick, kNetMaxSpeculationClient)
  - sub-05: .rep への記録は onCommitted (クライアント) / TryConfirm の戻り値 (サーバ) で。サーバは「記録してから Pump で送る」順序を呼び出し側が守ること
  - sub-06: 非サーバ構成の既定値 (DefaultLanesFor) はそのまま。クライアントの SessionLanes は snapshot と ApplySystemInput で sim が持つ (ClientSession は持たない)
  - 未解明の観察: Debug と Release の Editor.exe --selftest を同時に走らせると M79 の surface material 系が FAIL する (シェーダキャッシュ置き場の共有と推定、未調査)。CI が並列に回していないか司会が確認してください
  - tmp\ 配下に自分の検証ログを残している (build_*.log / st_*.log / replay_verify.log / net_verify.log)。いずれも未追跡で再生成可能。削除の許可をいただければ片付けます
```

## フィードバック履歴
- round 1: VERDICT OK (planner 2026-10-02)。N1〜N4 を GameLogic のスクリプト込み (R-4 解決) で自動化、システムイベントをまたぐロールバックとネガティブコントロール、ログ再生での一致、C1〜C3 / C5〜C7 を確認。不安 1 (Live でない peer を待たない) は承認して spec 4.1.4 を改定。不安 2 (既定値) は 3 tick / 30 秒で確定、CLI で上書き。不安 3 (予測の外れ) はクライアント予測を D12 から切り離し sub-05 で改善 (should、sub-05 に移管)。nit: `ServerSession::TestSkipEventSeq` は本番クラスに試験用の口が残る形 — 名前と宣言のコメントで「試験専用」を明示しておくこと (既にそうなら不要)。
