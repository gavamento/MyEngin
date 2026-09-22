# プロジェクトポスト — Properties 2D テクスチャ既定名 (M78)

spec §4.1 エッジケースに従い、Tex2D プロパティ未割当または組み込み名指定時はエンジンが SRV を供給する。

| 名前 | 現状 (M78c) |
|---|---|
| `white` | `TextureLibrary::White()` (1×1 白) |
| `gray` | 専用 SRV 未整備 → **white にフォールバック**（初回 WARN） |
| `black` | 専用 SRV 未整備 → **white にフォールバック**（初回 WARN） |
| `bump` | 専用 SRV 未整備 → **white にフォールバック**（初回 WARN） |

解決は `RenderSystem::ResolvePost` 内の Tex2D リゾルバ。専用 1×1 テクスチャの追加は任意（後続タスク可）。

手動サンプル: `assets/shaders/MyTint.post.hlsl` + `assets/MyTint.fxstack.json` をカメラ `fxStack` に割り当てる。
