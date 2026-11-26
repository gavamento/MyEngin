# sub-03: aws CLI を非同期に呼ぶ層と応答の解析 + 規則 22

- 依存: なし (sub-01 / sub-02 と並列にできる)
- 状態: 未着手
- 往復: 0

## やること

spec 4.1.3 をすべて。窓 (UI) はまだ作らない。

1. **子プロセスの実行器** (aws.exe 専用にせず「CLI を 1 本走らせて出力をメモリへ取る」もの。Server.exe の起動にも使えるよう、
   環境の追加と作業ディレクトリを受ける):
   - 入力: exe パス、argv の配列 (UTF-16)、追加環境 (名前 = 値の列)、作業ディレクトリ、タイムアウト (0 = なし)、秘密フラグ。
   - argv → コマンドライン: Windows の引用規則 (`CommandLineToArgvW` が元の配列へ戻す形。空白・`"`・末尾の `\` の連なり)。
   - stdout / stderr を別々の匿名パイプでメモリへ。**子が大量に書いても詰まらない**ように読み続ける (読み取りスレッドでも、
     毎フレームの非ブロッキング読み取りでもよい。どちらにしたか・理由を技術解説に書く)。stdin は NUL。`CREATE_NO_WINDOW`。
   - 環境ブロック: 親の環境 + 固定 3 つ (`PYTHONIOENCODING=utf-8` / `AWS_CLI_FILE_ENCODING=UTF-8` / `AWS_PAGER=` 空) + 呼び出しごと。
     **エディタ自身の環境変数は書き換えない** (`SetEnvironmentVariableW` を使わない)。同名は後勝ち。
   - ポーリング (UI スレッドから毎フレーム呼んでも待たない)、取消 (子を終了)、タイムアウト、終了コード。
   - 秘密フラグの呼び出しは、コマンドの記録・ログに stdout を出さず、結果を読んだら呼び出し側が消せる形 (保持期間を短く)。
   - 既存 `src/Editor/App/ChildProcess.{h,cpp}` は stdout をファイルへ書く用途で使われている (BuildSettings / Perf 等)。
     **既存の呼び出し側は変えない**。新しい実行器を別に作るか、ChildProcess に足すかは coder 判断 (足すなら既存の振る舞いを変えないこと)。
2. **aws CLI の呼び出しの組み立て** (純関数): 操作ごとに argv を作る (`gamelift <op> ... --output json --no-cli-pager --region <r>`)。
   利用者が入れる値 (ロケーション接尾辞・フリート名・コンピュート名・ビルド名/版・インスタンスタイプ) の許可文字検査
   (英数と `-_.`、長さ上限。IP はドット区切りの 4 数、ポートは 1〜65535、人数は 1〜4)。
   aws.exe の探索 (設定の明示パス → `SearchPathW` → `C:\Program Files\Amazon\AWSCLIV2\aws.exe`)。
3. **応答の解析** (純関数、nlohmann/json): spec 4.1.3 の一覧 + エラー分類。少なくとも:
   `sts get-caller-identity` / `configure get region` (テキスト 1 行) / `--version` (テキスト) / `list-locations` / `create-location` /
   `create-fleet` / `describe-fleet-attributes` / `describe-fleet-capacity` / `describe-fleet-events` / `register-compute` / `describe-compute` /
   `get-compute-auth-token` / `create-game-session` / `describe-game-sessions` / `create-player-session` / `describe-build` /
   `upload-build` (テキスト: 進捗 % と `Build ID:`)。
   エラー分類は stderr の `An error occurred (<Code>) when calling the <Op> operation: <msg>` の Code を読む
   (資格情報切れ = `ExpiredToken` / `ExpiredTokenException` / `UnrecognizedClientException` 等、`NotFoundException`、
   `FleetCapacityExceededException`、`ConflictException`、`InvalidRequestException`、その他)。資格情報が無いときの
   `Unable to locate credentials` 等の非定型文も「ログインが要る」へ寄せる。
4. **規則 22** を `tools/check_rules.ps1` に足す: Engine / Runtime / GameLogic / Shared / Server から `Editor/Tools/GameLift/` を
   include したら ERROR (規則 19〜21 と同じ作り。`-SourceRoot` のフィクスチャで自己検査できること)。
5. 自己テスト: 引数の往復 (`CommandLineToArgvW`)、許可文字、解析 (fixture は公式ドキュメントの応答例の形に沿って自作)、
   `cmd.exe` を偽 CLI にした実プロセス (stdout/stderr の分離、終了コード、1 MB 超、タイムアウト、取消、環境の到達と親の不変)。

## やらないこと (このサブでは)

- 窓・状態機械 (sub-04 / 05)
- 実 AWS への呼び出し (自己テストで aws.exe を起動しない)

## 触る場所 (planner の見立て)

- 新規 `src/Editor/Tools/GameLift/` (例: `CliProcess.{h,cpp}`、`AwsCliCommands.{h,cpp}`、`AwsCliParse.{h,cpp}`、`GameLiftCliSelfTest.{h,cpp}`)
- `src/Editor/SelfTest/SelfTestSuites.cpp` (Fast 層に登録。実プロセスを起こす試験が重ければ分ける)
- `tools/check_rules.ps1` (規則 22) と、その自己検査のフィクスチャ (規則 21 の作りを参照: `tools/check_rules.ps1:1514-1525`)
- 参考: `src/Editor/Tools/VisualStudioLauncher.cpp:257` (CreatePipe の使い方)、`src/Editor/Tools/Terminal/TerminalVsEnvWin32.cpp:132`
- ファイル追加後は `tools\gen_project_files.ps1`
- ★規則 13-c に触れない (ファイル名・文字列に `aws/gamelift` `aws\gamelift` `GameLiftServerAPI` を書かない)

## 受け入れ条件 (このサブ)

spec 5. の 9〜12。

1. 空白・`"`・末尾 `\`・`\"`・日本語を含む argv が `CommandLineToArgvW` で元に戻る。不正な利用者入力 (空白・`/`・`custom-` 始まりの接尾辞 等) が拒否される。[E]
2. `cmd.exe` の偽 CLI で、stdout / stderr の分離、終了コード (例 3)、1 MB 超の出力の完走、タイムアウトでの終了、取消での終了、
   追加環境の到達 (`cmd /c echo %NAME%` 相当) と、呼び出し後にエディタの `GetEnvironmentVariableW` が変わっていないこと。[E]
3. 各解析が fixture で正しい値を返す。エラー分類が Code ごとに正しい。`upload-build` の進捗行から % を、最終行から Build ID を読む。[E]
4. 規則 22: フィクスチャで ERROR、実ツリーで 0 件。[R]

## 検証コマンド

- `tools\gen_project_files.ps1` → ビルド
- `MYE_SELFTEST_ONLY=<新スイート>` で `bin\x64\Debug\Editor.exe --selftest`
- `tools\check_rules.ps1` (と規則 22 のフィクスチャ自己検査)

## 実装メモ (coder が追記)

## フィードバック履歴
