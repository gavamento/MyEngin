# sub-03: aws CLI を非同期に呼ぶ層と応答の解析 + 規則 22

- 依存: なし (sub-01 / sub-02 と並列にできる)
- 状態: OK (commit d34384a8)
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

### round 1 (SELF_EVAL の写し)

実装:
- `src/Editor/Tools/GameLift/CliProcess.{h,cpp}`: 子プロセス実行器 `CliProcess` (Start / Poll / Cancel / StdoutText / StderrText / ClearOutput)。純関数 `BuildCommandLine` (CommandLineToArgvW 往復の引用) / `BuildEnvironmentBlock` / `FixedCliEnvironment` / `ReadParentEnvironment`。ChildProcess.{h,cpp} は触らず別に新設。
- `AwsCliCommands.{h,cpp}`: 利用者入力の検査 (`IsValid*`) と、操作ごとの argv 組み立て (`GameLift*Call` 等。不正入力は false で out 不変)、`AwsExecutableCandidates` / `FindAwsExecutable`。
- `AwsCliParse.{h,cpp}`: `ClassifyAwsFailure` (stderr の `An error occurred (<Code>)` を読む)、テキスト解析 (version / 1 行 / upload-build の % と Build ID)、JSON 解析 17 種。
- `GameLiftCliSelfTest.{h,cpp}`: スイート `gameliftcli` (純関数 約 100 項目) と `gameliftcliprocess` (cmd.exe 偽 CLI の実プロセス)。どちらも Fast。`SelfTestSuites.cpp` の include と表の末尾へ登録。
- `tools/check_rules.ps1`: 規則 22。`tools/check_rules_selftest.ps1`: 規則 22 のフィクスチャ (違反 3 件・Editor 側と別名と コメントは通す)。

