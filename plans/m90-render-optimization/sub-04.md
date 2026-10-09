# sub-04: 影のカスケードごとのカリングとスキンの保守的 AABB

- 依存: sub-01
- 状態: 未着手
- 往復: 0

## やること
spec §4.1.3、§2 #7 #8。
1. CSM のキャスター候補を、カメラの視錐台で落とす前の全不透明 (ステージ 1 の候補) から取る。カスケードごとにライトの直交視錐台 (奥行き方向はシーン AABB まで伸ばす) で判定し、カスケード別の描画リストで描く。
   - CSM のフィット AABB (`sceneMin/sceneMax` の取り方、`RenderSystem.cpp:1289-1300`) は**変えない**。変えると解像度配分が変わり golden が動く。変える必要が出たら「仕様との差分」に出す。
   - 画面外のキャスターも影の描画に必要なデータ (ワールド行列、スキンならパレット) を持つこと。スキンのキャスターが画面外のときのパレットは sub-04 では毎回評価してよい (間引きは sub-06)。
   - (sub-01 VERDICT から) 現状、影のキャスターは `queue_.Sort()` の前の収集順で `ShadowPass::Render` に渡り、CSM では run が組めずバラ描き (render_bench で 1 カスケード 3364 draw)。**カスケード別のリストは本描画のキューと同じキー (material → mesh) で並べ、`BuildInstanceRuns` でインスタンシングする**。深度だけのパスなので並び替えで深度の結果は変わらない想定。golden が動いたら原因を断定して報告。受け入れ条件 2 の draw 数は、この並べ替えによる減少と、カスケード別カリングによる減少を分けて報告する。
2. スキンの保守的 AABB: モデル登録時に、全クリップの全キーフレームについて「ボーンごとの頂点包絡 (バインド空間、ウェイト > 0 の頂点)」をそのフレームの骨行列で変換した和集合 + 余白をモデル空間 AABB として `SkinnedModel` に持つ。クックしない (kCookVersion に関係しない)。決定的に計算する。
   - `RenderableInFrustum` のスキン分岐をこの AABB で判定する。ラグドール作動中は従来どおり常に可視。
   - 余白の値と根拠をコメントに書く (IK の到達、ブレンドの補間)。
3. 統計: カスケード別の影 draw、パレット評価数 (sub-01 の欄)。

## やらないこと (このサブでは)
- 影へのオクルージョン、LOD、並列化 (sub-06)。

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (`CollectDrawables` のステージ 1〜3、`RenderCascadeShadows`)
- `src\Engine\Renderer\Passes\ShadowPass.cpp` (キューを受け取る形 → カスケード別リスト。`BuildInstanceRuns` の呼び方)
- `src\Engine\Renderer\Pipeline\FrustumCull.h` (`RenderableInFrustum`)
- `src\Engine\Renderer\Mesh\Skeleton.*` / `SkinnedModel` の登録 (`SkinnedModelLibrary`)
- 前例: `ShadowAtlas` のタイル単位カリング

## 受け入れ条件 (このサブ)
1. `render_bench` の「画面外キャスター」のカメラで影が描かれる (変更前は消える)。前後のスクショを報告。 — `--screenshot`
2. dump のカスケード別 draw が、全カスケード共通の候補数より少ない (少なくとも 1 カスケードで)。
3. selftest: テスト用スキンモデルの全クリップの全フレームを CPU スキニングし、全頂点が保守的 AABB の中にある。
4. 画面外のスキンキャラ (影も画面に落ちない位置) の描画とパレット評価が 0 (dump)。
5. 既存 golden が全部 PASS。動いたら原因を断定して報告 (影が増えた = 正しい修正による差かどうか。memory「golden が動いたときの切り分け」の 4 点計測)。`--update` で黙って塗らない。
6. `tools\check_rules.ps1` PASS。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、`render_bench` の撮影と dump

## 実装メモ (coder が追記)

## フィードバック履歴
