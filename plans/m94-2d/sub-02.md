# sub-02: 正射影カメラと Scene View の 2D モード

- 依存: なし
- 状態: OK (commit 9142d88)
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #23, #26、4.1.1、4.3

## やること
1. `CameraComponent` 末尾に `bool orthographic = false`、`float orthoSize = 5.0f` を append (登録表、和名)。`kSimSnapshotVersion` +1 (生バイトが伸びる)。
2. `RenderSystem::ResolveCamera` (`RenderSystem.cpp:661-726`) で `orthographic` なら `XMMatrixOrthographicLH(2·orthoSize·aspect, 2·orthoSize, nearZ, farZ)`。`projNoJitter` も同じ。`UILayout.cpp:529-544` (ワールド追従 UI の射影) に同じ分岐。正射影のとき froxel グリッドは既存どおりスキップ (`engine_spec.md:406`) — 壊れないことだけ確認。
3. Scene View の「2D」トグル (`SceneViewWindow.cpp`): ON で正射影 + XY 正面 (yaw/pitch を 0 に固定、camPos.z は負側)、ホイールで `orthoSize2d_` をズーム (1.1 倍刻み、下限 0.1)、中 / 右ドラッグでパン (画面 px → ワールド単位は orthoSize から)、F で選択へ寄る。XY グリッド (`EditorLinePass::AddGrid` に平面引数を足すか `AddGridXY` を足す。既存の XZ 呼び出しはバイト同一の出力)。ImGuizmo は並進 XY のみ・回転 Z のみ (`ImGuizmo::OPERATION` のマスク)。トグル状態はレイアウト保存に入れる (`layouts_` の既存の仕組み)。OFF に戻すと以前のカメラ姿勢へ戻る。
4. Inspector: Camera の `orthographic` ON のときだけ `orthoSize` を表示、OFF のとき `fovYDeg` (登録表の自動表示に加えて `InspectorWindow` のカメラ節で表示切替。難しければ両方表示でよい、その旨を差分に書く)。
5. 受け入れ用の確認: 正射影カメラ + 1 単位の箔 (MeshRenderer) のシーンを temp で組み `--screenshot` で撮り、箔の画素幅が `H / (2·orthoSize)` ± 1 px (960×540、orthoSize 5 → 54 px)。

## やらないこと (このサブでは)
- Sprite2D (sub-04)、2D コライダーギズモ (sub-04 / sub-08 で足す)、ピクセルパーフェクト (sub-22)

## 触る場所 (planner の見立て)
- `Components.h:51-57`、`Components.cpp` (Camera の登録)
- `src\Engine\Engine\Rendering\RenderSystem.cpp:661-726`、`src\Engine\Engine\UI\UILayout.cpp:529-544`
- `src\Editor\Windows\Scene\SceneViewWindow.{h,cpp}` (:212-239 射影、:1669-1758 HandleCamera、:1162 Ortho トグル、:490-491 グリッド、:1561 ImGuizmo)
- `src\Engine\Renderer\Passes\EditorLinePass.{h,cpp}` (:239-250 AddGrid)
- `src\Editor\App\EditorApp.cpp:100-122` (レイアウト保存)
- `LocalizationTable.inl` (「2D」トグルのツールチップ)
- `SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`

## 受け入れ条件 (このサブ)
1. (spec 9) 正射影スクショの箔の画素幅が計算値 ± 1 px。透視のシーンは golden 30 枚 maxDiff=0。
2. (spec 10) 2D トグル ON のエディタスクショ `plans\m94-2d\screenshots\sceneview_2d.png` (XY グリッドとギズモが見える。撮り方は `--screenshot` ではなくエディタの SceneView を表示した状態の Editor.exe のスクショ。既存の `screenshot-probe-recipes` にあるエディタ撮影の手順で)。
3. (spec 1-3) 0 警告、check_rules 0、snapshot +1 のみ。CameraComponent の append で hash 値は変わるが挙動は不変 — `--hash-diff` が append のみであることを 1 本 (`demo`) で確認。golden 30 枚 / replay_verify / 全体 selftest は sub-05 のチェックポイント。

