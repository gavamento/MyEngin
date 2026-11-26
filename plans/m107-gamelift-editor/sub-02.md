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
2. サーバ版: `bin\x64\Release\Editor.exe --project <proj> --gamelift-server-build <dist>`
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

結果として司会に返してほしいもの: 8 の最終状態 (ACTIVE / ERROR とイベント)、作成直後の InstanceCounts、11 の成否 (Runtime のログの Reject 理由があれば)、13 のログの有無。
ERROR の場合の切り分け: イベントが `SERVER_PROCESS_CRASHED` / `SERVER_PROCESS_TERMINATED_UNHEALTHY` なら VC++ ランタイム等の依存、
`SERVER_PROCESS_INVALID_PATH` なら LaunchPath、`SERVER_PROCESS_SDK_INITIALIZATION_TIMEOUT` / `SERVER_PROCESS_PROCESS_READY_TIMEOUT` なら InitSDK の呼び方か起動の遅さ。

## 実装メモ (coder が追記)

## フィードバック履歴
