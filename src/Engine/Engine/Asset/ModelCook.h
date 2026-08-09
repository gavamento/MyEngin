#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Renderer/GpuResources.h"
#include "Engine/Renderer/Skeleton.h"

namespace mye {

class ShaderManager;

namespace ModelCook {

// クックファイルの拡張子 (CookedCache::ReadValidated/Write に渡す)。selftest も参照する
inline constexpr const wchar_t* kModelExt = L".mmdl";

// モデル 1 ファイル分のクック結果 = RegisterAssets がライブラリへ登録した内容の完全な記録。
// 両ローダ (FbxLoader / ModelLoader) が LoadContext::cook 経由で登録と同時に追記する (sink)。
// Replay はこれを同じ順序・同じバイト列で Register し直す — フレッシュパースとビット同一が契約。
struct CookedTexture {
    uint8_t kind = 0; // 0 = 埋め込み (CreateFromEncoded) / 1 = 外部ファイル (LoadFile)
    uint8_t srgb = 0;
    std::string key;            // kind=0: テクスチャキー
    std::wstring path;          // kind=1: 解決済み絶対パス (CookedCache の deps にも入る)
    std::vector<uint8_t> bytes; // kind=0: エンコード済み画像 (デコードはリプレイ時 = .meta 設定が生きる)
};

struct CookedMesh {
    std::string key;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

struct CookedMaterial {
    std::string key;
    Material mat; // POD。内包する AssetID はキー由来の決定的ハッシュなので保存してよい
};

struct CookedSkin {
    std::string key;
    SkinnedModel model;
};

struct ModelCookData {
    std::vector<CookedTexture> textures;
    std::vector<CookedMesh> meshes;
    std::vector<CookedMaterial> materials;
    std::vector<CookedSkin> skins;

    // sink 追記 (ローダの各 Register サイトから呼ぶ)。同一キーは 1 回だけ記録する —
    // フレッシュパースは同じキーを何度も再登録するが (glTF: プリミティブ毎の材質、
    // FBX: インスタンス毎のメッシュ)、同キー再登録は同内容の上書きなので最終状態は
    // 変わらず、埋め込みテクスチャの blob 重複だけを避けられる
    void AddTexture(uint8_t kind, bool srgb, std::string key, const std::wstring& path,
                    const void* bytes, size_t size);
    void AddMesh(std::string key, const std::vector<MeshVertex>& vertices,
                 const std::vector<uint32_t>& indices);
    void AddMaterial(std::string key, const Material& mat);
    void AddSkin(std::string key, const SkinnedModel& model);

    // CookedCache::Write の deps (外部テクスチャの存在検証用)
    std::vector<std::wstring> ExternalDeps() const;
};

// blob ⇄ 構造体。Deserialize は境界検査つき (破損ファイルで false、絶対に落ちない)
void Serialize(const ModelCookData& d, std::vector<uint8_t>& out);
bool Deserialize(const std::vector<uint8_t>& in, ModelCookData& out);

// クック内容をライブラリへ登録し直す (フレッシュパースの Register 列と同一)
void Replay(RenderResources& resources, ShaderManager& shaders, const ModelCookData& d);

// RegisterAssets 用の一括ヘルパ: キャッシュが有効ならロード + Replay まで行う。
// false = キャッシュ無し/無効 → 呼び出し側はフレッシュパース (+ Save) する
bool TryReplayFromCache(RenderResources& resources, ShaderManager& shaders,
                        const std::wstring& srcPath);
void SaveToCache(const std::wstring& srcPath, const ModelCookData& d);

} // namespace ModelCook
} // namespace mye
