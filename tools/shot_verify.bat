@echo off
rem shot_verify.bat — 決定的スクリーンショットの回帰テスト (M52c、engine_spec.md 11.3 系)
rem   tests\golden\*.png と撮り直した tests\actual\*.png をピクセル比較する。
rem   差があれば diff ヒートマップを tests\actual\<name>.diff.png に残して exit 1。
rem
rem   使い方:  tools\shot_verify.bat            … 照合する (CI と同じ)
rem            tools\shot_verify.bat --update   … golden を撮り直す (差分を目視してからコミット)
rem
rem ★撮影は必ず --warp (ソフトウェアラスタライザ) で行う。実 GPU と WARP は同じシーンでも
rem   maxDiff=2 で 7 割の画素が違う (M52c 実測。FXAA/トーンマップの丸め) ので、
rem   golden とランナーのラスタライザを揃えないと回帰テストにならない。
rem   逆に Debug と Release は WARP 同士でビット一致する (実測) ので撮影は Release だけ。
rem
rem ★--font-embedded も必須。フォントアトラスは assets\fonts → システム日本語 TTF の順で
rem   探すので、golden を撮った機械と CI ランナーで別のフォントになりうる
rem   (英語版 Windows Server には日本語 TTF が無い)。内蔵 8x8 に固定して機種差を消す。
rem
rem ★--no-fxaa も撮影条件の一部 (M52c 追補)。WARP 同士でも **OS ビルドが違うと** 出力は
rem   ビット一致しない: 開発機 Win11 (d3d10warp/d3dcompiler 10.0.26100) と CI の
rem   windows-2022 (同 10.0.20348) で下記の実測が出た (AVX-512 はどちらの CPU にも無し)。
rem
rem       構成            maxDiff  tol=2 超え画素
rem       素の描画のみ          1       0        (--no-postfx)
rem       + トーンマップ        3    1601        (--no-fxaa、deferred。差は全部ちょうど 3)
rem       + ブルーム            3       -        (--no-bloom の有無で数字が動かない = 無関係)
rem       + FXAA               35    1443        (既定。forward でも 17)
rem
rem   ラスタライズとライティングの素の出力は 2 台で **最大 1 レベル** しか違わない。
rem   その 1 レベルを FXAA が一桁に増幅する — 近傍輝度のしきい値で分岐する演算なので、
rem   ULP 差が分岐を反転させるとブレンド係数ごと変わるため (差はエッジと路面ラインに乗る)。
rem   つまり「tol を 35 まで緩める」のは FXAA 1 パスのために画面全体の検出力を捨てる取引で、
rem   撮影から FXAA を外せば tol=3 の厳格運用がランナー上でも成立する。
rem   代わりに FXAA 自体の被覆は **ローカル限定の 10 枚目** (demo_forward_fxaa、tol=0 の
rem   ビット一致検査) で確保する。CI は MYE_SHOT_SKIP_FXAA=1 でこの 1 枚を飛ばす。
rem
rem ★TAA (M55d) も同じ扱い。近傍 min/max のクランプは FXAA と同型の「しきい値で分岐する」
rem   演算で、しかも履歴でフレーム間に蓄積する。増幅率は測っていない — 測る前に CI へ
rem   載せると「理由不明で赤い」1 枚が増えるだけなので、demo_render_taa もローカル限定の
rem   tol=0 検査 (MYE_SHOT_SKIP_TAA=1 で飛ばす) にしてある。
rem
rem ★ビルドはしない。replay_verify.bat の後に回す前提 (CI もその順序)。
setlocal enabledelayedexpansion
cd /d "%~dp0.."

set UPDATE=0
if /i "%~1"=="--update" set UPDATE=1

set REL=bin\x64\Release
if not exist %REL%\Runtime.exe (
    echo [shot_verify] %REL%\Runtime.exe not found - build Release first ^(tools\replay_verify.bat^) & exit /b 1
)
if not exist %REL%\Editor.exe (
    echo [shot_verify] %REL%\Editor.exe not found - build Release first ^(tools\replay_verify.bat^) & exit /b 1
)

