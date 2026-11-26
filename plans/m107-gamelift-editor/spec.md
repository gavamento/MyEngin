# M107 エディタから AWS GameLift を操作する — 仕様書

- slug: m107-gamelift-editor
- 状態: 確定 (2026-11-27。Q1〜Q5 と受け入れ条件 1〜29 を司会経由でユーザーが裁定どおり承認)
- 依頼原文: AWSとつなげてオンラインできる機能をもっと簡単に使えるように死体
  (「死体」は「したい」の誤変換。司会がユーザーに確認済み)
- 出発点: `plans/m107-gamelift-editor/design-draft.md` (司会の事前調査。事実は下の 2. で planner が実コードと照合し直した)

## 1. 目的 (なぜ作るか)

M81 で `Server.exe --hosting gamelift` (GameLift Anywhere) は動いたが、使うには AWS CLI を 10 本以上手で打ち、
FleetId / エンドポイント / 15 分で切れるトークン / 60 秒で切れる player session ID を手で写す必要がある。
実疎通でつまずいた 3 点 (折り返し貼り付けで引数が落ちた・player session の 60 秒切れ・`custom-custom-` の二重付け) は
すべて「人が値を写す」ことが原因だった。さらに AWS 上 (マネージド EC2) で Server.exe を動かす経路は存在しない。

達成したい状態: **エンジンの開発者が、エディタの 1 つの窓から**
- 自分の PC で動かす専用サーバ (GameLift Anywhere) と
- AWS 上で動かす専用サーバ (GameLift マネージド EC2 フリート)

