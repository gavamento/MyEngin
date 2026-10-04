# sub-04: NavMesh の半透明の塗り (エリア色) と golden `nav`

- 依存: sub-10 (sub-03 の後に sub-10 を挟む)
- 状態: OK (commit 4c88d83)
- 往復: 1

## やること
spec 2. #13 (Q4 のユーザー回答で範囲入り)、4.3 (塗り)、受け入れ条件 17。

1. **描画レーンの追加**: いまの sim → 描画のデバッグ経路は線分の `DebugLineCmd` だけ (`Rendering\DebugDraw.h`、`EngineLoop.cpp:197` の `debugLines`、`RenderSystem.cpp:1804-1815` が `EditorLinePass` で描く)。三角形の塗りを運ぶ口を足す。形は coder が決めてよいが、次を満たすこと:
   - 生の D3D 型を Renderer の外へ出さない (spec 4.4 / AGENTS.md 4.1)。新しいパスは `src\Engine\Renderer\Passes\` に置く。
   - 半透明 (アルファブレンド)、深度テストあり・深度書き込みなし、床と Z ファイトしないためのオフセット (Recast の DebugUtils と同じく NavMesh 自体を少し浮かせるか、深度バイアス)。
   - **NavMesh が変わらない tick では三角形を作り直さない** (NavSystem の世代番号などで判定。TileCache の更新で変わったタイルだけでもよい)。作り直しの所要時間と三角形数をログで測る。resim 中は積まない。
   - 描画パスが 2 系統ある (ADR-007) なら両方で出る。
2. DebugUtils の `duDebugDraw` 実装を「三角形 → 塗りレーン、線 → 既存の線レーン」に振り分ける (sub-02 では三角形を輪郭線へ落としていた)。色はエリアごと (0 = 水色系の Unity 風、Jump などは別色、ユーザー定義エリアは固定パレット)。
3. 表示切り替えに「塗り」を足す (Surface のインスペクタ。既定 on)。Runtime でも出せる。
3b. (sub-02 VERDICT、spec 2. #18 / 4.3) **編集中 (非 Play) の SceneView にも塗り + 輪郭を出す**。sim の tick (`NavSystem::Update` は stepSim でしか走らない) に頼らない表示専用の読み込み / 線の作り方を足す (例: Editor が持つ表示用の NavSystem か、Engine 側の「.mnav → 描画用の線・三角形」関数を Editor から呼ぶ)。Bake の完了直後に Play せず表示が更新されること。sim 状態・ハッシュには触れない。
3c. **Surface の範囲箱ギズモ** (spec 4.3、`center` / `size` のローカル AABB を SceneView に描く。既存のギズモ描画箇所に合わせる)。sub-02 で割り当て漏れだった分。
3d. Agent の経路線 (毎 tick 作り直す) を golden `nav` に含めるかはこのサブで決めて理由を書く (sub-03 申し送り)。実効の坂上限の表示は sub-10 へ移した。
4. **golden `nav`**: `--nav-demo` の固定 tick を NavMesh の塗り + 輪郭 + Agent 経路の表示ありで撮る golden を `tests\golden\` に追加し `tools\shot_verify.bat` に載せる。既存 golden の作り方 (撮影構成、許容差、CI の WARP でのスキップ条件 `MYE_SHOT_SKIP_*`) に合わせ、WARP と実 GPU で半透明の混合が割れるなら既存の前例どおり許容差かスキップを決めて理由を書く。

## やらないこと (このサブでは)
- Obstacle / Modifier / Link の描画 (各サブで足す。golden `nav` はそこで更新)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\DebugDraw.h` (三角形のコマンド)、`src\Engine\Engine\Loop\EngineLoop.cpp` / `TickRunner.cpp` (レーンの受け渡し)、`src\Engine\Renderer\Passes\` (新パス + シェーダ `assets\shaders\`)、`RenderSystem.cpp`、`src\Engine\Engine\Navigation\NavDebugDraw.*`、Inspector、Localization、`tests\golden\`、`tools\shot_verify.bat`
- 手本: `EditorLinePass` (`src\Engine\Renderer\Passes\EditorLinePass.h:22-57`)

## 受け入れ条件 (このサブ)
1. (spec 17) `Runtime.exe --nav-demo --screenshot` で、床の上に半透明のエリア色の塗り + 輪郭線が見え、床と Z ファイトしない (画像パスを SELF_EVAL に)。Editor の SceneView でも同じ。
2. NavMesh 不変の tick で三角形を作り直していないことを、ログ (作り直し回数) で示す。
3. golden `nav` が shot_verify に載って PASS。既存 golden は不変。
3b. 編集中 (Play していない) の SceneView で、Bake 直後に塗り + 輪郭と Surface の範囲箱が見える。— Editor の `--screenshot` (一時プローブ可) または手動確認の画像パス
4. NavMesh 系の無いシーンでは描画コストが増えない (空のレーンでパスを呼ばない)。replay_verify 全 PASS (描画は sim 外だがレーン追加で sim を触っていないことの確認)、0 警告、check_rules 0 (シェーダ定数を共有するなら rule 9)。

## 検証コマンド
- 両構成ビルド (`/p:MyeWarnAsError=true`)、`--selftest` 両構成、`tools\shot_verify.bat`、`tools\replay_verify.bat`、`tools\check_rules.ps1`
- `bin\x64\Release\Runtime.exe --nav-demo --screenshot` (一時スクショ検証の道具の使い分けはメモリ「screenshot-probe-recipes」)

## 実装メモ (coder が追記)

### round 1
- 経路: 塗り・輪郭・タイル境界は新設の `NavDebugView` (NavDebugDraw.h/.cpp) が `.mnav` から自前で読んで作る。EngineLoop が描画フレームごとに `Refresh(world)` を呼び (OnRenderViews の前)、`RenderSystem::navView` 経由で `DrawParticlesAndDebug` が `NavFillPass` (塗り) → `EditorLinePass` (輪郭、既存の線パス) の順に描く。ForwardPath / DeferredPath の両方が `DrawParticlesAndDebug` を通る。
- Surface の構成 (entity, navAsset, 表示フラグ 3 種) が前回と同じなら何も作り直さない。作り直し回数と所要時間は `[nav] debug view rebuilt (#N)` のログと `GetStats()`。
- 新パスは editor_line.hlsl を流用 (位置 + 色をそのまま出す) = 新シェーダ・.meta 無し。床とは持ち上げ (塗り 2 cm / 線 3 cm) + 傾斜つき深度バイアス -1.5 で Z ファイトを避ける。
- golden `nav` = `--nav-demo` frame 120、tol=3、`MYE_SHOT_SKIP_NAV` の囲い付き。Agent の経路線は golden に含める (3d の決定、下記)。
- 検証結果 (Release/Debug/shot_verify/replay_verify/check_rules/画像) は SELF_EVAL を参照。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-04)。描画フレーム側のレーンへの逸脱を採用した (spec 4.3)。経路線を golden に含めること、tol=3、`MYE_SHOT_SKIP_NAV` の囲いも採用。should: Bake ボタンを実際に押した直後に、編集中の SceneView の表示が更新されるかは reviewer が観察する (SelfTest で GUID を差し替えて確認済みで、GUI 操作は未観測)。実行時の NavMesh の変化の表示は sub-05 の やること 8 へ。nit: `--nav-demo` の Inspector で『.mnav を読めない』と誤った警告が出る (メモリ上の資産を見ていない) → sub-05 の やること 9 へ。
