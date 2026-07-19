#pragma once

namespace mye {

struct EngineContext;

// デモ用リソース (メッシュ / マテリアル / モデル) を登録する。
// シーンファイルは AssetID しか持たないため実体登録は起動側の責務 — Editor / Runtime 共用。
void RegisterDemoContent(EngineContext& ctx);

// デモシーン (spinner / パーティクル / スクリプト) を構築する (シーンファイルが無い時のみ)。
// perfRate>0 でパーティクル放出数を上書き (性能計測モード)
void BuildDemoScene(EngineContext& ctx, float perfRate = 0.0f, int perfMax = 0);

// assets\ 以下の .prefab.json / .anim.json を各ライブラリへ登録する (Editor / Runtime 共用)
void RegisterAssetLibraries(EngineContext& ctx);

} // namespace mye