のどちらでも、「準備 → サーバを動かす → セッションを作って参加 → 片付け」をボタンで行え、
値の手写しが一切なく、課金中のリソースを見落とさないこと。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| D1 | 誰のどの場面か | design-draft 1 章 | **エディタの窓から操作 (開発者向け)**。プレイヤー向けロビー連携は対象外 (決定済み) | そのまま |
| D2 | サーバの場所 | 同上 | **Anywhere と EC2 の両方を選べる** (決定済み) | そのまま |
| D3 | AWS の呼び方 | 同上 | **aws CLI を子プロセスで呼び JSON を読む** (決定済み)。AWS SDK for C++ は組み込まない | そのまま |
| D4 | 引数なし `InitSDK()` はマネージド EC2 の正しい呼び方か | `external/gamelift-server-sdk/include/aws/gamelift/server/GameLiftServerAPI.h:53` に引数なし版あり。公式 (C++ server SDK 5.x Actions) に「managed EC2 fleet … call InitSDK() without server parameters」と明記。同ページに「5 分以内に InitSDK、その後 5 分以内に ProcessReady」(fleets-creating-debug) | — | **確認済み**。Server.exe に `--hosting gamelift-managed` を**明示値**として足し、引数なし `InitSDK()` を呼ぶ。Anywhere の引数と同時指定は矛盾として exit 1 (暗黙の自動判定はしない) |
| D5 | まっさらな Windows Server 2022 で Server.exe が起動するか | `dumpbin /dependents bin\x64\Release\Server.exe` (2026-11-27 planner 実測): 静的依存は WINMM / WS2_32 / KERNEL32 / ole32 (OS 同梱) と `libssl-3-x64.dll` / `libcrypto-3-x64.dll`。XAudio2_9 / D3DCOMPILER_47 は遅延ロード (呼ばない限り不要)。`libssl` / `libcrypto` は **`VCRUNTIME140.dll`** と `api-ms-win-crt-*` (UCRT) に依存。UCRT は Windows 10 / Server 2016 以降の OS 同梱だが、**VCRUNTIME140 は OS 同梱ではない** (VC++ 再頒布パッケージ)。Server.exe 自身と GameLogic.dll は `/MT` (`build/Common.props:61`) で CRT DLL 依存なし | 未確認 `[ユーザーに聞ける]` (Q3) | **裁定: サーバ版に `vc_redist.x64.exe` と `install.bat` を同梱**し、フリート作成時に入れる。AWS 公式の例 (gamelift-build-cli-uploading-install: `vcredist_x64.exe /install /quiet /norestart /log ...`) と Microsoft Learn の推奨 (再頒布パッケージによる集中配置。より新しい版があると失敗コードを返す = install.bat は戻り値をログに残して成功で抜ける) の両方に合う。却下: app-local (`vcruntime140.dll` を exe の隣に置く。Microsoft は可能だが非推奨 = Windows Update で直らない) / 何もしない (AMI に入っている保証が無い)。実機で起動するかは**手動チェックポイント (受け入れ 8)** で確かめるまで未確認 |
| D6 | サーバとクライアントの出自照合が揃うか | `Provenance.cpp:402 CompareProvenance` は engineVersion (git) / gameVersion (GameLogic.dll のハッシュ) / contentHash を比べる。`ResolveContentHash` (`Provenance.cpp:331`) は `assets\content_manifest.json` があればそれ、無ければ assets を走査。書き出し (`BuildSettingsWindow.cpp:243-329`) はエンジンシェーダの追加・`main.scene.json` の配置・manifest の焼き込みをするので、**開発中のプロジェクト assets と書き出し物の contentHash は一致しない** | — | **Anywhere**: サーバ = Editor.exe の隣の `Server.exe --project <proj>`、クライアント = 同じフォルダの `Runtime.exe --project <proj>` (両方が開発中の assets を読むので一致。M81 の実疎通と同じ組)。**EC2**: サーバ版は書き出し済みフォルダの `assets\` / `GameLogic.dll` / `cache\cooked\` を**そのまま写して**作り、参加するクライアントは**その書き出しフォルダの `Runtime.exe`** (manifest が同じバイトなので一致) |
| D7 | 参加するクライアントは Runtime か Editor か | 既存の `NetWindow::LaunchClient` (`NetWindow.cpp:94`) は自 exe (Editor) を `--autoplay` 付きで起動。player session は**作成から 60 秒以内**に Accept されないと TIMEDOUT (`docs/gamelift-anywhere.md` 5 章)。Debug の Editor は起動が重い | 未確認 `[ユーザーに聞ける]` (Q2) | **裁定: Runtime.exe** (Anywhere は Editor.exe の隣、EC2 は書き出しフォルダ)。起動が速く 60 秒に余裕があり、EC2 では書き出し物と出自が揃う唯一の選択。NetWindow の既存「接続」欄は残す (変えない) |
| D8 | Ctrl+C で止めると ProcessEnding が届くか (M81 の未確定点) | `SetConsoleCtrlHandler` は `src/Server/Hosting/LocalHosting.cpp:36` **だけ**。`GameLiftHosting` は登録しない → `--hosting gamelift` の Ctrl+C は既定ハンドラで即終了し、**ProcessEnding は送られない**。`docs/gamelift-anywhere.md` 8 章の「どちらも同じ経路で…ProcessEnding」は**コードと食い違う** (M81 で「× で閉じたら TERMINATED まで 4 分」が観測されたのと整合) | — | **バグとして直す** (sub-01): 停止要求 (コンソールの Ctrl+C / Ctrl+Break と、新設の名前付きイベント) をホスティングの種類によらず同じ Terminate 経路へ入れる。8 章の記述は sub-06 で実装に合わせて直す |
| D9 | エディタはサーバをどう止めるか | エディタは GUI でコンソールを持たない。`CREATE_NO_WINDOW` の子へ Ctrl+Break を送るには AttachConsole の細工が要り、エディタ自身のコンソール状態 (`--selftest` 時) を壊しうる。`HostServerProcess::Stop` (`src/Engine/Engine/Online/HostServerProcess.cpp:155`) は TerminateProcess (ProcessEnding なし) | — | **Server.exe に `--stop-event <名前>` を足す** (エディタが作る名前付きイベント。シグナルで Terminate)。暗黙の仕組み (コンソールの細工) は使わない |
| D10 | エディタがサーバの「ProcessReady 送信済み」を知れるか | ログは `fputs(stdout/stderr)` (`Log.cpp:49,60`) で **fflush しない**。stdout をファイルへ向けると CRT は全バッファ (数 KB たまるまで書かれない) | — | sub-01 で Server.exe の stdout / stderr を**行単位で届く**ようにする。エディタはログの `[gamelift] ProcessReady sent` を見る + `create-game-session` の `FleetCapacityExceededException` を一定回数再試行する (二重の安全策) |
| D11 | EC2 でサーバのログを回収できるか | `ServerLoop.cpp:158` が ProcessReady に渡すのは `.rep` のパスだけ。マネージド EC2 では起動パス (LaunchPath) の exe 自身が SDK を呼ぶ必要があり (fleets-creating-debug: 別プロセスが SDK を呼ぶと登録が時間切れ)、`cmd /c ... > log` で包めない | — | **Server.exe に `--log-dir <dir>`** を足す: `<dir>\server-<pid>.log` へ stdout/stderr を向け、そのパスを ProcessReady の logPaths に加える。プロセスが ProcessReady まで来ない失敗はフリートイベント (`describe-fleet-events`) で窓に出す |
| D12 | 認証トークンが漏れないか | トークンは環境変数 `MYE_GAMELIFT_AUTH_TOKEN` で渡す設計 (`ServerMain.cpp:83-148`)。既存の `StartChildProcess` (`src/Editor/App/ChildProcess.cpp:11`) は stdout を**ファイル**へ書き、環境ブロックは親のまま | — | CLI の出力は**パイプでメモリへ**読む (ファイルに書かない)。`get-compute-auth-token` の応答はログにも出さない。Server.exe への受け渡しは**子の環境ブロックだけ** (エディタ自身の環境変数は書き換えない)。設定ファイルにも保存しない |
| D13 | CLI の文字コードとページャ | aws CLI v2 は同梱 Python。パイプ出力の文字コードは環境 (`PYTHONIOENCODING`) に左右される (メモリ: cp932 で化けた前例) | — | 子の環境に `PYTHONIOENCODING=utf-8` / `AWS_CLI_FILE_ENCODING=UTF-8` / `AWS_PAGER=` (空) を**明示**し、全呼び出しに `--output json --no-cli-pager --region <r>` を付ける (利用者の環境に依存しない) |
| D14 | 引数の引用・注入 | `create-fleet` の runtime configuration は shorthand だと `C:\game\Server.exe` の `\` や `,` `=` が壊れやすい。`--cli-input-json file://<path>` は create-fleet が対応 (CLI リファレンスの synopsis に `[--cli-input-json \| --cli-input-yaml]`) | — | 構造のある要求 (create-fleet) は **JSON ファイル + `--cli-input-json`**。単純な値は argv の配列から Windows の引用規則 (`CommandLineToArgvW` と往復する形) で組み、利用者が入れた値は許可文字 (英数と `-_.`) で検査してから使う |
| D15 | ロケーション名の `custom-` 二重付け | M81 実疎通で発生 (`docs/gamelift-anywhere.md` 2 章) | — | 窓は接尾辞だけを入力させ、`custom-` はエディタが付ける。入力が `custom-` で始まっていたら拒否して理由を出す |
| D16 | 窓を閉じても EC2 フリートが残る = 課金が続く | EC2 は起動から終了まで秒課金 (design-draft 3 章、pricing ページ)。新規フリートは作成直後に 1 台立つ (**記憶による**。未照合) | 未確認 `[ユーザーに聞ける]` (Q4) | 窓を開いている間は台数を定期確認して「課金中」帯を出す。**裁定: エディタ起動時にも**、設定に EC2 フリート ID が残っていれば読み取り 1 回 (`describe-fleet-capacity`) を走らせ、台数 > 0 ならログ WARN + 窓を開いて赤帯を出す |
| D17 | 設定の保存先 | `EditorSettings` は `<project>\.mye\editor_settings.json` (個人設定・共有しない。`EditorSettings.h:32`) | — | リージョン / ロケーション接尾辞 / フリート ID / ビルド ID / コンピュート名 / IP / ポート / インスタンスタイプを `editor_settings.json` に保存 (AWS アカウントは人ごとなので `project_settings.json` に置かない)。トークン・資格情報は保存しない |
| D18 | AI が AWS を触らないか | M81 で AI による削除実行が止められた (`docs/gamelift-anywhere.md` 9 章末尾)。司会の補足 | — | 作成・削除・課金開始・台数変更は**確認ダイアログを経てユーザーが押したときだけ** CLI を出す。自動検証は偽 CLI (プロセス内の差し替え + `cmd.exe`) と偽 SDK だけ。実 AWS は受け入れ条件の「手動確認」に分ける |
| D19 | 新しい窓か、Network 窓へ足すか | Network 窓 (`NetWindow.cpp`) はセッションの統計表示が主で、未接続時の「接続」欄は手入力の 1 行 | 未確認 `[ユーザーに聞ける]` (Q5) | **裁定: 新しい窓「GameLift」**を Window メニューの Network の隣に置く (状態と手順が多く、Network 窓の役割を広げすぎない) |
| D20 | EC2 の実機検証をいつするか | D5 の起動可否は自動検証できない (ユーザーの PC は Windows 11 Home = Windows Sandbox なし)。外れたら sub-05 (EC2 の窓) の前提が崩れる | 未確認 `[ユーザーに聞ける]` (Q1) | **裁定: sub-02 の直後に手動チェックポイント**を置く (CLI を手で打つ手順を sub-02.md に用意。費用は c5.large 1 台を 1 時間未満)。sub-05 はチェックポイントの結果を見てから着手する |
| D21 | サーバ版はどこに作るか | 書き出し (`BuildSettingsWindow::StageCopy`) は Runtime.exe / .NET / C# ホスト一式を含む (サーバは C# レーンを持たない = 不要) | — | 書き出し済みフォルダから**サーバに要る分だけ**を `<project>\cache\gamelift\server-build\` へ組む (作り直すたびに中身を入れ替える)。配布物 (書き出し先) には Server.exe を入れない (プレイヤーへ配らない) |
| D22 | 判断の出典 | — | — | 7. の「出典」に URL を列挙。取得できなかった項目は「記憶による」と明記 |

