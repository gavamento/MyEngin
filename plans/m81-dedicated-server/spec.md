# M81 汎用 Dedicated Server + AWS GameLift 対応 — 仕様書

- slug: m81-dedicated-server
- 状態: 確定 (2026-10-01、planner 裁定。AskUserQuestion 不可のため [ユーザーに聞ける] 印の論点は司会経由で差し戻し可)
- 依頼原文: M81 汎用 Dedicated Server + AWS GameLift 対応。ユーザーと合意済みの設計案は C:\HAL\MyEngin\plans\m81-dedicated-server\design-draft.md (slug: m81-dedicated-server)。planner はこれを起点に仕様を確定し、サブへ分割すること。決定論を壊さないこと (design-draft §3) が全サブ共通の必須条件。
  - 元のユーザー発言: 「汎用Dedicated Server機能を作り、そのホスティング先の一つとしてAWS GameLiftを対応させたい」「決定論を壊さないように開発/ネットワーク管理を行うこと」
- 正本の関係: 合意済みの設計は `design-draft.md` (以下 DD)。本書は DD を前提に、DD が planner に委ねた未決点の裁定・コード調査で判明した事実・受け入れ条件・サブ分割を書く。**DD と本書が食い違う箇所は本書の 2. に理由ごと記録してあり、本書が優先する。**

## 1. 目的 (なぜ作るか)

今のネットは 2 人 P2P (遅延ロックステップ + 予測ロールバック、ADR-013) だけで、
「ゲームを置いておけばクラウドで誰でも途中から入れる対戦サーバ」を作れない。
達成したい状態は次の 3 つ。