rem チャンネル差の許容。同一マシンなら 0 で一致する。3 は「開発機と windows-2022 ランナーで
rem 実測した最大差がちょうど 3 (deferred のトーンマップ)」という数字そのもので、余裕は 1 レベル
rem しかない。数字は毎回ログに出す — 隠さないことがこのテストの価値。
rem
rem ★tol は 3 種類ある。**どれも実測値から決めていて、赤くなったから緩めた数字は 1 つも無い**:
rem   tol=3  … 既定。ラスタ + ライティング + トーンマップの丸め (実測 maxDiff 1〜3)
rem   tol=12 … demo_terrain_deferred の 1 枚だけ。**異方性フィルタは実装依存**で
rem            WARP のビルド違いで一致しない (実測 maxDiff=8)。詳細は該当 call の直前
rem   tol=0  … ローカル限定の 5 枚 (fxaa / taa / ssr / froxel / fog)。どれも**離散的に分岐する**
rem            演算で、1 ULP の差が分岐を反転させると数十画素が丸ごと飛ぶ。この形は
rem            tol をいくつにしても守れない (上げると本物の回帰も一緒に見逃す) ので、
rem            ランナーでは撮らず、開発機でのビット一致だけを主張する
set TOL=3
if defined MYE_SHOT_TOL set TOL=%MYE_SHOT_TOL%

set GOLDEN=tests\golden
set ACTUAL=tests\actual
if not exist %GOLDEN% mkdir %GOLDEN%
if not exist %ACTUAL% mkdir %ACTUAL%

rem 撮影条件はこの 2 行に固定する (golden を撮った条件と照合の条件が食い違わないように)。
rem --screenshot 指定で EngineLoop が決定的撮影モードに入る = frame 番号 == tick 番号
set SHOTBASE=--warp --no-audio --font-embedded --width 960 --height 540 --frames 6 --shot-frame 3
set SHOT=%SHOTBASE% --no-fxaa
set TOLNOW=%TOL%

rem ---- コードから組み直すシーン (replay_verify と同じ流儀) ----
rem parts はモデル由来のサブアセット ID が絶対パスのハッシュなのでシーンファイルを
rem コミットできない。flow は builtin のみだが正解はコード側なので同じく生成物
echo === build generated scenes (parts / flow) ===
set PARTS_SCENE=cache\parts_showcase.scene.json
if exist %PARTS_SCENE% del /q %PARTS_SCENE%
%REL%\Editor.exe --parts-demo --save-scene-on-start --frames 2 --no-audio --warp || exit /b 1
if not exist %PARTS_SCENE% (echo [shot_verify] parts showcase scene was not written & exit /b 1)

set FLOW_TITLE=assets\scenes\flow_title.scene.json
if exist %FLOW_TITLE% del /q %FLOW_TITLE%
%REL%\Editor.exe --flow-demo --frames 2 --no-audio --warp || exit /b 1
if not exist %FLOW_TITLE% (echo [shot_verify] flow title scene was not written & exit /b 1)

rem M54a: 描画ショーケース (--render-demo) は Runtime がコードから毎回組む。保存済みが
rem       cache\ に残っていると RuntimeMain の exists() 経路へ落ちて **golden が静かに変わる**
set RENDER_SCENE=cache\render_showcase.scene.json
if exist %RENDER_SCENE% del /q %RENDER_SCENE%

rem M58c: 地形ショーケース (--terrain-demo) も同じ理由でコードから毎回組む
set TERRAIN_SCENE=cache\terrain_showcase.scene.json
if exist %TERRAIN_SCENE% del /q %TERRAIN_SCENE%

set FAILED=0
set SHOTS=0

rem ---- 19 本。既定デモの 2 経路 (Forward / Deferred) + 生成シーン 2 本 + UI プローブ
rem      + 描画ショーケースの 2 経路 (M54a) + 地形 (M58c) + 物理 (M59l) + 関節 (M60k)
rem      + 霧 (M57追補) + パーティクル 2 経路 (M63a) + 音響 2 経路 (M65e)
rem      + ローカル限定 4 本 (ssr / fxaa / taa / froxel) ----
rem RT デモは WARP では重すぎるので CI 対象外 (ローカル任意)
call :shot demo_forward
call :shot demo_deferred --deferred
call :shot parts --scene %PARTS_SCENE%
call :shot flow_title --scene %FLOW_TITLE%
call :shot ui_probe --scene assets\scenes\ui_probe.scene.json

