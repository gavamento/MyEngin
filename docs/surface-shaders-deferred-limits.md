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
