# sub-02: サーバ版の組み立てと局所起動確認 (+ 手動チェックポイント)

- 依存: sub-01 (EC2 の起動引数 `--hosting gamelift-managed` / `--log-dir` をチェックポイントで使う)
- 状態: 未着手
- 往復: 0

## やること

1. spec 4.1.2 の「サーバ版の組み立て」を Editor 層の関数として実装する (窓はまだ作らない)。
   入力 = 書き出しフォルダ `<dist>`、`<exeDir>`、出力先 (既定 `<project>\cache\gamelift\server-build\`)。
   - 表のとおりに入れる / 入れないを守る。`assets\` は**そのまま写す** (manifest を焼き直さない = 書き出しと contentHash が一致)。
   - `install.bat` / `mye_server_build.json` を生成する。
   - `vc_redist.x64.exe` は vswhere (`%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe -latest -property installationPath`)
     で VS を見つけ、`VC\Redist\MSVC\*\vc_redist.x64.exe` の最も新しい版を使う (この PC では `...\18\Community\VC\Redist\MSVC\14.51.36231\` と `v145\` にある)。
   - 足りない入力は**全部挙げて**失敗し、出力先に中途半端な組み立てを残さない (一時フォルダで組んでから入れ替える。BuildSettings の staging と同じ考え方)。
   - Server.exe が Debug 構成なら警告を出して続ける。
2. 局所起動確認: 組んだフォルダの Server.exe を `--hosting local --port <空き> --server-timeout 3` で起動し、exit 5 と
   ログの `ready on UDP port` を確かめる関数。作業ディレクトリは組んだフォルダ。UI スレッドを止めない形 (開始 / ポーリング / 結果) にする
   (sub-05 の窓がそのまま使う)。
3. 手動チェックポイント用の入口: `Editor.exe --project <proj> --gamelift-server-build <dist>` (窓を出さずに 1 と 2 を行い、
   結果を 1 行出して exit 0 / 1)。既存の Editor の非対話 CLI (`--selftest` 等) と同じ置き方に合わせる。
4. 自己テスト (新スイート。名前は coder が既存の命名に合わせる。実プロセスを起こす 2 は Full 層でよい)。

## やらないこと (このサブでは)

- GameLift 窓・aws CLI の呼び出し (sub-03〜05)
- BuildSettings の書き出し処理の変更 (配布物に Server.exe を入れない = D21)

## 触る場所 (planner の見立て)

- 新規 `src/Editor/Tools/GameLift/ServerPackage.{h,cpp}` (名前は coder 判断。フォルダは `Editor/Tools/GameLift/` に固定 = 規則 22 の対象)
- 新規 自己テスト `src/Editor/Tools/GameLift/*SelfTest.{h,cpp}` と `src/Editor/SelfTest/SelfTestSuites.cpp` への登録
- `src/Editor/App/EditorMain.cpp` (CLI の入口)
- 参考: `src/Editor/Windows/Project/BuildSettingsWindow.cpp:88-344` (StageCopy の copy の仕方、staging → 公開の入れ替え)、
  `src/Engine/Engine/Session/Provenance.cpp` (`ReadContentManifest` で manifest の hash を読む)、`HashFileBytes`
- ファイル追加後は `tools\gen_project_files.ps1`
- ★規則 13-c (`aws[\\/]gamelift` 等の文字列は src\Server 以外で ERROR、大文字小文字無視) に触れない命名にする

## 受け入れ条件 (このサブ)

spec 5. の 6〜8。

1. 偽の書き出しフォルダ (temp に Runtime.exe / GameLogic.dll / assets\ + manifest / cache\cooked\ / dotnet\ / nethost.dll / EOS DLL のダミー) と
   偽の exeDir から組むと、入れるものだけが入り、`assets\` の全ファイルがバイト一致し、入れないものが無い。`install.bat` の中身が spec どおり。[E]
2. 足りない入力 (manifest 無し・Server.exe 無し・vc_redist の探索先なし 等) で、足りないものが全部メッセージに挙がり、出力先が変わらない。[E]
3. 実際の Server.exe (exeDir にあれば) と実際の小さな assets で組んで局所起動確認が成功する。無ければ SKIP をログに明示。[E Full]
4. **手動チェックポイント (ユーザー)**: 下の手順で EC2 まで通る。[M]

## 検証コマンド

- `tools\gen_project_files.ps1` → ビルド (Debug / Release)
- `MYE_SELFTEST_ONLY=<新スイート>` で `bin\x64\Debug\Editor.exe --selftest-full`
- `tools\check_rules.ps1`
- 手で: `bin\x64\Release\Editor.exe --project <proj> --gamelift-server-build <dist>` → exit 0、`<proj>\cache\gamelift\server-build\` の中身を目視

## 手動チェックポイント (司会がユーザーに依頼する。費用: c5.large Windows 1 台 × 1 時間未満)

D5 (まっさらな Windows Server 2022 で起動するか) と R2 (作成直後に 1 台立つか)・R4 (作業ディレクトリ) を確かめる。
コマンドは PowerShell で **1 行ずつ**貼る (折り返さない)。`<...>` は置き換える。リージョンは例として東京。

1. Release でソリューションをビルドし、Release のエディタでプロジェクトを開いて BuildSettings から書き出す (出力 = `<dist>`)。
2. サーバ版 (Editor.exe は GUI なので結果の 1 行は次の形で受ける。1 行のまま貼る):
   `$o = "$env:TEMP\mye_sb.txt"; (Start-Process -FilePath bin\x64\Release\Editor.exe -ArgumentList '--project "<proj>" --gamelift-server-build "<dist>"' -Wait -PassThru -NoNewWindow -RedirectStandardOutput $o).ExitCode; Get-Content $o`
   → exit 0 と `OK: ... local start check passed` を確かめる。
   ★Defender が Editor.exe を検疫したら (実装中に 1 回 `Behavior:Win32/DefenseEvasion.A!ml` が出た。再現条件は不明)、検出の記録 (Windows セキュリティの保護の履歴) を残して司会へ伝え、ビルドし直してから再実行する
3. `$r = "ap-northeast-1"`
4. アップロード: `aws gamelift upload-build --name mye-smoke --build-version 1 --build-root <proj>\cache\gamelift\server-build --operating-system WINDOWS_2022 --server-sdk-version 5.6.0 --region $r`
   → 最後の行の `Build ID: build-...` を `$b = "build-..."` に入れる
5. `aws gamelift describe-build --build-id $b --region $r --query Build.Status` が `READY` になるまで待つ
6. 次の内容を `<proj>\cache\gamelift\smoke-fleet.json` に保存する (BuildId を書き換える):
   ```json
   {"Name":"mye-smoke","BuildId":"build-...","ComputeType":"EC2","EC2InstanceType":"c5.large","FleetType":"ON_DEMAND",
    "EC2InboundPermissions":[{"FromPort":7777,"ToPort":7777,"IpRange":"0.0.0.0/0","Protocol":"UDP"}],
    "RuntimeConfiguration":{"ServerProcesses":[{"LaunchPath":"C:\\game\\Server.exe","Parameters":"--hosting gamelift-managed --port 7777 --max-players 4 --exit-when-empty --log-dir C:\\game\\logs","ConcurrentExecutions":1}]}}
   ```
7. **課金開始**: `aws gamelift create-fleet --cli-input-json file://<proj>/cache/gamelift/smoke-fleet.json --region $r --query FleetAttributes.FleetId --output text` → `$f = "fleet-..."`
8. 状態: `aws gamelift describe-fleet-attributes --fleet-ids $f --region $r --query "FleetAttributes[0].Status"` (ACTIVE まで数分〜数十分)。
   台数 (R2): `aws gamelift describe-fleet-capacity --fleet-ids $f --region $r --query "FleetCapacity[0].InstanceCounts"`
   ERROR になったら: `aws gamelift describe-fleet-events --fleet-id $f --region $r --limit 10` の出力を保存して止める (→ 片付けへ)
9. セッション: `$gs = aws gamelift create-game-session --fleet-id $f --maximum-player-session-count 4 --region $r --query GameSession.GameSessionId --output text`
10. `aws gamelift describe-game-sessions --game-session-id $gs --region $r --query "GameSessions[0].[Status,IpAddress,Port]"` が ACTIVE になるまで待つ
11. 参加 (1 行のまま): `$p = (aws gamelift create-player-session --game-session-id $gs --player-id p1 --region $r --output json | ConvertFrom-Json).PlayerSession; & "<dist>\Runtime.exe" --net-connect "$($p.IpAddress):$($p.Port)" --player-session-id $p.PlayerSessionId`
12. Runtime の窓で動くことを見て閉じる (`--exit-when-empty` でサーバは 2 秒後に終わり、GameLift が次のプロセスを立てる)。
13. ログ: `aws gamelift get-game-session-log-url --game-session-id $gs --region $r` (URL から zip を取れる。`server-<pid>.log` が入っているはず)
14. **片付け (必ず)**: `aws gamelift delete-fleet --fleet-id $f --region $r` → `aws gamelift describe-fleet-attributes --fleet-ids $f --region $r` が空になったら `aws gamelift delete-build --build-id $b --region $r`

追加で見てほしいもの (install.bat が vc_redist の完了を待てているか): フリートが ACTIVE になった場合でも ERROR の場合でも、
`describe-fleet-events` に install script の失敗が出ていないこと。RDP は使わない (後回し)。Server.exe が起動した = VCRUNTIME140 が入った、と読む。

結果として司会に返してほしいもの: 8 の最終状態 (ACTIVE / ERROR とイベント)、作成直後の InstanceCounts、11 の成否 (Runtime のログの Reject 理由があれば)、13 のログの有無。
ERROR の場合の切り分け: イベントが `SERVER_PROCESS_CRASHED` / `SERVER_PROCESS_TERMINATED_UNHEALTHY` なら VC++ ランタイム等の依存、
`SERVER_PROCESS_INVALID_PATH` なら LaunchPath、`SERVER_PROCESS_SDK_INITIALIZATION_TIMEOUT` / `SERVER_PROCESS_PROCESS_READY_TIMEOUT` なら InitSDK の呼び方か起動の遅さ。

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
実装:
  - src/Editor/Tools/GameLift/ServerPackage.{h,cpp}:BuildServerPackage — 4.1.2 の組み立て。入力の点検 (足りないものを全部 problems へ) → 一時フォルダ `<out>.staging.<pid>.<n>` へコピー → 旧を `.old` へ退避して新を rename → 旧を削除。失敗時は出力を触らない
  - 同 MakeServerInstallBat / FindVcRedistIn / FindVsInstallDirViaVswhere / IsDebugBuildDir — install.bat の本文、vc_redist の探索 (数値の版を優先、無ければ v145 など)、vswhere の同期実行 (最大 20 秒)、Debug 構成の判定 (exeDir のフォルダ名)
  - 同 ServerLocalCheck (Start / Poll / Cancel) — 局所起動確認。空き UDP ポートを bind(0) で取り、既存の StartChildProcess で `Server.exe --hosting local --port N --server-timeout 3` を作業ディレクトリ = 組んだフォルダで起動。毎フレーム Poll が終了を見て exit 5 + ログの `ready on UDP port` で Passed。終わったら子が作った cache\ / save_server\ / .meta を消して組み立て直後の中身へ戻す
  - 同 RunServerBuildCli と src/Editor/App/EditorMain.cpp — `--gamelift-server-build <dist>` (`--project` 必須)。連鎖の手前で拾い、`--dump-index` と同じ早期 return 群に置いた。結果は stdout に 1 行、詳細は stderr、exit 0 / 1
  - src/Editor/Tools/GameLift/ServerPackageSelfTest.{h,cpp} + src/Editor/SelfTest/SelfTestSuites.cpp — 新スイート `serverpackage` (Fast、偽の書き出し・偽の exeDir・偽の VS) と `serverpackagelocal` (Full、実プロセス)
技術解説:
  - 技術: staging → rename 入れ替え (BuildSettingsWindow::StageCopy と同じ考え方) — 一時フォルダで組み切ってから `rename(out, out.old)` → `rename(staging, out)`。どちらかの rename が失敗したら元へ戻す。同一ボリューム内の rename なのでコピー中に利用者が見る出力先は常に「旧」か「新」のどちらか。却下: 出力先へ直接コピー (途中失敗で壊れた組み立てが残る)
  - 技術: 入れ替えの安全側 — 出力先は「無い / 空 / `mye_server_build.json` がある (前回の組み立て)」の 3 つだけ置き換える。引数を間違えて別のフォルダを渡しても消さない。書き出しフォルダと出力先の包含関係 (どちら向きも) も拒否 (出力先が親だと dist ごと消える)
  - 技術: 世代の選び方 — vc_redist は `VC\Redist\MSVC\<版>\` の版を「ドット区切りの数値の配列」として辞書順でなく数値で比べる (14.9.1 より 14.51.36231 が新しい。文字列比較だと逆になる)。v145 のような数値でない名前は数値の版が 1 つも無いときの代替。この PC には 14.44.35112 / 14.51.36231 / v145 があり、実測で 14.51.36231 が選ばれた
  - 技術: 局所起動確認のあと始末 — Server.exe は作業ディレクトリ (= 組んだフォルダ) に `cache\hot_server\...` (GameLogic.dll のシャドウコピー)、`save_server\` を作る (実測)。そのまま upload-build すると混ざるので、起動前に全パスのスナップショット (小文字の相対パス・整列済み) を取り、終了後に「無かったものだけ」を remove_all する。却下: 確認用に一時フォルダへ丸ごとコピー (441 MB の実例で時間と容量が無駄)
  - 技術: 非同期の形 — Start (CreateProcess して即戻る) / Poll (WaitForSingleObject(0) 相当 = 既存の PollChildProcess) / 結果 (State + Summary)。60 秒のハードタイムアウトで子を TerminateProcess。sub-05 の窓は毎フレーム Poll を呼ぶだけで使える。BuildServerPackage 自体は同期 (assets のコピー) なので、窓から使うときは呼び出し側が作業スレッドへ載せる (グローバル状態なし。static はステージ名の通し番号の atomic だけ)
  - コード: ServerPackage.cpp:BuildServerPackage — 流れは (1) 点検: dist の Runtime.exe / GameLogic.dll / assets\content_manifest.json (ReadContentManifest で畳み直し検査まで) / exeDir の Server.exe・libssl・libcrypto / vc_redist / 出力先、を全部見て problems に積む (2) 1 件でもあれば何も書かず return (3) staging へ copy_file / fs::copy(recursive) (4) install.bat と mye_server_build.json を書く (5) 入れ替え。assets は fs::copy のバイトコピーで、manifest を焼き直さないので contentHash は書き出しと一致する
      ```
      fs::rename(in.outputDir, retired, ec);          // 旧を退避 (使用中ならここで失敗 = 出力先は無傷)
      fs::rename(staging, in.outputDir, ec);          // 新を置く
      if (ec) { fs::rename(retired, in.outputDir, ec); /* 戻す */ }
      ```
  - コード: ServerPackage.cpp:ServerLocalCheck — state_ (Idle/Running/Passed/Failed)、process_ (HANDLE)、existingPaths_ (起動前のスナップショット)。Start は Cancel → Server.exe の存在 / ログが組んだフォルダの外か / 空きポートを確認 → スナップショット → `StartChildProcess(cmdline, buildDir, logPath, ...)`。ログは StartChildProcess が FILE_SHARE_READ で開くので、Poll 側は FILE_SHARE_READ|WRITE|DELETE で読む。Server.exe は sub-01 で stdout / stderr を無バッファにしてあるので終了後でなく実行中でも読める (Poll は終了時にしか読まない)
  - コード: RunServerBuildCli — `<project>\cache\gamelift\server-build` へ BuildServerPackage → `ServerLocalCheck` を 50 ms 刻みで Poll。vswhere は入力の vsInstallDirOverride が空のときだけ呼ぶ (テストは偽の VS を指して環境に依存しない)
  - 前提・落とし穴: (a) 規則 13-d で `EOSSDK-...` の文字列がテストに書けず、偽 DLL は `OnlineRuntime.dll` にした (入れない物は「固定の許可リストだけコピー」なので、EOS の DLL が入らないのは構造上) (b) Debug 構成の判定は exeDir のフォルダ名 (`bin\x64\Debug`)。インストール先へ移した Editor は判定できず「Release 扱い」になる (警告が出ないだけで動作は変わらない) (c) `install.bat` は仕様どおり相対パス `vc_redist.x64.exe` で、AWS が `C:\game` を作業ディレクトリにして走らせる前提 (実機は手動チェックポイントで確認) (d) 局所起動確認が成功しても VC++ ランタイムの有無は分からない (この PC は VS 入りで必ず通る) (e) GUI サブシステムの Editor.exe は PowerShell から `Start-Process -Wait -PassThru` で起動しないと終了を待てない
  - 選ばなかった案: `--log-dir` で pid 付きのログを作る案 — StartChildProcess が pid を返さず、Server.exe の stdout は無バッファなので、ログ先を引数でなくリダイレクトで決めるほうが単純。`--log-dir` は EC2 の LaunchPath 用 (sub-05) に残る
仕様との差分:
  - [追加] 出力先の入れ替えは「無い / 空 / mye_server_build.json を持つ」場合だけ許す (それ以外は "refusing" で失敗) — 仕様は「既存なら中身を入れ替える」だけで、引数ミスで別フォルダを消す危険を塞ぐため
  - [追加] 書き出しフォルダと出力先が包含関係 (どちら向きも) なら失敗 — 同上 (出力先が dist の親だと入れ替えで dist が消える)
  - [追加] 局所起動確認の終了後、起動前に無かったファイル (cache\hot_server・save_server・.meta) を組んだフォルダから消す — 仕様に無いが、残すと upload-build に混ざる (実測で作られると確認)
  - [追加] 子の stdout / stderr の置き場 (`<project>\cache\gamelift\server-localcheck.log`) と 60 秒のハードタイムアウト — 仕様は未指定
  - [追加] 失敗の詳細は stderr に問題 1 件ずつ、stdout の 1 行にも件数と連結を出した — 「1 行を出して」と「全部挙げる」の両立
  - [解釈] 仕様は vc_redist を「`VC\Redist\MSVC\*\vc_redist.x64.exe` の最も新しい版」とするが、`v145` が混ざる (この PC で実在)。数値の版を優先し、数値が無ければ名前順で最後のものを使う
  - [未実装] 手動チェックポイント (実 AWS) — 指示どおり実行していない
検証:
  - `pwsh tools\gen_project_files.ps1` → Editor.vcxproj / .filters が更新された (Windows PowerShell 5 では構文エラーになる。pwsh で実行)
  - MSBuild MyEngine.sln Release → 成功 / Editor.vcxproj Debug → 成功 (警告なし)
  - `MYE_SELFTEST_ONLY=serverpackage,serverpackagelocal Debug\Editor.exe --selftest-full` → 2 スイート ALL PASS (serverpackage 755 ms、serverpackagelocal 5.3 s。実 Server.exe の局所起動確認は PASS、SKIP なし)
  - 実物の書き出し (demo テンプレートのプロジェクトを `--package` で Release 書き出し) → `Release\Editor.exe --project <p> --gamelift-server-build <dist>` → exit 0、`OK: ... (681 files, 441.0 MB, contentHash 0x2d455a3e19c8d774, local start check passed)`。contentHash は dist の manifest と一致、vswhere で VS を見つけ vc_redist は 14.51.36231 を選んだ。組んだフォルダは Server.exe / OpenSSL 2 本 / GameLogic.dll / assets / cache\cooked / vc_redist / install.bat / mye_server_build.json だけで、起動確認が作った cache\hot_server・save_server は消えていた。もう 1 度実行して入れ替え (既存あり) も成功
  - 失敗系の CLI: 存在しない dist → exit 1・stdout 1 行 + stderr に問題; `--project` 無し → exit 1
  - `tools\check_rules.ps1` → 0 error (65 warn は既存。GameLift ディレクトリ由来の warn 0)。最初の版で規則 13-d (EOS の DLL 名) に 3 件当たり、偽 DLL 名を変えて解消
  - `Debug\Editor.exe --selftest` (fast 層全体) → 119 スイート中 1 失敗: `modalsynth` ("transient burst increases initial impact energy")。このサブは音響・modal を一切触らず、単独実行でも再現。HEAD での失敗確認 (git stash 禁止のため) は未実施 = 既存と推測 (未検証)
  - Release Editor.exe は一度 Defender (Behavior:Win32/DefenseEvasion.A!ml、2026-11-27 02:01:45) に検疫された。M98 の 11/10 にも MyEngineLauncher.exe で同じ検出名が出ている。再ビルドで復旧し、同じ CLI を 3 回実行しても再検出なし。因果 (組み立て + 起動 + 削除の挙動が引き金か、新規バイナリへのクラウド判定か) は未確定
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜3 を満たし、手動チェックポイント (4) 以外は検証済み。上の [追加] が数点ある
  正しさ: 4 — 偽 / 実物の両方で組み立て・局所起動・失敗系・入れ替えを確認。VC++ ランタイム無しの環境と実 EC2 は未確認 (チェックポイントの範囲)
  コード品質: 4 — 責務は 1 ファイルに集約。BuildServerPackage が長め (点検・コピー・入れ替えを 1 関数)
  テスト: 4 — 2 スイート 40 項目弱。Release 構成の Editor での selftest-full は未実行 (Debug のみ)
不安・質問:
  - (planner へ・推奨で進めた) Defender の検出: 手動チェックポイントでユーザーが Release Editor.exe でこの CLI を打つと、同様に検疫される可能性がある。再現条件は不明。ユーザー向けに「検疫されたら再ビルド」の注意を司会が伝えるか、`--gamelift-server-build` の代わりに窓 (sub-05) を使う前提にするか
  - (planner へ) install.bat は仕様の本文どおり相対パスの `vc_redist.x64.exe`。AWS は install.bat を `C:\game` で実行するはずだが (公式ドキュメントの例も相対)、vc_redist のブートストラップが子プロセスへ移ると batch が先に終わる懸念が (記憶ベースで) ある。チェックポイントで `C:\game\install_vcredist.log` の有無が分かるので、問題なら `start /wait` に変える
  - `modalsynth` の失敗は別件として台帳へ
触ったファイル:
  - src/Editor/Tools/GameLift/ServerPackage.h
  - src/Editor/Tools/GameLift/ServerPackage.cpp
  - src/Editor/Tools/GameLift/ServerPackageSelfTest.h
  - src/Editor/Tools/GameLift/ServerPackageSelfTest.cpp
  - src/Editor/SelfTest/SelfTestSuites.cpp
  - src/Editor/App/EditorMain.cpp
  - plans/m107-gamelift-editor/sub-02.md (実装メモのみ)
申し送り:
  - build/Editor.vcxproj と build/Editor.vcxproj.filters が gen_project_files で更新されている (生成物だが、ファイル追加のためコミットに含めること)
  - sub-03 は同じ `src/Editor/Tools/GameLift/` へファイルを足す。このサブは `ServerPackage*` だけ。SelfTestSuites.cpp は include を FractureEditorSelfTest.h の直後、表を末尾 (terminalvsenv の直後) に足したので、sub-03 は同じ末尾へ追記すれば衝突は 1 行差分の範囲
  - sub-05 へ: `ServerLocalCheck` を窓から使うとき、ログの置き場は `<project>\cache\gamelift\server-localcheck.log` (組んだフォルダの外であること)。`BuildServerPackage` は同期なので作業スレッドで呼ぶ。problems / warnings は英語の診断文 (窓に出すなら Tr() の包みが要る)
  - 手動チェックポイントの手順 2 の `Editor.exe` は Release。GUI サブシステムなので PowerShell から直接打つと終了を待たず結果の 1 行も見えにくい (`Start-Process -Wait -NoNewWindow -RedirectStandardOutput` 経由か、Windows のコンソール親へ繋がる `AttachParentConsole` で見える端末から)
  - `Debug\Server.exe` でも局所起動は通る。Debug の exeDir から組むと警告が出る (動くが遅い)

## フィードバック履歴

- round 1: VERDICT OK (planner, 2026-11-27)。planner が `serverpackage,serverpackagelocal` を `--selftest-full` で再実行し exit 0 (PASS 37 項目、局所起動の SKIP なし)。`modalsynth` の FAIL は単独でも再現し、`src/Engine` は基点 276f8613 から作業ツリーまで無変更、最後に触ったのは 2026-11-19 の 7ee637a3 (人の手のコミット) = 本サブ由来ではない (別件)。coder の [追加] 4 点は spec 4.1.2 の明確化として採用 (spec 8. 変更履歴)。Defender と install.bat の待ちはリスク R8 / R9 として spec 7. に積み、チェックポイントで観測する。