## 検証コマンド
毎サブ必須: Debug / Release ビルド (`/p:MyeWarnAsError=true`)、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (このサブで足した節があれば)
- `Runtime.exe --replay-record` → `--hash-diff` (`demo` 1 本)
- temp シーンの `Runtime.exe --scene <temp> --warp --width 960 --height 540 --screenshot <out>`
チェックポイントではない (重い 3 本は sub-05)。

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1) の要約。正本は司会への返信。
- Camera に orthographic / orthoSize を append (kSimSnapshotVersion 52)。ResolveCamera と UILayout (BuildSimWorldContext) に正射影分岐。
- SceneView に 2D トグル (mode2d、レイアウト "Scene 2D" に保存)・XY グリッド (EditorLinePass::AddGridXY)・ズーム / パン / F・ImGuizmo の XY/Z マスク。
- Inspector: Camera は orthographic に応じて fovYDeg / orthoSize を出し分け。
- 検証: Debug / Release 0 警告ビルド、MYE_SELFTEST_ONLY=m94 (Physics + UI) 両構成 PASS、正射影スクショの箱 54 px、hash-diff は Camera の 2 フィールド追加のみ。

### round 1 SELF_EVAL (全文、司会が転記)

```
SELF_EVAL: sub-02 (round 1)
実装:
  - Components.h / Components.cpp:CameraComponent — 末尾に orthographic (bool, false) と orthoSize (float, 5.0) を append。登録表と和名「正射影」「正射影の半高」を追加。
  - RenderSystem.cpp:ResolveCamera — シーンカメラが orthographic なら XMMatrixOrthographicLH(2·orthoSize·aspect, 2·orthoSize, nearZ, farZ)。projNoJitter は view.proj のコピーなので自動で同じ行列になる。froxel のスキップは既存の正射影判定のまま (正射影の froxel スキップは未確認)。
  - UILayout.cpp:BuildSimWorldContext — 正射影分岐を追加 (xs = 1/(orthoSize·aspect)、ys = 1/orthoSize、w = 1)。透視側の式は元の演算順を保った (_43 は vtz·zs − near·zs)。
  - SimSnapshot.h と AcousticAudioSelfTest.cpp:129 — kSimSnapshotVersion 51 から 52。
  - SceneViewWindow.{h,cpp}
    - public bool mode2d、private の orthoSize2d_、applied2d_、saved3d* を追加。SyncMode2d() が ON で 3D 姿勢を退避し、OFF で復元する。ON 時は光軸が z=0 と交わる点を画面中心にし、yaw/pitch を 0、camPos.z を −50 にする。
    - 射影は mode2d なら XMMatrixOrthographicLH(2·orthoSize2d_·aspect, 2·orthoSize2d_)。描画・ギズモ・ピッキングは同じ lastProj_ を使う。ImGuizmo::SetOrthographic は ViewIsOrtho() に変更。
    - 2D の操作: ホイールで ×1.1 刻み (下限 0.1、上限 1000)、中/右ドラッグでパン (worldPerPixel = 2·orthoSize/ビュー高)。F は x/y を選択へ移し、orthoSize2d_ を radius·1.2 にする。Alt+LMB のオービットは 2D で無効。
    - 2D のグリッドは XY。間隔は 10 の冪で、画面に入る範囲だけ引く。
    - ImGuizmo の mask は並進 XY、回転 Z、拡縮 XY。ツールバーに「2D」チェックとツールチップを追加。2D 中は Ortho チェックを無効化。
  - EditorLinePass.{h,cpp}:AddGridXY — 新設。既存 AddGrid は無変更。
  - EditorApp.cpp — レイアウトの PanelBinding に "Scene 2D" を追加して mode2d を保存。キーが無い旧レイアウトは現状維持。
  - InspectorWindow.cpp — Camera は orthographic=true で fovYDeg を、false で orthoSize を非表示にする。
  - LocalizationTable.inl — SceneView_Mode2D と SceneView_Mode2DTip を追加 (en/ja)。
  - UISelfTest.cpp — 正射影カメラ上のワールド追従 UI のテストを追加 (orthoSize 5・1920x1080 で (1176, 216)、z に依存しない)。
  - EditorMain.cpp — MYE_SELFTEST_ONLY=m94 に RunUISelfTest を追加。
  - plans/m94-2d/screenshots/sceneview_2d.png — 受け入れ条件 2 のエディタスクショ。
仕様との差分:
  - [追加] ImGuizmo の拡縮も XY のみ (SCALE_X|SCALE_Y)。2D で Z 拡縮は意味が薄いため。
  - [追加] 2D 突入時の画面中心は「3D 視点の光軸が z=0 と交わる点」。
  - [追加] 2D のグリッドは固定 20×1.0 ではなく、ズームに応じて 10 の冪の間隔で画面範囲だけ引く。
  - [追加] MYE_SELFTEST_ONLY=m94 に RunUISelfTest を足し、UISelfTest に正射影のワールド UI の節を足した。
  - [逸脱なし] Inspector は spec の主案 (表示切替) をそのまま実装した。
  - 台帳の見立てと違った点: Inspector の Camera 専用節は無く、フィールドループ内に分岐を足した。
検証:
  - Debug / Release ビルド (/p:MyeWarnAsError=true) → 0 警告 0 エラー。初回の Release は既知の C4100 で落ち、再実行で通った。
  - MYE_SELFTEST_ONLY=m94 (Physics + UI) を Debug / Release で実行 → どちらも ALL PASS。ortho world UI の 2 節 (z=10、z=40) も PASS。
  - check_rules.ps1 → 3 error / 50 warning。error は既存の 3 件のみ。変更ファイルに新しい error / warning は無い。
  - 正射影スクショ (temp シーン、Runtime Release、--warp 960x540) → 箔 (1 単位) の画素幅 54 px = H/(2·orthoSize)。高さも 54 px。
  - hash-diff (physics-demo、sub-01 の after_physics.dump と突き合わせ): 追加キーは Camera.orthographic と Camera.orthoSize の 2 つだけ。値の変化はカメラ entity の #entity と UIInteraction.prevSurfX/Y のみ (run 間で元から変動)。demo の dump (cache\m94\b_cam.dump) でも Camera 行の追加を確認。
  - エディタスクショ (--scene --select Hero、mode2d を一時的に true にした Release) → XY グリッド、X 軸 (赤)、Y 軸 (緑)、XY のみの移動ギズモを目視確認。
  - replay_verify / shot_verify / golden 30 枚 / 全体 selftest / AcousticAudio T1 → 未実行 (ユーザー判断 9 で sub-05 へ)。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜3 を満たした。レイアウト保存の実機往復とホイール/ドラッグの実操作は未確認。
  正しさ: 4 — 画素幅一致、透視側の演算順は保存、UI の正射影は selftest で検証済み。透視のビット同一は golden で未確認 (sub-05)。
  コード品質: 4 — 既存の流儀に沿い、AddGrid は無変更。2D の定数は名前付き。
  テスト: 3 — selftest と画素幅スクショ。SceneView の 2D 入力は GUI の自動テストが無く、実操作は未確認。
不安・質問:
  - 正射影時の froxel を有効にした描画は未確認。
  - 2D 中の 3D ライト/影は既存の非 perspective フォールバックに乗る。sub-04 以降で見た目を再確認したい。
  - レイアウトのキー "Scene 2D" が無ければ現状維持。これで問題ないか。
申し送り:
  - 次サブ (sub-04 以降): Camera.orthographic / orthoSize が使える。SceneView の 2D 状態は mode2d、orthoSize2d_、lastView_/lastProj_ (ピッキング・ギズモ共通)。Sprite2D のギズモは sub-04 で DrawSpriteGizmos 周辺に足す。
  - sub-12: 2D モードの Scene View 上で塗る場合、lastView_/lastProj_ と MouseRay で z=0 平面との交点を取れる。
  - エディタのスクショ撮影は imgui.ini (Git 管理外) が窓位置を覚えているため、一時的に退避して撮り、元に戻した。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-20)。`RenderSystem.cpp` / `UILayout.cpp` の差分を読み、透視側は `v13 * 1.0f` (IEEE で恒等) と演算順の保存でビット同一、正射影側は `XMMatrixOrthographicLH` と同式 (_33 = 1/(f−n)、_43 = −n/(f−n)) であることを確認。画素幅 54 px = H/(2·orthoSize)。スクショで 2D トグル・XY グリッド・XY 限定ギズモを確認。hash-diff は `physics-demo` 1 本で可 (Camera を持つシーンなら同じ検証)。拡縮 XY 限定・光軸中心・10 の冪グリッドの 3 つの [追加] は採用 (spec 4.1.1 に反映)。nit: 正射影 + froxel の描画確認は未 (spec R11)。