rem ---- 6/7 枚目 (M54a): 描画ロードマップ M54〜M58 の被写体が揃ったショーケース。
rem      既存 5 枚は平行光 1 本だけで組まれていて点光源もスポットも無いため、局所ライトの影 /
rem      デカール / SSR / プローブ / フロクセル / 地形は **どれも既定でピクセル不変** =
rem      「壊れても誰も気づかない」。この 2 枚がそれ以降 27 サブの回帰の土台になる
call :shot demo_render_forward --render-demo
call :shot demo_render_deferred --render-demo --deferred

rem ---- 8 枚目 (M58c): 地形。**--render-demo に地形を足さない**のがこの 1 枚の存在理由 —
rem      足すと既存 golden 2 枚 (demo_render_*) が動き、同じ Wave の M54/M55 ブランチと
rem      PNG (マージ不能なバイナリ) で衝突する。専用シーンなら新設 1 枚で済む
rem
rem ★この 1 枚だけ tol が違う (12)。**異方性フィルタリングは実装依存**で、開発機の WARP
rem   (10.0.26100) とランナーの WARP (10.0.20348) で結果が一致しない。地形は s0 の
rem   D3D11_FILTER_ANISOTROPIC (MaxAnisotropy=4) を借り、しかも 4 レイヤ x (albedo+normal)
rem   = 8 サンプル/画素を混ぜたうえで PerturbNormal の微分 TBN がその差をライティングへ増幅する。
rem   実測 (CI run 32622063559): **maxDiff=8 / tol=3 超え 252 画素 / 何かしら違う 62879 画素**。
rem   ★差分ヒートマップは **遠景 (斜め入射) ほど密で手前ほど薄い** = 異方比が大きいほど食い違う、
rem     という異方性フィルタの署名そのもの。既存の床つきシーン (demo_render_deferred) が
rem     maxDiff=3 で収まるのは、画面に占める斜め入射の地表の割合が小さいから。
rem   tol を上げる代わりに三線形へ落とすことも考えたが、それは**テストのために product の
rem   絵をぼかす**取引になる (--warp / --font-embedded が変えているのは撮影条件であって
rem   描画そのものではない)。実測 8 に 4 レベルの余裕を足した 12 を採る — 本物の地形回帰
rem   (LOD の隙間 / スプラット重みの狂い / レイヤ欠落) は 12 レベルで収まる変化ではない。
set TOLNOW=12
call :shot demo_terrain_deferred --terrain-demo --deferred
set TOLNOW=%TOL%

rem ---- 9 枚目 (M56d): SSR。--render-demo の反射床パッチ (rdemo_mirror、粗さ 0.10) に
rem      柱と灯りが映り込む。**この 1 枚が SSR の唯一の自動被覆**で、既定 off の SSR は
rem      これが無いと壊れても全 golden が緑のままになる。
rem      撮影条件は demo_render_deferred と --ssr だけ違う = 差分がまるごと SSR の寄与。
rem      ★**当初 CI 判定 (tol=3) に載せたが、実測で降格した** (CI run 32622063559)。
rem        SSR の交差判定はレイが当たったか外れたかで**離散的に分岐する**演算で、
rem        1 ULP の深度差が hit/miss を反転させると、その画素は反射色 ⇔ IBL フォールバックへ
rem        丸ごと飛ぶ。実測は **maxDiff=95 / tol=3 超えはわずか 30 画素** — 「広く薄く」ではなく
rem        「狭く極端に」違う形で、FXAA (1 → 35 へ増幅) と同型。
rem        ★差分ヒートマップも反射床と柱の輪郭に**孤立した点**が散る = 分岐反転の署名。
rem        **この形は tol をいくつにしても守れない** — 本物の SSR 回帰も同じ「数十画素が
rem        大きく飛ぶ」形で出るので、tol を 95 まで上げると検出力がゼロになる。
rem        よって FXAA / TAA / froxel と同じローカル限定 tol=0 の枠へ移す。
rem        ローカルでは maxDiff=0 のビット一致なので降格しても検出力は落ちない
rem        (落ちるのは「ランナー上でも SSR が同じ絵を出す」という主張だけ)。
if defined MYE_SHOT_SKIP_SSR goto :skip_ssr
set TOLNOW=0
call :shot demo_render_ssr --render-demo --deferred --ssr
set TOLNOW=%TOL%
:skip_ssr

