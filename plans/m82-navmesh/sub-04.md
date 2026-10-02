# sub-04: NavMesh の半透明の塗り (エリア色) と golden `nav`

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること
spec 2. #13 (Q4 のユーザー回答で範囲入り)、4.3 (塗り)、受け入れ条件 17。

1. **描画レーンの追加**: いまの sim → 描画のデバッグ経路は線分の `DebugLineCmd` だけ (`Rendering\DebugDraw.h`、`EngineLoop.cpp:197` の `debugLines`、`RenderSystem.cpp:1804-1815` が `EditorLinePass` で描く)。三角形の塗りを運ぶ口を足す。形は coder が決めてよいが、次を満たすこと:
   - 生の D3D 型を Renderer の外へ出さない (spec 4.4 / AGENTS.md 4.1)。新しいパスは `src\Engine\Renderer\Passes\` に置く。
   - 半透明 (アルファブレンド)、深度テストあり・深度書き込みなし、床と Z ファイトしないためのオフセット (Recast の DebugUtils と同じく NavMesh 自体を少し浮かせるか、深度バイアス)。
   - **NavMesh が変わらない tick では三角形を作り直さない** (NavSystem の世代番号などで判定。TileCache の更新で変わったタイルだけでもよい)。作り直しの所要時間と三角形数をログで測る。resim 中は積まない。
   - 描画パスが 2 系統ある (ADR-007) なら両方で出る。
2. DebugUtils の `duDebugDraw` 実装を「三角形 → 塗りレーン、線 → 既存の線レーン」に振り分ける (sub-02 では三角形を輪郭線へ落としていた)。色はエリアごと (0 = 水色系の Unity 風、Jump などは別色、ユーザー定義エリアは固定パレット)。
3. 表示切り替えに「塗り」を足す (Surface のインスペクタ。既定 on)。Runtime でも出せる。
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
4. NavMesh 系の無いシーンでは描画コストが増えない (空のレーンでパスを呼ばない)。replay_verify 全 PASS (描画は sim 外だがレーン追加で sim を触っていないことの確認)、0 警告、check_rules 0 (シェーダ定数を共有するなら rule 9)。

## 検証コマンド
- 両構成ビルド (`/p:MyeWarnAsError=true`)、`--selftest` 両構成、`tools\shot_verify.bat`、`tools\replay_verify.bat`、`tools\check_rules.ps1`
- `bin\x64\Release\Runtime.exe --nav-demo --screenshot` (一時スクショ検証の道具の使い分けはメモリ「screenshot-probe-recipes」)

## 実装メモ (coder が追記)

## フィードバック履歴
