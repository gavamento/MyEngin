# EOS (Epic Online Services) の準備と疎通確認

M93 のオンライン (ロビー・P2P 接続・無線ボイス) を EOS で動かすための手順。設計は `docs\adr\ADR-031-online-eos.md`。
SDK の置き方は `external\eos-sdk\BUILD.md`。

## 1. ポータルの設定 (1 回だけ)

1. [Epic Developer Portal](https://dev.epicgames.com/portal) で組織とプロダクトを作り、EOS Developer Agreement に同意する。
2. プロダクトの「Product Settings」で次の 3 つを控える。
   - Product ID
   - Sandbox ID (開発中は Dev サンドボックス)
   - Deployment ID (そのサンドボックスのデプロイ)
3. 「Clients」で **Client Policy を Peer2Peer** にしたクライアントを作り、Client ID と Client Secret を控える。
   ポリシーで Connect・Lobbies・P2P・Voice (RTC) が許可されていることを確かめる。
   Peer2Peer ポリシーの資格情報は実行ファイルに同梱される前提の値で、秘密鍵ではない。
4. Epic Account Services (Epic アカウントでのログイン) は**使わない**。Device ID でログインするので Brand Review も要らない。

## 2. プロジェクトの設定

ゲームのプロジェクトの `assets\project_settings.json` に `online` 節を書く (エンジンの assets なら `C:\HAL\MyEngin\assets\project_settings.json`)。

```json
"online": {
  "backend": "eos",
  "displayName": "",
  "eos": {
    "productId": "...", "sandboxId": "...", "deploymentId": "...",
    "clientId": "...", "clientSecret": "..."
  },
  "voice": { "enabled": true, "pushToTalk": false, "pushToTalkKey": "T" }
}
```

- 公開リポジトリには 5 つの値を空にしてコミットし、配布ビルドの直前に埋める。
- `displayName` が空なら Windows のユーザー名を使う (32 字まで)。
- 設定ファイルを触らずに試すときは `--online-backend eos|local|none` で上書きできる。

## 3. 動作確認 (EOS 無しの流れの確認)

まず `local` で、同じ PC の 2 プロセスがロビー → 接続 → 中継まで通ることを確かめる。

```bat
bin\x64\Release\Runtime.exe --local-demo --online-backend local --online-host --online-code TESTAB
bin\x64\Release\Runtime.exe --local-demo --online-backend local --online-join TESTAB
```

窓の左上にロビー窓が出る。手で操作するときは `--online-host` / `--online-join` を付けずに起動し、
「部屋を作る」と「参加コード」を使う。

## 4. EOS での疎通確認 (別の PC・別の回線で)

1. 両方の PC に同じビルド (同じ Runtime.exe / Server.exe / GameLogic.dll / assets) を置く。
   決定論の出自 (エンジン・ゲーム・コンテンツのハッシュ) が食い違うとサーバが接続を拒否する。
2. ホストの PC で Runtime を起動し「部屋を作る」。窓に出た 6 文字の部屋のコードを相手に伝える。
3. 相手の PC で Runtime を起動し、参加コードを入れて「参加」。
4. 両方の窓にメンバーが 2 人出ること、ゲームの中で互いのキャラクターが動くこと、声が聞こえることを確かめる。
5. 記録を残すなら両方に `--replay-record cache\eos_<名前>.rep` を付け、終わったら次で照合する。

```bat
bin\x64\Release\Runtime.exe --rep-diff cache\eos_host.rep cache\eos_guest.rep --rep-diff-overlap 100
```

結果は `plans\m93-eos\field-log\<日付>.md` に残す (M81 の `anywhere-log` と同じ)。

## 5. 困ったとき

| 症状 | 見るところ |
|---|---|
| ロビー窓に「サインインできませんでした」 | `[online] EOS:` のログ。ID の打ち間違い、クライアントポリシー、サンドボックスとデプロイの組み合わせ |
| ボイスの欄が出ない | `xaudio2_9redist.dll` が exe の隣に無い (BUILD.md)。`voice.enabled` |
| 部屋に入れるがゲームが始まらない | ホストの `cache\logs\online_host_server.log` (Server.exe のログ)。出自の不一致で拒否されていないか |
| 参加側だけ止まる | ホストが抜けた / ホストの Server.exe が落ちた。ホスト移譲は無いので、ホストが部屋を作り直す |

## 販売時にやること (エンジンの外)

- EOS Developer Agreement への同意 (ポータル)
- プレイヤー向けプライバシーポリシー (IP アドレス・デバイス ID・音声を Epic が処理する旨) をストアページに載せる
- SDK 同梱の ThirdPartyNotices を配布物かクレジットに含める
- Steam ストアの機能フラグ (オンライン協力・ボイスチャット)
- SDK の版を年 1 回程度は上げる (Epic は古い SDK を段階的に非対応にする)