1. **エンジンで作ったゲームを、GPU も窓も無いマシンで専用サーバとして動かせる** (Server.exe)。最大 4 人、途中参加・切断・再接続を扱う。
2. **ホスティング先を差し替えられる**。開発・CI は LocalHosting、本番の 1 例として AWS GameLift (Anywhere で実 AWS と疎通)。
3. **ネットとホスティングの非決定性を sim に漏らさない**。サーバの .rep をどの PC で再生しても、サーバ実機と同じハッシュ列になる。Debug/Release/WARP のビット一致 (replay_verify) は 1 tick も壊さない。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| D1 | `RunOneTick` は GPU 無しで回るか (最大の未知) | `TickRunner.cpp:227-228` で `*ts.vfxRenderer` / `*ts.resources` を無条件に参照。`SkinningSystem::Update` / `PartFollowSystem::Update` (`TickRunner.cpp:388,393`) は `RenderResources::skinnedModels` を読み、PartFollow は LocalTransform (ハッシュ対象) を書く。メッシュコライダは `Mesh::positions` (`GpuResources.h:50`) を使う。一方 `TextureLibrary::CreateFromPixels` / `LoadDdsInto` は `device_ == null` を「ヘッドレス (M48a)」として素通しする前例がある (`GpuResources.cpp:511,632`)、`MeshLibrary` には「Init を呼ばない CPU 専用モード」がある (`GpuResources.h:72`) | 合意済み (DD §4「最初のサブで検証」) | **sub-01 で縦切り検証する**。RenderResources は「device 無しで CPU 側データ (positions / skinnedModels) だけ持つ」形で使い、VfxRenderer / ParticleSystem 等は実体を作って描画しないだけにする。**TickServices の契約 (null 許容の一覧) は変えない** — 経路ごとの分岐を RunOneTick に入れると「サーバのときだけ違う」バグの温床になる (`TickRunner.h:48-50` の 1 本化原則)。Explore の調査で、tick 本体と .rep ロード / スナップショット復元 / ハッシュは GPU を使わないこと、sim が RenderResources から読むのは CPU データ (skinnedModels、Mesh の positions / indices) だけであること、`--fracture-bench` が D3D 無しで RenderResources + 物理を回している前例 (`FractureBenchmark.cpp:158,221-235`) を確認。**RunOneTick をヘッドレスで回した前例は無い**。難所は初期化の切り出し (`EngineLoop.cpp:107-591` の一枚岩) とシーン再構築の Editor 一致。詳細は sub-01 の落とし穴 a〜i |
| D2 | Server.exe は D3D をリンクしなくて済むか | `Runtime.vcxproj:30` は d3d11/dxgi/d3dcompiler/dxguid/winmm/nethost をリンク。Engine は静的ライブラリ (`Engine.vcxproj:20`) で、TickRunner が `Renderer/Device/GpuResources.h` 等を include している | 裁定 | **リンクはする、ロードはしない**。DD §4 の「D3D・ImGui・オーディオなし」は「デバイス・窓・音声出力を作らない」と解釈する。証拠として d3d11.dll / dxgi.dll / d3dcompiler_47.dll を `/DELAYLOAD` にし、実行後に `GetModuleHandleW` で**ロードされていないこと**を Server.exe 自身が検査してログに出す (受け入れ条件 1-3)。Engine を sim 専用ライブラリへ割る案は 57 ファイルの依存整理になり M81 の範囲を超えるので却下 (後続候補) |
| D3 | `NetIsServer` / `NetIsClient` を ABI に足して意味があるか | GameLogic のスクリプトが呼ばれる口は `start/update/lateUpdate/onTrigger*/onCollision*/onBreak` だけで、**全部 tick の中** (`ScriptTypes.h:57-71`)。C# レーンはネット中は止まる (`TickRunner.cpp:323-324`)。つまり「tick 中はエラーログ + 0」の規則のもとでは、スクリプトから呼ぶと**常に 0 とエラー**になり、正しく使える呼び出し元が存在しない | DD で合意済み (tick 中はエラー + 0) だが、事実として使い道が無い | **ABI v23 には入れない** (役割はエンジン側の `NetRuntimeInfo.role` / NetWindow / ログで見せる)。sim へ role が漏れる口が最初から無いので、合意の目的 (「role を sim で使わせない」) はより強く満たされる。[ユーザーに聞ける] (逆を選ぶと sub-06 のスロットが 2 本増え、selftest 1 本が増える。挙動は「呼ぶと常に 0 + エラー」) |
| D4 | `kNetMaxSpeculation = 8` (133ms) で RTT + 締め切りに足りるか | `NetRollback.h:36`、`EngineLoop.cpp:1670-1688`。入力確定型では、クライアントの先行量 ≈ (RTT + 先行マージン - inputDelay) で、確定はさらに RTT/2 遅れて届く。RTT 100ms (6 tick)・inputDelay 3・マージン 2 で ≈ 5〜8 tick、RTT 150ms で 8 を超える。リングは `kNetSpecRing = kNetMaxSpeculation + 4` 本 × 約 150KB | 裁定 | **P2P は 8 のまま。クライアント/サーバ構成のクライアントは 12 (200ms)**。リングの容量はコンパイル時に最大 16+4 本にし、上限は実行時の値 (`NetRollback::Begin` の引数) で持つ。同一リージョンの GameLift の RTT は 100ms 未満を想定し、12 なら RTT 150ms + ジッタまで stall しない。超えたら既存どおり stall (sim には影響しない)。[ユーザーに聞ける] (16 にすると最大メモリ 3MB・最悪の再シム 16 tick/フレーム、8 のままだと RTT 120ms 前後から stall が出る) |
| D5 | `contentHash` の対象範囲 | sim が読む資産は種類が多い: シーン、プレハブ、構成アセット、アニメクリップ / コントローラ (LocalTransform を書く)、スキンドモデルの骨 (PartFollow)、メッシュコライダの頂点、地形 (SampleTerrainHeight)、物理・破壊・音響の設定、アクションマップ、`project_settings.json` (UI 基準解像度)、フォント計測表 (既存の fontMetricsHash)。種類ごとに「入れる」を列挙すると、新しい資産種別が増えたときに 1 か所漏れて静かに desync する | 裁定 | **assets\ 配下の全ファイルから「描画・音声専用の拡張子」を除いた集合**を対象にする (除外は明示リスト: 画像 `.png .jpg .jpeg .tga .dds .hdr .bmp`、シェーダ `.hlsl .hlsli .surface .cso`、音声 `.wav .ogg .mp3 .flac`、フォント実体 `.ttf .otf`、プローブベイク成果物、`.meta` は**含める**)。多めに含めて誤って弾くのは安全側、少なすぎて desync するのは危険側。パッケージ時に `content_manifest.json` (正規化パス昇順・各ファイルのサイズと 64bit ハッシュ・全体ハッシュ) を焼き、実行時はその全体ハッシュを読むだけ。manifest が無い開発環境では起動時に同じ関数で計算する (時間をログに出す)。[ユーザーに聞ける] (逆 = 種類の許可リストにすると、走査は小さくなるが新資産種別のたびに更新が要る) |
| D6 | `gameVersion` を GameLogic.dll のバイト列ハッシュにすると、Debug と Release の DLL が別の値になる | Debug/Release の GameLogic.dll は別バイナリだが sim は同値でなければならない (それがビット一致の契約)。net_verify は Debug↔Release を繋いで検証している (`net_verify.bat` ケース B/C)。DLL バイトのハッシュだと、この検証そのものが入口で弾かれる | 裁定 | **gameVersion = ロードした GameLogic.dll のバイト列の 64bit ハッシュ**のまま採用し、一致しないときは拒否する。ただし検証用に `--allow-game-mismatch` (Server / Runtime 共通) を設け、指定時は WARN を出して接続を許し、**その事実を .rep の SessionConfig.configBits に記録する**。server_verify の Debug↔Release 混在ケースだけがこれを使う (本番は使わない)。ソースのハッシュをビルド時に焼く案は外部プロジェクト (三校 / HAL Collector) のビルド手順にまで手が入るので却下 |
| D7 | `gameVersion` に C# アセンブリを含めるか (DD は「と C# アセンブリ」) | C# レーンはネット中は走らない (`TickRunner.cpp:323-324`、`Networked()` で停止)。サーバでも同じ RunOneTick を通るので停止する | 裁定 | **含めない**。sim に効かないものを照合に入れると、無関係な差で拒否されるだけ。DD からの逸脱として記録 |
| D8 | `engineVersion` (ビルド ID) の作り方 | 今は存在しない (DD の表)。Debug/Release で同じ値でなければならない (D6 と同じ理由) 既に `build\Common.props:81-96` の `MyeBuildInfo` ターゲットが `git describe --always --dirty --abbrev=12` を `obj\generated\<Config>\MyeBuildInfo.h` の `MYE_GIT_HASH` に焼いている (M52f、クラッシュ報告用)。Debug と Release で同じ値になる | 裁定 | **新しい仕組みを作らず `MYE_GIT_HASH` 文字列の 64bit ハッシュを engineVersion にする** (二重実装を避ける)。`"unknown"` (git 無し) は 0 とし、0 は相手も 0 のときだけ WARN 付きで一致扱い。`-dirty` 同士は中身が違っても一致してしまう (開発時の既知の穴としてログに WARN を出す) |
| D9 | .rep ヘッダの `rngState/rngInc` は開始スナップショットと役割が重なる | `EngineLoop.cpp:777-790`: 埋め込みスナップショットがあれば RNG はそこから戻り、無ければ `rngState/rngInc` を復元する。通常の replay_verify はスナップショットを埋め込まない (`--rep-snapshot` 時のみ) ので、`rngState/rngInc` はまだ現役 | 裁定 | **残す**。役割を「スナップショットが無いときの開始 RNG」に限定し、スナップショット埋め込み時は**書き手が World の RNG と同じ値を書き、読み手が blob の RNG と一致を検査して不一致なら拒否**する (真値を 2 つ持っても食い違えない)。サーバの .rep は常にスナップショットを埋め込む |
| D10 | 旧版 .rep (v8) を読めるか | `kReplayFileVersion = 8` (`Replay.h:36`)。DD は「旧版の .rep を読めるようにする」 | 裁定 | v9 のリーダーは v8 も読む (SessionConfig / SimProvenance は「不明」で埋める)。ただし **v8 に埋め込まれたスナップショットは `kSimSnapshotVersion` の版で拒否される** (今までの版 bump と同じ扱い)。新しいハッシュ節 (後述 S1) は「システム入力を持つ記録」のときだけ畳むので、v8 のスナップショット無し .rep のハッシュ列は 1 tick も変わらない |
| D11 | DD のシステムイベント種別 (Join/Leave/Rejoin) だけでは、レーン割り当てを純関数にできない | DD §3:「切断中のレーンは、同じ playerId が戻るか**タイムアウトするまで**予約」。タイムアウトは実時間の出来事なので、イベント列に載らないと「空いている最小のレーン」がイベント列の純関数にならない | 裁定 | **種別 `Release` (予約の解放) を追加**する。タイムアウトはサーバで 1 回だけ Release イベントへ変換して記録する (DD §3 の原則どおり) |
| D12 | 締め切り超過の代替入力「前 tick の入力を繰り返す」をそのまま適用してよいか | `InputSnapshot` の `chars/charCount`・`mouseDeltaX/Y`・`wheelDelta` は「その tick だけが読む」消費型 (`Input.h:50-59,85-92`)。そのまま繰り返すと文字が 2 回打たれ、視点が 2 回回る | 裁定 | 代替入力 = 前 tick の確定入力から**消費型フィールド (chars / charCount / mouseDeltaX / mouseDeltaY / wheelDelta) を 0 にしたもの**。純関数 `SubstituteLateInput` 1 本をサーバとクライアントの予測で共有する |
| D13 | UI はレーン 0 の入力だけで評価される (`TickRunner.cpp:259-277`、`Input.h:72-92`) | サーバ構成ではレーン 0 = 最初の参加者。他の参加者は UI を操作できない。P2P はキャンバス寸法を照合している (`NetSession.h:83-88`) がサーバには窓が無い | 裁定 | **既存の制約のまま** (多レーン UI は対象外)。サーバ構成ではキャンバス寸法の照合はしない (サーバに値が無い)。基準解像度とフォント計測表は SimProvenance / SessionConfig 側で照合する。文書に既知の制限として書く |
| D14 | 既存の Net* (v13) スロットはサーバ構成で何を返すか | `EngineAPI.h:409-419` は表示専用の機種依存値 | 裁定 | 意味を変えない。サーバプロセスでは `NetLocalPlayer` = 0、`NetIsConnected` = 1 (セッション稼働中)、`NetPlayerCount` = 4 (SessionConfig.playerCount)。v23 の新スロットは**確定入力から導く sim 値**で、v13 とは出どころが違うことをヘッダのコメントで区別する |
| D15 | システム入力をどこに持つか | InputSnapshot はレーンごと 112 バイトで、`.rep` / スナップショット / プロトコルの 3 版に縛られている (`Input.h:99-101`)。イベントはレーンではなく tick に属する | 裁定 | InputSnapshot は**変えない**。tick ごとの別レコード `SystemInputTick` を新設し、.rep v9 の tick レコード・`Confirmed` パケット・ロールバックの投機記録に載せる。sim 側の状態は `SessionLanes` (レーン状態・playerId・最後に適用した eventSeq) として Scene が持ち、スナップショットとハッシュに入れる |
| D17 | ビルド済みの GameLift SDK .lib (Debug 72MB + Release 53MB、gzip で計 8.5MB) と OpenSSL の DLL を git にコミットするか | DD §5 で「ビルド済み .lib と公開ヘッダを external\ に置く」と合意済み。リポジトリの方針は「クローン → F5 で動く」(external\VERSIONS.md)。CI は sln 全体をビルドするので、.lib が無いと Server のリンクで落ちる。GitHub の 1 ファイル 100MB 制限の内側。代替案は 2 つ: (a) .lib を gitignore にし、build_sdk.ps1 を CI と各開発機で実行する (OpenSSL の開発版・ネット接続・数分のビルドが必要)、(b) Git LFS | 裁定 (sub-07 VERDICT) | **コミットする** (合意どおり)。ただし .lib はリポジトリ履歴に恒久的に残るので、コミット前にユーザーに確認する。[ユーザーに聞ける] |
| D16 | 型の置き場所 (sim 側から Net/ を include させない規則と両立するか) | DD §3「sim 側から Net/ Hosting/ Platform/Net/ を include しない」。だが Replay (.rep) と TickRunner は SessionConfig / SystemInputTick を読む | 裁定 | `SessionConfig` / `SimProvenance` / `SnapshotMeta` / `SystemEvent` / `SystemInputTick` / `SessionLanes` と純関数群は **`src\Engine\Engine\Session\`** (新設、sim 側) に置く。Net/ はここを include してよいが逆は禁止 (check_rules 規則 13 で検査) |

## 3. スコープ

- やる:
  - ヘッドレス Server.exe (`build\Server.vcxproj`、Console、窓・デバイス・音声出力を作らない)
  - 入力確定型のサーバ/クライアントプロトコル (Hello/Welcome/Reject、ClientInput、Confirmed、SnapshotChunk/Ack、時刻同期、再同期)
  - 最大 4 人、締め切りによる確定、途中参加、切断→再接続、予約の解放
  - SessionConfig / SimProvenance / SnapshotMeta、.rep v9、NetIdentity の SimProvenance への寄せ (P2P プロトコル v6)
  - ホスティング抽象 `IHostingProvider` + `LocalHosting` + `GameLiftHosting` (Server SDK 5.x、Anywhere で実疎通)
  - ABI v23 (レーン状態 / playerId / システムイベントの読み取り)
  - Runtime の `--net-connect`、エディタ NetWindow の表示追加
  - `tools\server_verify.bat`、selftest、check_rules 規則 13、ADR-022、文書更新、Anywhere 手順書
- やらない (明示的に外したもの): Linux ビルド、EC2 フリート、マッチメイク / FlexMatch、通信暗号化 (認証は player session ID の照合のみ)、状態配信型、5 人以上、ゲーム独自メッセージ、NAT 越え / IPv6、多レーン UI、`NetIsServer`/`NetIsClient` スロット (D3)、Engine の sim 専用ライブラリ分割 (D2)
- 後回し: Server.exe の CI 常設 (server_verify は手元 + CI ジョブ化まで。GameLift は CI に載せない)、ホットリロード (サーバでは不要)

## 4. 仕様

### 4.1 振る舞い

**4.1.1 役割**
- `NetRole` に `Server = 3` / `Client = 4` を追加 (既存 `Host`/`Join` は P2P として残す)。
- サーバはレーンを持たない。全レーンがリモート。サーバは確定してから回す (投機しない・巻き戻さない)。
- クライアントは自レーン 1 本を `t + inputDelay` に確定して送り、他レーンは `PredictLane` と同じ規則で予測、システム入力は「イベント無し・レーン状態は直前のまま」と予測する。

**4.1.2 sim への入力 (DD §3 の原則を型で固定)**
- 不変: `SessionConfig` (§4.2)。
- tick ごと: `InputSnapshot[kMaxPlayers]` + `SystemInputTick`。
- `RunOneTick` の契約は変えない (「呼び出し側が確定させてから渡す」、`TickRunner.h:155-157`)。`SystemInputTick` も呼び出し側が確定させ、`ctx` 経由で渡す。適用はフェーズ 1 直後 (`inputActions.Evaluate` の前) に純関数 `ApplySystemInput(SessionLanes&, const SystemInputTick&)` 1 回。

**4.1.3 システムイベント**
- `SystemEvent { uint64 eventSeq; uint64 playerId; uint8 kind; uint8 lane; uint8 pad[6]; }` (24 バイト、POD)。kind = `Join=1 / Leave=2 / Rejoin=3 / Release=4`。
- `eventSeq` はセッション全体で 1 本の単調増加 (1 始まり)。`playerId` = 初回 Join の eventSeq。
- 1 tick あたり最大 `kMaxSystemEventsPerTick = 8`。超えた分はサーバが次 tick へ回す (回したこと自体が記録される)。
- 同一 tick 内は eventSeq 昇順で適用。レーン割り当て = 「eventSeq 順に処理し、Empty の最小レーン」。Leave はレーンを `Reserved` (playerId を保持) にし、同じ playerId の Rejoin で `Connected` に戻る。Release で `Empty` に戻る。満員の Join はサーバが Reject (イベントを発行しない)。
- `SessionLanes` の不変条件違反 (存在しないレーンへの Leave など) は MYE_CHECK ではなく**エラーログ + そのイベントを無視**で、サーバ/クライアント/再生で同じ判定になる (純関数の中で決まる)。
- クライアントは eventSeq の欠番を検出したら再同期を要求する。

**4.1.4 サーバの確定処理**
- tick T の確定条件: 接続中 (Connected) の全レーンの T の入力が届いた、**または** サーバの実時間が「T の予定時刻 + deadlineTicks × 16.67ms」を過ぎた。
- 未着レーン → `SubstituteLateInput(前 tick の確定値)` (D12)。Reserved / Empty のレーン → ゼロ入力。
- 走査順はレーン番号順・eventSeq 順に固定。unordered コンテナとアドレス順は使わない。
- Hosting / 受信 / SDK コールバックはキューへ積み、tick 境界で決まった順 (受信キュー → ホスティングイベント → 締め切り判定 → 確定 → RunOneTick → 記録 → 送信) で処理する。
- 確定した tick は `.rep` へ書いてから送信する (記録より先に外へ出さない)。

**4.1.5 途中参加・再接続**
- サーバは Hello を受けたら次の tick 境界 S で Join (または Rejoin) を tick S のシステム入力に入れ、**tick S が走る前の状態** (= tick S-1 の末、`ApplyStructuralChanges` 直後) のスナップショットを撮って `SnapshotMeta.tick = S` で送る。
- クライアントは受信完了 → 復元 → 復元直後の `HashWorld` が `SnapshotMeta.worldHash` と一致しなければ参加を中止して再要求 (最大 3 回、超えたら Failed)。一致したら tick S から確定入力で回る。
- スナップショットは `SnapshotChunk` (1 チャンク ≤ 1024 バイト、通し番号付き) で送り、`SnapshotAck` (受信済みビットマップ) で欠落分だけ再送する。今回唯一の信頼性チャネル。

**4.1.6 時刻同期**
- サーバは各クライアントへ「そのクライアントの tick T の入力が締め切りに対して何 ms 前に届いたか」(到着余裕) を Confirmed に載せて返す。
- クライアントは到着余裕が目標 (既定 1 tick ぶん) を保つよう、tick の進め方を ±最大 2% だけ速め/遅めにする (accumulator への加算係数)。**これは「いつ tick が回るか」だけを変え、sim には入らない。**

**4.1.7 desync**
- サーバは `kNetHashCheckpoint` (8) の倍数 tick の確定ハッシュを Confirmed に載せる。
- クライアントは自分の**確定** tick のハッシュと突き合わせ、不一致なら `WriteNetDesyncBundle` で診断バンドルを出したうえで再同期 (4.1.5 と同じスナップショット送付) を要求する。黙って飲み込まない (ログ + NetRuntimeInfo.desync + 統計)。
- サーバ側は止まらない (1 人の不具合で試合全体を落とさない)。

**4.1.8 予測上限**
- クライアント/サーバ構成のクライアント: 12 tick。P2P: 8 tick (現状維持)。D4。

**4.1.9 ホスティング**
- `IHostingProvider`: `Init` / `NotifyReady(port, logPaths)` / `Poll() → HostingEvent 列 (StartSession(SessionConfig の上書き値), Terminate, HealthCheck)` / `ValidatePlayer(playerSessionId) → bool` / `PlayerLeft(playerSessionId)` / `NotifySessionEnded()` / `Shutdown`。
- `LocalHosting`: `--port` で待受、起動直後に StartSession、ValidatePlayer は常に true。
- `GameLiftHosting`: Server SDK 5.x の `InitSDK` / `ProcessReady` / `OnStartGameSession → ActivateGameSession` / `AcceptPlayerSession` / `RemovePlayerSession` / `OnProcessTerminate → ProcessEnding` / `OnHealthCheck`。**SDK のコールバックは SDK のスレッドから来るので、ミューテックス付きキューへ積むだけ**にし、処理は Poll (メインスレッドの tick 境界) で行う。
- GameLift の依存は `build\Server.vcxproj` だけに閉じる (Engine / Runtime / Editor / GameLogic は参照しない。check_rules 規則 13)。

**4.1.10 CLI**
- Runtime / Editor: `--net-connect HOST:PORT [--player-session-id ID]`、`--allow-game-mismatch` (D6)。
- Server: `--port N` / `--hosting local|gamelift` / `--max-players N (1..4)` / `--net-delay N` / `--net-deadline N` / `--replay-record PATH` / `--replay-ticks N` (検証用の打ち切り) / `--net-loss N` / `--allow-game-mismatch` / `--replay-verify PATH` (sub-01 のヘッドレス照合) / シーン指定 (Runtime と同じ起動シーン解決)。
- 未知の引数・矛盾した組み合わせは EngineCli と同じ流儀でエラー終了 (exit 1)。

### 4.2 データ・保存形式・互換性

- `SessionConfig` (POD、固定長): `role`、`playerCount` (=最大人数)、`tickRate` (60 固定、照合用)、`seed`、`inputDelay`、`deadlineTicks`、`rejoinTimeoutTicks`、`configBits` (既存 NetConfigBits + `kCfgAllowGameMismatch`)、`referenceW/H`、`fontMetricsHash`、予約領域。role は記録者の役割で、**再生結果は role に依存しない**。
- `SimProvenance` (POD): `engineVersion` (D8)、`protocolVersion`、`apiVersion`、`schemaVersion` (= kSimSnapshotVersion と InputSnapshot のサイズ/版を畳んだ値)、`replayVersion`、`gameVersion` (D6)、`contentHash` (D5)、`initialSnapshotHash` (開始スナップショット blob のバイト列ハッシュ)。
- `SnapshotMeta`: `tick`、`worldHash`、`blobHash`、`SessionConfig`、`SimProvenance`、`lastEventSeq`。RNG は持たない (DD §3)。
- 照合は 1 関数 `CompareProvenance(a, b) → 最初に食い違った項目` (NetIdentity の `CompareNetIdentity` と同じ流儀)。P2P の `NetIdentity` もこの型から作る (`kNetProtoVersion` 6)。
- **.rep v9** (`kReplayFileVersion = 9`):
  - ヘッダ: 既存項目 + `flags` (bit0 = システム入力レコードあり) + `SessionConfig` + `SimProvenance` + 開始 `SnapshotMeta`。`rngState/rngInc` は D9 の規則で残す。
  - tick レコード: `InputSnapshot × playerCount` + (flags.bit0 のとき) `SystemInputTick` + `uint64 worldHash`。
  - `SystemInputTick` (固定長): `uint32 eventCount` + `uint32 pad` + `SystemEvent[8]` = 200 バイト。`events[eventCount..]` は 0 埋め。
  - v8 は読める (D10)。`--rep-diff` は v9 の新フィールドも項目名つきで差分を出す。
- **SimSnapshot**: `SessionLanes` の節を追加 (`kSimSnapshotVersion` 24)。
- **WorldHasher**: `SessionLanes` 節は「システム入力を持つ記録 (SessionConfig.role が Server/Client)」のときだけ畳む (D10)。
- **ABI v23** (`MYE_API_VERSION 23`、126 → 131 スロット、末尾追加):
  - `uint32_t NetLaneMask(void*)` — Connected レーンのビット
  - `uint32_t NetLaneState(void*, uint32_t lane)` — 0 Empty / 1 Connected / 2 Reserved
  - `uint64_t NetLanePlayerId(void*, uint32_t lane)` — Empty は 0
  - `uint32_t NetSystemEventCount(void*)` — この tick に適用したイベント数
  - `int NetGetSystemEvent(void*, uint32_t index, MyeNetSystemEvent* out)` — 範囲外は 0
  - 非サーバ構成 (オフライン / P2P) では: レーン [0, playerCount) が Connected、playerId 0、イベント 0 件 (どれも ctx.playerCount から決まる = .rep で再現する値)。
  - `ScriptAPI.h` の糖衣、`Interop.cs` の位置ミラー、`EngineApiTable.cpp`、check_rules 11-c の表を同時に更新。
- プロトコル: 新メッセージは既存 `NetPacketHeader` (64 バイト) を流用し `NetMsg` に追加 (`Hello=6, Welcome=7, ClientInput=8, Confirmed=9, SnapshotChunk=10, SnapshotAck=11, ResyncRequest=12`、既存値は動かさない)。`kNetProtoVersion` 6。

### 4.3 UI / ビジュアル

- エディタ NetWindow: 既存の P2P 表示に加え、(a) クライアント接続欄 (HOST:PORT、player session ID、接続ボタン)、(b) 接続中の表示: 役割、自レーン、playerId、レーン 4 本の状態 (Empty/Connected/Reserved と playerId)、確定 tick、先行量、到着余裕、再同期回数、desync 回数。文字列は `Tr()` + LocalizationTable の en/ja 両方。
- 検証: Release ビルドで NetWindow を開いた状態のスクショ (未接続 / ローカル Server.exe へ接続中の 2 枚) を `--screenshot` で撮る。目視はユーザー。

### 4.4 非機能

- **決定論 (全サブ共通の必須条件、5. の C 群で検査)**: DD §3 の全項目。特に「sim はネット・ホスティング・実時間・到着順を読まない」「非決定な出来事はサーバで 1 回だけ確定入力の値へ変換して記録」「走査順はレーン番号と eventSeq」「Debug/Release で sim を変えない」。
- 性能: Server.exe は 60Hz を実時間で維持 (4 クライアント、replay_verify の mp シーン相当で tick 平均 < 4ms を Release で計測しログに出す。満たさなければ報告)。スナップショット送付は 1 フレームに送るチャンク数を上限 (既定 32) で抑え、tick を止めない。
- 互換: 既存 P2P (`net_verify.bat`) は全ケース PASS のまま。P2P のプロトコル版は 6 になるので旧ビルドとは繋がらない (仕様どおり Reject)。
- ローカライズ: NetWindow の追加文字列は en/ja 両方、check_rules 10-b を通す。
- ログ: サーバは実時間・ネットの統計を sim の外 (ログ / NetRuntimeInfo) にだけ出す。

## 5. 受け入れ条件

番号は「群-番号」。各サブは自分の番号を満たし、**C 群 (決定論の不変条件) は全サブに課す**。

### C 群 — 決定論の不変条件 (全サブ、reviewer が毎回検査)
- **C1** `tools\replay_verify.bat` が PASS (Debug/Release、9 シーン + タイムトラベル + What-if + 規則)。各サブのコミット前に実行し、SELF_EVAL の検証欄に結果行を貼る。
- **C2** `pwsh -File tools\check_rules.ps1` が exit 0 (規則 13 ができた後はそれも含む)。
- **C3** `Editor.exe --selftest` (Debug と Release) が全 PASS。ただし基点 4e67907 で既に失敗している Source control self test の 2 項目 (`external cherry-pick state closes the normal write gate` / `external revert state survives status refresh`、sub-01 で HEAD の別ビルドにより確認) は除外し、SELF_EVAL に「その 2 項目以外 ALL PASS」と書く。除外はこの 2 項目に限る
- **C4** sub-05 以降: `tools\server_verify.bat` が PASS。
- **C5** 差分の中に「ネット / ホスティング / 実時間 / 受信順 / SDK コールバック由来の値を、確定入力 (`InputSnapshot` / `SystemInputTick`) 以外の経路で sim (RunOneTick から到達するコード、World / Scene の状態) へ渡すコード」が無いこと。reviewer は diff の中で sim 側へ値が入る箇所を列挙し、それぞれが確定入力として .rep に記録される値であることを確認する。
- **C6** `_DEBUG` / `NDEBUG` で sim 状態が変わる分岐、unordered コンテナの走査順やアドレスに依存する順序決定、`rand()` / `std::random_device` を足していないこと (check_rules 規則 1/7/8 + 目視)。
- **C7** `tools\net_verify.bat` が PASS (P2P を壊していない)。sub-03 以降 (P2P のハンドシェイクに触れるサブ) で必須、それ以外は Net/ に触れたサブで必須。

### H 群 — ヘッドレス (sub-01)
- **H1** `bin\x64\{Debug,Release}\Server.exe --replay-verify <rep>` が、replay_verify.bat が Editor で録った golden .rep (少なくとも mp / physics / parts / joints / fracture の 5 シーン、可能なら 9 シーン全部) を**全 tick 一致**で通す (exit 0、ログに `verified N ticks`)。通せないシーンがあれば、原因 (file:line) と「サーバで扱えるシーンの条件」を SELF_EVAL に書く — planner が範囲を裁定する。
- **H2** 1 回の実行で GraphicsDevice・窓・オーディオデバイスを作らない。Server.exe 終了前に `GetModuleHandleW(L"d3d11.dll")` / `dxgi.dll` / `d3dcompiler_47.dll` / `xaudio2_9.dll` が null (いずれも `/DELAYLOAD`)。ヘッドレス実行は Server の cook キャッシュ・ホットリロード用シャドウコピーを Editor / Runtime と共有しない (sub-01 落とし穴 g) であることを自己検査しログに出す (null でなければ exit 2)。
- **H4** `tools\replay_verify.bat` の各シーンのチェーンに Release の Server.exe による `--replay-verify` が加わり (H1 で通ったシーン)、以後の C1 がヘッドレス照合も含む。
- **H3** `RunOneTick` と TickServices の null 許容一覧 (`TickRunner.h:52-56`) を変えていない、または変えた場合は全経路で同じ分岐になることを SELF_EVAL で説明。

### S 群 — Session 型・.rep v9・レーン状態 (sub-02)
- **S1** `SessionLanes` / `ApplySystemInput` / `AllocateLane` / `SubstituteLateInput` の selftest: Join×4 → 5 人目は割り当てなし、Leave → Reserved、同 playerId の Rejoin で同じレーン、Release 後の Join は最小の空きレーン、同 tick 複数イベントは eventSeq 順、不正イベントはログ + 無視、消費型フィールドが 0 になる。
- **S2** .rep v9 の往復 selftest (SystemInputTick 付き / 無し)、v8 .rep の読込 selftest (ヘッダと tick 列が読める)。`--rep-diff` が SystemInputTick の差分をフィールド名つきで出す。
- **S3** スナップショット往復 (`--snapshot-stress` 相当の selftest) で SessionLanes がバイト一致で戻る。システム入力を持たない記録のハッシュ列が sub-02 前後で不変 (replay_verify が録り直しで PASS するだけでなく、sub-02 前に録った .rep を sub-02 後に verify して一致 — 手順を SELF_EVAL に記録)。
- **S4** check_rules 規則 13 が入り、意図的な違反 (一時ファイル) で exit 1 になることを確認してから外す (手順を SELF_EVAL に記録)。

### P 群 — 出自情報 (sub-03)
- **P1** `engineVersion` が Debug と Release で同じ値、ソースを 1 行変えて dirty にすると値が変わる (ログで確認)。
- **P2** `gameVersion`: 同じ GameLogic.dll で一致、別の DLL で `CompareProvenance` が `GameVersion` を返す selftest。`--allow-game-mismatch` で WARN + configBits に記録。
- **P3** `content_manifest.json` の生成 (CLI `--write-content-manifest PATH`) と読込。除外拡張子のファイルを変えても contentHash 不変、`.scene.json` / `.prefab` / `.meta` を 1 バイト変えると変わる selftest。manifest 無しの起動で計算時間がログに出る。
- **P4** P2P の NetIdentity が SimProvenance から作られ、`kNetProtoVersion` 6。net_verify PASS (C7)。

### N 群 — プロトコルと 1 プロセス内検証 (sub-04)
- **N1** (R-4 は sub-04 で解決済み: スクリプト込みで実施) 決定的な偽トランスポート (seed 付きの遅延・ロス・並べ替え・重複) で、サーバ 1 + クライアント 3 を 1 プロセス内に立てる selftest。シナリオ: 締め切り超過、途中参加 (tick > 0)、切断 → 再接続 (同じ playerId・同じレーン)、予約のタイムアウト → Release、ロス 20%、並べ替え。**全クライアントの確定 tick のハッシュ列がサーバと一致**。
- **N2** 同じシナリオを到着順・遅延パターンだけ変えて 3 通り流し、**サーバの確定入力列 (レーン入力 + SystemInputTick) が同じなら、sim のハッシュ列も同じ**。確定入力列が変わる場合 (締め切り超過の有無が変わる) も、各実行の中でサーバとクライアントが一致。
- **N3** eventSeq の欠番 → 再同期要求、desync 注入 → バンドル出力 + 再同期 → 以降一致、の selftest。
- **N4** 予測上限 12 で RTT 150ms 相当の偽トランスポートが stall しない (stall 回数 0)、RTT 300ms 相当では stall するが sim は一致。

### R 群 — プロセス間の通し (sub-05)
- **R1** `tools\server_verify.bat` (新規): Server.exe (Debug) + Runtime.exe × 3 (Debug / Release 混在、`--warp --no-audio --synth-input`) をローカルで起動、ロス 20% 注入、途中参加 1 名 (遅れて起動)、1 名の切断 → 再接続。終了後に (a) サーバ .rep と各クライアントの確定 tick ハッシュ (クライアントが書く `.rep`) が全 tick 一致、(b) サーバ .rep を Debug と Release の Server.exe `--replay-verify` でオフライン再生して全 tick 一致、(c) 同じ .rep を Runtime (`--replay-verify`、窓あり) でも一致 (= role に依存しない)。
- **R2** Server.exe がタイムアウト付きで必ず終了する (クライアント全員の Bye、または `--replay-ticks`)。
- **R3** Release の Server.exe で 4 クライアント時の tick 平均時間をログに出す (4.4)。
- **R4** (sub-07 で実施) サーバの .rep は逐次書き出し、異常終了しても完了済みの tick まで読める。Server.exe にクラッシュハンドラ。逐次モードと一括モードの .rep がバイト一致 (selftest)。

### A 群 — ABI v23 (sub-06)
- **A1** `MYE_API_VERSION 23`、スロット 131、check_rules 11-a〜d PASS、`Interop.cs` を機械照合 (ABI bump の検証レシピ)。
- **A2** GameLogic に参加/離脱を数えて World に書く検証用スクリプト (既存の検証用スクリプトの流儀で) を置き、N1 のシナリオで「全員が同じ tick に同じイベントを読んだ」ことがハッシュ一致で示される selftest。非サーバ構成の既定値 (D14 / 4.2) の selftest。

### G 群 — GameLift (sub-07 / sub-08)
- **G1** Server SDK 5.x をビルド済み .lib + ヘッダで `external\gamelift-server-sdk\` に置き、`external\VERSIONS.md` に版・取得元・ライセンス・ビルド手順を記録。Server.vcxproj だけが参照 (規則 13)。Debug/Release とも Server.exe が **/MTd・/MT のまま** (LNK2038 なし、`/p:MyeWarnAsError=true` で警告ゼロ) ビルドできる。OpenSSL 3 の DLL を出力先へ同梱。
- **G2** `Server.exe --hosting gamelift` を AWS 接続情報なしで起動すると、InitSDK の失敗を 1 行で説明して exit 1 (ハングしない、30 秒以内)。
- **G3** SDK コールバックがキュー経由で tick 境界でのみ処理されることを、コールバックを偽装した selftest (GameLiftHosting の SDK 呼び出し部を差し替え可能にして) で確認。
- **G4** `docs\gamelift-anywhere.md` (手順書): IAM、カスタムロケーション、Anywhere フリート、RegisterCompute、認証トークン、Server.exe の起動引数、CreateGameSession / CreatePlayerSession、Runtime からの接続、片付け。
- **G5** (sub-08、ユーザー手動) GameLift Anywhere にこの PC を登録し、AWS CLI でゲームセッションと player session を作成 → Runtime から `--net-connect` で接続 → プレイ → 終了、までをユーザーが実行し、サーバログと AWS 側のセッション状態 (ACTIVE → TERMINATED) のログを `plans\m81-dedicated-server\anywhere-log\` に残す。

### E 群 — エディタと文書 (sub-09)
- **E1** NetWindow の追加 (4.3)、未接続 / 接続中の 2 枚のスクショ、LocalizationTable en/ja、規則 10 PASS。
- **E2** `docs\adr\ADR-022-dedicated-server.md` (入力確定型を選んだ理由、ホスティング抽象、決定論の原則、D3/D4/D5/D6 の裁定)、`engine_spec.md` §11 に §11.5 (専用サーバ) と .rep v9 / ABI v23、`docs\engine-feature-guide.md:315` (「専用サーバは含めない」) の更新、`docs\test_checklists.md` に server_verify と Anywhere 手動確認の項目。

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | ヘッドレス Server.exe で golden .rep を照合する縦切り | なし | H1-H4, C1-C3, C5, C6 | `M81a: ヘッドレス Server.exe を追加し golden .rep を GPU 無しで照合` |
| sub-02 | Session 型・システム入力・SessionLanes・.rep v9・規則 13 | sub-01 | S1-S4, C1-C3, C5, C6 | `M81b: SessionConfig とシステム入力を sim に通し .rep v9 へ` |
| sub-03 | 出自情報 (engine/game/content) と NetIdentity の統合 | sub-02 | P1-P4, C1-C3, C5-C7 | `M81c: SimProvenance (ビルド ID / DLL / content manifest) で接続を照合` |
| sub-04 | サーバ/クライアントのプロトコルと 1 プロセス内検証 | sub-03 | N1-N4, C1-C3, C5-C7 | `M81d: 入力確定型サーバのプロトコルと途中参加・再接続` |
| sub-05 | Server.exe の実運用ループ・Runtime の --net-connect・server_verify | sub-04 | R1-R3, C1-C7 | `M81e: Server.exe と Runtime を繋ぎ server_verify を追加` |
| sub-06 | ABI v23 (レーン状態・playerId・システムイベント) | sub-02 | A1-A2, C1-C3, C5, C6 (sub-05 後なら C4 も) | `M81f: ABI v23 でレーン状態と参加・離脱をゲームへ公開 (ABI 変更)` |
| sub-07 | GameLiftHosting と SDK 組込・Anywhere 手順書・サーバ記録の逐次化 | sub-05 | G1-G4, R4, C1-C6 | `M81g: GameLift Server SDK 5.x を Server.exe へ組み込む` |
| sub-08 | GameLift Anywhere 実疎通 (ユーザー手動確認) | sub-07 | G5, (修正が出たら C1-C6) | `M81h: GameLift Anywhere の実疎通で出た問題を修正` (修正が無ければ記録のみのコミット) |
| sub-09 | エディタ NetWindow と文書 (ADR-022 等) | sub-05, sub-06 | E1-E2, C1-C3 | `M81i: NetWindow のサーバ接続表示と ADR-022・仕様書の更新` |

並列にできるもの: sub-06 は sub-02 の後なら sub-03〜05 と並列可。sub-09 の文書部分は sub-06 と sub-05 の後。

## 7. 未決事項・リスク

- **R-1 (最大)**: sub-01 でヘッドレス照合が一部シーンで通らない可能性 (スキンドモデル / メッシュコライダ / 音響 / UI のフォント計測が device 前提で登録されている場合)。通らなければ sub-01 の SELF_EVAL で原因を出させ、planner が「直す / サーバ対象外シーンとして文書化」を裁定する。後続サブは sub-01 の結論に依存する。
- R-2: GameLift Server SDK 5.x の Windows 静的ビルド。調査で確認した事実 (一次情報 = SDK リポジトリ `amazon-gamelift/amazon-gamelift-servers-cpp-server-sdk` main / 5.6.0 の CMakeLists とソース、公式 docs):
  - 最新 5.6.0。CMake + VS2022 でビルド、既定で静的 .lib (`BUILD_SHARED_LIBS=OFF`)。asio / websocketpp / rapidjson / spdlog / concurrentqueue はビルド時に取得するヘッダオンリーで**公開ヘッダには漏れない**。Boost・AWS SDK for C++ は不要。
  - **OpenSSL 3 が必須で常に DLL リンク** (`libssl-3-x64.dll` / `libcrypto-3-x64.dll` を Server.exe と同じ場所に同梱)。
  - **CRT の不一致**: SDK の既定は /MD・/MDd だが、MyEngine は全プロジェクト /MT・/MTd (`build\Common.props:53,61`)。そのままでは LNK2038 になる。SDK の CMake は ExternalProject が下位へ引数を渡さないので、`gamelift-server-sdk\` を直接 configure するなどの手当てが要る (sub-07 の最初の未知)。
  - 利用側は `GAMELIFT_USE_STD=1` と `NOMINMAX` (既に Common.props にある) を定義。ライセンスは Apache-2.0 で、LICENSE / NOTICE (asio / websocketpp 等) の同梱が要る。
  - SDK のコールバックは**SDK 内部のスレッド** (onStartGameSession 等は呼ぶたびに detach したスレッド、ヘルスチェックは専用スレッド、応答待ち 50 秒) から来る — 4.1.9 のキュー方式が必須である根拠。
  - Anywhere の認証トークンは約 15 分で失効する (AWS 公式ブログ)。sub-07 はトークンを環境変数 `MYE_GAMELIFT_AUTH_TOKEN` から読む (コマンド履歴に残さない)。長時間稼働のトークン更新は後続 (sub-08 の手順書に「15 分以内に疎通を終える」と書く)。
  - ビルド済み .lib を external\ に置く例外は DD で合意済み。OpenSSL の DLL と import lib も同じ流儀で `external\openssl\` に置き VERSIONS.md に記録する。
- R-3: Server.exe を CI で回すと UDP + 複数プロセス + 実時間で不安定になる可能性 (net_verify が CI 外にしている理由と同じ、`net_verify.bat` 冒頭)。CI へは sub-04 の selftest (1 プロセス・偽トランスポート) で論理を押さえ、server_verify は手元必須 + CI は任意ジョブとする。
- R-4: GameLogic.dll を 1 プロセス内で複数の sim から共有する selftest (N1) は、スクリプト状態がエンジン側にあるため (ADR-003) 原理的には可能だが未実測。不可なら N1 はスクリプト無しのコード構築シーンで行い、スクリプト込みの一致は R1 (プロセス間) で担保する。
- R-6 (sub-01 で判明): `project_settings.json` の `particleBackend` が `gpu` のとき、Server は device 無しで GpuParticleBackend を選び Update が no-op になる。GPU パーティクルはスナップショットにもハッシュにも入らない (SimSnapshot.h の対象外一覧) ので、**サーバとクライアントが同じ設定である限り**ハッシュは揃う見込みだが未検証。裁定: Server で CPU へ強制しない (強制するとクライアントと Particles 節の有無が食い違う)。設定の一致は contentHash (project_settings.json を含む、D5) が保証する。現 golden は cpu のみ。
- R-7 (sub-01 で判明): コンピュート ABI (v21) は device 無しで 0 / no-op を返す。GPU の結果を読み戻して sim 状態へ書くスクリプトは Server と描画クライアントで割れる。裁定: **サーバ対象のゲームでは禁止の既知制限**として sub-09 の文書 (ADR-022 / engine_spec §11.5) に明記する (GPU 結果は Debug/Release/WARP でもビット一致が保証されないので、もともと sim に入れてよい値ではない)。
- R-8 (sub-01 で判明): Server は cook キャッシュを常に無効にしている (起動時に毎回モデルをパース。Release 約 0.9 秒)。Server 専用の cook 置き場は「毎回パース ≡ クック再生」のビット一致がヘッドレスで未証明なので今は入れない。起動時間が問題になったら後続で、replay_verify と同じ「コールド録画 → ウォーム照合」の形で証明してから入れる。
- R-9 (sub-03 で判明): 除外種類の .meta (テクスチャ等の GUID) は contentHash に入らない。テクスチャの AssetID をハッシュ対象のコンポーネントへ書くコードがあり、かつサーバとクライアントで .meta の GUID が違う場合は contentHash で弾けず desync する。現状そうした sim コンポーネントは見つかっておらず (render-demo の DecalComponent のみ)、GUID が食い違うのはアセットの移動時 = シーン / プレハブも同時に変わる場合が大半なので許容する。desync 検出 (4.1.7) が最後の防波堤。
- R-10 (sub-05 で判明): 4 クライアントでも tick 時間の max が 7〜14ms に跳ねる (参加時のスナップショット撮影と推定、未切り分け)。締め切り 3 tick (50ms) には収まる。sub-08 の実疎通で参加時の max をログで見て、問題があればスナップショット撮影の分割を後続で検討する。
- R-11 (sub-07 で判明): `AcceptPlayerSession` / `RemovePlayerSession` は SDK の同期呼び出し (内部で再試行) で、60Hz のループの中で呼ばれる。GameLift への WebSocket が詰まると tick が止まる (sim の結果は変わらず、遅れるだけ)。非同期化するには ServerSession の Hello 処理に「検証待ち」の状態が要る。sub-08 で参加時の tick 時間を見てから、後続で判断する。
- R-10 続報 (sub-07): server_verify ケース D (Release・4 クライアント) で avg 0.201ms / max 39.5ms。締め切り 50ms には収まるが余裕が小さい。sub-08 で再確認する。
- 既知の観察 (M81 範囲外): 初回の Debug `Editor.exe --selftest` で Fracture editor の weight cache 3 項目が一度だけ FAIL し、再実行では出なかった (sub-07)。C3 の除外対象ではないので、再現したら報告すること。
- R-5: GameLift 実疎通はユーザーの AWS アカウント・IAM・費用を伴う。sub-08 はユーザーの手が空くまで保留してよい (他サブの完了を妨げない)。

## 8. 変更履歴

(確定後の変更のみ)
- 2026-10-02 ユーザー: D3/D4/D5/D6 は planner 裁定どおりで確定 (台帳に記録)。
- 2026-10-02 sub-01 VERDICT round 1 (coder SELF_EVAL の不安 1-4 への回答): C3 に基点から失敗している 2 項目の除外を明記、R-6 (GPU パーティクル設定)・R-7 (コンピュート ABI の結果を sim に入れない制限)・R-8 (Server の cook キャッシュ無効) を追加。sub-09 に R-7 / D13 を既知制限として書く項目を追加。
- 2026-10-02 sub-02 VERDICT round 1: (1) SessionLanes は SimRefs に別参照を持たず Scene 経由で撮る (coder 逸脱を承認、配線漏れ防止)。(2) D10 のハッシュ節ゲートは SessionLanes::systemInput (sim 状態、初めてシステム入力を適用した tick に 1) で表す — SessionConfig.role で判定するのと同値で、スナップショットに乗る。(3) `--rep-diff` はヘッダのうち tick 列の意味に効く項目だけを比較する (role / inputDelay / 締め切り / 版番号は比較しない — net_verify が role の違う .rep を突き合わせるため)。(4) 規則 13-a の許可リストに既存経路の EngineApiTable.cpp (v13 Net* → 表示専用 NetRuntimeInfo) を理由付きで入れる。許可は sub-03 で NetRuntime.h の include だけに絞る。(5) 再シム (ロールバック / タイムトラベル) で SystemInputTick を tick ごとに差し替える要件を sub-04 に追加。
- 2026-10-02 sub-03 VERDICT round 1: (1) D5 の contentHash に**パス単位の除外**を追加: `content_manifest.json` 自身とその `.meta` (AssetDatabase が自動生成し、manifest の有無で集合が変わる)、`scripts/Generated/` (起動のたびに書き直す派生物)。実測で割れたことが根拠。(2) EngineLoop はネット接続か `--replay-record` のときだけ contentHash を計算する (他は 0 とログに明記)。Server は常に計算。(3) D6: `--allow-game-mismatch` は server_verify に加えて net_verify の Debug↔Release ケース (B/C) も使う (同じ事情 = 構成の違う GameLogic.dll は必ず別バイト)。本番では使わない。(4) `--flow-demo` が OnStart で gitignore 済みのシーンファイルを assets へ書くため、デモ同士の接続では contentHash が実行履歴で揺れうる — デモ専用の既知事項として許容し、sub-09 の文書に書く。(5) `--rep-diff` は configBits の jobs / simcache / cookcache / synth を引き続き比較する (起動構成の違いを差分として見せるのは診断として有益。net_verify は PASS)。(6) P1 の dirty 実機対は selftest で代替を承認 (作業ツリーが常に dirty なため)。
- 2026-10-02 sub-03 VERDICT round 2: (1) D5 の「.meta は含める」を改める — **除外する種類 (画像・シェーダ・音声・フォント実体) の .meta も除外**し、それ以外の .meta (シーン・プレハブ等の GUID) は含める。根拠: DDS 一括クックが配布先の初回起動で .dds.meta を増やし、manifest の値と実体が割れたのを実測。(2) 配布物にはブートシーンの .meta もコピーする (配布先の初回起動が新しい GUID の .meta を作って中身を変えないため)。manifest はブートシーン配置の後に焼く。(3) ci.yml の package contents が `assets\content_manifest.json` の存在を検査する。残るリスクは R-9。
- 2026-10-02 sub-04 VERDICT round 1: (1) 4.1.4 の確定条件を改定 — 待つのは「有効な tick の入力を 1 本でも届けた Live の peer」のレーンだけ。スナップショット受信中・復元中・追いつき中の参加者は待たず、代替入力で確定する (待つと参加のたびに全員の tick が止まる)。また、サーバは予定時刻の inputDelay tick 前より早く確定しない。どちらも「どの tick に何を入れたか」を変えるだけで、値は記録されるので決定論に影響しない。(2) 既定値: deadlineTicks 3 (50ms)、rejoinTimeoutTicks 1800 (30 秒)。CLI (`--net-deadline` 等) で上書き。(3) D12 を分割 — サーバの代替入力 (SubstituteLateInput) は消費型を全部 0 のまま。**クライアントの予測は別関数**にし、chars / charCount / wheelDelta だけ 0、mouseDelta は繰り返す (sub-05)。予測は確定値に入らないので sim の正しさに無関係、当たりやすさで決める。根拠: 合成入力で RTT 150ms のとき 530 tick 中 520 回巻き戻した実測。(4) サーバ/クライアント構成 (hasSystemInput) ではタイムトラベルのリングを起こさない。(5) 再接続は Leave → Rejoin を同じ tick に積む「乗っ取り」を許す。認証は playerId + player session ID の一致。(6) R-4 解決: GameLogic.dll を 1 プロセスの 4 sim で共有して tick を交互に回しても一致 (実測)。
- 2026-10-02 sub-05 VERDICT round 1: (1) 終了条件「全員の Bye 後」を `--exit-when-empty` (peer が 0 になって 2 秒、Bye とタイムアウトを区別しない) と `--server-timeout` (exit 5) に置き換える。(2) クライアントの入力置換は ClientSimRunner の runTick フックの中で、同じ `ApplyConfirmedInputs` を呼ぶ (置換の関数は 1 本のまま)。クライアントの再シムは `ClientSimRunner::ResimFrom` (NetResimFrom は P2P 専用)。(3) クライアントは再同期のたびに .rep を `<stem>.rs<N>.rep` に切る。`--rep-diff-overlap N` で開始 tick の違う .rep の重なり区間を比べる。(4) server_verify は CI に載せない (net_verify と同じ理由。論理は selftest が CI で押さえる)。(5) サーバの記録の逐次化とクラッシュ時の記録を R4 として sub-07 に追加 (GameLift 運用の前提)。(6) R-10 を追加。(7) D12 の改定 (予測 = PredictLaneInput) は sub-04 VERDICT で反映済み、実装は sub-05 で完了。
- 2026-10-02 sub-06 VERDICT round 1: (1) 非サーバ構成の既定値の元として ScriptApiContext に playerCount (TickContext 由来 = .rep で再現する値) を足す (エンジン側だけで C# の構造体は不変)。(2) C# 側は位置ミラーのみで糖衣は足さない (ネット中は C# レーンが止まる)。(3) 検証用スクリプト NetEventProbe はデモへ自動では付けない (デモの生成順 = 粒子 RNG のストリームと golden を動かさない)。(4) ABI v23 により外部プロジェクトの GameLogic.dll は再ビルドが必須。これは ABI bump の通常の帰結なのでユーザー判断の論点にはせず、sub-09 の文書と台帳に記録する。
- 2026-10-02 sub-07 VERDICT round 1: (1) D17 を追加 (ビルド済み .lib をコミットする裁定。ユーザーに確認する論点)。(2) Release 確定で RemovePlayerSession を呼ぶ口として `playerReleased` フックと `IHostingProvider::PlayerReleased` を追加 (playerLeft は一時切断のたびに呼ばれ、そこで Remove すると再接続を GameLift が拒否するため)。(3) GameLift のゲームプロパティ `myeDeadlineTicks` / `myeRejoinTimeoutTicks` で SessionConfig の締め切りと予約期間を上書きする。人数の上限は ValidatePlayer で絞る (レーン数は sim の構築時に決まるため)。(4) InitSDK は 30 秒でタイムアウトする (実測で、繋がらない接続先だと 171 秒たっても戻らなかった)。(5) R-10 の続報と R-11 を追加。
