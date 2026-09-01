@echo off
rem replay_verify.bat — Debug/Release 一貫性の自動検証 (engine_spec.md 11.3)
rem   1. 両構成をビルド
rem   2. 並列プールで 9 ジョブを回す (tools\run_parallel.ps1、並列度の既定 = 論理コア数):
rem        - 6 シーンチェーン: record (Debug, --replay-fast) →
rem          snapshot stress 付き verify (Debug) → verify (Release)
rem        - タイムトラベルの巻き戻し (Debug / Release)
rem        - 静的規則チェック (check_rules.ps1)
rem   3. 失敗時は mismatch マーカーの残ったシーンだけ :diagnose を直列で回す
rem 全て成功で exit 0、いずれか失敗で exit 1
rem
rem 素の Debug verify は stress 付き verify (M52d) へ統合した — ハッシュ照合は素の
rem verify と同一機構で毎 tick 走るので検出能力は同じ。赤いときだけ素の verify を
rem 再実行して「素の非決定」と「スナップショット復元の非対称」を切り分ける (:chain)。
rem
rem 並列化の設計メモ:
rem   - ジョブは「この bat 自身への --job <名前> 再入」。コマンド文字列をファイルや
rem     引数で受け渡さない = cmd のエスケープ地獄を構造的に回避する。
rem     手元で 1 本だけ回すのにも使える (ビルド済み前提):
rem       tools\replay_verify.bat --job joints
rem     tick 数は MYE_RV_TICKS、並列度は MYE_REPLAY_JOBS で上書きできる。
rem     ★--job 再入の子 cmd は chcp 437 (単バイト CP) で呼ぶこと (runner が強制する)。
rem       コンソール CP が多バイト (932/65001) だと、cmd のバッチ読取りが goto の後に
rem       バイト数と文字数のずれで読み位置をドリフトさせ、日本語 rem の断片をコマンド
rem       実行して即死する。壊れるかはバイト配置の運次第 (rem を 1 行足すだけで変わる
rem       ことを実測) なので、単バイト CP でドリフトを構造的に殺す。ジョブ経路の echo を
rem       ASCII 限定に保つのもこのため。
rem   - cook キャッシュ: 各シーンの record が自シーン分をコールドで焼き、verify が
rem     ウォームで読む。並列で他シーンが先に焼いたアセットは「先に焼いた側のペアが
rem     コールドを証明する」ので、cook 有無のビット一致証明 (M51b) は全アセットで
rem     保たれる。同一アセットの同時クックは CookedCache の PID 付きテンポラリ +
rem     rename で無害 (どちらかの完全な内容しか観測されない)。
rem   - 録画は --replay-fast で実時間から切り離した (旧: 600 tick = 実時間 10 秒 × 6 本)。
rem     sim は実時間を読まない (規則 3) ので .rep はバイト一致する — 導入時に
rem     遅い録画と fc /b で機械確認済み。
rem
rem M52a: 照合が失敗したときだけ :diagnose を呼び、
rem   失敗側の <rep>.tickN.actual.dump (EngineLoop が自動で残す) と
rem   期待側 (同じコマンドで録り直した Debug) のフィールド単位ダンプを
rem   --hash-diff で突き合わせて「どのフィールドが割れたか」まで表示する
rem
rem M52b: CI もこの bat を**そのまま**呼ぶ (CI 専用の検証ロジックを書かない)。
rem   CI 固有の事情は環境変数の 2 本だけで注入する:
rem     MYE_EXTRA_ARGS  … 全 Editor.exe 実行へ後置する引数 (CI では "--warp --no-audio")
rem     MYE_MSBUILD_ARGS… MSBuild へ後置する引数 (CI では "/p:MyeWarnAsError=true")
setlocal
cd /d "%~dp0.."

rem ---- 並列 runner からの再入口 (ビルドと掃除はしない) ----
if "%~1"=="--job" goto :job

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
    echo [replay_verify] MSBuild not found & exit /b 1
)

if defined MYE_EXTRA_ARGS   echo [replay_verify] extra exe args: %MYE_EXTRA_ARGS%
if defined MYE_MSBUILD_ARGS echo [replay_verify] extra msbuild args: %MYE_MSBUILD_ARGS%

