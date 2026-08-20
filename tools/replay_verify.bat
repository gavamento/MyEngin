@echo off
rem replay_verify.bat — Debug/Release 一貫性の自動検証 (engine_spec.md 11.3)
rem   1. 両構成をビルド
rem   2. Debug でゴールデンリプレイを記録
rem   3. Debug と Release の両方でハッシュ照合
rem   4. スナップショット往復ストレス (M52d)
rem   5. 静的規則チェック
rem 全て成功で exit 0、いずれか失敗で exit 1
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

set REP=cache\golden.rep
set TICKS=600
if not "%~1"=="" set TICKS=%~1

rem ---- M51b: アセットクックキャッシュをコールドから始める ----
rem レガシー起動のキャッシュは exe ディレクトリ配下 (<exeDir>\cache\cooked)。
rem 削除直後の record はコールド (フルパース + クック書き込み)、以降の verify はウォーム
rem (クック再生)。600 tick のハッシュ一致が「クック有無で登録内容がビット同一」の機械証明
if exist bin\x64\Debug\cache\cooked rd /s /q bin\x64\Debug\cache\cooked
if exist bin\x64\Release\cache\cooked rd /s /q bin\x64\Release\cache\cooked

rem 前回の失敗マーカーが残っていると :diagnose が古い tick を掴む (M52a)
del /q cache\*.mismatch.txt 2>nul

echo === record golden replay (Debug, %TICKS% ticks) ===
bin\x64\Debug\Editor.exe --replay-record %REP% --replay-ticks %TICKS% %MYE_EXTRA_ARGS% || exit /b 1

echo === verify in Debug ===
bin\x64\Debug\Editor.exe --replay-verify %REP% %MYE_EXTRA_ARGS% || (call :diagnose "%REP%" "" & echo [FAIL] Debug verify & exit /b 1)

echo === verify in Release ===
bin\x64\Release\Editor.exe --replay-verify %REP% %MYE_EXTRA_ARGS% || (call :diagnose "%REP%" "" & echo [FAIL] Release verify & exit /b 1)

rem ---- 2 本目: 部位のボーン追従シーン (M48g) ----
rem 既定デモシーンにはスキンメッシュが 1 体も無く、骨演算は一度もハッシュ被覆に
rem 入ったことがなかった。このペアで「骨駆動の LocalTransform が Debug/Release で
rem ビット一致する」ことまで機械検証する。
rem **シーンはコードから毎回組み直す** — モデル由来のサブアセット ID は正規化した
rem 絶対パスのハッシュなので、保存した .scene.json はチェックアウト先に依存する
rem (= コミットできない)。版管理された唯一の正解は BuildPartsShowcaseScene。
rem 組んだ後は保存ファイル経由でロードする = 起動時のヘッドレススケルトン登録も被覆する
set REP2=cache\golden_parts.rep
set PARTS_SCENE=cache\parts_showcase.scene.json

echo === build parts showcase scene (from code) ===
if exist %PARTS_SCENE% del /q %PARTS_SCENE%
bin\x64\Debug\Editor.exe --parts-demo --save-scene-on-start --frames 2 --no-audio %MYE_EXTRA_ARGS% || exit /b 1
if not exist %PARTS_SCENE% (echo [FAIL] parts showcase scene was not written & exit /b 1)

echo === record golden replay: parts (Debug, %TICKS% ticks) ===
bin\x64\Debug\Editor.exe --parts-demo --replay-record %REP2% --replay-ticks %TICKS% %MYE_EXTRA_ARGS% || exit /b 1

echo === verify parts in Debug ===
bin\x64\Debug\Editor.exe --parts-demo --replay-verify %REP2% %MYE_EXTRA_ARGS% || (call :diagnose "%REP2%" "--parts-demo" & echo [FAIL] Debug parts verify & exit /b 1)

echo === verify parts in Release ===
bin\x64\Release\Editor.exe --parts-demo --replay-verify %REP2% %MYE_EXTRA_ARGS% || (call :diagnose "%REP2%" "--parts-demo" & echo [FAIL] Release parts verify & exit /b 1)