rem ---- 10 枚目 (統合契約の予約 3 では 10 番): FXAA を通した 1 枚。機種差が乗るので照合はローカルだけ (tol=0 の
rem      ビット一致)。CI は MYE_SHOT_SKIP_FXAA=1 を立てて飛ばす。
rem      golden は --update で一緒に撮り直される (CI 側で撮ることは無い)
if defined MYE_SHOT_SKIP_FXAA goto :skip_fxaa
set SHOT=%SHOTBASE%
set TOLNOW=0
call :shot demo_forward_fxaa
set SHOT=%SHOTBASE% --no-fxaa
set TOLNOW=%TOL%
:skip_fxaa

rem ---- 11 枚目 (統合契約の予約 3 では 11 番、M55d): TAA を通した 1 枚。
rem      FXAA と同じ理由でローカル限定 (tol=0 のビット一致)。CI は MYE_SHOT_SKIP_TAA=1 で飛ばす。
rem      TAA は Deferred のみ (画面速度が GBuffer RT4 にしかない) なので --deferred が要る。
rem      撮影条件は --no-fxaa のまま = demo_render_deferred との差が **TAA だけ** になる
if defined MYE_SHOT_SKIP_TAA goto :skip_taa
set TOLNOW=0
call :shot demo_render_taa --render-demo --deferred --taa
set TOLNOW=%TOL%
:skip_taa

rem ---- 12 枚目 (統合契約の予約 3、M57d): フロクセル・ボリュメトリック。
rem      FXAA / TAA と同じ理由でローカル限定 (tol=0 のビット一致)。CI は MYE_SHOT_SKIP_FROXEL=1 で飛ばす。
rem      ★M57c ではこの枠を撮らなかった — 積分結果を読む者が 1 人も居ない段階で撮ると
rem        demo_render_deferred と tol=0 でビット一致する「同じ絵の 2 枚目」にしかならず、
rem        以後それが動いたときに原因が機能なのか撮影条件なのか切り分けられなくなるため。
rem        絵が初めて変わる M57d がこの枠を撮る。
rem      合成は Deferred 光パスの t15 なので --deferred が要る。撮影条件は --no-fxaa のまま =
rem      demo_render_deferred との差が **フロクセルだけ** になる
if defined MYE_SHOT_SKIP_FROXEL goto :skip_froxel
set TOLNOW=0
call :shot demo_render_froxel --render-demo --deferred --froxel
set TOLNOW=%TOL%
:skip_froxel

rem ---- 13 枚目 (M59l): 物理ショーケース。**frame 120 (2 秒) で撮る 2 枚のうちの 1 枚目**
rem      (もう 1 枚は 14 枚目の joints)。他の 12 枚は frame 3 = ほぼ初期配置で、それは
rem      「描画が壊れていないか」を見る撮り方。
rem      物理は 3 tick では 1 ミリも動いていないので、同じ撮り方をすると M59 で足した数式
rem      (空力 / 浮力 / マグヌス / ジャイロ / 材料 / CCD) が 1 つも絵に出ない = 守るものが無い。
rem      120 tick 回すと 羽根のひらひら / 鉄球の着地 / 紙飛行機の滑空 / カーブボールの曲がり /
rem      浮きの喫水 / 箱の山 が全部同じフレームに乗る。
rem
rem ★これは**シミュレーションの機種独立性を絵で検査する 1 枚**でもある (14 枚目の joints と
rem   合わせて 2 枚)。他の 12 枚はどれも tick 3 なので、sim が機種で割れても絵はほとんど
rem   変わらない。120 tick ぶん
rem   積み上がった状態を照合するということは、ランナーの sim が開発機と 1 ビットでも
rem   違えば必ず赤くなるということ (scalar float + /fp:precise の契約が守られていれば一致する)。
rem   ★もしランナーで赤くなったら、**tol を上げて誤魔化さないこと** — 意味のある後退先は
rem     「--shot-frame 3 に落として描画だけの検査に戻す」か「tol=0 のローカル限定枠へ移す」。
rem   経路は既定の Forward (このシーンは平行光 1 本だけで deferred 固有の被写体が無い)
set PHYS_SCENE=cache\physics_showcase.scene.json
if exist %PHYS_SCENE% del /q %PHYS_SCENE%
set SHOT=--warp --no-audio --font-embedded --width 960 --height 540 --frames 123 --shot-frame 120 --no-fxaa
call :shot physics --physics-demo
set SHOT=%SHOTBASE% --no-fxaa