echo === build Debug ===
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo %MYE_MSBUILD_ARGS% || exit /b 1
echo === build Release ===
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo %MYE_MSBUILD_ARGS% || exit /b 1

set TICKS=600
if not "%~1"=="" set TICKS=%~1
rem ジョブ側は tick 数を環境変数で受け取る (--job 再入では引数がジョブ名で埋まっている)
set MYE_RV_TICKS=%TICKS%

rem ---- M51b: アセットクックキャッシュをコールドから始める ----
rem レガシー起動のキャッシュは exe ディレクトリ配下 (<exeDir>\cache\cooked)。
rem 削除直後の record はコールド (フルパース + クック書き込み)、以降の verify はウォーム
rem (クック再生)。600 tick のハッシュ一致が「クック有無で登録内容がビット同一」の機械証明
if exist bin\x64\Debug\cache\cooked rd /s /q bin\x64\Debug\cache\cooked
if exist bin\x64\Release\cache\cooked rd /s /q bin\x64\Release\cache\cooked

rem 前回の失敗マーカーが残っていると :diagnose が古い tick を掴む (M52a)
del /q cache\*.mismatch.txt 2>nul
if exist cache\replay_logs rd /s /q cache\replay_logs

echo === parallel verification: 7 scene chains + time travel x2 + rule check ===
rem ★Entry は空白なし相対パスで渡す (人間/CI が bat を叩くのと同じ呼び形に固定。
rem   バッチ読取りの罠と chcp 437 の理由は runner 冒頭のコメント参照)
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\run_parallel.ps1 -Entry tools\replay_verify.bat -LogDir cache\replay_logs -Jobs "demo,parts,flow,mp,physics,joints,acoustic,ttdebug,ttrelease,rules" || goto :failed

echo.
echo [PASS] replay consistency (Debug/Release, 7 scenes: demo + parts + flow + mp + physics + joints + acoustic) + snapshot round-trip + time travel + rule check
exit /b 0

rem ---------------------------------------------------------------- :failed
rem 各ジョブのログは runner が全文出している。ここではハッシュ照合まで到達して
rem 割れたシーン (= mismatch マーカーが残ったシーン) だけフィールド単位まで落とす
:failed
set DIAGFOUND=0
if exist cache\golden.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden.rep" ""
)
if exist cache\golden_parts.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_parts.rep" "--parts-demo"
)
if exist cache\golden_flow.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_flow.rep" "--flow-demo"
)
if exist cache\golden_mp.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_mp.rep" "--local-demo"
)
if exist cache\golden_physics.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_physics.rep" "--physics-demo"
)
if exist cache\golden_joints.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_joints.rep" "--joint-demo"
)
if exist cache\golden_acoustic.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_acoustic.rep" "--acoustic-demo"
)
if "%DIAGFOUND%"=="0" echo [diag] no mismatch markers - failures happened before any hash comparison, see the job logs above
echo [FAIL] replay verification
exit /b 1

rem ---------------------------------------------------------------- :job
rem 並列 runner からの再入口: %2 = ジョブ名。ビルド済みの bin を前提にする
:job
set TICKS=600
if defined MYE_RV_TICKS set TICKS=%MYE_RV_TICKS%
goto :job_%~2

:job_demo
call :chain cache\golden.rep "" ""
exit /b %ERRORLEVEL%

rem ---- 部位のボーン追従シーン (M48g) ----
rem 既定デモシーンにはスキンメッシュが 1 体も無く、骨演算は一度もハッシュ被覆に
rem 入ったことがなかった。このペアで「骨駆動の LocalTransform が Debug/Release で
rem ビット一致する」ことまで機械検証する。
rem **シーンはコードから毎回組み直す** — モデル由来のサブアセット ID は正規化した
rem 絶対パスのハッシュなので、保存した .scene.json はチェックアウト先に依存する
rem (= コミットできない)。版管理された唯一の正解は BuildPartsShowcaseScene。
rem 組んだ後は保存ファイル経由でロードする = 起動時のヘッドレススケルトン登録も被覆する
:job_parts
if exist cache\parts_showcase.scene.json del /q cache\parts_showcase.scene.json
bin\x64\Debug\Editor.exe --parts-demo --save-scene-on-start --frames 2 --no-audio %MYE_EXTRA_ARGS% || exit /b 1
if not exist cache\parts_showcase.scene.json (
    echo [FAIL] parts showcase scene was not written
    exit /b 1
)
call :chain cache\golden_parts.rep "--parts-demo" "--parts-demo"
exit /b %ERRORLEVEL%