## 3. スコープ

- やる:
  - Server.exe: `--hosting gamelift-managed` (引数なし InitSDK)、停止要求の一本化 (コンソール + `--stop-event`) と GameLift 経路での ProcessEnding、`--log-dir`、ログの行単位出力
  - エディタ: aws CLI を非同期に呼ぶ層 (パイプ・タイムアウト・取消・環境の明示・JSON 解析)
  - エディタ: GameLift 窓 (状態表示 / Anywhere の手順 / EC2 の手順 / 課金中の表示 / 確認ダイアログ)
  - エディタ: サーバ版の組み立て (書き出しフォルダから) と、組んだ Server.exe の局所起動確認。窓を出さない入口
    `Editor.exe --project <proj> --gamelift-server-build <dist>` も付ける (手動チェックポイントと、窓を使わない手順書のため)
  - check_rules 規則 22 (新しい Editor のフォルダを下位層から include させない)
  - 文書: ADR-042、`docs/gamelift-editor.md` (新規)、`docs/gamelift-anywhere.md` の食い違い修正、`engine_spec.md`、`docs/test_checklists.md`
  - 製品の版 0.89.4 → **0.90.4** (機能追加 = xxx を 1 上げる。yyy は戻さない)
- やらない (明示的に外したもの):
  - 配布ゲームのプレイヤー向けロビー連携 (API Gateway + Lambda 等の受付)。M93 のロビー (`src/Engine/Engine/Online/`) は触らない
  - AWS SDK for C++ の組み込み
  - Linux フリート / コンテナフリート / Spot / 複数ロケーション / オートスケール / キュー / FlexMatch
  - 1 インスタンスで複数プロセス (ConcurrentExecutions は 1、ポートは 1 つ)
  - トークンの自動更新 (M81 と同じく、起動ごとに取り直す)
  - IAM の作成・権限の設定 (文書で必要な権限を示すだけ)
  - 料金の表示 (単価を誤って出す危険のほうが大きい。台数と経過時間だけ出す)
  - サーバ側での C# レーン (M81 からの既知の制限。文書に書くだけ)
  - ABI / SimSnapshot / NetProto / Replay の版の変更 (変えない)
- 後回し:
  - RDP でのインスタンス調査の補助 (`get-compute-access` 等)。文書で手順を示すだけ
  - GameLift のログ回収 (`get-game-session-log-url`) のダウンロードボタン (文書でコマンドを示す)

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 Server.exe (sub-01)