rem ---- 14 枚目 (M60k): 関節ショーケース。**physics と同じく frame 120 で撮る 2 枚目**。
rem      M60 が足したものは「複数の剛体を繋ぐ層」なので、frame 3 = ほぼ初期配置では
rem      拘束が 1 行も仕事をしていない絵にしかならない。120 tick 回すと
rem      二重振り子の軌道 / ロープの張り / ドアのリミット / モータのクランク /
rem      スライダのエレベータ / 固定の片持ち / 破断した桟橋 / 複合 L 字 / 凸包の山 /
rem      倒れたラグドール / 走る車 が全部同じフレームに乗る。
rem
rem ★physics.png と同じく**シミュレーションの機種独立性を絵で検査する 1 枚**。
rem   このシーンは substeps 16 なので 120 tick = 1920 サブステップぶん積み上がっており、
rem   1 ビットの差でも必ず絵に出る。赤くなったときの後退先も physics と同じ
rem   (frame 3 へ落とすか tol=0 のローカル限定枠へ移す。**tol は上げない**)。
rem ★車の運転入力は C++ スクリプト (VehicleDemoDriver) が書くので、GameLogic.dll が
rem   焼けていないと車だけ止まった絵になる = golden が静かに変わる。
rem   replay_verify が先にビルドしている前提なのは physics と同じ
set JOINT_SCENE=cache\joint_showcase.scene.json
if exist %JOINT_SCENE% del /q %JOINT_SCENE%
set SHOT=--warp --no-audio --font-embedded --width 960 --height 540 --frames 123 --shot-frame 120 --no-fxaa
call :shot joints --joint-demo
set SHOT=%SHOTBASE% --no-fxaa

rem ---- 15 枚目 (M57追補): 霧のショーケース。**GPU パーティクル描画経路と VfxRenderer
rem      (Sprite / Trail / TextMesh) の唯一のピクセル被覆**。
rem      それまで GPU バックエンドは --screenshot で撮る手段が無く (エディタ GUI からしか
rem      選べなかった)、VFX 3 種は 14 枚のどれにも写っていなかったので、**どちらも壊れても
rem      全部緑のまま通る**状態だった。
rem
rem ★frame 120 で撮る (physics / joints と同じ理由)。frame 3 だとトレイルの点が 3 つしか
rem   無く粒子も数個で、守るものが絵に出ない。Rotator (GameLogic.dll) が 30 deg/s なので
rem   120 tick = 2 秒 = 60 度ぶんの弧が溜まる。**DLL が焼けていないとリボンが消える**
rem ★tol=0 のローカル限定。理由が 2 つ重なっている:
rem     (a) froxel の注入/積分は exp/pow を含む CS で、開発機とランナーの WARP の
rem         バージョンが違う (demo_render_froxel を tol=0 にした先例そのもの)
rem     (b) **GPU パーティクルの sim 自体が WARP 上の float 演算**なので、粒子位置が
rem         機種で動きうる (CPU バックエンドと違い sim が GPU に載っている)
rem   ★赤くなったから tol を上げる、はやらないこと。後退先は「--particle-backend を外して
rem     CPU 粒子で撮る」か「--froxel を外す」で、どちらも被覆を 1 段落とすだけで済む
if defined MYE_SHOT_SKIP_FOG goto :skip_fog
set FOG_SCENE=cache\fog_showcase.scene.json
if exist %FOG_SCENE% del /q %FOG_SCENE%
set SHOT=--warp --no-audio --font-embedded --width 960 --height 540 --frames 123 --shot-frame 120 --no-fxaa
set TOLNOW=0
call :shot fog --fog-demo --froxel --particle-backend gpu
set TOLNOW=%TOL%
set SHOT=%SHOTBASE% --no-fxaa
:skip_fog

