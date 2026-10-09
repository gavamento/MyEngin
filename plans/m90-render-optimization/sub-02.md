# sub-02: GPU オクルージョンの縦切り (max-Z HZB を viewKey ごと、2 フェーズ、Deferred の不透明)

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
spec §4.1.4、§2 #1 #2 #3。M90 で最もリスクの高い未知 (2 フェーズが既存の Deferred 経路と WARP で組めるか) を潰す縦切り。**Deferred の GBuffer 不透明だけ**に通す。
1. HZB の縮小を min / max で切り替えられるようにする。SSR 用の min-Z ピラミッドの結果は 1 ビットも変えない (既存の selftest がそのまま通ること)。max 版の selftest を足す (同じ `HzbReduceSpan` の分割規則、奇数段の 3 テクセル読みでも max が正しい)。
2. viewKey ごとのオクルージョン状態 (max-Z ピラミッド、エンティティごとの可視ビット = 安定スロット) を持つ。viewKey 0 と ProbeBaker は持たない (OFF)。リサイズ・デバイス消失 (M88 の復旧経路) で捨てる。
3. 2 フェーズ:
   - フェーズ 1: 前フレームに可視だった項目だけを GBuffer に描く。
   - その深度から max-Z HZB を作る。
   - compute: 全項目 (CPU 視錐台を通った物) の AABB をビューへ投影し、HZB の該当 mip で判定。フェーズ 1 で描いていない可視の物を run ごとに詰め、indirect 引数を書く。可視ビットを更新。
   - フェーズ 2: `DrawIndexedInstancedIndirect` で描く。インスタンシングできない項目は instanceCount 0/1 の個別 indirect。
4. AABB の判定は保守的: 投影がニア面をまたぐ物、画面の外へはみ出す物は可視。判定の CPU 版 (同じ式) を純関数で持ち、selftest で境界値を確かめる (シェーダと定数・式を一致させる。C++/HLSL の共有定数は規約 4.3)。
5. `RenderSystem::enableOcclusionCulling` (既定 true)、CLI `--no-occlusion`、エディタの描画設定メニューに ON/OFF (`Tr()`)。
6. 統計: フェーズ 1 / 2 の描画数、落とした数を sub-01 の欄へ (数フレーム遅れのステージング読み、統計専用)。GPU ms の欄も埋める。
7. 失敗の局所化: リソース作成失敗でオクルージョンだけ OFF + ログ 1 回。
8. (sub-01 VERDICT から移管) `render_bench` にカメラカットを足す。**sim には触れない**: 描画側のカメラ上書き (`RenderSystem::Render` の `CameraOverride`) を、撮影フレーム番号で切り替えるデバッグ用 CLI (例 `--render-bench-cut-frame N`。名前は coder が既存 CLI に合わせる) で入れる。カット後のカメラは、カット前に壁で隠れていた物が見える位置にする。`cache\render_bench.scene.json` は生成シーンのキャッシュなので、シーン生成を変えたら消してから撮る。
9. (sub-01 VERDICT から) ProfilerWindow の GPU 行の「フレーム」は `RenderSystem::Render` 1 回分 (複数ビューのエディタでは最後に描いたビュー) を計っている。表示を実態に合わせる (例: 「Render (最後のビュー)」)。ビュー別の ms は任意。

## やらないこと (このサブでは)
- Forward 経路、`--hzb-debug` の拡張 (sub-03)。影・半透明。
- サーフェスマテリアル・水面など別経路の項目を判定対象にすること (常に描く)。

## 触る場所 (planner の見立て)
- `src\Engine\Renderer\Passes\HzbPass.*`、`assets\shaders\hzb_reduce.cs.hlsl`
- 新しい compute シェーダ (判定 + 詰め込み)、新しいパス (`Renderer\Passes\OcclusionCullPass.*` 等。名前は coder が既存の命名に合わせる)
- `src\Engine\Renderer\Pipeline\DeferredPath.cpp` (GBuffer の描画を 2 回に分ける。DeferredPath.cpp:1421 の「深度プリパスを足さない」方針とは矛盾しない — フェーズ 1 は本描画そのもの)
- `src\Engine\Renderer\Mesh\MeshInstancing.h` (`BuildInstanceRuns`、t0 の StructuredBuffer。可視 index の間接参照を VS に足す必要があるか)
- `src\Engine\Engine\Rendering\RenderSystem.*` (viewKey 別の状態、prevRender と同じ流儀の安定スロット)
- 前例: `GpuParticleBackend.cpp` の Indirect
- 確認すべき既存の前提: velocity (prevWorld)、TAA のジッタ、ステンシルを使う経路、GBuffer のクリア位置

## 受け入れ条件 (このサブ)
1. `render_bench` (Deferred) で dump の occluded > 0、ON/OFF のスクショの画素差 0 (差があれば箇所と原因を示す。z-fight 以外は不具合)。 — `--screenshot` A/B + img-diff (Editor.exe)
2. カメラカットのフレームで欠けない (カット直後のフレームを ON/OFF で撮って差 0)。 — 同上
3. 既存の HZB (SSR) の selftest と golden が全部 PASS。max 縮小と保守的判定の selftest を足して PASS。 — `--selftest` Debug / Release
4. `--warp` で動き、WARP での `render_bench` 1 フレームの所要時間を ON/OFF で報告 (CI 時間のリスク確認。ゲートではない)。
5. リソース作成失敗の注入で、オクルージョンだけが OFF になり描画が続く。 — selftest
6. `tools\check_rules.ps1` PASS。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`
- `render_bench` の `--screenshot` を `--no-occlusion` と比較、`--render-stats-dump`
- 実機 GPU での ms を参考値として報告

## 実装メモ (coder が追記)

## フィードバック履歴