技術解説:
- 技術: 読み取りスレッド方式 — stdout と stderr それぞれに専用 `std::thread` が `ReadFile` をブロッキングで回し、mutex 保護の `std::string` へ追記する。匿名パイプは overlapped I/O が使えず、毎フレーム `PeekNamedPipe` で読む方式は 1 フレームに読める量 (パイプ 64 KB) で子が待たされ、1 MB 超で遅くなる。スレッドなら子は常に書き続けられ、UI の Poll は状態を見るだけで待たない。2 本を順に読む方式は「stderr が先に大量」で必ず詰まる (テストで再現する並びを入れた)。
- 技術: Job Object (KILL_ON_JOB_CLOSE) — 子を `CREATE_SUSPENDED` で作り、Job へ入れてから `ResumeThread`。取消・タイムアウト・破棄で `TerminateJobObject` し、孫 (cmd の下の ping など) も止める。孫が stderr のパイプ書き込み端を握ったままだと読み取りが EOF にならず永久に終わらないため。それでも終わらない場合の保険として、子の終了後 2 秒で Job ごと止める。
- 技術: `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` — 継承するハンドルを NUL・stdout 書き込み端・stderr 書き込み端の 3 本に限る。`bInheritHandles=TRUE` だけだと、別スレッドが同時に起こす別の子へこのパイプの書き込み端が漏れ、読み取りが EOF にならなくなる。
- 技術: Windows の引用規則 (「Everyone quotes command line arguments the wrong way」の手順) — 空・空白・引用符を含む引数だけ `"..."` で囲み、`"` の直前のバックスラッシュの連なりは 2n+1 本、閉じ引用符の直前は 2n 本。先頭の exe 名は CommandLineToArgvW が別規則で読むので、空白があるときだけ素直に囲む。往復をテストで確認 (空・空白・`\"`・末尾 `\`・日本語)。
- 技術: 環境ブロック — `GetEnvironmentStringsW` で親を読み、固定 3 つ → 呼び出しごとの順に重ねる。同名判定は `CompareStringOrdinal(ignoreCase)`、結果は名前順に並べて二重 NUL で閉じる。`CreateProcessW` へ `CREATE_UNICODE_ENVIRONMENT` で渡すので `SetEnvironmentVariableW` は不要 = エディタ自身の環境は変わらない (テストで前後比較)。名前が `=` で始まる項目 (`=C:`) は検索を 2 文字目から始めて壊さない。
- 技術: 純関数 + 構造体の解析 — nlohmann/json を `parse(text, nullptr, false)` (例外なし) で読み、型違いは既定値へ落とす小さなヘルパ (`Str` / `Int`) で取り出す。必須の ID が無い応答は error 付きで false。キー自体が無い配列は空として許す (CLI は空結果でキーを省くことがある)。
- コード: `CliProcess::Start` — 入力は `CliRunSpec` (exe・argv・追加環境・作業 dir・タイムアウト・秘密)。処理: 環境ブロックとコマンドラインを作る → 匿名パイプ 2 本 (読み取り端は継承オフ) + NUL を開く → ハンドルリスト属性付きの `STARTUPINFOEXW` で `CREATE_NO_WINDOW | CREATE_SUSPENDED` 起動 → Job へ入れて Resume → 親側の書き込み端を閉じる → 読み取りスレッド 2 本を起こす。秘密のときは環境ブロックとコマンドラインを `SecureZeroMemory` する。UI スレッドから呼んでも待たない。
  ```cpp
  HANDLE inheritList[3] = { nulIn.handle, outWriteEnd.handle, errWriteEnd.handle };
  ...
  CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE,
                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, ...);
  AssignProcessToJobObject(job, info.hProcess);  // 失敗なら Job なしで TerminateProcess にフォールバック
  ResumeThread(info.hThread);
  ```
- コード: `CliProcess::Poll` — 呼び出し元は UI の毎フレーム (sub-04 の窓)。`WaitForSingleObject(process, 0)` で終了を見る → 未終了で `timeoutMs` 超なら `timedOut_` を立てて `Terminate` → 終了済みで読み取りスレッド 2 本が EOF に達していれば `Finalize` (join・ハンドル解放・状態確定: 取消 > タイムアウト > 終了)。終了後 2 秒たってもパイプが閉じなければ Job ごと止める。`Running` 以外が返ったら出力は確定。状態は `state_` だけが持ち、スレッドが書くのは出力バッファと完了カウンタ (atomic) のみ。
- コード: `CliProcess::Cancel` — 実行中なら `TerminateJobObject`。自然終了と競ったとき (既に終了済み) は取消扱いにせず終了コードを正本にする。AWS 側へ受け付け済みの操作は取り消されない (窓の文言は sub-04)。デストラクタは実行中なら Job ごと止めてから join する。
- コード: `ClearOutput` — 出力 2 本を `SecureZeroMemory` してから clear。読み取りスレッドのスタックバッファも終了時にゼロで埋める。秘密フラグの呼び出しは、`CliProcess` 自身が何もログに出さない (ラベルは `AwsCall::label` = 値を含まない `gamelift get-compute-auth-token`)。
- コード: `AwsCliCommands.cpp` — `MakeGameLiftCall(op, opArgs, region, secret)` が `gamelift <op> <opArgs...> --output json --no-cli-pager --region <r>` を作る。各ビルダーは先に全入力を検査し、1 つでも不正なら false で `out` を触らない。検査は「先頭が英数、残りは英数と `-_.`、長さ上限」の `IsSimpleToken` 一本 (先頭英数にしたのは `-x` が CLI のオプションに化けるのを防ぐため。ID / ARN だけ `:/` を許す `IsValidResourceId`)。検査後の値は 1 要素の argv になるので、BuildCommandLine の引用で割れない (空白や `"` はそもそも通らない)。
  ```cpp
  AwsCall MakeGameLiftCall(const wchar_t* op, std::vector<std::wstring> opArgs, const std::string& region, bool secret = false)
  {
      call.args = { L"gamelift", op };  // + opArgs + --output json --no-cli-pager --region <r>
      call.label = std::wstring(L"gamelift ") + op;  // 値を含まない表示名
  ```