rem ---- 16/17 枚目 (M63a): パーティクル表現ショーケース。
rem      **CPU バックエンドと GPU バックエンドを同じ被写体で突き合わせる唯一の golden**。
rem      M63 の 5 機能 (回転 / 速度ストレッチ / フリップブック / ライティング / 深度衝突) は
rem      全部既定 off なので、この 2 枚が無いと回帰検出がゼロになる。
rem
rem ★2 枚撮るのが要点。C1〜C3 は「CPU が畳んでインスタンスへ送る」経路と「GPU が VS で
rem   作る」経路の 2 実装を持ち、共有しているのは particle_billboard.hlsli の式だけ。
rem   1 枚だけだと**片方の実装が壊れても緑のまま通る** (M57追補 で GPU 描画経路が
rem   14 枚のどれにも写っていなかったのと同じ穴を、最初から開けないための 2 枚)。
rem ★2 枚は**意図的に食い違う** — 5 本目のエミッタ (depthCollision=1) は GPU 限定の
rem   見た目効果 (spec 7.5 の例外) なので、CPU 側では衝突しない。M63a 時点の実測で
rem   「深度衝突を切ると 2 枚は maxDiff=0 でビット一致」を確認済み = 食い違いは
rem   **この 1 エミッタだけ**に閉じている。差が P5 の外へ広がったら回転かフリップの
rem   ミラーが割れた証拠。
rem ★frame 120 で撮る (physics / joints / fog と同じ理由)。frame 3 では粒子が数個しか
rem   湧いておらず、回転もコマ送りも絵に出ない。
rem ★tol=0 のローカル限定。GPU 側は sim が WARP 上の float 演算なので粒子位置が機種で
rem   動きうる (fog 15 枚目と同じ理由 (b))。CPU 側も対で外す — 片方だけ CI に載せると
rem   「2 枚を突き合わせる」という存在理由が崩れる
if defined MYE_SHOT_SKIP_PARTICLE goto :skip_particle
set SHOT=--warp --no-audio --font-embedded --width 960 --height 540 --frames 123 --shot-frame 120 --no-fxaa
set TOLNOW=0
call :shot particle_cpu --particle-demo --particle-backend cpu
call :shot particle_gpu --particle-demo --particle-backend gpu
set TOLNOW=%TOL%
set SHOT=%SHOTBASE% --no-fxaa
:skip_particle

rem ---- 18/19 枚目 (M65e): 音響ショーケースの 2 経路。
rem      **M65 で初めてピクセルが動くサブの、唯一の回帰検出**。
rem      M65a〜M65d は「存在ゲートの内側なので既存 17 枚が maxDiff=0」を主張し続けてきたが、
rem      裏を返すと **M65 の成果物は 4 サブぶん 1 画素も golden に写っていなかった**。
rem      この 2 枚がその全部 (波面伝播 / 床材 / 残光 / 転送 / 合成) を初めて絵に固定する。
rem
rem ★★**真っ黒な画にしないことが 1 枚目の設計要件**。企画は「世界は真っ暗」だが、
rem   全画素が黒い golden は**機能が壊れて残光が 1 画素も出なくても一致して通る** =
rem   回帰検出がゼロになる。デモには弱い環境光 (Sun 0.35) + 設置光 1 個 (M65e) を
rem   置いてあり、合成を意図的に殺した A/B で **49,315 画素 / maxDiff=170 が動く**ことを
rem   実測済み (Forward / Deferred とも。差は 14 画素)。ここが 0 に近づいたら
rem   「暗くしすぎて golden が守るものを失った」合図。
rem ★frame 120 で撮る (physics / joints / fog / particle と同じ理由)。frame 3 では
rem   歩行者が 1 歩も踏み出しておらず、波も残光も箱の落下も絵に出ない。
rem   120 tick 回すと 足音 6 材質ぶんの波 / L 字を曲がった残光 / 金属板への着地 /
rem   設置光 が全部同じフレームに乗る。
rem ★**CI 判定に載せる** (tol=3。skip 5 本の仲間には入れない)。載せられる根拠:
rem   波面は整数チャンファ距離 = 機種非依存、残光の符号化は sqrt (IEEE-754 で正しく
rem   丸められる)、合成は lerp と乗算だけで**しきい値分岐もテンポラル蓄積も無い** —
rem   FXAA / TAA / SSR / froxel を降格させた「1 ULP が増幅する」機構がどこにも無い。
rem   ★もしランナーで赤くなったら tol を上げずに MYE_SHOT_SKIP_ACOUSTIC を立てること
rem     (ci.yml の env に 1 行足すだけ。囲いは下に用意してある)
rem ★2 経路撮るのは、残光の合成が **deferred_light.hlsl と forward_lit.hlsl の 2 実装**に
rem   あるため。共有しているのは acoustic_common.hlsli の式だけなので、1 枚だと
rem   片方が壊れても緑のまま通る (particle_cpu/gpu を 2 枚撮ったのと同じ理由)
if defined MYE_SHOT_SKIP_ACOUSTIC goto :skip_acoustic
set SHOT=--warp --no-audio --font-embedded --width 960 --height 540 --frames 123 --shot-frame 120 --no-fxaa
call :shot acoustic_forward --acoustic-demo
call :shot acoustic_deferred --acoustic-demo --deferred
set SHOT=%SHOTBASE% --no-fxaa
:skip_acoustic

