# M81 汎用 Dedicated Server + AWS GameLift 対応 — 設計案

## Context
現状のネットは 2 人 P2P (遅延ロックステップ + 予測ロールバック、ADR-013) だけで、ヘッドレス実行・3 人以上・サーバ/クライアント構成・ホスティング連携が無い。
「汎用の専用サーバ」を作り、ホスティング先の 1 つとして AWS GameLift を差し込めるようにする。
決定性の土台 (`RunOneTick` 1 本、`InputSnapshot` 4 レーン、`SimSnapshot`、`HashWorld`、入力列 .rep) はそのまま流用する。

承認後の進め方: 本内容を `C:\HAL\MyEngin\plans\m81-dedicated-server\design-draft.md` に書き出し、`/harness` に載せる (仕様確定とサブ分割は planner)。

## ユーザーと合意済みの決定
| 論点 | 決定 |
|---|---|
| 同期方式 | **入力確定型サーバ**。サーバが各 tick の全レーン入力を確定して配り、自らも sim を回す。クライアントは既存の予測ロールバックで追従。状態配信型は採らない (ADR-013 と整合) |
| サーバ OS | **Windows 先行**。Linux は後続マイルストーン (ADR-006 は今回改訂しない) |
| GameLift 範囲 | **Server SDK 組込 + GameLift Anywhere で実 AWS と疎通**。EC2 フリート、マッチメイク用バックエンド、FlexMatch は対象外 |
| 人数 | **最大 4 人** (既存 `kMaxPlayers=4` / .rep / ABI のレーン数を維持) |
| 遅延入力 | **期限で確定**。締め切り超過のレーンは前 tick の入力を繰り返して確定。遅れた本人はロールバックで合わせる |
| 参加/切断 | **再接続と途中参加まで**。切断レーンは無入力で続行、(再)参加者にはサーバが `SimSnapshot` を送って合流 |
| ゲーム側 API | **読取 + 参加/離脱の通知**。接続状態は確定入力に含める「システム入力」として全員同じ tick に届ける。ABI bump |

## 設計の骨子

### 1. 役割とトポロジ
- `NetRole` に `Server` / `Client` を追加。既存の P2P (`Host`/`Join`) は残す。
- サーバは入力を持たない (レーン 0〜3 はすべてリモート)。クライアントは自レーン 1 本を送り、他レーンは予測する。

### 2. プロトコル (`src\Engine\Engine\Net\` に追加、`NetPacketHeader` / `NetIdentity` を流用)
- `Hello(playerSessionId, SimProvenance)` → `Welcome(SessionConfig, playerId, lane, SnapshotMeta)` / `Reject(食い違った項目)`。
- `ClientInput`: 自レーンの直近 N tick を冗長送信 (既存 `kNetRedundancy` と同じ流儀)。
- `Confirmed`: tick ごとの「全レーン入力 + システム入力 (レーン接続ビット・参加/離脱イベント)」を冗長配信。サーバの checkpoint ハッシュも載せる。
- `SnapshotChunk` / `SnapshotAck`: 約 150KB の `SimSnapshot` を分割して確実に送る (今回唯一の信頼性チャネル)。
- 時刻同期: サーバが各クライアントへ「入力の到着余裕」を返し、クライアントは tick の進め方を微調整して先行量を保つ。
- desync の扱い: P2P のような停止ではなく、クライアントの再同期 (スナップショットの再送) を基本にする。診断バンドルは既存の `WriteNetDesyncBundle` を流用する。
- 未決 (planner): 予測の上限 `kNetMaxSpeculation=8` (133ms) が RTT + 締め切りに足りるか。

### 3. 決定論を壊さないためのルール (全サブ共通の必須条件)
ネットとホスティングは本質的に非決定 (到着順、到着時刻、パケットロス、SDK のスレッド、実時間)。これらを sim へ漏らさないことを設計の第一条件にする。

**原則: 非決定な出来事は、サーバで 1 回だけ「確定入力の値」に変換し、記録してから sim へ渡す。sim はその記録だけを見る。**

