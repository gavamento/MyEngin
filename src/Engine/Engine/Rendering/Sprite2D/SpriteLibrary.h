//====================================================================================
//                          SpriteLibrary.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライト (テクスチャのスライス) の AssetID 解決表
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// 1 スプライトの描画定義。rect の w / h が 0 のものは「テクスチャ全面」(寸法は描画側が Texture から引く)。
// rect は画素・左上原点、pivot は 0..1・左下原点、border は左・下・右・上の画素数
struct SpriteDef {
    AssetID texture;
    int32_t rect[4] = {};
    float pivot[2] = { 0.5f, 0.5f };
    int32_t border[4] = {};
    int32_t ppu = 100;
    int32_t filter = 0; // 0 auto (linear) / 1 linear / 2 point
    bool texSrgbOn = false; // テクスチャの .meta が srgb = on (2D の Passthrough では暗く出る。Inspector が警告する)
    char name[64] = {};

    bool IsFullTexture() const { return rect[2] == 0 && rect[3] == 0; }
};

// Inspector のピッカー用の 1 行
struct SpriteListing {
    AssetID id;
    std::string displayName; // "tex" (全面) または "tex#name"
};

// スプライトの AssetID → SpriteDef。描画専用で sim の入力ではない (ハッシュ対象外)。
// AssetID は HashStr(SubAssetKeyPrefix(texPath) + "#sprite" + id): GUID 接頭辞なので移動・改名で変わらない。
// テクスチャ自身の AssetID も全面スプライトとして引ける。メインスレッド専用。
class SpriteLibrary {
public:
    // .meta を読み直し、そのテクスチャのスライスと全面を登録し直す (以前の登録は消える)。
    // .meta が無くても全面は既定値で登録する。戻り値はスライス数
    int Register(const std::wstring& texPath);

    // 未登録のテクスチャ AssetID は GUID からパスを引いて遅延登録する。解決できなければ nullptr
    // (呼び手がマゼンタ等へ落とす)。返したポインタは次の Register / Clear まで有効
    const SpriteDef* Get(AssetID id);

    // 表示名の昇順 (読み順に依存しない)
    void Enumerate(std::vector<SpriteListing>& out) const;

    const SpriteDef* FindByName(const std::wstring& texPath, const char* name);

    void Clear();

    // Register / Clear のたびに増える。タイルのチャンクキャッシュが古い UV を捨てる判断に使う
    uint64_t Epoch() const { return epoch_; }

    // スライス 1 枚の AssetID
    static AssetID SliceId(const std::wstring& texPath, uint32_t sliceId);

private:
    struct Entry {
        SpriteDef def;
        std::string displayName;
    };

    void Unregister(uint64_t textureId);

    std::map<uint64_t, Entry> entries_; // 順序つき: Enumerate が読み順に依存しない
    std::unordered_map<uint64_t, std::vector<uint64_t>> idsByTexture_; // Unregister 用
    uint64_t epoch_ = 0;
    std::unordered_set<uint64_t> unresolved_; // 遅延登録に失敗した ID (毎フレームの GUID 引きを避ける)
};

// モジュール注入 (physmat:: と同じ流儀)。メインスレッド専用
namespace spritelib {
void Install(SpriteLibrary* lib);
SpriteLibrary* Library(); // 未接続 = nullptr
} // namespace spritelib

} // namespace mye
