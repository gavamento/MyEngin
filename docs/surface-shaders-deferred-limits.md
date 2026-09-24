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
