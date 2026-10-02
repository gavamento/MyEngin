# ADR-022: 専用サーバ (入力確定型) とホスティング抽象

- 状態: 採用 (2026-10-02、M81a〜M81i。GameLift Anywhere の実疎通 = M81h はユーザーの手作業で、結果は追記する)
- 出所: 「汎用 Dedicated Server 機能を作り、そのホスティング先の一つとして AWS GameLift に対応させたい。
  決定論を壊さないように開発 / ネットワーク管理を行うこと」(依頼原文)。合意済みの設計は
  `plans\m81-dedicated-server\design-draft.md`、疑った点と裁定 (D1〜D17) は `plans\m81-dedicated-server\spec.md` §2。
  仕様の本文は `engine_spec.md` §11.5、AWS 側の手順は `docs\gamelift-anywhere.md`。

## 背景: P2P の限界

ADR-013 の 2 人 P2P (遅延ロックステップ + 予測ロールバック) は、同じ PC どうしを繋ぐ検証には十分だが、
「ゲームを置いておけばクラウドで誰でも途中から入れる対戦サーバ」は作れない。

- 相手が 1 人の前提 (ハンドシェイク、ハッシュ交換、入力冗長送信が 2 peer 固定)。3 人以上、途中参加、再接続の概念が無い。
- 全員が揃ってから始まる。1 人が落ちると全員が止まる (または desync で停止する)。
- 窓と GPU が要る。クラウドの GPU 無しのマシンでは動かせない。
- ホスティング (プロセスの起動・終了・ヘルスチェック・参加者の認証) の差し込み口が無い。

## 決定 1: 入力確定型のサーバにする。状態配信型は採らない

サーバが tick ごとの全レーン入力 (+ システム入力) を**確定してから**配り、自らも同じ `RunOneTick` を回す。
クライアントは P2P と同じ予測ロールバックで追従する。サーバ → クライアントに流れるのは
「確定入力」と「途中参加用のスナップショット」だけで、ワールドの状態は流さない。

理由は ADR-013 / ADR-004 と同じ根: エンジンの決定論の資産 (`RunOneTick` 1 本、`InputSnapshot` 4 レーン、
`SimSnapshot`、`HashWorld`、入力列 = `.rep`) をそのまま使え、**サーバの `.rep` をどの PC で再生しても
サーバ実機と同じハッシュ列になる**。状態配信型にすると、(a) 配る状態の範囲 (どのコンポーネントを同期するか) を
ゲームごとに決める必要があり、(b) ECS 全体の差分圧縮・補間・権限の設計が要り、(c) `.rep` の再生と
サーバ実機の一致という検証の軸が失われる。

却下: **状態配信型** (上記)。**P2P のまま 3〜4 人へ拡張**: 全員が全員へ送るメッシュは NAT 越えを要求し、
途中参加と権威の置き場が無い。

## 決定 2: 決定論の原則 (design-draft §3 の要約)

ネットとホスティングは本質的に非決定 (到着順・到着時刻・ロス・SDK のスレッド・実時間)。
**非決定な出来事は、サーバで 1 回だけ「確定入力の値」に変換し、記録してから sim へ渡す。sim はその記録だけを見る。**

- sim への入力は 2 種類だけ: 不変の `SessionConfig` と、tick ごとの確定入力 (`InputSnapshot[4]` + `SystemInputTick`)。
- 参加・離脱は「システムイベント」。サーバが単調増加の `eventSeq` を振り、レーン割り当ては
  「`eventSeq` 順に処理して空いている最小のレーン」という純関数 (`ApplySystemInput`)。`playerId` は
  初回 Join の `eventSeq` で、レーンとは別。切断中のレーンは同じ `playerId` が戻るか `Release` されるまで予約する。
- 締め切りは実時間で判定するが、結果は「どの tick に何を入れたか」の値として記録する。代替入力の規則
  (`SubstituteLateInput`) は純関数。走査順はレーン番号順 / `eventSeq` 順で、unordered コンテナとアドレスを使わない。
- SDK コールバック・受信・スナップショット受信完了はキューに積み、tick 境界で決まった順に処理する
  (受信 → ホスティングの出来事 → 締め切り判定 → 確定 → `RunOneTick` → `.rep` へ記録 → 送信。記録より先に外へ出さない)。
- 実時間を使うのはペーシング・締め切り・時刻同期・タイムアウトだけで、どれも sim の外。
- 途中参加の合流点は固定: スナップショットは構造変更を Commit した直後にだけ撮り、`SnapshotMeta` を付け、
  復元直後のワールドハッシュが一致しなければ参加させない。
- Debug / Release で sim 状態を変えない。Server.exe もビット一致の対象 (`replay_verify.bat` が Release の
  Server.exe による `--replay-verify` を含む)。