| 起動 | 振る舞い |
|---|---|
| `--hosting gamelift-managed` | 引数なし `InitSDK()` (30 秒の上限は既存と同じ)。トークン・`--gamelift-*` 引数は要らない |
| `--hosting gamelift-managed` と `--gamelift-ws-url` / `--gamelift-fleet-id` / `--gamelift-host-id` / `--gamelift-process-id` のどれかを同時指定 | InitSDK 前に「矛盾」と 1 行出して exit 1 |
| `--hosting gamelift` (Anywhere) | 現状どおり (引数とトークンが揃わなければ exit 1) |
| `--stop-event <名前>` | 起動時に名前付きイベントを開く (無ければ作らず exit 1 — 開けない = 呼び出し側の誤り)。シグナルされたら Ctrl+C と同じ Terminate (記録中の .rep を閉じる → GameLift なら ProcessEnding → Destroy → exit 0) |
| Ctrl+C / Ctrl+Break / コンソールを閉じる | **ホスティングの種類によらず** Terminate (D8 の修正) |
| `--log-dir <dir>` | `<dir>` を作り、`<dir>\server-<pid>.log` へ stdout と stderr を向ける。そのパスを ProcessReady の logPaths に加える (`.rep` があればそれも) |
| stdout / stderr がファイル・パイプのとき | ログは 1 行ごとに届く (全バッファで溜めない) |

- 停止要求の受け口は 1 か所にまとめ (LocalHosting にあるコンソールハンドラを含めて)、ServerLoop が tick 境界で
  見る既存の Terminate 経路へ入れる。sim には触れない。
- `--hosting gamelift-managed` の窓口は `IGameLiftSdk` に足す (偽 SDK が「どちらの InitSDK が呼ばれたか」を記録できること)。
  SDK の型は `GameLiftSdk.h` に漏らさない (規則 13-c の境界)。
- usage 文 (`PrintUsage`) を更新する。

#### 4.1.2 サーバ版の組み立て (sub-02)