- コード: `ClassifyAwsFailure(exitCode, stderr)` — `An error occurred (` から `)` まで = Code、` when calling the <Op> operation` = 操作名、`(reached max retries: 4)` があれば飛ばして `:` 以降の先頭行 = message。Code → 種別: `ExpiredToken*` / `UnrecognizedClientException` / `InvalidClientTokenId` 等 = Credentials、`NotFoundException`、`FleetCapacityExceededException`、`ConflictException` (+ `UniqueIdentifierConflictException`)、`InvalidRequestException`、他は Other。型付きでない stderr は `Unable to locate credentials` / `Error loading SSO Token` / `Your session has expired` / `aws login` を含めば Credentials、`You must specify a region` は NoRegion、それ以外は先頭の非空行を message にして Other。
- コード: `ParseUploadBuildOutput` — `\r` と `\n` の両方で分割 (進捗は `\r` で上書きされる)。`Build ID:` 行なら値を読み、それ以外は行内の最後の `%` から手前の数字と `.` を `std::from_chars` で double にして percent。途中の stdout (Build ID 未出力) でも呼べるので、sub-05 は実行中に毎フレーム `StdoutText()` を渡してよい。
- コード: 規則 22 — `check_rules.ps1` の規則 19〜21 と同じ作り。Engine / Runtime / GameLogic / Shared / Server 配下で `#include` の行 (コメント除外) に `Editor/Tools/GameLift/` が出れば ERROR。
- 選ばなかった案: (1) 既存 `ChildProcess` へ足す — stdout を「ファイルへ」書く前提で pid も返さず、BuildSettings / Perf の既存呼び出しを巻き込むので別に作った。(2) 毎フレームの `PeekNamedPipe` 非ブロッキング読み取り — 上記のとおり 1 MB 超で遅く、順序依存の詰まりを自前で管理することになる。(3) 追加環境を `SetEnvironmentVariableW` で入れて子を起こす — エディタ全体 (他スレッド・他の子) の環境が変わる。(4) `cmd /c aws ... > file` でファイルへ出す — 秘密がディスクへ残り、stderr の分離も面倒。
- 前提・落とし穴: `CliProcess` はスレッドが `this` を握るのでコピー・移動不可 (delete 済み)。1 インスタンスで同時に走らせるのは 1 本 (spec 4.1.3 の「窓ごとに 1 本」)。`StdoutText()` / `StderrText()` は実行中も呼べるがコピーを返す (大きい出力を毎フレーム呼ぶと高い = 進捗だけ見るときは upload-build 程度に留める)。出力は 1 本あたり 64 MiB で打ち切り (`Truncated()`)。`Poll` を呼ばないと状態は進まない (終了したプロセスの出力は溜まるが、Finalize されずハンドルが残る)。Server.exe を起こす用途 (sub-04) で使うなら stdout は読み取りスレッドが溜め続けるので、長時間動くサーバにはログをファイルへ出す方を使い、`CliProcess` は短命な CLI 用にする。`aws --version` は v2 が stdout、v1 は stderr に出すので、呼び出し側が結合して `ParseAwsCliVersion` へ渡す。cmd.exe の偽 CLI は `/d /s /c "<command>"` の形で組むこと (BuildCommandLine が全体を 1 引数として囲み、`/s` が外側の引用符を剥がす)。

仕様との差分:
- [追加] `AwsErrorKind::NoRegion` を足した — 「リージョン未設定」は sub-04 で別理由として出すので、非定型 stderr (`You must specify a region`) をその他と区別した。
- [追加] 利用者入力の検査は「先頭は英数」を足した (spec は英数と `-_.`) — `-x` のような値が CLI のオプションとして解釈されるのを防ぐため。
- [追加] `register-compute` に `--location custom-<suffix>` を付けた (spec 4.1.5 の記述は `--ip-address` のみ) — API 上、Anywhere フリートへの登録で Location は必須のため (CLI リファレンスの記述に基づく。実 AWS では未確認)。
- [追加] `AwsCall::label` (値を含まない表示名) を足した — sub-04 の「実行中の CLI (コマンド名 + 経過秒)」と秘密のログ除外のため。
- [追加] 子プロセスは Job Object (KILL_ON_JOB_CLOSE) に入れ、孫まで止める。sub-03 の指定にはないが、取消・タイムアウトの正しさに必要 (テストで孫が残ると猶予 2 秒を待つ形になるのを検出している)。
- [追加] ビルダーは EC2 の `create-fleet` を `--cli-input-json file://<path>` の形だけ用意した (JSON の中身は sub-05)。
- [未実装] `aws login` を開く呼び出し (新しいコンソール窓) は sub-04 (対話コマンドは別扱い)。aws.exe の実 AWS への呼び出しは一切していない。

検証:
- `pwsh tools\gen_project_files.ps1` → build\Editor.vcxproj / .filters 更新 (4 ファイル + テスト 2 ファイル)
- MSBuild `build\Editor.vcxproj` Debug|x64 → 成功 (最初に出た NarrowAscii の C4244 警告は修正して再ビルド、警告なし)
- `MYE_SELFTEST_ONLY=gameliftcli,gameliftcliprocess` `Editor.exe --selftest` → 2 suites, 0 failed (約 1 秒)。PASS 124 項目 / FAIL 0。cmd.exe の偽 CLI で stdout/stderr 分離・終了コード 3・30000 行 (1.4 MB) の stdout・stderr 先行の両方大量・Poll の非ブロッキング・取消 (< 1.5 秒 = 孫の猶予待ちにならない)・タイムアウト (400ms 付近、1.9 秒未満)・環境の到達と親の不変・秘密の消去・起動失敗・再利用・実行中の二重 Start 拒否・実行中のデストラクタ。
- `Editor.exe --selftest` (fast 層全体) → 121 suites, 1 failed = `modalsynth` のみ (基点からの既存の別件)。150 秒。
- `pwsh tools\check_rules.ps1` → 0 error(s), 65 warning(s) (警告は既存の規則 17 のみ)。
- `pwsh tools\check_rules_selftest.ps1` → rule 22 PASS (違反 3 件を検出、Editor 側・別名 `GameLiftLike.h`・コメントは通す)。
- 未実行: Release ビルドと replay_verify (Editor 層のみの追加で sim に触れないため)。実 aws.exe の起動 (禁止)。