- 機械的に見張る: `check_rules` 規則 13 (sim 側から `Net/` `Hosting/` `Platform/Net/` を include しない、
  GameLift の依存は `Server.vcxproj` だけ)。サーバの checkpoint ハッシュ (8 tick ごと) とクライアントの
  確定ハッシュを突き合わせ、不一致は診断バンドル + 再同期 (黙って飲み込まない)。

## 決定 3: ホスティング抽象 `IHostingProvider`

`src\Server\Hosting\` に `IHostingProvider` (`Init` / `NotifyReady` / `Poll` → `StartSession` / `Terminate` /
`HealthCheck` / `ValidatePlayer` / `PlayerLeft` / `PlayerReleased` / `NotifySessionEnded` / `Shutdown`) を置く。
`LocalHosting` (`--port` で待受、起動直後に StartSession、認証は常に通す) と `GameLiftHosting` (Server SDK 5.x)
の 2 実装。GameLift の依存は `build\Server.vcxproj` だけに閉じ、Engine / Runtime / Editor / GameLogic は参照しない。
SDK のコールバックは SDK のスレッドから来るので、ミューテックス付きキューへ積むだけにして `Poll` (メインスレッドの
tick 境界) で処理する。

- 一時切断 (`PlayerLeft`) で SDK の `RemovePlayerSession` を呼ばない: 呼ぶと再接続を GameLift が拒否する。
  席を解放する `Release` の確定で `PlayerReleased` を呼ぶ。
- ゲームプロパティ `myeDeadlineTicks` (1..600、既定 3) / `myeRejoinTimeoutTicks` (1..216000、既定 1800 = 30 秒) で
  SessionConfig の締め切りと予約期間を上書きできる。最大人数は sim の構築時に決まるので上書きしない
  (`ValidatePlayer` で絞る)。
- SDK は `/MT`・`/MTd` の静的 `.lib` に自前ビルドして `external\gamelift-server-sdk\` へ置く (D17 でコミットを裁定、
  Debug 72MB + Release 53MB)。OpenSSL 3 は DLL を出力先へ同梱 (`external\openssl\`)。手順は各 `BUILD.md`、版は `external\VERSIONS.md`。

却下: **Engine を sim 専用ライブラリへ割る** (D2: 57 ファイルの依存整理で M81 の範囲を超える。Server.exe は
D3D をリンクするがロードしない = d3d11 / dxgi / d3dcompiler_47 / xaudio2 を `/DELAYLOAD` にして、実行後に未ロードを自己検査する)。
**Linux / EC2 フリート**: 後続マイルストーン (ADR-006 は改訂しない)。

## 決定 4: 個別の裁定 (spec §2)

| # | 裁定 | 理由 |
|---|---|---|
| D3 | ABI に `NetIsServer` / `NetIsClient` を**足さない** (ユーザー確認済み) | スクリプトが呼ばれる口は全部 tick の中で、「tick 中はエラー + 0」にすると呼べる場所が存在しない。役割は `NetRuntimeInfo.role` / NetWindow / ログで見せる。sim へ role が漏れる口が最初から無い |
| D4 | 予測上限は P2P が 8 tick のまま、サーバ構成のクライアントは 12 tick (200ms)。リングの容量は 16+4 本のコンパイル時最大で、上限は実行時 | クライアントの先行量は RTT + 締め切り + 先行マージン − inputDelay。RTT 150ms まで stall しない (selftest N4)。超えたら既存どおり stall = sim に影響しない |
| D5 | `contentHash` は assets 配下の全ファイルから**描画・音声専用の拡張子**を除いた集合 (除外リスト方式) | 種類ごとの許可リストは新資産種別の追加で 1 か所漏れて静かに desync する。多めに含めて弾くのは安全側 |
| D6 | `gameVersion` = ロードした GameLogic.dll のバイト列ハッシュ。食い違いは拒否。検証用に `--allow-game-mismatch` (WARN + `configBits` に記録) | Debug と Release の DLL は必ず別バイトだが sim は同値でなければならない。server_verify / net_verify の混在ケースだけが使う。本番では使わない |
| D11 | システムイベントに `Release` (予約の解放) を足す | タイムアウトは実時間の出来事なので、イベント列に載らないと割り当てが純関数にならない。サーバが 1 回だけ変換して記録 |
| D12 | 代替入力 (サーバ) は前 tick の確定入力から消費型 (chars / mouseDelta / wheelDelta) を 0 にしたもの。クライアントの予測は別関数 (`PredictLaneInput`: chars / wheelDelta だけ 0、mouseDelta は繰り返す) | 消費型を繰り返すと文字が 2 回打たれ視点が 2 回回る。予測は確定値に入らないので当たりやすさで決めてよい |
| D8 | `engineVersion` = `MYE_GIT_HASH` (既存の `MyeBuildInfo`) の 64bit ハッシュ | 新しい仕組みを作らない。Debug / Release で同値 |
| D9/D10 | `.rep` v9 でも `rngState/rngInc` を残す。v8 の `.rep` は読める | スナップショット無し記録の開始 RNG。システム入力を持たない記録のハッシュ列は 1 tick も動かさない |

## 決定 5: 互換性

- ABI v23 (126 → 131 スロット、末尾追加): `NetLaneMask` / `NetLaneState` / `NetLanePlayerId` / `NetSystemEventCount` / `NetGetSystemEvent`。
  v13 の `Net*` (`NetLocalPlayer` / `NetPingMs` ...) は**表示専用・機種依存**、v23 は**確定入力から導く sim 値**で出どころが違う
  (非サーバ構成ではレーン [0, playerCount) が Connected、playerId 0、イベント 0 件 = `ctx.playerCount` から決まる `.rep` で再現する値)。
- **外部プロジェクト (三校 / HAL Collector) の GameLogic.dll は `apiVersion` 22 のままだと v23 のエンジンに拒否される。再ビルドが必須。**
- P2P のプロトコルは版 6 (`NetIdentity` が `SimProvenance` から作られる)。旧ビルドとは繋がらない (仕様どおり Reject)。
- `.rep` v9 (`SessionConfig` / `SimProvenance` / 開始 `SnapshotMeta` / `SystemInputTick`)、`kSimSnapshotVersion` 24。
- `NetEventProbe` (参加・離脱を数えて World に書く検証用スクリプト) はデモへ自動では付かない。デモの生成順 = 粒子 RNG のストリームと
  golden を動かさないため、selftest が名前で付ける。

## 既知の制限

- 多レーン UI は不可 (D13): UI はレーン 0 の入力だけで評価される。サーバ構成ではキャンバス寸法の照合をしない (基準解像度とフォント計測表は照合する)。
- コンピュート ABI (v21) は device 無しの Server では 0 / no-op。GPU の結果を sim 状態へ書き戻すスクリプトは Server と描画クライアントで割れるので、サーバ対象のゲームでは禁止 (R-7)。
- GPU パーティクル設定 (`particleBackend: gpu`) は全員一致が前提 (R-6)。Server は device 無しで CPU 側を選ぶ代わりに Update が no-op になり、GPU パーティクルはハッシュに入らない。設定の一致は `contentHash` (`project_settings.json` を含む) が保証する。
- Server は cook キャッシュを常に無効にする (R-8)。起動時に毎回モデルをパースする (Release 約 0.9 秒)。
- `contentHash` の対象: 拡張子の除外 + その種類の `.meta`、`content_manifest.json` 自身と `scripts/Generated/` のパス除外 (R-9)。除外した種類の `.meta` (テクスチャ等の GUID) は入らない。`--flow-demo` が起動時に書くシーンファイルなど、実行履歴で揺れうるデモ専用の事例がある。
- `AcceptPlayerSession` / `RemovePlayerSession` は SDK の同期呼び出しで、60Hz ループの中で呼ばれる。GameLift への WebSocket が詰まると tick が止まる (結果は変わらず遅れるだけ。R-11)。tick 時間の max は 7〜40ms に跳ねる (参加時のスナップショット撮影と推定、R-10)。締め切り 3 tick (50ms) には収まる。
- Anywhere の認証トークンは約 15 分で失効する。疎通確認はトークン取得から 15 分以内に終える。長時間稼働のトークン更新は未対応。
- SDK の TLS は証明書を検証しない設定 (SDK 側の実装)。通信の暗号化は M81 の対象外で、認証は player session ID の照合のみ。
- Windows のみ。最大 4 人。マッチメイク / FlexMatch / NAT 越え / IPv6 / ゲーム独自メッセージは対象外。

## 検証

`Editor.exe --selftest` (Session / Server/client net / 偽トランスポートの N1〜N4)、`Server.exe --selftest` (GameLift 偽 SDK、`.rep` の逐次書出し)、
`tools\server_verify.bat` (実プロセス、ロス 20%・途中参加・切断 → 再接続・desync 注入、サーバ `.rep` を Debug / Release の Server.exe と窓あり Runtime で再生)、
`tools\replay_verify.bat` (Server.exe の `--replay-verify` を含む)、`tools\net_verify.bat` (P2P の回帰)、`tools\check_rules.ps1` (規則 13)。
`.rep` の逐次書出し: サーバは `.rep` を tick ごとに書き、異常終了しても完了済みの tick まで読める (`tickCount = 0` の救済)。
クラッシュバンドルの `crash.txt` は Server では「`--replay-record` の `.rep` を `Server.exe --replay-verify` にかける」手順を示す。
実 AWS との疎通 (G5) は `docs\gamelift-anywhere.md` の手順でユーザーが実施し、`plans\m81-dedicated-server\anywhere-log\` に記録する。