入力: 書き出し済みフォルダ `<dist>` (BuildSettings の出力。`Runtime.exe` と `assets\content_manifest.json` があること)、
Editor.exe のフォルダ `<exeDir>`。出力: `<project>\cache\gamelift\server-build\` (既存なら中身を入れ替える)。

| 入れるもの | 出所 |
|---|---|
| `Server.exe` | `<exeDir>` |
| `libssl-3-x64.dll` / `libcrypto-3-x64.dll` | `<exeDir>` |
| `GameLogic.dll` | `<dist>` |
| `assets\` (manifest を含め**バイト一致**) | `<dist>\assets\` |
| `cache\cooked\` (あれば) | `<dist>\cache\cooked\` |
| `vc_redist.x64.exe` | Visual Studio の `VC\Redist\MSVC\<版>\vc_redist.x64.exe` (vswhere で探す。最も新しい版) |
| `install.bat` | エディタが生成 (下記) |
| `mye_server_build.json` | エディタが生成: 元の `<dist>` の絶対パス、組んだ日時、Server.exe の構成 (Debug/Release)、contentHash (manifest の値)、GameLogic.dll のハッシュ |

入れないもの: `Runtime.exe`、`dotnet\`、`nethost.dll`、`MyeScripting*`、`Microsoft.CodeAnalysis*`、EOS の DLL、`.pdb`。

`install.bat` (冪等。何度走っても成功で終わる):
```
vc_redist.x64.exe /install /quiet /norestart /log C:\game\install_vcredist.log
exit /b 0
```
(vc_redist の戻り値は再起動要求 3010 / 既に新しい版 1638 などを含むので、結果はログに残して成功で抜ける。
本当に入らなかった場合は Server.exe が起動できず、フリートイベントに出る。)

失敗時: 足りないもの (dist の Runtime.exe / manifest、exeDir の Server.exe / OpenSSL、vc_redist) を**全部挙げて**止める
(何も書かない、または途中の組み立てを残さない)。Debug 構成の Server.exe なら警告 (動くが遅い) を出して続ける。

局所起動確認: 組んだフォルダの `Server.exe --hosting local --port <空きポート> --server-timeout 3` を組んだフォルダを
作業ディレクトリにして起動し、exit 5 (時間切れ = 起動して待ち受けまで来た) とログの `ready on UDP port` を確かめる。
これで assets / GameLogic.dll / シーンの同梱漏れは分かる (VC++ ランタイムの有無は分からない = D5 は手動で)。

#### 4.1.3 aws CLI 層 (sub-03)

- aws.exe の場所: 設定の明示パス → `SearchPathW("aws.exe")` → `C:\Program Files\Amazon\AWSCLIV2\aws.exe`。
- 1 回の呼び出し = argv の配列 + 追加環境 + タイムアウト + 「秘密を含む」フラグ。
  - 標準出力と標準エラーは**パイプでメモリへ** (大量出力でも子が詰まらないように読み続ける)。stdin は NUL。窓は出さない。
  - 環境: 親の環境ブロック + `PYTHONIOENCODING=utf-8` / `AWS_CLI_FILE_ENCODING=UTF-8` / `AWS_PAGER=` (空) + 呼び出しごとの追加。
  - 全呼び出しに `--output json --no-cli-pager --region <r>` (aws login 等の対話コマンドは別扱い)。
  - 毎フレームのポーリングで進む (UI スレッドを待たせない)。タイムアウト・取消 (子を終了) あり。取消は AWS 側で
    既に受け付けた操作を取り消さないことを UI に出す。
  - 「秘密を含む」呼び出し (`get-compute-auth-token`) は stdout をログに出さず、結果を使ったら消す。
- 呼び出しは窓ごとに**同時に 1 本** (前の結果を見て次を決める手順なので並べない)。
- 応答の解析は純関数 (JSON 文字列 → 構造体 + エラー分類)。分類: 成功 / 資格情報切れ (`ExpiredToken` 等、`aws login` を促す) /
  見つからない (`NotFoundException`) / 容量不足 (`FleetCapacityExceededException`) / 既にある (`ConflictException` 等) / その他 (stderr の先頭行)。
- `upload-build` は JSON を返さない (CLI リファレンスの出力例: `Uploading ... (21.45%)` の進捗行と最後の `Build ID: build-...`)。
  進捗の % と Build ID を行から読む。

#### 4.1.4 GameLift 窓: 共通 (sub-04)

- Window メニューの Network の隣に「GameLift」。
- 上段: aws CLI の場所と版 (`aws --version`) / アカウント (`aws sts get-caller-identity` の Account と Arn) / リージョン
  (初期値は `aws configure get region`、編集可) / 「再確認」/「aws login を開く」(新しいコンソール窓で `aws login` を起動。
  対話はユーザーがそちらで行い、終わったら「再確認」)。
  - CLI が無い / v1 / ログイン切れ / リージョン未設定 / プロジェクト未オープン / GameLogic.dll 未ビルドのときは、
    該当する操作ボタンを無効にし、理由を 1 行で出す。
- タブ: 「自分の PC (Anywhere)」/「AWS 上 (EC2)」。
- 下段: 実行中の CLI (コマンド名 + 経過秒、秘密は出さない) と「取消」、直近のエラー (分類 + stderr の先頭 3 行)。
- 作成・削除・課金開始・台数変更の前には確認ダイアログ (何を・どのリージョンに・課金の有無)。「やめる」なら CLI を出さない。
- 設定は `<project>\.mye\editor_settings.json` に保存 (D17)。

#### 4.1.5 Anywhere の手順 (sub-04)

入力欄: ロケーション接尾辞 (既定 `mye-dev`、表示は `custom-` + 入力)、フリート名 (既定 `mye-anywhere`)、コンピュート名
(既定 = PC 名を許可文字へ整えたもの)、IP (既定 `127.0.0.1`)、ポート (既定 7777)、最大人数 (1〜4、既定 4)。

| ボタン | 動き |
|---|---|
| 準備 (確認あり: AWS にリソースを作る) | `list-locations --filters CUSTOM` で無ければ `create-location` → 保存済みフリート ID を `describe-fleet-attributes` で確かめ、無ければ `create-fleet --compute-type ANYWHERE --locations Location=custom-<x>` → `describe-compute` で無ければ `register-compute --ip-address <ip>`。あるものは作らない (冪等)。フリート ID を保存 |
| サーバ起動 | `describe-compute` で SDK エンドポイント → `get-compute-auth-token` (秘密) → `<exeDir>\Server.exe --project <proj> --hosting gamelift --gamelift-ws-url <ep> --gamelift-fleet-id <id> --gamelift-host-id <compute> --port <p> --max-players <n> --stop-event <名前>` を、トークンを**子の環境だけ**に入れて起動。ログは `<project>\cache\gamelift\server-<日時>.log`。kill-on-close のジョブに入れる (エディタが落ちたら道連れ)。ログに `ProcessReady sent` が出たら「待機中」 |
| セッションを作って参加 | `create-game-session --fleet-id --location custom-<x> --maximum-player-session-count <n>` (`FleetCapacityExceededException` なら 3 秒おきに最大 20 回再試行) → `describe-game-sessions` で ACTIVE まで (2 秒おき、最大 60 秒) → `create-player-session` → **同じ流れで直ちに** `<exeDir>\Runtime.exe --project <proj> --net-connect <IpAddress>:<Port> --player-session-id <id>` を起動。押すたびに 1 人増える (最大人数まで)。既存の ACTIVE なゲームセッションがあればそれへ player session を足す |
| サーバ停止 | stop イベントをシグナル → 最大 10 秒待つ → 終わらなければ TerminateProcess (その旨を警告) |
| 片付け (確認あり: 削除) | サーバが動いていれば先に停止 → `deregister-compute` → `delete-fleet` → `describe-fleet-attributes` で消えるまで (15 秒おき、最大 15 分) → `delete-location`。保存済み ID を消す |

- サーバが自分で終わったら (exit code とログの末尾 8 行を出して)「停止」に戻す。
- エディタ終了時にサーバが動いていれば「サーバ停止」と同じ手順で止める。

#### 4.1.6 EC2 の手順 (sub-05)

入力欄: 書き出しフォルダ (既定 = BuildSettings の出力先)、ビルド名 (既定 `mye-server`)、ビルド版 (既定 = 日時)、
フリート名 (既定 `mye-ec2`)、インスタンスタイプ (既定 `c5.large`)、ポート (既定 7777)、最大人数。

| ボタン | 動き |
|---|---|
| サーバ版を作る | 4.1.2 の組み立て + 局所起動確認 (AWS に触れない) |
| アップロード (確認あり) | `upload-build --name --build-version --build-root <server-build> --operating-system WINDOWS_2022 --server-sdk-version 5.6.0` (タイムアウトなし・取消あり・進捗 %) → Build ID を保存 → `describe-build` で READY まで (10 秒おき、最大 10 分)。FAILED なら止める |
| フリート作成 (確認あり: **課金が始まる**) | `create-fleet --cli-input-json file://<project>\cache\gamelift\create-fleet.json`。JSON: Name / BuildId / ComputeType `EC2` / EC2InstanceType / FleetType `ON_DEMAND` / EC2InboundPermissions `[{FromPort:p, ToPort:p, IpRange:"0.0.0.0/0", Protocol:"UDP"}]` / RuntimeConfiguration `{ServerProcesses:[{LaunchPath:"C:\\game\\Server.exe", Parameters:"--hosting gamelift-managed --port <p> --max-players <n> --exit-when-empty --log-dir C:\\game\\logs", ConcurrentExecutions:1}]}`。Parameters は CLI リファレンスの許可パターン (`^[A-Za-z0-9_:.+\/\\\- =@;{},?'\[\]"]+$`、1〜1024 文字) で事前検査。フリート ID を保存 → `describe-fleet-attributes` の Status を 15 秒おき (最大 60 分) に表示 (NEW → DOWNLOADING → VALIDATING → BUILDING → ACTIVATING → ACTIVE)。ERROR なら `describe-fleet-events` の直近 10 件を出す |
| セッションを作って参加 | Anywhere と同じ流れ (`--location` なし)。クライアントは **`<書き出しフォルダ>\Runtime.exe --net-connect <IpAddress>:<Port> --player-session-id <id>`** (`--project` なし、作業ディレクトリ = 書き出しフォルダ) |
| 台数 0 (確認あり: 課金停止) / 台数 1 (確認あり: 課金再開) | `update-fleet-capacity --desired-instances 0|1 --min-size 0 --max-size 1` |
| フリート削除 (確認あり) | `delete-fleet` → 消えるまで表示。保存済み ID を消す |
| ビルド削除 (確認あり) | `delete-build`。保存済み ID を消す |