rem ---- 3 本目: ゲームフロー統合デモ (M51j) ----
rem M51 のフロー系 (LoadScene 遷移 / TimeControl ポーズ+タイムスケール / PersistStore の
rem シーン跨ぎ持ち越し / SaveGame 書出 / アクションマップ評価) を 1 本の tick タイムラインで
rem 実走し、Debug/Release のビット一致まで機械検証する = M51 決定論保証の総括。
rem シーンはコードから毎回組み直す (parts と同じ流儀)。builtin メッシュ + 名前マテリアル
rem のみなので内容はチェックアウト非依存だが、正解はコード側なので生成物は gitignore
set REP3=cache\golden_flow.rep
set FLOW_TITLE=assets\scenes\flow_title.scene.json
set FLOW_GAME=assets\scenes\flow_game.scene.json

echo === build flow demo scenes (from code) ===
if exist %FLOW_TITLE% del /q %FLOW_TITLE%
if exist %FLOW_GAME% del /q %FLOW_GAME%
bin\x64\Debug\Editor.exe --flow-demo --frames 2 --no-audio %MYE_EXTRA_ARGS% || exit /b 1
if not exist %FLOW_TITLE% (echo [FAIL] flow title scene was not written & exit /b 1)
if not exist %FLOW_GAME% (echo [FAIL] flow game scene was not written & exit /b 1)

echo === record golden replay: flow (Debug, %TICKS% ticks) ===
bin\x64\Debug\Editor.exe --flow-demo --replay-record %REP3% --replay-ticks %TICKS% %MYE_EXTRA_ARGS% || exit /b 1

echo === verify flow in Debug ===
bin\x64\Debug\Editor.exe --flow-demo --replay-verify %REP3% %MYE_EXTRA_ARGS% || (call :diagnose "%REP3%" "--flow-demo" & echo [FAIL] Debug flow verify & exit /b 1)

echo === verify flow in Release ===
bin\x64\Release\Editor.exe --flow-demo --replay-verify %REP3% %MYE_EXTRA_ARGS% || (call :diagnose "%REP3%" "--flow-demo" & echo [FAIL] Release flow verify & exit /b 1)

rem ---- 4 段目: スナップショット往復ストレス (M52d) ----
rem 「撮って戻す」を 37 tick ごとに挟んでも 600 tick の期待ハッシュが全一致することを
rem 3 ペアすべてで固定する。既存の .rep をそのまま使い回すので追加コストは verify 1 回分。
rem ここが赤い = 復元が非対称 (撮れているのに戻していない sim 状態がある) という意味で、
rem タイムトラベル (M52e) / クラッシュ再現 (M52f) / ロールバック (M52i) の土台が崩れている。
rem selftest の小さな世界では出ない取りこぼしは、この実データ 600 tick でしか捕まらない
echo === snapshot round-trip stress (Debug, every 37 ticks) ===
bin\x64\Debug\Editor.exe --replay-verify %REP% --snapshot-stress 37 %MYE_EXTRA_ARGS% || (echo [FAIL] snapshot stress: demo & exit /b 1)
bin\x64\Debug\Editor.exe --parts-demo --replay-verify %REP2% --snapshot-stress 37 %MYE_EXTRA_ARGS% || (echo [FAIL] snapshot stress: parts & exit /b 1)
bin\x64\Debug\Editor.exe --flow-demo --replay-verify %REP3% --snapshot-stress 37 %MYE_EXTRA_ARGS% || (echo [FAIL] snapshot stress: flow & exit /b 1)

echo === static rule check ===
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\check_rules.ps1 || (echo [FAIL] rule check & exit /b 1)

echo.
echo [PASS] replay consistency (Debug/Release, 3 scenes: demo + parts + flow) + snapshot round-trip + rule check
exit /b 0

rem ---------------------------------------------------------------- :diagnose
rem 失敗した照合の「どのフィールドが割れたか」を出す (M52a)。
rem   %1 = .rep パス / %2 = シーン切替の追加引数 ("" / "--parts-demo" / "--flow-demo")
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
bin\x64\Debug\Editor.exe %DARG% --replay-record "%DREP%.diag.rep" --replay-ticks %TICKS% --hash-dump "%DREP%.tick%DTICK%.expected.dump" --hash-dump-tick %DTICK% %MYE_EXTRA_ARGS%
echo [diag] field-level diff (expected vs actual):
bin\x64\Debug\Editor.exe --hash-diff "%DREP%.tick%DTICK%.expected.dump" "%DREP%.tick%DTICK%.actual.dump"
echo.
endlocal & exit /b 0