rem ---- ゲームフロー統合デモ (M51j) ----
rem M51 のフロー系 (LoadScene 遷移 / TimeControl ポーズ+タイムスケール / PersistStore の
rem シーン跨ぎ持ち越し / SaveGame 書出 / アクションマップ評価) を 1 本の tick タイムラインで
rem 実走し、Debug/Release のビット一致まで機械検証する = M51 決定論保証の総括。
rem シーンはコードから毎回組み直す (parts と同じ流儀)。builtin メッシュ + 名前マテリアル
rem のみなので内容はチェックアウト非依存だが、正解はコード側なので生成物は gitignore
:job_flow
if exist assets\scenes\flow_title.scene.json del /q assets\scenes\flow_title.scene.json
if exist assets\scenes\flow_game.scene.json del /q assets\scenes\flow_game.scene.json
bin\x64\Debug\Editor.exe --flow-demo --frames 2 --no-audio %MYE_EXTRA_ARGS% || exit /b 1
if not exist assets\scenes\flow_title.scene.json (
    echo [FAIL] flow title scene was not written
    exit /b 1
)
if not exist assets\scenes\flow_game.scene.json (
    echo [FAIL] flow game scene was not written
    exit /b 1
)
call :chain cache\golden_flow.rep "--flow-demo" "--flow-demo"
exit /b %ERRORLEVEL%

rem ---- マルチプレイヤー入力レーン (M52g) ----
rem レーンを足しただけでは決定論の証明にならない: ヘッドレスの実入力は全レーン恒常ゼロで、
rem 「レーン 1 がレーン 0 を読んでいる」類の配線ミスは記録側と検証側で**対称に**起きて
rem ハッシュが一致してしまう。そこで
rem   --synth-input   … (tick, lane) の純関数でレーンごとに違う入力を流し込む
rem   PlayerInput     … その評価結果を ECS のミラーへ書いてワールドハッシュに載せる
rem の 2 つを噛ませる。これで「レーン n の入力がレーン n のエンティティへ届いたか」が
rem Debug/Release のビット一致として機械検証される。
rem シーンは 4 体 (kMaxPlayers) 置いてあり、--local-players 2 では 3-4 体目のミラーが
rem 恒常ゼロであることまで同じハッシュで固定される。コードから毎回組む (parts と同じ流儀)
rem ★検証側に --synth-input は渡さない。合成入力は .rep に記録済みで、verify は
rem   記録値で置換するのが正しい経路 (ここで渡すと「合成入力が無いと再生できない .rep」
rem   という嘘の仕様を作ってしまう)
:job_mp
if exist cache\local_players.scene.json del /q cache\local_players.scene.json
call :chain cache\golden_mp.rep "--local-demo --local-players 2 --synth-input" "--local-demo"
exit /b %ERRORLEVEL%

rem ---- 物理 (空力・浮力・材料) (M59d) ----
rem M59 で足した数式 — 重力ベクトル / 等方抗力 / マグヌス / 面サンプリング / 翼面 /
rem 浮力 / 材料と密度 — が Debug と Release でビット一致することを 600 tick 実走で固定する。
rem selftest は 1 項目ずつの小さな世界しか見ないので、「全部が同じ tick に同居したときの
rem 加算順序」までは押さえられない。ここが唯一その検査になっている。
rem シーンはコードから毎回組み直す (parts と同じ流儀) — .physmat の AssetID は同伴 .meta の
rem GUID 優先で解決されるが、シーンファイルを版管理する理由が無いので cache\ へ置く
:job_physics
if exist cache\physics_showcase.scene.json del /q cache\physics_showcase.scene.json
call :chain cache\golden_physics.rep "--physics-demo" "--physics-demo"
exit /b %ERRORLEVEL%