自己採点 (1-5):
  仕様適合: 4 — spec 4.1.3 の一覧をすべて実装。aws.exe の実機応答との突き合わせは fixture (公式の形に沿った自作) のみで、register-compute の `--location` など実 AWS 未確認の点がある。
  正しさ: 4 — 124 項目のテストと反証 (stderr 先行の大量出力、孫の残留、環境の不変、実行中の破棄) を実プロセスで確認。upload-build の出力形式と各応答の細部は実機未確認。
  コード品質: 4 — 責務を 3 ファイル (実行器 / 組み立て / 解析) に分け、純関数と副作用を分離。ビルダーの繰り返しが多い (表駆動にもできる) 点が残る。
  テスト: 4 — 純関数と実プロセスを分け、各受け入れ条件 1〜3 を直接検証。規則 22 は selftest のフィクスチャで確認。実 aws.exe はない。

不安・質問:
- `register-compute --location` の要否は、実 AWS で確かめていない (CLI リファレンスでは Anywhere フリートで必須)。sub-04 / 手動確認で外れたら 1 行直す。
- `upload-build` の進捗の実際の文字列 (`\r` のみで更新されるか、行頭の文言) は fixture の推測 (spec の例 `Uploading ... (21.45%)` に準拠)。% の拾い方は「行内の最後の `NN.NN%`」なので文言が違っても動くはずだが、手動チェックポイントで実出力を 1 回見ると確実。
- Server.exe を起こす用途に `CliProcess` を使う (sub-04) と、長時間の stdout を全部メモリへ溜める。ログはファイルへ出す方針 (`--log-dir` / ログファイル) なので問題ない想定だが、sub-04 で `CliProcess` を使うか専用の起動 (出力を捨てる) にするかは sub-04 の判断。

触ったファイル:
- C:\HAL\MyEngin\src\Editor\Tools\GameLift\CliProcess.h / .cpp
- C:\HAL\MyEngin\src\Editor\Tools\GameLift\AwsCliCommands.h / .cpp
- C:\HAL\MyEngin\src\Editor\Tools\GameLift\AwsCliParse.h / .cpp
- C:\HAL\MyEngin\src\Editor\Tools\GameLift\GameLiftCliSelfTest.h / .cpp
- C:\HAL\MyEngin\src\Editor\SelfTest\SelfTestSuites.cpp
- C:\HAL\MyEngin\tools\check_rules.ps1
- C:\HAL\MyEngin\tools\check_rules_selftest.ps1
- C:\HAL\MyEngin\build\Editor.vcxproj / Editor.vcxproj.filters (生成物だがコミット対象)
- C:\HAL\MyEngin\plans\m107-gamelift-editor\sub-03.md (実装メモ)

申し送り:
- sub-04 は `CliProcess` を 1 窓 1 インスタンスで持ち、毎フレーム `Poll()`。終了後 `ClassifyAwsFailure(process.ExitCode(), process.StderrText())` と `Parse*(process.StdoutText())`。秘密 (`AwsCall::secret`) は結果を使ったら `ClearOutput()` と `AwsComputeAuthToken::Wipe()`。
- テスト用の偽 CLI は `CmdSpec` (cmd.exe /d /s /c "...") のやり方が使える (GameLiftCliSelfTest.cpp)。
- 既知の別件: `modalsynth` の 1 項目 FAIL (触らない)。

## フィードバック履歴

- round 1: VERDICT OK (planner, 2026-11-27)。planner が `gameliftcli,gameliftcliprocess` を再実行して exit 0 (PASS 124)、`check_rules.ps1` 0 error、`check_rules_selftest.ps1` の rule 22 PASS を確認した。register-compute の CLI リファレンスで `--location` が Anywhere で必須なことを確かめた (coder の [追加] は正しい)。同じページで compute-name / location の許可文字が `-_.` より狭いと分かったので、spec D14 を改訂し、直しを sub-04 の 9 項へ持ち越した (should)。
