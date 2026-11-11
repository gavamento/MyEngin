# sub-02: カリング設定の土台と視錐台のスイッチ

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.1 / 4.1.2 / 4.1.7 の「理由別の数」の欄 / 4.3。受け入れ条件 1, 2, 3, 12 (一部)。
- 設定モジュール (例 `src\Engine\Engine\Rendering\CullingSettings.{h,cpp}`): spec 4.1.1 の 7 キーを読む・書く (1 キーずつ差し替える read-modify-write。読めないファイルは上書きしない)。実効値を CLI と合成する関数を 1 つ置き、EngineLoop と selftest が同じ関数を使う。既存の `Load/Save/ResolveOcclusionCulling` (`TagNames.cpp:200-226`) は呼び出し元と既存テストを壊さずに新モジュールへ寄せるか、そのまま残して新モジュールから呼ぶ (どちらにしたか実装メモに書く)。**距離・画面サイズのキーもこのサブで読み書きまで作る** (判定は sub-06 / 07)。
- CLI: `--no-frustum-culling`、`--no-distance-culling`、`--no-size-culling`、`--cull-max-distance D` (D ≥ 0)、`--cull-min-screen-size F` (0 < F ≤ 1)。後ろ 4 つはこのサブでは値を RenderSystem に渡すだけ。
- RenderSystem: `enableFrustumCulling` (と距離・画面サイズの設定を入れる場所) を足す。`f.cullEnabled` を「カメラがある」と「視錐台で落とす」に分ける。OFF のときの規則は spec 4.1.2 (CSM フィットは視錐台の内側だけ、`prevRender` の記録は ON と同じ規則、LOD / URO / 地形 / ライト選別は不変)。CPU パーティクルのプール (`CpuParticleBackend.cpp:739`) とタイルマップのチャンク (`Sprite2DRenderer.cpp:1089-1101`) もこのスイッチに従わせる。地形は従わせない。
- `prof::RenderStats` に `culledDistance` / `culledScreenSize` / `culledNonMesh` / `unculledGpuEmitters` の欄を**先に**足し (このサブでは 0 のまま)、`RenderStatsDump` の `counts`・`HasActivity`・`RenderStatsSelfTest`・ProfilerWindow の表まで形を固定する (M90a の「欄と dump の形を先に固定」と同じ)。`culled` の意味は変えない。
- UI: Project Settings > Rendering の「カリング」区切りに 4 つのチェックと数値欄 2 つ (閾値・既定距離)、タグ別の距離の表 (このサブで表の編集と保存まで。効くのは sub-06)。Rendering メニューに「カリング」サブメニューを作り、既存のオクルージョンの項目を移し、視錐台・距離・画面サイズのスイッチを足す。切り替えた瞬間に RenderSystem へ反映しファイルへ書く (`scmhint::Changed` も既存と同じく呼ぶ)。数値は編集を終えたときに書く。
- ローカライズ: 新しい文字列を `LocalizationTable.inl` に英日。

## やらないこと (このサブでは)
- 距離・画面サイズの判定 (sub-06 / 07)、局所影 (sub-03)、新しい経路の判定 (sub-04 / 05)
- 地形のカリングの変更

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Scene\TagNames.{h,cpp}` (既存のオクルージョン設定)、新規 `CullingSettings`
- `src\Engine\Engine\Loop\EngineLoop.cpp:473-476`、`src\Engine\Engine\App\EngineCli.cpp` / `EngineConfig` 相当 / `EngineCliSelfTest.cpp:261-283`
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` — `CollectLights` (:1010-1020)、`CollectDrawables` ステージ 2 (:1476-1504) とステージ 3 (:1508-1628)
- `src\Engine\Engine\Particles\CpuParticleBackend.cpp:720-740`、`src\Engine\Engine\Rendering\Sprite2D\Sprite2DRenderer.cpp:1089-1101`
- `src\Engine\Core\Diagnostics\Profiler.h:34-55`、`src\Engine\Engine\Rendering\RenderStatsDump.cpp`、`RenderStatsSelfTest.cpp`、`src\Editor\Windows\Debug\ProfilerWindow.cpp`
- `src\Editor\Windows\Project\ProjectSettingsWindow.cpp` (Rendering 節 :59-98)、`src\Editor\App\EditorApp.cpp:1268-1282`
- `src\Engine\Core\Localization\LocalizationTable.inl`
- ファイル追加後は `tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
1. 設定の往復 (spec 5-1): 7 キーの保存・読み戻し、キー無し / 型違い / 範囲外 → 既定値、読めないファイルを上書きしない、`rayTracingTags` と `rendering` 内の他キーが残る
2. CLI (spec 5-2 のうち `--no-frustum-culling` / `--no-distance-culling` / `--no-size-culling` / `--cull-max-distance` / `--cull-min-screen-size`): 解析・不正値の拒否・実効値の式・ファイル不変
3. 視錐台 OFF (spec 5-3): 既定ベンチで ON / OFF の撮影が全画素一致 (tol 0)、OFF で `drawCalls` 増・`culled` = 0、影の数 (`shadowCascadeCasters` など) が ON と同じ。可能なら selftest (OcclusionSelfTest の ON/OFF 全画素一致の作り) にする
4. 既定設定で、既定ベンチの `counts` (新しい欄を除く) が sub-01 後と一致
5. `counts` が Debug / Release / `--warp` で一致 (新しい欄を含む)
6. ローカライズ規則と check_rules が 0 error

## 検証コマンド
- Debug / Release ビルド、`tools\gen_project_files.ps1` (ファイル追加時)
- `MYE_SELFTEST_ONLY=Tag,EngineCli,RenderStats,Occlusion,<新スイート>` で `Editor.exe --selftest`
- `Editor.exe --render-bench-demo --render-stats-dump` (ON / `--no-frustum-culling`)、`--screenshot` の ON/OFF を img-diff (memory: img-diff は Editor.exe)
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
