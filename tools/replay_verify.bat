@echo off
rem replay_verify.bat — Debug/Release 一貫性の自動検証 (engine_spec.md 11.3)
rem   1. 両構成をビルド
rem   2. 並列プールで下の -Jobs の全ジョブを回す (tools\run_parallel.ps1、並列度の既定 = 論理コア数):
rem        - シーンごとのチェーン: record (Debug, --replay-fast) →
rem          snapshot stress 付き verify (Debug) → verify (Release) →
rem          ヘッドレス Server.exe (Release) の verify (M81a)
rem        - jobs A/B (M90g): 直列 (--no-jobs) で録った .rep を jobs あり (ワーカー起動) で照合する
rem        - タイムトラベルの巻き戻しと分岐 What-if (それぞれ Debug / Release)
rem        - 静的規則チェック (check_rules.ps1)
rem   3. 失敗時は mismatch マーカーの残ったシーンだけ :diagnose を直列で回す
rem 全て成功で exit 0、いずれか失敗で exit 1
rem
rem Debug の verify は stress 付き (M52d) だけを回す — ハッシュ照合は素の
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
rem       多バイト CP だと cmd のバッチ読取りがドリフトし、日本語 rem の断片をコマンド
rem       実行して即死する (詳細は run_parallel.ps1 冒頭)。ジョブ経路の echo を
rem       ASCII 限定に保つのもこのため。
rem   - cook キャッシュ: 各シーンの record が自シーン分をコールドで焼き、verify が
rem     ウォームで読む。並列で他シーンが先に焼いたアセットは「先に焼いた側のペアが
rem     コールドを証明する」ので、cook 有無のビット一致証明 (M51b) は全アセットで
rem     保たれる。同一アセットの同時クックは CookedCache の PID 付きテンポラリ +
rem     rename で無害 (どちらかの完全な内容しか観測されない)。
rem   - 録画は --replay-fast で実時間から切り離す。
rem     sim は実時間を読まない (規則 3) ので .rep はバイト一致する。
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

echo === parallel verification: 13 scene chains + jobs A/B + time travel x2 + what-if x2 + rule check ===
rem ★Entry は空白なし相対パスで渡す (人間/CI が bat を叩くのと同じ呼び形に固定。
rem   バッチ読取りの罠と chcp 437 の理由は runner 冒頭のコメント参照)
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\run_parallel.ps1 -Entry tools\replay_verify.bat -LogDir cache\replay_logs -Jobs "demo,parts,flow,mp,physics,joints,acoustic,ui,fracture,nav,perception,bt,anim,jobsab,ttdebug,ttrelease,whatifdebug,whatifrelease,rules" || goto :failed

