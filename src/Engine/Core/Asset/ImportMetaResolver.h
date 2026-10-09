#pragma once
#include <cstdint>
#include <string>

namespace mye {
namespace importmeta {

// テクスチャのインポート設定 (.meta v2、M39b)。
// 既定値 = .meta が無いときの挙動 (srgb はロードサイトのヒント / mips 有 / Cook は BCn 自動)。
// wrap (サンプラアドレス) はサンプラがパス単位のため v1 では見送り。
struct TextureImportSettings {
    int32_t srgb = 0;         // 0=auto (ロードサイトのヒント) / 1=on (_SRGB) / 2=off (UNORM)
    int32_t generateMips = 1; // 0=off (mip0 のみ) / 1=on (フルチェーン)
    int32_t compress = 0;     // Cook to DDS: 0=auto (BC1/BC3 自動) / 1=none (RGBA8 非圧縮)
};

// モデルの LOD 設定 (.meta の "lod"、M90e)。levels = 0 (既定) は段なし = 従来と 1 ビットも変わらない。
// 段 k (1..levels) の三角形数 = LOD0 × ratio[k-1]。screenSize[k-1] は段 k へ落とす画面高さ比で、0 = 比から自動
struct ModelLodSettings {
    static constexpr int kMaxExtraLevels = 3; // LOD0 + 3 段 = 最大 4 段

    int32_t levels = 0;
    float ratio[kMaxExtraLevels] = { 0.5f, 0.25f, 0.125f };
    float screenSize[kMaxExtraLevels] = { 0.0f, 0.0f, 0.0f };

    // levels を範囲に収め、使わない段の値を既定へ戻す (blob に書く値を 1 通りにして比較を安定させる)
    void Normalize()
    {
        const ModelLodSettings defaults;
        levels = (levels < 0) ? 0 : ((levels > kMaxExtraLevels) ? kMaxExtraLevels : levels);
        for (int i = 0; i < kMaxExtraLevels; ++i) {
            if (i >= levels) {
                ratio[i] = defaults.ratio[i];
                screenSize[i] = 0.0f;
                continue;
            }
            ratio[i] = (ratio[i] > 0.01f) ? ((ratio[i] < 0.95f) ? ratio[i] : 0.95f) : 0.01f;
            screenSize[i] = (screenSize[i] > 0.0f) ? ((screenSize[i] < 1.0f) ? screenSize[i] : 1.0f) : 0.0f;
        }
    }
    bool operator==(const ModelLodSettings&) const = default;
};

// パス → インポート設定解決のグローバルフック (assetkey::/assetguid:: と同じ流儀)。
// TextureLibrary (Renderer 層) が AssetDatabase (Engine 層) を参照せずに .meta の
// 設定を引くための関数ポインタ注入。AssetDatabase::InstallAsKeyResolver が接続する。
// 戻り値 false = .meta 無し等で未解決 — 呼び出し側は既定値を使う。
// スレッド規約: Install/Resolve はメインスレッド専用 (LoadFile/PollAsyncLoads と同じ)。
using ResolveFn = bool (*)(void* user, const std::wstring& path, TextureImportSettings& out);

void Install(ResolveFn fn, void* user); // fn=null で既定 (常に未解決) に戻す
bool Resolve(const std::wstring& path, TextureImportSettings& out);

// モデルの LOD 設定の解決 (テクスチャの Resolve と同じ流儀。Install の fn が同時に差し替わる)。
// 未解決 (.meta 無し・"lod" 無し) は out を段なしの既定にして false を返す。戻り値は Normalize 済み
using ModelLodResolveFn = bool (*)(void* user, const std::wstring& path, ModelLodSettings& out);
void InstallModelLod(ModelLodResolveFn fn, void* user);
bool ResolveModelLod(const std::wstring& path, ModelLodSettings& out);

} // namespace importmeta
} // namespace mye