echo.
if %UPDATE%==1 (
    echo [shot_verify] golden updated in %GOLDEN% - review the images before committing
    exit /b 0
)
if not %FAILED%==0 (
    echo [FAIL] screenshot regression: %FAILED% shot^(s^) differ ^(see %ACTUAL%\*.diff.png^)
    echo        expected images are in %GOLDEN%. If the change is intended:
    echo          tools\shot_verify.bat --update
    exit /b 1
)
if defined MYE_SHOT_SKIP_FXAA (
    echo [PASS] screenshot regression ^(%SHOTS% shots, warp, no-fxaa, tol=%TOL% + terrain at 12, physics/joints/fog/particle/acoustic at frame 120^)
) else (
    echo [PASS] screenshot regression ^(%SHOTS% shots, warp, tol=%TOL% + terrain at 12 + physics/joints/fog/particle/acoustic at frame 120 + fxaa/taa/ssr/froxel/fog/particle at tol=0^)
)
exit /b 0

rem -------------------------------------------------------------------- :shot
rem %1 = 名前 / %2.. = Runtime へ渡す追加引数 (シーン指定など)
rem 撮影条件は %SHOT%、判定の許容は %TOLNOW% を見る (呼ぶ側が組み立てる)
:shot
set NAME=%1
shift
rem M57追補: 4 トークンへ広げた (--fog-demo --froxel --particle-backend gpu で
rem   3 つでは足りず、末尾の "gpu" が黙って落ちて CPU の絵が撮れてしまう)
set EXTRA=%1 %2 %3 %4
set OUT=%ACTUAL%\%NAME%.png
if %UPDATE%==1 set OUT=%GOLDEN%\%NAME%.png
if exist "%OUT%" del /q "%OUT%"
set /a SHOTS+=1

echo === shot: %NAME% ^(tol=%TOLNOW%^) ===
%REL%\Runtime.exe %SHOT% %EXTRA% --screenshot %OUT%
rem ★"if errorlevel 1" は使わない — SEH で落ちた exit code (0xC0000005 等) は符号付きだと
rem   負なので「1 以上か」の判定が偽になり、クラッシュを PASS に混ぜてしまう (M52f)
if !ERRORLEVEL! NEQ 0 (
    echo [shot_verify] %NAME%: runtime exited with an error
    set /a FAILED+=1
    goto :eof
)
if not exist "%OUT%" (
    echo [shot_verify] %NAME%: no screenshot was written
    set /a FAILED+=1
    goto :eof
)
if %UPDATE%==1 goto :eof

if not exist "%GOLDEN%\%NAME%.png" (
    echo [shot_verify] %NAME%: no golden image - run "tools\shot_verify.bat --update" once and commit it
    set /a FAILED+=1
    goto :eof
)
%REL%\Editor.exe --img-diff %GOLDEN%\%NAME%.png %OUT% --tol %TOLNOW% --diff-out %ACTUAL%\%NAME%.diff.png
if !ERRORLEVEL! NEQ 0 set /a FAILED+=1
goto :eof