rem ---- 関節と機構 (M60i) ----
rem M60 で足した層 — 拘束ブロック (K の逆行列で 1〜3 自由度をまとめて解く) / ヒンジ /
rem 固定 / スライダ / リミット / モータ / 破断 / 複合コライダー / 凸包 / ラグドールの
rem 逆駆動 / 車両 — が Debug と Release でビット一致することを 600 tick 実走で固定する。
rem selftest は 1 項目ずつの小さな世界しか見ないので、「全部が同じ tick に同居したときの
rem 加算順序」はここでしか押さえられない。
rem ★このシーンだけ **substeps 16** (env の上限) で回る — ラグドールが要求する。
rem ★凸包を 1 個だけモデル由来 (.glb) にしてあるので .mcvx クックもここで被覆される。
rem   Debug と Release は cooked ディレクトリが別なので、両者が独立に焼いた凸包で
rem   同じハッシュが出ること = 「キャッシュの有無でワールドハッシュが変わらない」の検査。
rem シーンはコードから毎回組み直す (parts / physics と同じ流儀)
:job_joints
if exist cache\joint_showcase.scene.json del /q cache\joint_showcase.scene.json
call :chain cache\golden_joints.rep "--joint-demo" "--joint-demo"
exit /b %ERRORLEVEL%

rem ---- 音響伝播 (M65b) ----
rem M65 で足した層 — 整数チャンファ距離の bucket Dijkstra で広がる波面 — が Debug と
rem Release でビット一致することを 600 tick 実走で固定する。
rem ★このペアが押さえているのは **ECS 外 sim 状態の 3 例目 (波スロット表)** の 3 点セット。
rem   snapshot 往復 (--snapshot-stress 37) が通ることが「復元後に距離場を引き直す経路」の
rem   実走検査で、selftest の memcmp と合わせて「増分と引き直しが同値」を二重に固定する。
rem ★波は WavePinger (GameLogic.dll) が 150 tick ごとに立てる。DLL が焼けていないと
rem   波が 1 本も出ず、**ハッシュ節が内容ゲートで畳まれないまま緑になる** (= 何も検査
rem   していない状態で PASS する) ので、DLL のビルドはこの検査の前提。
rem ★M65g から **記録側に --synth-input を渡す**。プレイヤー (Watcher* 3 本) の視点角は
rem   GetMouseDelta を積分した登録フィールドで、無入力だと恒常ゼロのまま「配線ミスが
rem   記録側と検証側で対称に起きて一致してしまう」= mp ペアと同じ穴が開く。合成入力は
rem   生マウスデルタも流す (M64a) ので、これで視点・移動・足音・敵との接触までが
rem   Debug/Release のビット一致として機械検証される。
rem   ★検証側には渡さない (合成入力は .rep に記録済み。mp ペアと同じ理由)
rem シーンはコードから毎回組み直す (parts / physics / joints と同じ流儀)
:job_acoustic
call :chain cache\golden_acoustic.rep "--acoustic-demo --synth-input" "--acoustic-demo"
exit /b %ERRORLEVEL%

rem ---- タイムトラベルの巻き戻し (M52e) ----
rem 「T まで進める → T-K へ戻す → 記録入力で T まで再シム → 元の T とハッシュ一致」を
rem 複数の K で実走し、続けて「スクラブ中は tick が止まる」「再開すると分岐して未来を捨てる」
rem をライブのフレームループ上で確認する。ここが赤い = 巻き戻した世界が元と別物という意味で、
rem タイムライン窓が見せる過去がそもそも嘘になる。Debug/Release 両方で回す
:job_ttdebug
bin\x64\Debug\Editor.exe --timetravel-selftest 400 %MYE_EXTRA_ARGS% || exit /b 1
exit /b 0

:job_ttrelease
bin\x64\Release\Editor.exe --timetravel-selftest 400 %MYE_EXTRA_ARGS% || exit /b 1
exit /b 0

