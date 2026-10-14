# M93 EOS 連携 (ロビー・P2P 接続・ボイスチャット) — 設計書 (design-draft)

- slug: m93-eos
- 状態: ユーザー承認済み (2026-10-14、プランモード)。planner はこれを起点に spec.md を確定しサブへ分割する

## Context

宇宙服穴ゲーム (`C:\HAL\就プレ作品`) を 4 人協力で売るには「友人と集まる入口」と「ボイスチャット」が要る。
M81 で専用サーバ (Server.exe + GameLift) と入力確定型プロトコルは済んだが、M81 は NAT 越え・マッチメイク・暗号化・音声を明示的に対象外にした (`plans\m81-dedicated-server\spec.md` §3)。
Epic Online Services (EOS) は無料で、C SDK がありアカウント不要 (Device ID) で動き、ロビー・P2P (NAT 越え + リレー)・RTC 音声が 1 つの SDK に揃う。

ユーザー決定 (2026-10-14):
- 通信の入口まで EOS で寄せる
- **ホスト兼サーバ + EOS P2P**: ロビーのオーナーの Runtime が既存 `ServerSession` を内部で動かし、他 3 人は EOS P2P で繋ぐ。Server.exe / GameLift は残すが本線から外す
- ロビー UI の最初の形は **Runtime に小さな ImGui 窓** (後で uGUI へ置換)
- **SDK はコミットしない** (`external\eos-sdk\` を gitignore、無い環境でもビルドが通る)
- ボイスは **無線 = 距離減衰なしで全員に聞こえる**。常時送信を既定にし、設定でプッシュトゥトークへ切替

想定の規模はマイルストーン 1 本 (M93)。これまでの慣例どおり **`/harness` に載せる**。この文書を `plans\m93-eos\design-draft.md` として planner へ渡す。

## 1. 調査で確定した事実 (計画の根拠)

- サーバ接続のトランスポートは差し替え可能: `ServerSession` / `ClientSession` は `NetSendFn = std::function<void(uint32 peer, const void*, size_t)>` の送信口と `OnPacket` の受信口だけを持ち、UdpSocket を知らない (`src\Engine\Engine\Net\NetProtocol.h:162`、`ServerSession.h:26-32`、`ClientSession.h:24`)。
- ただし `EngineLoop.cpp:1592-1754` は `UdpSocket` / `NetAddress` を直接使い、接続は **起動時 CLI (`--net-connect`) で 1 回だけ** 始まる。実行中にロビーから接続を始める構造が無い。
- `ServerSession` は Engine 側 (`src\Engine\Engine\Net\`) にあり Server.exe 専用ではない → Runtime 内でホストが動かせる。`ServerLoop.h:101-107` の 1 周の順序 (受信 → ホスティング → 確定 → RunTick → 記録 → 送信) をそのまま Runtime のフレームに移植する。
- 旧 P2P の `NetSession` は 2 人固定・UdpSocket 直結 (`NetSession.h:283`)。触らない (net_verify は PASS のまま維持)。
- sim の外 (出力レーン) の置き場: `EngineLoop.cpp:2920-2981` (オーディオ・振動・カーソル)。`EOS_Platform_Tick` もここ。
- GameLift の閉じ込め方が手本: `src\Server\Hosting\IHostingProvider.h`、`GameLiftSdk.h` (selftest 用の偽 SDK 抽象)、`check_rules.ps1` 規則 13-a/13-c (`:866-948`)。
- 音声出力は XAudio2 だけ、外部 PCM の流し込み口もマイク入力も無い (`Audio\Playback\AudioSystem.cpp:232`)。EOS RTC は SDK がマイク取り込みとスピーカー出力を自前で行うので、**エンジンのミキサーには繋がない** (繋ぐのは将来の任意項目)。
- 外部 SDK の置き場は `external\` + `VERSIONS.md`。DLL の bin 配置は各 vcxproj の PostBuildEvent 手書き (`Runtime.vcxproj:32-34`)、配布物は `tools\make_dist.ps1:66-67` の `$required`。
- 設定の置き場: `assets\project_settings.json` に機能ごとの節 (`ProjectSettingsFile.h:23-29`)。EOS の ProductId / SandboxId / DeploymentId / ClientId / ClientSecret は Peer2Peer クライアントポリシーでは実行ファイルに同梱される前提の値なので、ここに `"eos": {...}` 節で置く。
- Runtime は `enableImGui=false` (`RuntimeMain.cpp:88`、`EngineLoop.cpp:332,3080`)。
- ローカルレーンの取得は `clientSession.Lane()` → `NetRuntimeInfo.localPlayer` (`EngineLoop.cpp:2493`)。表示専用、sim へ書き戻し禁止。
- ADR の次番号は **ADR-031**。ABI は v29 (= 174 スロット)。M75h が v30 を予約しているので、M93 で ABI を足すなら **着手時点の末尾** を取る。

## 2. 設計

### 2.1 レイヤーと依存

```
src\Engine\Engine\Online\          (新設。sim 側から include 禁止 = 規則 13-a の対象に追加)
  IOnlineService.h                 ログイン / ロビー / P2P 送受信 / 音声 の抽象 (POD + コールバックキュー)
  OnlineTypes.h                    LobbyInfo / LobbyMember / OnlineEvent (POD)
  NullOnlineService.cpp            SDK 無し環境・CI 用 (常に「未接続」、selftest の偽実装の土台)
  Eos\EosService.cpp/.h            EOS SDK 実装 (MYE_HAS_EOS が立つときだけビルド)
  Eos\EosVoice.cpp/.h              RTC ルーム・ミュート・PTT
  HostSessionRunner.cpp/.h         Runtime 内でホストが ServerSession を回す (ServerLoop.h の移植)
  OnlineNetWindow.cpp/.h           ImGui のロビー窓 (Runtime / Editor 共通)
```

- 依存方向: `Online\` → `Net\` (ServerSession / ClientSession / ClientSimRunner) → `Session\`。逆は禁止。
- EOS の名前 (`EOS_`、`eos_sdk.h`、`EOSSDK`) が出てよい場所は `Online\Eos\`、`build\*.vcxproj`、`external\eos-sdk\` だけ (規則 13-d として `check_rules.ps1` に追加)。
- SDK のコールバックは **SDK のスレッドから来る可能性がある** 前提で、`GameLiftHosting.cpp:114-220` と同じくミューテックス付きキューへ積み、`Poll()` (メインスレッド、フレーム後半) で処理する。実際には `EOS_Platform_Tick` を呼んだスレッドで来るが、設計を変えずに安全側へ倒す。

### 2.2 接続の流れ (ホスト兼サーバ)

1. 起動時 `EosService::Init` (project_settings の `eos` 節)。`EOS_Connect_CreateDeviceId` → `EOS_Connect_Login` (Device ID、表示名は設定値かマシン名)。Product User ID (PUID) を得る。
2. **ホスト**: ロビー窓「部屋を作る」→ `EOS_Lobby_CreateLobby` (MaxMembers 4、`bEnableRTCRoom = true`、属性 `JOINCODE` = 6 文字の英大文字+数字、`EOS_Lobby_UpdateLobby` で public 検索可)。同時に `HostSessionRunner::Start(ServerSession)` を起動し、**ホスト自身は in-process ループバックで ClientSession として参加**する (ホストのレーンも通常の参加者として Join イベントで割り当て = サーバのレーン規則を変えない)。
3. **参加者**: 窓でコードを入力 → `EOS_LobbySearch_SetParameter(JOINCODE)` → `EOS_Lobby_JoinLobby`。ロビーのオーナー PUID へ `EOS_P2P_SendPacket` (SocketId `"mye"`、信頼性 `EOS_PR_UnreliableUnordered`) で既存の `Hello` を送る。以降は `ClientSession` の送受信口を EOS P2P に差し替えただけ。
4. ホスト側は `EOS_P2P_AddNotifyPeerConnectionRequest` で接続要求を受け、ロビーメンバーなら `AcceptConnection`。`ServerHooks::validatePlayer(psid)` には **PUID 文字列** を渡す (psid は最大 64 文字、PUID は 32 文字の hex なので収まる)。再接続は同じ PUID で `Rejoin`。
5. 「開始」はホストのみ。開始前は `ServerSession` が `StartSession` を待つ (サーバと同じ)。開始後の途中参加は M81 の仕組みどおり (スナップショット転送)。
6. 退出は `Bye` + `EOS_Lobby_LeaveLobby`。ホストが落ちた場合はセッション終了 (ホスト移譲はやらない。明示的にスコープ外)。

実行中に接続を始めるために、`EngineLoop.cpp:1592-1754` の「起動時に 1 回」を **`NetConnectRequest` を受けてフレーム境界で開始する** 形に直す。CLI の `--net-connect` はこの要求を起動直後に 1 回積むだけにし、挙動を変えない。

### 2.3 音声 (無線)

- ロビー作成時に `bEnableRTCRoom = true` → 参加者は自動でルームに入る。SDK が既定デバイスのマイク取り込みと再生を行うので、**位置情報は送らない・減衰もしない** (ユーザー決定: 無線)。
- 常時送信が既定。`EOS_RTCAudio_UpdateSending` で自分のミュート、`EOS_RTCAudio_UpdateReceiving` で相手ごとのミュート。
- プッシュトゥトークは project_settings `eos.voice.pushToTalk = true` と `actions.json` のアクション `VoicePtt` で切替。押している間だけ `UpdateSending(Enabled)`。アクションの読み取りは **描画レーンの生入力** (`netLiveInput` ではなく `Input` の直読み) で行い、sim の入力レコードには載せない (載せると PTT が .rep の内容を変える)。
- 「誰が喋っているか」は `EOS_RTCAudio_AddNotifyParticipantUpdated` → `OnlineEvent` → ロビー窓と、ABI 経由で NoHash の UI へ (任意)。
- リプレイ記録・検証中や `--no-audio` でも RTC は **止めない** (sim と無関係)。ただし `--no-audio` ではマイク/スピーカーも無効にする選択肢を設定に持たせる。

### 2.4 スクリプト (GameLogic) への露出

必須は小さく:
- `MyeOnlineLocalDisplayName(buf)` / `MyeOnlineMemberCount()` / `MyeOnlineMemberName(i, buf)` / `MyeOnlineIsSpeaking(i)` / `MyeOnlineSetMuted(i, bool)` — すべて **表示専用 (機種依存)**。`MyeNetLocalPlayer` と同じ注意書き (sim へ書き戻し禁止) をヘッダに書く。
- ABI 版を 1 つ上げる (着手時の末尾、M75h と衝突しないよう harness で確定)。`Interop.cs` の位置ミラー、規則 11 の表、`MYE_API_VERSION` を同時に更新 (記憶「ABI bump の検証レシピ」)。
- ロビー作成・参加・開始の操作はゲームスクリプトに持たせない (最初は ImGui 窓だけ)。uGUI 化は M75h 以降の別件。

### 2.5 SDK の置き方

- `external\eos-sdk\` (gitignore)。`BUILD.md` に: Epic Developer Portal でプロダクト作成 → SDK (C) をダウンロード → `SDK\Include`、`SDK\Lib\EOSSDK-Win64-Shipping.lib`、`SDK\Bin\EOSSDK-Win64-Shipping.dll` をここへ置く手順。`VERSIONS.md` に版と「未コミット」の理由を記録。
- `build\Common.props` に `MyeHasEos` プロパティ: `Exists('$(RepoRoot)external\eos-sdk\Include\eos_sdk.h')` で自動判定し、`MYE_HAS_EOS=1` と lib/DLL コピーを `Runtime.vcxproj` / `Editor.vcxproj` に条件付きで足す。無いときは `NullOnlineService` だけがビルドされ、CI はそのまま通る。
- `tools\make_dist.ps1` の `$required` に DLL を条件付きで足す。
- 規則 14 (改行): SDK は gitignore なので対象外。

### 2.6 設定

`assets\project_settings.json`:
```json
"eos": {
  "productId": "", "sandboxId": "", "deploymentId": "",
  "clientId": "", "clientSecret": "",
  "displayName": "",
  "voice": { "enabled": true, "pushToTalk": false }
}
```
空なら EOS を初期化しない (ログに INFO)。`project.mye.json` には置かない (エンジン版と起動シーンだけの器)。

### 2.7 スコープ外 (明示)

ホスト移譲、Epic アカウント / Steam 連携の招待 (後で `EOS_Connect` の外部認証を足せる設計にはする)、距離減衰・空間音声、エンジンミキサーへの RTC 合流、マッチメイク、暗号化の追加 (EOS P2P は SDK 側で暗号化される)、Linux、5 人以上、旧 P2P `NetSession` の変更。

## 3. サブ分割 (harness の sub-NN 候補)

| # | 内容 | 検証 |
|---|---|---|
| 01 | `Online\` 層の骨組み: `IOnlineService` / `OnlineTypes` / `NullOnlineService`、`check_rules` 規則 13-d、`Common.props` の `MyeHasEos` 自動判定、`external\eos-sdk\BUILD.md` + gitignore + VERSIONS.md | SDK 有無の両方でビルド成功、check_rules PASS |
| 02 | **接続開始の実行時化**: `EngineLoop` の接続開始を `NetConnectRequest` (フレーム境界) に直し、UdpSocket 送受信を `INetTransport` (Udp 実装) に包む。`--net-connect` の挙動は不変 | `tools\server_verify.bat` PASS、`net_verify.bat` PASS |
| 03 | `HostSessionRunner`: Runtime 内で `ServerSession` を回し、ホスト自身が in-process ループバックの `ClientSession` で参加。Udp トランスポートで LAN 4 人 (Runtime 4 本) が繋がる | 新 selftest (偽トランスポートで 1 プロセス内 4 クライアント)、LAN での 4 人実走 + .rep 照合 |
| 04 | `EosService`: Init / Device ID ログイン / Platform_Tick / ロビー作成・検索 (JOINCODE)・参加・退出 / イベントキュー | 実 EOS で 2 PC がロビーに入るログ (anywhere-log と同じ形式の記録) |
| 05 | EOS P2P トランスポート + ロビー → Hello の接続。`validatePlayer` に PUID | 別 NAT の 2 PC で接続・.rep ハッシュ一致。4 人は手元 + 友人で |
| 06 | `EosVoice`: RTC ルーム自動参加、常時送信 / PTT、自分・相手のミュート、発話通知 | 2 PC で会話、PTT 切替 |
| 07 | ImGui ロビー窓 (Runtime で有効化。`--online-ui` 既定 ON、`enableImGui` の Runtime 側の最小化 = 窓 1 枚だけ)、Editor の NetWindow にも同じ内容 | スクショ (`--screenshot`)、Tr() の en/ja |
| 08 | ABI: 表示専用 5 スロット、Interop.cs、規則 11、`MyeNetLocalPlayer` と同じ注意書き | `Editor.exe --selftest`、ABI bump レシピ |
| 09 | 文書: ADR-031 (ホスト兼サーバ + EOS、SDK 非同梱、無線音声の理由)、`engine_spec.md` のネット章、`docs\eos-setup.md` (ポータル手順)、`MyEngine の不足機能.md` の C 節更新、M81 spec の「NAT 越え」注記 | 参照整合 |

波: 01 → 02 → 03 (EOS 無しで完結) ‖ 04 (EOS 単体) → 05 → 06 → 07 → 08 → 09。04 は 02/03 と並列可。

## 4. 検証 (全体)

- `Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`tools\net_verify.bat`、`tools\server_verify.bat` が全部 PASS (既存経路を壊さない証拠)。
- SDK を外した状態 (`external\eos-sdk` を一時的に退避) でソリューション全体がビルドできる (CI 相当)。
- 実機: 別 NAT の 2 PC で、ロビー作成 → コード参加 → 開始 → 穴ゲームの試作 (`--project C:\HAL\就プレ作品`) を 5 分遊び、双方の `--replay-record` の .rep を `--replay-verify` で突き合わせ、全 tick ハッシュ一致。音声が双方向に通る。
- 記録は `plans\m93-eos\field-log\` に日付付きで残す (M81 の `anywhere-log` と同じ)。

## 5. リスク・未知

- **EOS のクライアントポリシー**: Peer2Peer ポリシーで Lobby / P2P / RTC の権限が足りるかはポータルで設定して確かめる (sub-04 の最初)。
- **SDK のスレッド**: RTC のコールバックが別スレッドから来る実装の可能性 → キュー経由に統一して吸収。
- **ホストの再シム負荷**: ホストはサーバ役 (確定のみ) と自分のクライアント (予測 12 tick) を同じプロセスで回す。物理の多いシーンで 1 フレームに収まるかは不足機能 A 節の「再シム負荷計測」と同じ未知。sub-03 で Profiler に行を足して計測する。
- **ループバックのレイテンシ**: ホスト自身の入力も Hello → 確定の経路を通るので、ホストにも inputDelay 分の遅延が乗る。公平性と引き換え。許容できなければ後で「ホストは確定済み扱い」の最適化。
- **公開リポジトリと認証情報**: clientSecret は Peer2Peer ポリシーでは機密扱いでないが、`project_settings.json` を公開するなら値を空にしてコミットする運用 (就プレ作品のリポジトリの公開範囲をユーザーが確認)。
- EOS SDK の再配布可否はユーザーが Epic の契約で確認する (本計画では同梱しないので直ちには問題にならない)。
- **販売時の義務 (エンジン外、ユーザー対応)**: Developer Portal の登録と EOS Developer Agreement 同意 / プレイヤー向けプライバシーポリシー (IP・デバイス ID・音声を Epic が処理する旨) / SDK 同梱の ThirdPartyNotices をクレジットか配布物へ / Steam ストアの機能フラグ (オンライン協力・ボイス) / 相手ごとのミュート (AUP 対応、sub-06 に含む)。Brand Review は Epic Account Services を使う場合だけで、Device ID + Connect では不要 (要確認)。SDK の版は年 1 回程度追従する。

## 6. 次のアクション

1. この文書を `C:\HAL\MyEngin\plans\m93-eos\design-draft.md` として保存し、`/harness` で planner に渡す (ユーザーが `/harness` を起動)。
2. ユーザー側の準備: Epic Developer Portal でプロダクトを作り、SDK を `external\eos-sdk\` に配置、`eos` 節の値を埋める (sub-04 までに)。
3. `MyEngine の不足機能.md` の C 節「近接ボイスチャット」を「無線ボイス (EOS RTC)、M93 で対応中」に更新するのは sub-09 で行う。

## 7. 実装時の変更 (2026-10-14、harness を使わず直接実装すると決めた後)

- **ホストのサーバは同じプロセスではなく子プロセスの Server.exe にする。** HeadlessSim は 1 プロセスに複数立てられるが、物理ライブラリ・アセットのキー解決・C# ホストの注入先がプロセス共有で、tick ごとに `Activate()` で付け替える必要がある。EngineLoop 側の世界には付け替えの口が無く、取り違えると静かに壊れる。子プロセスなら M81 で検証済みの Server.exe をそのまま使え、サーバが落ちてもホストの描画は残る (障害の局所化)。構成 (ホストの PC がサーバ、サーバ費用ゼロ) は承認どおり。
- **中継はローカル UDP と EOS P2P のブリッジにする。** ホストは参加者ごとにローカル UDP ソケットを開いて `127.0.0.1:<サーバのポート>` へ転送し、ホスト自身のクライアントは直接 `127.0.0.1` へ繋ぐ。参加者は `127.0.0.1:<ブリッジのポート>` へ繋ぎ、ブリッジが EOS P2P でホストへ運ぶ。ClientSession / ServerSession の送受信口は変えない (計画の sub-02「INetTransport」は不要になった)。残る変更は「接続を実行中に始める」だけ。
- **設定の節は `online` にし、`backend` で `eos` / `local` / `none` を選ぶ。** `local` は同じ PC の別プロセス同士を UDP で繋ぐ開発用の代役で、EOS SDK が無くてもロビー → 接続 → 中継の流れ全体を実プロセスで検証できる。
