# sub-05: GameLift 窓: EC2 の手順と課金中の表示

- 依存: sub-02 (サーバ版の組み立て。**手動チェックポイントの結果を司会から受けてから着手**)、sub-04 (窓と状態機械の土台)
- 状態: 未着手
- 往復: 0

## やること

0. **(VERDICT sub-04 round 1 の回答)** 着手の最初に `GameLiftFlow.cpp` (約 1100 行) を分ける: 共通 (確認・待ち・Issue・FailWith・状態確認) / Anywhere の操作 / EC2 の操作。
   振る舞いは変えない。分けた直後に `gameliftflow` が全 PASS することを確かめてから EC2 を足す。
   EC2 の操作も同じ `GameLiftFlow` の上に載せ、同じ runner を共有して、1 窓で同時に走る CLI を 1 本に保つ。
   upload-build は操作ごとのタイムアウト上書き (0 = なし) を `Issue` に足して使う。

spec 4.1.6 と 4.3 のうち EC2 タブ。

1. EC2 タブの入力欄 (書き出しフォルダ = 既定は BuildSettings の出力先、ビルド名・版、フリート名、インスタンスタイプ、ポート、最大人数) と保存。
2. 手順の状態機械 (sub-04 と同じ作り): サーバ版を作る (sub-02 の関数 + 局所起動確認) / アップロード / フリート作成 /
   セッションを作って参加 / 台数 0・台数 1 / フリート削除 / ビルド削除。CLI・JSON・待ちの上限は spec 4.1.6 の表のとおり。
   - `create-fleet` は `<project>\cache\gamelift\create-fleet.json` を書いて `--cli-input-json file://...`。Parameters を許可パターンと
     長さ (1〜1024) で事前検査。
   - フリートの状態推移を表示し、ERROR なら `describe-fleet-events` の直近 10 件。
   - 参加のクライアントは `<書き出しフォルダ>\Runtime.exe` を `--project` なし・作業ディレクトリ = 書き出しフォルダで。
     `mye_server_build.json` の元 dist と違うフォルダなら警告。
3. 確認ダイアログ: アップロード / フリート作成 (**課金が始まる**と明記) / 台数 0 / 台数 1 / フリート削除 / ビルド削除。
4. 「課金中」帯: 保存済みフリートの `describe-fleet-capacity` で ACTIVE + PENDING > 0 なら最上部に赤帯 (フリート名・台数・経過)。
   窓が開いている間 60 秒おきに更新。
5. エディタ起動時の確認: 保存済み EC2 フリート ID があれば読み取り 1 回。台数 > 0 ならログ WARN + 窓を開いて帯 (D16)。ID が無ければ何もしない
   (CLI を起動しない)。
6. 自己テスト (偽 CLI) と、一時プローブ + `--screenshot` の撮影。

## やらないこと (このサブでは)

- Spot / 複数ロケーション / オートスケール / 料金の表示 (spec 3. やらない)
- RDP の補助・ログの zip のダウンロード (文書で示すだけ)

## 触る場所 (planner の見立て)

- `src/Editor/Windows/Project/GameLiftWindow.{h,cpp}` (EC2 タブ、帯)
- 新規 `src/Editor/Tools/GameLift/` に EC2 の状態機械 (例 `GameLiftEc2Flow.{h,cpp}`)
- `src/Editor/App/EditorApp.cpp` (起動時の確認を走らせる位置。起動を待たせない = 非同期)
- `src/Editor/App/EditorSettings.{h,cpp}` (EC2 の ID 類)
- `src/Engine/Core/Localization/LocalizationTable.inl`
- 参考: BuildSettings の出力先の持ち方 (`BuildSettingsWindow.h:62 outputDir_`)

## 受け入れ条件 (このサブ)

spec 5. の 20〜24。

1. 「アップロード」の argv (`--operating-system WINDOWS_2022` `--server-sdk-version 5.6.0` `--build-root <server-build>`) が正しく、
   偽 CLI の進捗行で % が進み、`Build ID:` を保存し、describe-build の INITIALIZED → READY を待つ。FAILED で止まる。[E]
2. 「フリート作成」の JSON がキーと値ともに spec どおり。Parameters に許可外の文字 (例 `|` `&`) が入ると CLI を出さずに拒否。
   状態推移の表示と、ERROR でのイベント取得。[E]
3. EC2 の「参加」のクライアント argv に `--project` が無く、exe が書き出しフォルダの Runtime.exe。元 dist と違えば警告。[E]
4. 台数 1 で帯あり → 台数 0 で帯なし。起動時の確認は ID があるときだけ CLI 1 本、無ければ 0 本。台数変更・削除は確認を経る。[E]
5. スクショ: EC2 タブ (ACTIVE・課金中帯の偽状態)。日本語・英語。[P]
6. `tools\check_rules.ps1` 0 error。[R]

## 検証コマンド

- ビルド (Debug / Release)
- `MYE_SELFTEST_ONLY=<新スイート>` で `bin\x64\Debug\Editor.exe --selftest`
- `tools\check_rules.ps1`
- 一時プローブ + `--screenshot`

## 実装メモ (coder が追記)

## フィードバック履歴