:job_rules
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\check_rules.ps1 || exit /b 1
exit /b 0

rem ---------------------------------------------------------------- :chain
rem 1 シーンぶんの record → stress 付き Debug verify → Release verify。
rem   %1 = .rep パス / %2 = record 側のシーン引数 / %3 = verify 側のシーン引数
rem stress 付き verify (--snapshot-stress 37、M52d) はハッシュ照合が素の verify と
rem 同一機構なので完全上位互換 — 素の Debug verify はもう回さない。ここが赤い =
rem 「撮って戻す」を挟んでも 600 tick の期待ハッシュが全一致するはず、が崩れたという
rem 意味で、タイムトラベル (M52e) / クラッシュ再現 (M52f) / ロールバック (M52i) の
rem 土台が崩れている。赤いときだけ素の verify を再実行して切り分ける
:chain
setlocal
set "CREP=%~1"
set "CREC=%~2"
set "CVER=%~3"
echo === record %CREP% : Debug, %TICKS% ticks, --replay-fast ===
bin\x64\Debug\Editor.exe %CREC% --replay-record %CREP% --replay-ticks %TICKS% --replay-fast %MYE_EXTRA_ARGS% || (
    echo [FAIL] record: %CREP%
    endlocal & exit /b 1
)
echo === verify %CREP% : Debug + snapshot stress every 37 ticks ===
bin\x64\Debug\Editor.exe %CVER% --replay-verify %CREP% --snapshot-stress 37 %MYE_EXTRA_ARGS% || goto :chain_stress_fail
echo === verify %CREP% : Release ===
bin\x64\Release\Editor.exe %CVER% --replay-verify %CREP% %MYE_EXTRA_ARGS% || (
    echo [FAIL] Release verify: %CREP%
    endlocal & exit /b 1
)
endlocal & exit /b 0

:chain_stress_fail
rem stress で赤い。素の verify で「素の非決定か、復元の非対称か」を切り分ける。
rem 素でも赤いなら mismatch マーカーは同じ tick で上書きされるだけなので、
rem :diagnose の掴む値は変わらない
echo [FAIL] Debug verify with snapshot stress: %CREP%
echo [diag] re-running plain verify to disambiguate...
bin\x64\Debug\Editor.exe %CVER% --replay-verify %CREP% %MYE_EXTRA_ARGS% || (
    echo [diag] plain verify FAILS too = base determinism is broken, not the snapshot path
    endlocal & exit /b 1
)
echo [diag] plain verify passes = snapshot restore asymmetry - only --snapshot-stress broke it
endlocal & exit /b 1

rem ---------------------------------------------------------------- :diagnose
rem 失敗した照合の「どのフィールドが割れたか」を出す (M52a)。
rem   %1 = .rep パス / %2 = シーン切替の追加引数 ("" / "--parts-demo" / "--flow-demo" /
rem                        "--local-demo" / "--physics-demo" / "--joint-demo" /
rem                        "--acoustic-demo")
rem 失敗側のダンプは EngineLoop が MISMATCH 時に自動で残しているので、
rem ここでは期待側 (= その .rep を録ったのと同じコマンド) を撮り直して突き合わせる
:diagnose
setlocal
set "DREP=%~1"
set "DARG=%~2"
if not exist "%DREP%.mismatch.txt" (
    echo [diag] no mismatch marker - the run failed before any hash comparison
    endlocal & exit /b 0
)
set /p DTICK=<"%DREP%.mismatch.txt"
echo.
echo [diag] first mismatch tick: %DTICK%
echo [diag] re-recording in Debug to capture the expected-side field dump
bin\x64\Debug\Editor.exe %DARG% --replay-record "%DREP%.diag.rep" --replay-ticks %TICKS% --replay-fast --hash-dump "%DREP%.tick%DTICK%.expected.dump" --hash-dump-tick %DTICK% %MYE_EXTRA_ARGS%
echo [diag] field-level diff (expected vs actual):
bin\x64\Debug\Editor.exe --hash-diff "%DREP%.tick%DTICK%.expected.dump" "%DREP%.tick%DTICK%.actual.dump"
echo.
endlocal & exit /b 0