- **sim への入力は 2 種類だけ**。
  - セッション開始時に固定される `SessionConfig` (不変)。
  - tick ごとの確定入力列。中身は「レーン入力 × 4 ＋ システム入力 (システムイベント列と、そこから導いたレーン接続ビット)」。

  パケットの到着順・時刻・RTT・ロス・SDK のイベントを、sim のコードは直接読まない。
- **システムイベントには、サーバが発行する単調増加の `eventSeq` を振る** (セッション全体で 1 本の 64bit 連番)。
  - 形: `SystemEvent { eventSeq, tick, kind(Join/Leave/Rejoin), playerId, lane }`。
  - `playerId` は初回参加時の `eventSeq` で、再接続しても変わらない。レーンとは別の ID にする。
  - 同じ tick 内に複数のイベントがあるときは、`eventSeq` の昇順で適用する。
  - レーンの割り当ては「`eventSeq` 順に処理し、空いている最小のレーンを取る」という純関数で決める。切断中のレーンは、同じ `playerId` が戻るかタイムアウトするまで予約しておく。
  - クライアントは `eventSeq` の欠番を検出して、取りこぼしに気づける。
  - 連番を振るのは到着順なので、ネット由来の非決定性が消えるわけではない。連番の役割は「非決定な順序を、サーバで 1 回だけ明示的な値に固定し、記録する」こと。
- **サーバの確定処理は決定的なキーで行う**。締め切りの判定には実時間を使うが、その結果は「どの tick に何を入れたか」という値として記録する。走査順はレーン番号順と `eventSeq` 順に固定し、アドレスや unordered コンテナは使わない。
- **代替入力の規則は純関数**にする (締め切り超過 → 前 tick の確定入力、切断 → 無入力)。サーバとクライアントの予測で同じ関数を共有する。
- **非同期の出来事は tick 境界で、決まった順序で適用する**。GameLift SDK のコールバック、受信スレッド (作る場合)、スナップショットの受信完了は、どれもキューに積み、メインループの tick 境界でまとめて処理する。
- **実時間は sim に入れない**。実時間を使うのはペーシング、締め切り、時刻同期、タイムアウトだけで、どれも sim の外側に置く。
- **途中参加の合流点を固定する**。スナップショットは、その tick の構造変更がすべて Commit された後 (`World::ApplyStructuralChanges` の直後 = tick 末) にだけ撮る。これは既存の `World::SnapshotWrite` が MYE_CHECK で強制している撮影点と同じ。スナップショットには後述の `SnapshotMeta` を付け、復元直後にハッシュが一致しなければ参加させない。
- **Debug/Release で sim の状態を変えない**。Server.exe もビット一致の対象にする。ネットの統計や表示は sim の外で扱う。
- **ゲーム側 API の値は、出どころで 3 種類に分ける**。
  - 接続状態、参加/離脱、`playerId`: 確定入力から導く。sim から読んでよい。
  - `NetIsServer` / `NetIsClient`: `SessionConfig.role` から返す不変値。
    - 不変でも、sim から読むことは禁止する。同じ sim をサーバとクライアントの両方で回すので、`if (NetIsServer()) SpawnEnemy();` は必ず desync する。使ってよいのは表示、ログ、運用だけ。
    - tick 中に呼ばれたら毎回エラーログを出し、決まった値 (0) を返す。全員が同じ値になるので desync は起きない。これは Debug と Release で同じ挙動にする。止めずにログで知らせるのは、1 人の不具合で試合全体を落とさないため。
    - selftest で、tick 中に呼んだら 0 が返りログが出ることを固定する。
  - RTT、ロールバック回数など: 既存の Net* 系と同じく表示専用。
- **機械的に見張る**。`tools\check_rules.ps1` にルールを追加し、sim 側のシステム (TickRunner 配下) から `Net/` `Hosting/` `Platform/Net/` のヘッダを include していないことを検査する。ゲーム側からホスティング SDK へ依存していないことも同様に検査する。
- **常時検証する**。サーバは checkpoint ハッシュを配信し、クライアントは自分のハッシュと突き合わせる。不一致になったら診断バンドルを出したうえで再同期する (検出は黙って飲み込まない)。
- **記録で再現できるようにする**。サーバの .rep には、ユーザー入力とシステム入力に加えて、後述の `SessionConfig` と出自情報も記録する (`kReplayFileVersion` を bump)。オフラインで再生してサーバと同じハッシュ列になること (`--rep-diff` / `--hash-diff`) を、完了条件にする。