- 「課金中」帯: 保存済みフリートの `describe-fleet-capacity` で InstanceCounts の ACTIVE + PENDING > 0 なら、
  窓の最上部に赤帯「課金中: <フリート名> <台数> 台 (<経過>)」。窓を開いている間 60 秒おきに更新。
- エディタ起動時: 保存済みの EC2 フリート ID があれば読み取りを 1 回だけ走らせ、台数 > 0 ならログ WARN + 窓を開いて帯を出す (D16)。
- `mye_server_build.json` の元 dist と、参加に使う書き出しフォルダが違えば警告 (出自が合わず Reject になる)。

### 4.2 データ・保存形式・互換性

- `editor_settings.json` に `gameLift` オブジェクトを足す (無ければ既定値。壊れた値は無視して既定)。既存キーは変えない。
- Server.exe の新しい引数 3 つ (`--hosting gamelift-managed` の値、`--stop-event`、`--log-dir`)。既存の引数の意味は変えない。
- ABI (`MYE_API_VERSION`) / `kSimSnapshotVersion` / `kNetProtoVersion` / `kReplayFileVersion` は**変えない**。
- `kEngineVersion` は 0.90.4 (sub-06 で 3 か所 + コミット件名)。

### 4.3 UI / ビジュアル

- 窓の既定サイズ 640x560。上段 (状態) → 課金中の赤帯 (あるときだけ) → タブ → 下段 (実行中 / エラー)。
- 手順ボタンは上から実行順に並べ、各行の左に状態 (未 / 済 / 実行中 / 失敗) を `themeColor` の意味色で示す。
- 無効なボタンの横に理由 (灰色の 1 行)。
- 確認ダイアログはモーダル。資源名・リージョン・課金の有無を書き、既定のフォーカスは「やめる」。
- 検証: 一時プローブ + `--screenshot` で (a) Anywhere タブ (偽の状態: 準備済み・待機中)、(b) EC2 タブ (偽の状態: ACTIVE・課金中帯)、
  (c) 確認ダイアログ、を日本語と英語で撮る。プローブはコミットしない。

### 4.4 非機能

- 決定性: AWS 操作はエディタの表示レーンだけ。sim / Replay / ネットのプロトコルに触れない。Server.exe の変更は
  停止要求の受け口とログの向け先だけで、sim の更新に入らない。
- レイヤー: 新しいコードは `src/Editor/Tools/GameLift/` と `src/Editor/Windows/Project/GameLiftWindow.*`。
  Engine / Runtime / GameLogic / Shared / Server から include させない (規則 22)。
  ★既存の規則 13-c は `aws[\\/]gamelift` / `GameLiftServerAPI` / `gamelift-server-sdk` / `aws-cpp-sdk-gamelift` を
  src\Server 以外で禁止している (大文字小文字を区別しない)。フォルダ名を `Aws/GameLift` にしない・文字列に `aws\gamelift` を書かないこと。
- 応答性: CLI 呼び出しも Server.exe も UI スレッドを止めない。長い待ちは経過と上限を表示し、取消できる。
- 秘密: トークン・資格情報をファイル (ログ・設定・cache) にも Log にも書かない。
- ローカライズ: すべて `Tr()`、両言語。`###` 後の識別子と書式指定子の順序を一致。`Tr()` を printf の書式に直接渡さない。
- 失敗の局所化: CLI が無い・ログイン切れでも窓が開き、他の機能は止めない。

## 5. 受け入れ条件

検証の凡例: [S] = `bin\x64\Debug\Server.exe --selftest`、[E] = `Editor.exe --selftest` (該当スイートを `MYE_SELFTEST_ONLY` で)、
[R] = `tools\check_rules.ps1`、[P] = 一時プローブ + `--screenshot`、[M] = ユーザーの手動確認 (実 AWS。費用あり)。

Server.exe (sub-01)
1. `--hosting gamelift-managed` で偽 SDK に「引数なし InitSDK」が 1 回呼ばれ、ProcessReady 以降は Anywhere と同じ流れになる。Anywhere 用の引数を併記すると InitSDK 前に exit 1。[S]
2. `--stop-event` のシグナルで、local / gamelift の両ホスティングが Terminate を出し、GameLift なら ProcessEnding → Destroy が 1 回ずつ呼ばれる。実プロセス (local) でイベントを立てると .rep が正しく閉じて exit 0。存在しないイベント名は exit 1。[S]
3. GameLift ホスティングでもコンソールの Ctrl+Break が Terminate になる (偽 SDK で ProcessEnding が呼ばれる)。[S]
4. `--log-dir` で `server-<pid>.log` ができ、ProcessReady の logPaths にそのパスが入る。stdout をファイルへ向けた実プロセスで、終了を待たずにログの行が読める (行単位)。[S]
5. Server.exe の既存テストと Editor の `servernet` / `session` 系が通り、ABI / snapshot / proto / replay の版が変わっていない。[S][E][R]

