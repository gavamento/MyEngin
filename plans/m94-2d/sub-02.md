# sub-02: 正射影カメラと Scene View の 2D モード

- 依存: なし
- 状態: 未着手
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

## フィードバック履歴