#### SessionConfig と出自情報 (Replay・スナップショット・接続照合で共通の 1 つの型)
- `SessionConfig` (不変): `role`、`playerCount`、`tickRate`、`seed`、`inputDelay`、`configBits`、締め切り tick 数。
  - `tickRate` は 60 固定なので、記録するのは照合のため (違えば再生を拒否)。
  - `role` は記録者の役割。再生結果は role に依存してはならない。サーバの .rep をクライアントの PC で再生しても、同じハッシュ列になる。
- 出自情報 `SimProvenance`:

  | 項目 | 中身 | 今あるか |
  |---|---|---|
  | `engineVersion` | エンジンのビルド ID | 無い (新規) |
  | `protocolVersion` | プロトコルの版 | NetIdentity にある |
  | `apiVersion` | ABI の版 | NetIdentity にある |
  | `schemaVersion` | スナップショットの版 (`kSimSnapshotVersion`)。`InputSnapshot` のレイアウトの版も含める | NetIdentity にある |
  | `gameVersion` | GameLogic.dll (と C# アセンブリ) の内容ハッシュ | **無い**。今は ABI の版しか照合しておらず、ロジックだけ違う DLL は素通りする。専用サーバではクライアントとサーバのビルド違いが desync の最頻原因になるので、必須 |
  | `contentHash` | sim に効くアセットの内容ハッシュ | **無い**。今は fontMetricsHash だけ。対象はシーン、プレハブ、物理・破壊・音響など sim が読む資産で、範囲は planner が決める。パッケージ時に manifest として焼き、実行時に毎回全部を走査するのは避ける |
  | `initialSnapshotHash` | 開始スナップショット (blob) のハッシュ | 部分的。startWorldHash はワールドハッシュで、blob のハッシュではない |

  NetIdentity はこの型から作る形へ寄せ、照合項目が二重管理にならないようにする。
- `SnapshotMeta` (スナップショットごと): `tick`、`worldHash`、`SessionConfig`、`SimProvenance`、`lastEventSeq`。
  - RNG の状態は持たせない。すでに `SimSnapshot` の本体 (World の RNG) に入っていてハッシュの対象でもあるので、2 つ目の真値を作ると食い違いの元になる。
  - 初期 seed は `SessionConfig.seed` として持つ。こちらは「どこから始まったか」を示す値で、RNG の現在値とは別物。
- .rep ヘッダ: `SessionConfig` + `SimProvenance` + 開始時の `SnapshotMeta`。既存の `rngState/rngInc` は開始スナップショットと役割が重なるので、扱いは planner が整理する (旧版の .rep を読めるようにする)。

**開発の進め方**
- 各サブのコミット前に、`replay_verify.bat` (Debug/Release/WARP のビット一致) と、新しい `server_verify` の両方を通す。
- ネット由来の値を sim へ渡す経路を追加するときは、それが確定入力に記録される値であることをレビューで確認する。reviewer の観点に入れる。

### 4. ヘッドレス Server.exe
- 新プロジェクト `build\Server.vcxproj` (Console サブシステム、D3D・ImGui・オーディオなし) と `src\Server\`。
- `ServerLoop` は `EngineLoop` とは別に作り、`RunOneTick(TickServices&)` を描画系のサービスを null にして呼ぶ。60Hz の実時間ペースで回す。
- **最大の不確実性**: TickRunner と多数のシステムが Renderer のヘッダに依存している (Engine/Engine で 57 ファイル)。最初のサブで「GPU 無しで `RunOneTick` を回し、Runtime と同じハッシュになるか」を検証してから作り込む (selftest に部品レベルの実績はある)。
- GameLogic DLL はロードする (ホットリロードは不要)。C# レーンはネット中は既存どおり停止。

### 5. ホスティング抽象 (`src\Server\Hosting\`)
- `IHostingProvider`: `Init` / `NotifyReady(port, logPaths)` / `Poll() → イベント列 (StartSession, Terminate, HealthCheck要求)` / `ValidatePlayer(playerSessionId)` / `PlayerLeft` / `NotifySessionEnded`。
- 実装 `LocalHosting`: CLI でポートを指定し、即セッション開始・全員受け入れ。開発と CI 用。
- 実装 `GameLiftHosting`: Server SDK 5.x (C++) の `InitSDK` / `ProcessReady` / `OnStartGameSession` → `ActivateGameSession` / `AcceptPlayerSession` / `RemovePlayerSession` / `OnProcessTerminate` → `ProcessEnding` / `OnHealthCheck` に対応させる。
- SDK はビルド済み .lib と公開ヘッダを `external\` に置き、`VERSIONS.md` に版を記録する (nethost と同じ流儀)。CMake はビルド済みを作るときだけ使う。依存 (OpenSSL など) は planner が調べる。
- GameLift の依存は Server プロジェクトだけに閉じる。Engine / Runtime / Editor からは参照しない。

### 6. クライアント・エディタ・ABI
- CLI: Runtime は `--net-connect HOST:PORT [--player-session-id ID]`、Server は `--port` / `--hosting local|gamelift` / `--max-players`。
- エディタの `NetWindow` にクライアント接続とサーバ状態の表示を追加。
- ABI v23: レーン接続状態、`playerId`、参加/離脱イベントの読み取り (確定入力から導く決定的な値)。`NetIsServer` / `NetIsClient` は SessionConfig 由来で、sim からは使えない (§3 参照)。`EngineAPI.h`・`ScriptAPI.h`・`Interop.cs` を同時に更新する (ABI bump の検証レシピに従う)。

### 7. 文書
- 新 ADR (専用サーバ、入力確定型を選んだ理由、ホスティング抽象)。
- `engine_spec.md` §11、`docs\engine-feature-guide.md:315` (今「専用サーバは含めない」と書かれている箇所) を更新。
- Anywhere の手順書を docs に置く。

## 対象外 (後続候補)
Linux ビルド、EC2 フリートへのデプロイ、マッチメイク用バックエンド / FlexMatch、通信の暗号化 (今回の認証は player session ID の照合だけ)、状態配信型、5 人以上、ゲーム独自メッセージ、NAT 越え / IPv6。

## 主に触るファイル
- `src\Engine\Engine\Net\NetSession.*`, `NetRollback.*`, `NetSelfTest.cpp`, `NetRuntime.h`
- `src\Engine\Engine\Loop\TickRunner.*`, `EngineLoop.cpp` (クライアント側の統合 :635-750, :1666-1900)
- `src\Engine\Engine\Replay\Replay.h` (版 bump)
- `src\Engine\Engine\App\EngineCli.cpp`
- 新規 `src\Server\**`, `build\Server.vcxproj`, `external\gamelift-server-sdk\`
- `src\Shared\EngineAPI.h`, `ScriptAPI.h`, `src\Scripting\Interop.cs`
- `src\Editor\Windows\Project\NetWindow.cpp`

## 検証
1. selftest: 1 プロセス内のループバックで サーバ + クライアント 3 台。締め切り超過、途中参加、切断→再接続、パケットロスを試し、全員の最終ハッシュがサーバと一致すること。同じシナリオを到着順と遅延のパターンを変えて流し、確定入力列が同じなら sim ハッシュ列も同じになることを確かめる。
2. `tools\server_verify.bat` (新規、CI に載せる): Server.exe + Runtime.exe × 2〜4 を別プロセスで起動し、ロス注入あり。サーバの .rep と各クライアントの確定 tick ハッシュを照合する。さらにサーバの .rep を Debug/Release でオフライン再生し、ハッシュ列がサーバ実機と一致すること。
3. 既存の回帰: `tools\replay_verify.bat`、`Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\net_verify.bat` (P2P が壊れていないこと)。
4. 手動: GameLift Anywhere にこの PC を登録し、CLI でセッションと player session を作って Runtime から接続 → プレイ → 終了まで。AWS アカウントと IAM の準備はユーザー作業。