サーバ版 (sub-02)
6. 偽の書き出しフォルダから組むと 4.1.2 の表のとおりの中身になり、入れないものが入らず、`assets\` がバイト一致する。足りない入力は全部挙げて失敗し、出力を中途半端に残さない。`install.bat` の内容が仕様どおり。[E]
7. 組んだ Server.exe の局所起動確認が exit 5 + `ready on UDP port` で成功する (Server.exe が無い環境では SKIP と明示)。[E]
8. **手動チェックポイント**: sub-02.md の CLI 手順で、組んだサーバ版を upload-build → EC2 フリートが ACTIVE → ゲームセッション ACTIVE → 書き出しフォルダの Runtime.exe が参加 → フリート削除・ビルド削除まで通る。[M]

aws CLI 層 (sub-03)
9. 引数の組み立てが空白・引用符・バックスラッシュ・日本語を含む値で `CommandLineToArgvW` と往復一致する。許可文字の検査が不正な入力を拒否する。[E]
10. `cmd.exe` を偽 CLI にした実プロセスで: stdout と stderr が別々に取れる / 終了コードが取れる / 1 MB 超の出力で詰まらない / タイムアウトと取消で子が終わる / 追加環境 (固定 3 つ + 呼び出しごと) が子に届き、親 (エディタ) の環境は変わらない。[E]
11. 4.1.3 の応答の解析が、公式例に沿った JSON の写し (fixture) で正しい値を返し、エラー分類が正しい。`upload-build` の進捗行と Build ID を読める。[E]
12. 規則 22 が、Engine / Runtime / GameLogic / Shared / Server から `Editor/Tools/GameLift/` を include したフィクスチャで ERROR になり、実ツリーでは 0 件。[R]

窓・Anywhere (sub-04)
13. 偽 CLI (プロセス内の差し替え) で、CLI 無し / ログイン切れ / リージョン無し / プロジェクト無しのとき、該当ボタンが無効で理由が出る。[E]
14. 「準備」が、無いものだけを作る順序で CLI を出す (location → fleet → compute)。`custom-` は 1 回だけ付き、`custom-` で始まる入力は拒否される。確認ダイアログで「やめる」と CLI が 1 本も出ない。ID が `editor_settings.json` に往復し、トークンは書かれない。[E]
15. 「サーバ起動」の argv にトークンが無く、子の環境にだけ入る (偽の Server 代わりに `cmd.exe` で環境を確かめてよい)。`ProcessReady sent` の行で「待機中」になる。「停止」で stop イベントが立つ。[E]
16. 「セッションを作って参加」が容量不足の再試行 → ACTIVE 待ち → player session → クライアント起動の順で進み、クライアントの argv が仕様どおり (`--project`、`--net-connect IP:PORT`、`--player-session-id`)。[E]
17. 「片付け」が deregister → delete-fleet → 消えるまで待つ → delete-location の順。[E]
18. Anywhere タブと確認ダイアログのスクショ (日本語・英語) が 4.3 のとおり。[P]
19. 足した UI 文字列が両言語を持ち、規則 (書式指定子・`###`) に違反しない。[R][E]

窓・EC2 (sub-05)
20. 「アップロード」の argv が仕様どおり (`WINDOWS_2022`、`5.6.0`、build-root = server-build)。進捗 % と Build ID を拾い、READY 待ちに入る。[E]
21. 「フリート作成」の JSON が仕様どおり (LaunchPath / Parameters / ポート / ConcurrentExecutions 1 / ON_DEMAND) で、Parameters の事前検査が許可外の文字を拒否する。Status の推移と ERROR 時のイベント表示が出る。[E]
22. EC2 の「参加」が書き出しフォルダの Runtime.exe を `--project` なしで起動する。サーバ版の元 dist と違うフォルダなら警告。[E]
23. 台数 > 0 で「課金中」帯が出て、台数 0 で消える。起動時の確認が保存済み ID があるときだけ 1 回走る。台数変更・削除は確認ダイアログを経る。[E]
24. EC2 タブ (ACTIVE・課金中帯) のスクショ (日本語・英語)。[P]

文書・仕上げ (sub-06)
25. ADR-042 / `docs/gamelift-editor.md` / `docs/gamelift-anywhere.md` (8 章の食い違いを直し、窓への案内) / `engine_spec.md` / `docs/test_checklists.md` が実装と一致。必要な IAM 権限 (EC2 分を含む) を示す。[文書の照合]
26. 版 0.90.4 が `Project.h` / `engine_spec.md` 冒頭 / `tools/collab_fixture.ps1` に揃う。[R + 目視]
27. `Editor.exe --selftest-full`、`Server.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat` が通る (replay は sim 非変更の確認として)。

手動確認 (ユーザー、実 AWS)
28. 窓だけで Anywhere を一巡 (準備 → 起動 → 2 人参加 → 停止で数十秒以内に TERMINATED → 片付け)。M81 の未確定 2 点 (停止での即 TERMINATED、猶予切れの RemovePlayerSession) と参加時の tick 時間 (R-11) も見る。[M]
29. 窓だけで EC2 を一巡 (サーバ版 → アップロード → フリート作成 → 参加 → 台数 0 → 削除)。エディタを再起動して課金中帯が出ることも見る。[M]

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | Server.exe: マネージド EC2 の起動・停止要求の一本化・ログの向け先 | なし | 1〜5 | `M107a: Server.exe にマネージド EC2 (引数なし InitSDK)・停止イベント・ログの向け先を足し、GameLift でも Ctrl+C で ProcessEnding を送る` |
| sub-02 | サーバ版の組み立てと局所起動確認 (+ 手動チェックポイント) | sub-01 | 6〜8 | `M107b: 書き出しフォルダから GameLift 用のサーバ版を組み、局所起動で確かめる` |
| sub-03 | aws CLI を非同期に呼ぶ層と応答の解析 + 規則 22 | なし (sub-01 と並列可) | 9〜12 | `M107c: aws CLI を非同期に呼び JSON を読む層を Editor に足す (check_rules 規則 22)` |
| sub-04 | GameLift 窓: 共通部と Anywhere の手順 | sub-01, sub-03 | 13〜19 | `M107d: GameLift 窓を足し、自分の PC (Anywhere) の準備・起動・参加・片付けをボタンにする` |
| sub-05 | GameLift 窓: EC2 の手順と課金中の表示 | sub-02 (チェックポイント通過), sub-04 | 20〜24 | `M107e: GameLift 窓に AWS 上 (EC2 フリート) のアップロード・作成・参加・停止・削除と課金中の表示を足す` |
| sub-06 | 文書・ADR-042・版 0.90.4 | sub-01〜05 | 25〜27 | `M107f: GameLift 窓の文書と ADR-042 を足し、Anywhere 手順書の停止の記述を直す (版 0.90.4)` |

