# プロジェクトサーフェスシェーダー — Deferred の既知の制限 (M79 sub-03)

不透明サーフェス (`*.surface.hlsl`) は Deferred パスの GBuffer に参加しない
(`DeferredPath::RenderGeometry` が除外し、SSR 後・水面前の「フォワード段」
`DeferredPath::RenderSurfaceForward` が速度エントリで直接 HDR シーンへ描く)。
このため以下は効かない (spec §3 の「後回し」項目):

| 効果 | 状態 |
|---|---|
| SSAO | サーフェス画素は AO の対象にならない (GBuffer の法線/位置が無いため) |
| SSR | サーフェス画素は反射元にも反射先の受け面にもならない |
| デカール | サーフェス画素へは投影されない |
| RT (GI / 影 / 反射) | サーフェス画素はヒット対象・受け面のどちらにもならない |

太陽 CSM (`ShadowPass`) はサーフェスの不透明アイテムを**影エントリ**で変位込みに描く。
一方、局所ライト (スポット/ポイント) のシャドウアトラス (`ShadowAtlas`) は変位を反映しない
従来シェーダのまま描く (剛体形の影になる)。

いずれも初版の割り切りであり、後続マイルストーンで GBuffer 作者規約を足すまでの間の制限として
`plans/m79-project-surface-shaders/spec.md` §3 に記載済み。

## 影エントリでは `MyEnginePerFrame` が 0 埋め

`ShadowPass::Render` は `RenderSystem::PrepareEnvironment` (光/霧/IBL/カメラ位置の確定) より
**前**に呼ばれる。このため影エントリ (CSM) が読む `MyEnginePerFrame` は常に全 0 で渡る
(色・速度エントリでは通常どおり内容が入る)。

作者規約: VSMain の頂点変位に `gCameraPos` 等 `MyEnginePerFrame` の値を使うと、
影エントリだけ他パス (色・速度) と違う値 (常に 0) を見て変位の形が食い違う。
変位に使ってよいのは次のものだけ:

- 位置用 static (`gTime` / `gWaterTime`。`gViewProj` / `gWorld` はパスごとに生成エントリが代入する)
- `MyEngineSurfaceFrame` / `MyEnginePerObject` / `MyEnginePerMaterial` / 作者 `Texture2D`

(spec §4.1、M79 sub-03 round 2 で確定。`assets/shaders/MyEngineSurface.hlsli` のコメントにも同内容を記載)

## Deferred の透明段のサーフェスマテリアル対応 (M79 sub-05 round 2 で修正)

`DeferredPath::RenderTransparent` は当初 `mat->shader` を直接 `ShaderManager::Get` へ渡して
描いており (`GetOrBuildSurfaceState` を経由しない)、`*.surface` 短名のマテリアルは解決できず
`transparent: true` なサーフェスマテリアルのアイテムが黙って描かれない不具合があった
(sub-03 の「やること」に記載があったが実装が漏れていた。M79 sub-05 round 1 レビューで発見)。

sub-05 round 2 で修正: `RenderTransparent` はアイテムごとに `GetOrBuildSurfaceState` を呼び、
サーフェス材質なら **色エントリ** (`ForwardPath::DrawSurfaceItem` と同じ名前解決バインド) で
描く。予約 CB (`MyEnginePerFrame`/`MyEngineSurfaceFrame`/`MyEngineWater`) は同じ `Render()`
呼び出し内で `RenderSurfaceForward` (2.65 段) が既に埋めているものをそのまま使う
(`RenderSurfaceForward` は不透明サーフェスが 0 件でも CB だけは埋めるよう変更した)。
spec §4.1 のとおり **速度は書かない** — この段は単一 RT (`view.rtv`) のみを束ねており
`gbVelocity_` を張らないため、自然に速度なしが成立する。失敗時は `surface_error`
(マゼンタ、alpha=1) にフォールバックし黙って消えない。`SurfaceDeferredSelfTest.cpp` の
`TestDeferredTransparentDrawsSurfaceColorEntry` が read-back で検証する。

## カリング余白 (`boundsPadding`) と両面描画 (`doubleSided`) — M79 sub-06

視錐台カリング (`RenderSystem::CollectDrawables` ステージ 2) と CSM のキャスター AABB 集約は、
メッシュのローカル AABB を `.mat.json` の `boundsPadding` [m] だけワールド空間で広げた箱で
判定する (`FrustumCull.h` の `AabbInFrustum`/`RenderableInFrustum`/`WorldAabb` の
`worldPaddingM` 引数)。既定 0 = 従来と同じ判定。サーフェスでないマテリアルには効かない
(`MaterialLibrary::GetSurfaceBoundsPadding` は横テーブル未登録なら 0 を返す)。

余白の解決はステージ 1 (直列の収集) で行い `CullCand::boundsPadding` へキャッシュする —
ステージ 2 はジョブ並列の純関数なので、その中で `MaterialLibrary` を引くと要素独立の前提が
崩れる。頂点変位で大きく形が動くメッシュ (大波の巻き込み等) は、変位の最大量以上の余白を
作者が見積もって設定すること (自動推定はしない)。

`doubleSided: true` のサーフェスは、色・速度・影の全エントリを Cull None のラスタライザで
描く (`ForwardPath`/`DeferredPath`/`ShadowPass` それぞれが専用の `rasterizerCullNone_` を持つ)。
描画直後に必ずそのパスの既定ラスタライザ (`rasterizer_`/`rasterizerWire_`、ShadowPass は
深度バイアス付きの `rasterizer_`) へ戻す — 固定スロットの復元 (review-1 #1 #2) と同じ理由で、
戻し忘れると次の非サーフェスアイテムの Cull 設定が壊れたままになる。

裏面か表面かの判定 (`SV_IsFrontFace`) は作者へ渡さない (§4 の作者規約 `float4
PSMain(VSOut)` を変えないため)。裏面用の見た目が要る場合は、作者が法線とカメラ方向の
内積で判定すること (`dot(normalize(gCameraPos - posW), normalW) < 0` が裏面)。
