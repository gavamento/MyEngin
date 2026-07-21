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
#include "Engine/Renderer/RenderTexture.h"

namespace mye {

struct EngineContext;

// AssetBrowser のメッシュ/プレハブ立体サムネイル (M27d、Unity の Project ウィンドウ相当)。
//   - GetOrRequest: OnImGui から呼ぶ。キャッシュ済みなら SRV、未生成ならリクエストして nullptr
//   - OnRenderViews: D3D 描画フェーズ (EngineLoop フェーズ 6) で 1 フレーム最大 2 件生成。
//     一時 Scene にインスタンス化 → 包囲 AABB にフィットするカメラで 128px RT へ描画 →
//     CopyResource でエントリ保持テクスチャに複製
// 対象: .glb / .gltf / .fbx / .prefab.json。ファイル更新 (書込時刻) で自動再生成。LRU 256 件
class AssetPreviewCache {
public:
    static bool IsPreviewable(const std::wstring& path);

    ID3D11ShaderResourceView* GetOrRequest(EngineContext& ctx, const std::wstring& path);
    void OnRenderViews(EngineContext& ctx);

private:
    struct Entry {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        int64_t fileWriteTime = 0;
        uint64_t lastUsedFrame = 0;
        bool failed = false; // ロード失敗 (再リクエストの連打を防ぐ)
    };

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