手動確認 28 / 29 はサブに属さない (司会が最後にユーザーへ依頼する)。

## 7. 未決事項・リスク

### ユーザーに確かめた点 (planner の裁定 → 司会経由で回答済み)

- Q1 (D20): EC2 の実機確認を sub-02 の直後に挟むか。裁定 = 挟む (費用は c5.large 1 台を 1 時間未満)。**回答 (2026-11-27): 裁定どおり。sub-02 の直後に手動チェックポイント**
- Q2 (D7): 参加するクライアントを Runtime.exe にするか。裁定 = Runtime.exe。**回答 (2026-11-27): 裁定どおり。Anywhere = Editor.exe の隣の Runtime.exe + `--project`、EC2 = 書き出しフォルダの Runtime.exe (`--project` なし)**
- Q3 (D5): VC++ ランタイムの入れ方。裁定 = `install.bat` + `vc_redist.x64.exe`。**回答 (2026-11-27): 裁定どおり。両方を同梱**
- Q4 (D16): エディタ起動時の課金中チェック。裁定 = 保存済み ID があるときだけ読み取り 1 回 + 窓を開く。**回答 (2026-11-27): 裁定どおり。起動時に 1 回 (保存済みフリート ID があるときだけ) + 窓を開いている間は常時**
- Q5 (D19): 新しい窓にするか。裁定 = 新しい「GameLift」窓。**回答 (2026-11-27): 裁定どおり。新しい GameLift 窓**
- 確定確認: 5. の受け入れ条件で確定してよいか。**回答 (2026-11-27): 受け入れ条件 1〜29 のまま確定**

### リスク

- R1: D5 の起動可否は受け入れ 8 (手動) まで未確認。外れたら sub-02 を差し戻す (app-local への切替、同梱漏れの追加)。
- R2: 新規フリートが作成直後に 1 台立つ (= 課金開始) のは**記憶による**。受け入れ 8 で `describe-fleet-capacity` を見て確かめ、違えば sub-05 の確認文言を直す。
- R3: `aws login` は CLI の版に依存する (この PC は `aws-cli/2.37.8`)。古い CLI で `login` が無ければ、窓は「`aws configure sso` 等で資格情報を用意する」案内に落とす。
- R4: EC2 のインスタンスで Server.exe の作業ディレクトリが `C:\game` 以外でも、assets は exe から探す (`FindAssetsRoot`) ので動くはずだが未確認 (受け入れ 8)。
- R5: サーバに C# レーンが無いのは M81 からの制限。C# スクリプトを使うゲームは EC2 / Anywhere とも sim が食い違いうる (文書に明記)。
- R6: `--exit-when-empty` は「予約中の再接続」を待たずに終わる (`ServerLoop.cpp:354`)。EC2 では 1 セッション = 1 プロセスで回すための選択で、開発用途として許容。
- R7: 料金・無料枠は確認していない (design-draft 3 章のとおり期間・東京の Windows 単価は未確認)。窓に単価を出さない理由。

### 出典 (planner が 2026-11-27 に取得)

- C++ server SDK 5.x Actions (InitSDK の 2 形・ProcessEnding・Destroy): https://docs.aws.amazon.com/gameliftservers/latest/developerguide/integration-server-sdk5-cpp-actions.html
- Add a build install script (install.bat、`C:\game`、vcredist の例、冪等であること): https://docs.aws.amazon.com/gameliftservers/latest/developerguide/gamelift-build-cli-uploading-install.html
- upload-build (オプションと出力例 `Build ID:`): https://docs.aws.amazon.com/cli/latest/reference/gamelift/upload-build.html
- create-fleet (必須パラメータ、LaunchPath は `C:\game`、Parameters の許可パターンと長さ、`--cli-input-json`、ON_DEMAND が既定): https://docs.aws.amazon.com/cli/latest/reference/gamelift/create-fleet.html
- Debug fleet issues (InitSDK / ProcessReady の 5 分、別プロセスが SDK を呼ぶと時間切れ、SERVER_PROCESS_INVALID_PATH): https://docs.aws.amazon.com/gameliftservers/latest/developerguide/fleets-creating-debug.html (検索結果の要約で確認。本文は未取得)
- design-draft 3 章の出典 (fleets-creating / pricing / FAQ) は司会の取得による
- Microsoft Learn「Redistribute Visual C++ Files」(再頒布パッケージを推奨、app-local は servicing の理由で非推奨、`/install /quiet /norestart /log`、より新しい版が入っていると失敗コードを返す): https://learn.microsoft.com/en-us/cpp/windows/redistributing-visual-cpp-files

## 8. 変更履歴

(確定後の変更のみ)

- 2026-11-27 (出所: ユーザー、司会経由): Q1〜Q5 と受け入れ条件 1〜29 が裁定どおり承認され、状態を「仮確定」から「確定」にした。仕様の中身は変更なし。
