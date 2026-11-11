# sub-08: カリング用カメラの固定と理由別の可視化

- 依存: sub-07
- 状態: 未着手
- 往復: 0

## やること
spec 4.1.7 / 4.3 / 受け入れ条件 9 (と 2 の `--cull-freeze-frame` / `--cull-debug`)。
- カリング用カメラの固定: Rendering > カリング の「カリング用カメラを固定」と `--cull-freeze-frame N`。ON の瞬間にビュー別 (1..3) の非ジッタ view-proj・位置・`proj._22`・正射影かを控え、そのビューの視錐台 / 距離 / 画面サイズの判定 (メッシュと 4.1.5 の経路) に使う。ライト選別・CSM・アトラスのライト側・オクルージョン・描画は今のカメラ。固定中は控えた視錐台の 12 辺を線で描く。OFF で解除、`ResetRenderHistory` でも解除しない (明示的な操作だけで外す)。セッション限り。
- 理由別の枠表示: 「落とした物を表示」と `--cull-debug`。落としたメッシュのワールド AABB を理由別の色 (名前付き定数。視錐台 / 距離 / 画面サイズ / オクルージョン) で線描き。オクルージョンの箱は既存の `OcclusionDebugBoxes` (`RenderSystem.cpp:2443-2470`) を `--hzb-debug-max` 無しでも集める。上限 4096 箱、超えた数を表示 (画面かログ)。凡例はメニューのツールチップ。
- Profiler: 理由別の数 (`culled` / `culledDistance` / `culledScreenSize` / `occluded` / `culledNonMesh` / `unculledGpuEmitters`) をビュー別の表に出す (sub-02 で列を作っていれば値が入るだけ)。
- ローカライズ英日。

- **sub-04 からの申し送り**: GPU パーティクル・トレイル・スプライト・デカールの判定は `RenderView::cullFrustum` (CollectLights で設定、非ジッタ) と `FrameContext::frustum` から来る。固定の控えを差し込むのはそこ。CPU パーティクルのプール判定は `view.proj` から視錐台を作り直している (ジッタあり) ので、固定に従わせるにはここも `cullFrustum` へ寄せる。寄せた結果 CPU パーティクルの判定がジッタ無しに変わる場合は、`particle_cpu.png` と cull_verify の一致で確かめる。

- **sub-05 からの追加**: `culledNonMesh` に CPU パーティクルのプールで視錐台により落とした数も足す (spec 4.1.7。タイルチャンクは sub-05 で数え済み)。

## やらないこと (このサブでは)
- オクルージョンの固定 (spec D18)
- 4.1.5 の経路の枠表示 (メッシュだけ)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.{h,cpp}` (`CollectLights` で作る視錐台とは別にカリング用の視錐台、デバッグ線 :2443 付近)
- `src\Engine\Engine\Rendering\DebugDraw.h`、`src\Engine\Renderer\Passes\OcclusionCullPass.*`
- `src\Engine\Engine\App\EngineCli.cpp`、`EngineCliSelfTest.cpp`、`src\Editor\App\EditorApp.cpp`、`src\Editor\Windows\Debug\ProfilerWindow.cpp`

## 受け入れ条件 (このサブ)
1. `--render-bench-demo --cull-freeze-frame 10 --render-bench-cut-frame 20 --render-stats-dump` で、`culled` が固定しない run の「カット前の視点」の値と一致し、カット後の視点の値と異なる
2. `--cull-debug` (+ 固定 + カット) のスクショで、理由別の色の枠と固定した視錐台の線が見える (画像のパスを SELF_EVAL に書く)
3. 固定も枠表示も OFF のとき、`counts` と絵が sub-07 後と一致
4. CLI の解析と不正値 (EngineCliSelfTest)

## 検証コマンド
- Debug / Release ビルド
- 上記の dump 2 本と比較、`--screenshot` (memory: シーン撮りは Runtime.exe でもよい)
- `MYE_SELFTEST_ONLY=EngineCli,RenderStats` で `Editor.exe --selftest`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