echo.
echo [PASS] replay consistency (Debug/Release, 13 scenes: demo + parts + flow + mp + physics + joints + acoustic + ui + fracture + nav + perception + bt + anim) + snapshot round-trip + jobs A/B + time travel + rule check
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
if exist cache\golden_ui.rep.mismatch.txt (
    set DIAGFOUND=1
    rem 期待側の撮り直しは記録と同じ引数 (台本込み) でないと入力がずれる
    call :diagnose "cache\golden_ui.rep" "--ui-demo --ui-demo-input"
)
if exist cache\golden_fracture.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_fracture.rep" "--fracture-demo"
)
if exist cache\golden_nav.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_nav.rep" "--nav-demo"
)
if exist cache\golden_perception.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_perception.rep" "--perception-demo"
)
if exist cache\golden_bt.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_bt.rep" "--bt-demo"
)
if exist cache\golden_anim.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\golden_anim.rep" "--anim-demo"
)
rem jobs A/B: 期待側は直列で録り直す (記録と同じ --no-jobs)
if exist cache\jobsab_particle.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\jobsab_particle.rep" "--particle-demo --particle-backend cpu --no-jobs"
)
if exist cache\jobsab_perception.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\jobsab_perception.rep" "--perception-demo --no-jobs"
)
if exist cache\jobsab_parts.rep.mismatch.txt (
    set DIAGFOUND=1
    call :diagnose "cache\jobsab_parts.rep" "--parts-demo --no-jobs"
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
rem 既定デモシーンにはスキンメッシュが 1 体も無く、骨演算はこのペアでしかハッシュ被覆に
rem 入らない。このペアで「骨駆動の LocalTransform が Debug/Release で
rem ビット一致する」ことまで機械検証する。
rem **シーンはコードから毎回組み直す** — 版管理された唯一の正解は BuildPartsShowcaseScene。
rem 組んだ後は保存ファイル経由でロードする = 起動時のヘッドレススケルトン登録も被覆する
:job_parts
if exist cache\parts_showcase.scene.json del /q cache\parts_showcase.scene.json
bin\x64\Debug\Editor.exe --parts-demo --save-scene-on-start --frames 2 --no-audio %MYE_EXTRA_ARGS% || exit /b 1
if not exist cache\parts_showcase.scene.json (
    echo [FAIL] parts showcase scene was not written
    exit /b 1
)
call :chain cache\golden_parts.rep "--parts-demo" "--parts-demo"
if errorlevel 1 exit /b 1
rem 部位追従の jobs A/B (保存済みシーンを使うのでこのジョブの中で回す。別ジョブだとシーンの再生成と競合する)
call :jobsab_one cache\jobsab_parts.rep "--parts-demo"
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
rem ★M70c: 記録側にだけ --synth-input を渡す (7 ペア目の acoustic と同じ流儀)。
rem    合成入力は D-Pad を疎に押し A ボタンを押す = タイトルの 2 ボタンで
rem    フォーカス移動 + 決定が実際に起きる。**これがエンジン側 UI 対話
rem    (hovered/pressed/clicked/focused = ワールドハッシュ対象) の唯一の被覆**で、
rem    FlowTitleDriver の clearClicks (登録フィールド = ハッシュ対象) に結果が乗る。
rem    検証側に --synth-input は要らない (合成入力は .rep に記録済み)。
call :chain cache\golden_flow.rep "--flow-demo --synth-input" "--flow-demo"
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
rem ★**記録側に --synth-input を渡す** (M65g)。プレイヤー (Watcher* 3 本) の視点角は
rem   GetMouseDelta を積分した登録フィールドで、無入力だと恒常ゼロのまま「配線ミスが
rem   記録側と検証側で対称に起きて一致してしまう」= mp ペアと同じ穴が開く。合成入力は
rem   生マウスデルタも流す (M64a) ので、これで視点・移動・足音・敵との接触までが
rem   Debug/Release のビット一致として機械検証される。
rem   ★検証側には渡さない (合成入力は .rep に記録済み。mp ペアと同じ理由)
rem シーンはコードから毎回組み直す (parts / physics / joints と同じ流儀)
:job_acoustic
if exist cache\acoustic_showcase.scene.json del /q cache\acoustic_showcase.scene.json
call :chain cache\golden_acoustic.rep "--acoustic-demo --synth-input" "--acoustic-demo"
exit /b %ERRORLEVEL%

rem M75f: 8 ペア目 = ゲーム内 UI のウィジェット (--ui-demo)。UIToggle.isOn / UISlider.value はハッシュ対象で、
rem UIInteractionState (hovered / pressed / clicked / focused / changed) と合わせて「押下の泡立ち /
rem ToggleGroup の規則 / Slider の飛び・ドラッグ・掴んだ位置・キー / 操作不可」が Debug/Release の
rem ビット一致として検査される。
rem ★記録側にだけ --ui-demo-input (UiDemoScriptInput の台本) を渡す。無入力だと UI は一度も押されず、
rem   配線ミスが記録側と検証側で対称に起きて一致してしまう (acoustic の --synth-input と同じ理由)。
rem   検証側には渡さない (台本の入力は .rep に記録済み)
rem シーンはコードから毎回組み直す (保存済みが残っていると Editor はそちらをロードする)
:job_ui
if exist cache\ui_showcase.scene.json del /q cache\ui_showcase.scene.json
call :chain cache\golden_ui.rep "--ui-demo --ui-demo-input" "--ui-demo"
exit /b %ERRORLEVEL%

rem ---- 破壊 (M80)。資産はファイルを作らずビルド時にメモリ上で焼く (fracture://demo_*)。
rem Debug と Release が独立に焼いて replay が一致すること自体が、分割コアの構成間一致
rem (sub-02 の契約) を実行経路で証明する。着弾で箱・壁・スキン腕が割れて塊が分離し
rem (FractureSystem、sub-07/08/10)、固定壁は M80l の GameLogic スクリプト
rem (FractureDamageProbe) が着弾より先に ApplyFractureDamage で割って onBreak を受け取る —
rem ABI v22 の Debug/Release divergence もこのペアで検知する
rem シーンはコードから毎回組み直す (parts / physics / joints と同じ流儀)
:job_fracture
if exist cache\fracture_showcase.scene.json del /q cache\fracture_showcase.scene.json
call :chain cache\golden_fracture.rep "--fracture-demo" "--fracture-demo"
exit /b %ERRORLEVEL%

rem ---- ナビメッシュ (M82)。ナビメッシュはファイルを作らずシーン構築時にメモリ上で焼く (nav://demo)。
rem Debug / Release / Server.exe が独立に焼いて replay が一致すること自体が、ベイク (Recast) の
rem 構成間一致を実行経路で証明する。Agent 6 体が段差・坂・壁を越えて歩き (dtCrowd、CharacterController.moveInput)、
rem GameLogic の NavDemoDriver が 300 tick で目的地を出発点へ戻す (汎用フィールド ABI の書き込み) —
rem snapshot stress (毎 tick の 撮影 -> 復元 -> 再撮影) は Nav 節 (dtCrowd・スロット表) の往復も叩く。
rem 以降のサブ (Obstacle / Modifier / Link) もこのジョブに載せる
:job_nav
if exist cache\nav_showcase.scene.json del /q cache\nav_showcase.scene.json
call :chain cache\golden_nav.rep "--nav-demo" "--nav-demo"
exit /b %ERRORLEVEL%

rem ---- 知覚 (M83)。侵入者が柱のある広場を四角く歩き、見張り 3 体が視覚・遮蔽・接触・聴覚・ダメージで気付いて
rem 振り向く。侵入者の移動と ReportNoise / ReportDamage は GameLogic の PerceptionDemoIntruder、振り向きは
rem PerceptionDemoGuard (PerceptionGet の結果を LocalTransform と登録フィールドへ書き戻す) — ABI v25 の
rem Debug/Release divergence もこのジョブで検知する。知覚の状態は全部 AIPerception コンポーネントにあるので、
rem snapshot stress は World 節の往復だけで足りることもここで確かめる
:job_perception
if exist cache\perception_showcase.scene.json del /q cache\perception_showcase.scene.json
call :chain cache\golden_perception.rep "--perception-demo" "--perception-demo"
exit /b %ERRORLEVEL%

rem ---- ビヘイビアツリー (M85)。見張り 2 体 (NavMeshAgent + AIPerception + BehaviorTree + AnimatorController) が巡回し、
rem プレイヤー役を見つけて追跡・見失って捜索・巡回へ戻る (assets\ai\guard.bt.json)。ナビメッシュはメモリ上で焼く (nav://bt-demo)。
rem GameLogic の BtDemoDriver がプレイヤー役を動かし、見張り A の BB を BtGetBlackboard で読み、僚機 B の巡回ルートを
rem BtSetBlackboard で渡す (ABI v27)、BtProbeTask (C++ のタスク) が発見の一拍を作り、RotateTo が std::sin / cos / atan2 を通る —
rem Debug / Release / Server.exe でビット一致すること自体がそれらの構成間一致の証明になる。C# のタスクは入れない (C# レーンは被覆外)。
rem snapshot stress は BT 節 (実行状態・BB・イベントの配送待ち) の往復も叩く
:job_bt
if exist cache\bt_showcase.scene.json del /q cache\bt_showcase.scene.json
call :chain cache\golden_bt.rep "--bt-demo" "--bt-demo"
exit /b %ERRORLEVEL%

rem ---- 骨アニメ (M89)。生成素材 anim_test.glb (Y-up) / anim_test_zup.glb (Z-up) の 2 体を
rem assets\anims\anim_test.controller.json が骨クリップで Idle -> Walk -> Run -> Attack と hasExitTime で回す。
rem ステートの長さは骨クリップ (主 SkinnedMesh のモデル) から引くので、AnimatorController (ハッシュ対象) の
rem 遷移の tick が「骨クリップの長さを Debug / Release / Server.exe が同じに読めたか」の検査になる。
rem ポーズプログラム (SkinnedMesh の尾部) はハッシュに入らないが snapshot には載るので、stress がその往復を叩く。
rem 以降の M89 のサブ (ブレンドツリー / イベント / ルートモーション / IK) もこのジョブに載せる
:job_anim
if exist cache\anim_showcase.scene.json del /q cache\anim_showcase.scene.json
call :chain cache\golden_anim.rep "--anim-demo" "--anim-demo"
exit /b %ERRORLEVEL%

rem ---- jobs A/B (M90g、ADR-028) ----
rem sim の並列化 (CPU 粒子 / Perception / PartFollow / IK) が「直列と 1 ビットも違わない」ことの
rem 実シーンでの証明。直列 (--no-jobs) で録った .rep を、ワーカーを起こした Debug / Release で
rem 毎 tick のワールドハッシュと照合する。.rep のヘッダの jobs ビットは違うがハッシュ列は同一のはず
rem (verify はヘッダの jobs ビットを見ない)。IK を含むシーンは無いので IK は SimParallelSelfTest が受け持つ。
rem 部位追従のペアは :job_parts の中で回す (保存済みシーンの再生成と競合しないため)
:job_jobsab
call :jobsab_one cache\jobsab_particle.rep "--particle-demo --particle-backend cpu"
if errorlevel 1 exit /b 1
call :jobsab_one cache\jobsab_perception.rep "--perception-demo"
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

rem M72b: 分岐 (What-if)。「戻って再開しても元の未来が分岐として残る / 同じ入力なら畳まれる /
rem 編集して再開すると分岐点で乖離し、戻っても編集が残る / 元の分岐へ切り替えると元の N と
rem 一致する」をライブのフレームループ上で実走する。Debug/Release 両方で回す
:job_whatifdebug
bin\x64\Debug\Editor.exe --whatif-selftest 400 %MYE_EXTRA_ARGS% || exit /b 1
exit /b 0

:job_whatifrelease
bin\x64\Release\Editor.exe --whatif-selftest 400 %MYE_EXTRA_ARGS% || exit /b 1
exit /b 0

:job_rules
pwsh -NoProfile -ExecutionPolicy Bypass -File tools\check_rules.ps1 || exit /b 1
exit /b 0

rem ---------------------------------------------------------------- :jobsab_one
rem jobs A/B の 1 シーン。%1 = .rep パス / %2 = シーン引数 (record と verify で同じ)
:jobsab_one
setlocal
set "AREP=%~1"
set "AARG=%~2"
echo === jobs A/B record %AREP% : Debug, serial (--no-jobs), %TICKS% ticks ===
bin\x64\Debug\Editor.exe %AARG% --no-jobs --replay-record %AREP% --replay-ticks %TICKS% --replay-fast %MYE_EXTRA_ARGS% || (
    echo [FAIL] jobs A/B record: %AREP%
    endlocal & exit /b 1
)
echo === jobs A/B verify %AREP% : Debug, workers on ===
bin\x64\Debug\Editor.exe %AARG% --replay-verify %AREP% %MYE_EXTRA_ARGS% || (
    echo [FAIL] jobs A/B Debug verify: %AREP%
    endlocal & exit /b 1
)
echo === jobs A/B verify %AREP% : Release, workers on ===
bin\x64\Release\Editor.exe %AARG% --replay-verify %AREP% %MYE_EXTRA_ARGS% || (
    echo [FAIL] jobs A/B Release verify: %AREP%
    endlocal & exit /b 1
)
endlocal & exit /b 0

rem ---------------------------------------------------------------- :chain
rem 1 シーンぶんの record → stress 付き Debug verify → Release verify。
rem   %1 = .rep パス / %2 = record 側のシーン引数 / %3 = verify 側のシーン引数
rem stress 付き verify (--snapshot-stress 37、M52d) はハッシュ照合が素の verify と
rem 同一機構なので完全上位互換 — 素の Debug verify は回さない。ここが赤い =
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
rem M81a: 窓も GPU も無いヘッドレス Server.exe (Release) でも同じ .rep を全 tick 照合する。
rem Debug の Editor が録った .rep を Release の Server が通す = 構成間 + 実行形態間のビット一致。
rem Server は GPU を持たず終了前に d3d11 等がロードされていないことを自己検査する (exit 2)
echo === verify %CREP% : Release, headless Server.exe ===
bin\x64\Release\Server.exe %CVER% --replay-verify %CREP% %MYE_EXTRA_ARGS% || (
    echo [FAIL] headless Server verify: %CREP%
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
rem                        "--acoustic-demo" / "--ui-demo --ui-demo-input" / "--fracture-demo" / "--nav-demo" /
rem                        "--perception-demo" / "--bt-demo" / "--anim-demo")
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
