#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Engine/RenderSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/TransformSystem.h"
#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

struct EngineContext;

// マテリアルプレビューの当て先 (M53)。マテリアルには実体が無いので、
// 材質を当てる形状をこちらで用意する。モデル/プレハブは実体を描くので無関係
enum class PreviewShape : int32_t {
    Sphere = 0, // 既定。陰影のグラデーションが一番読める
    Cube = 1,   // 面ごとの見え方 (metallic のハイライト分布) を見る
    Plane = 2,  // カメラ正対の板 — テクスチャ/ノーマルマップの確認用
    Count = 3,
};

// AssetBrowser のメッシュ/プレハブ立体サムネイル (M27d、Unity の Project ウィンドウ相当)。
//   - GetOrRequest: OnImGui から呼ぶ。キャッシュ済みなら SRV、未生成ならリクエストして nullptr
//   - OnRenderViews: D3D 描画フェーズ (EngineLoop フェーズ 6) で 1 フレーム最大 2 件生成。
//     一時 Scene にインスタンス化 → 包囲 AABB にフィットするカメラで 128px RT へ描画 →
//     CopyResource でエントリ保持テクスチャに複製
// 対象: .glb / .gltf / .fbx / .prefab.json / .actor.json / .mat.json。
// ファイル更新 (書込時刻) で自動再生成。LRU 256 件
class AssetPreviewCache {
public:
    static bool IsPreviewable(const std::wstring& path);
    static bool IsMaterialPath(const std::wstring& path); // .mat.json (球で描く)

    static constexpr int kPreviewSize = 128; // 生成テクスチャの一辺 (px)

    ID3D11ShaderResourceView* GetOrRequest(EngineContext& ctx, const std::wstring& path);

    // 編集中マテリアルのライブプレビュー (Inspector 用、M53)。ファイルではなく**値**を描くので、
    // 保存前のスライダ操作がそのまま絵になる。valueHash (= 値の同一性キー) と shape が
    // 変わったときだけ再生成するので、呼び出し側は毎フレーム呼んでよい。
    // 生成は次の描画フェーズなので 1 フレーム遅れる (既存サムネイルと同じ)
    ID3D11ShaderResourceView* GetOrRequestMaterial(EngineContext& ctx, const Material& mat,
                                                   PreviewShape shape, uint64_t valueHash);
    void OnRenderViews(EngineContext& ctx);

private:
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        // 今の絵が対応するスタンプ / 要求されたスタンプ。ファイル由来なら書込時刻、
        // ライブマテリアルなら値ハッシュ — 同じ器で回すため一般化した (M53)
        uint64_t stamp = 0;
        uint64_t wantStamp = 0;
        uint64_t lastUsedFrame = 0;
        bool failed = false; // ロード失敗 (再リクエストの連打を防ぐ)
        bool live = false;   // ファイルを読まず liveMat / liveShape を描く
        Material liveMat;
        PreviewShape liveShape = PreviewShape::Sphere;
    };

    // スタンプ照合 + 生成キュー積み。ファイル版とライブ版の共通口
    ID3D11ShaderResourceView* Touch(EngineContext& ctx, const std::wstring& key,
                                    uint64_t wantStamp, Entry& entry);
    bool RenderOne(EngineContext& ctx, const std::wstring& path, Entry& entry);
    void EvictIfNeeded();

    std::unordered_map<std::wstring, Entry> cache_; // キー = NormalizePathKey
    std::vector<std::wstring> pending_;             // 生成待ち (正規化パス)
    std::unordered_set<std::wstring> pendingSet_;

    RenderTexture rt_;              // 共有 128x128 描画先 (使い回し)
    RenderSystem previewRender_;    // postFx/shadow 無効の専用インスタンス
    TransformSystem transforms_;    // 一時シーンの WorldMatrix 更新用
    Scene tempScene_;               // 使い回し (RenderOne 冒頭で Clear)
};

} // namespace mye
