//====================================================================================
//                          SpriteAnim.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                  スプライト列クリップ (.spriteanim.json) と登録表
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// コマ 1 枚。sprite は SpriteLibrary の AssetID (スライス or テクスチャ全面)
struct SpriteAnimFrame {
    uint64_t sprite = 0;
    int32_t ticks = 1; // 表示する sim tick 数。1 以上
};

// コマの数と 1 コマの tick 数の上限。Σticks が int32 に収まる範囲に抑える (4096 × 100000 < 2^31)
inline constexpr size_t kMaxSpriteAnimFrames = 4096;
inline constexpr int32_t kMaxSpriteAnimFrameTicks = 100000;

// スプライト列クリップ。長さ = Σticks。現在コマは時刻 (tick) からの整数演算だけで決まる = 導出値で sim 状態ではない。
// 決定論: ワールドハッシュ非対象 (「再生時に同じ資産がある」前提、physmat と同じ)。読み書きはメインスレッドのみ
struct SpriteAnim {
    uint64_t hash = 0; // = GUID (SpriteAnimLibrary のキー)
    std::string name;  // コントローラの clipEvents のキー。JSON の "name"、無ければファイル名
    uint64_t nameHash = 0; // HashStr(name)
    std::wstring path;
    std::vector<SpriteAnimFrame> frames;

    // frames から作る導出値 (RebuildDerived)。frameEnds[i] = コマ i の終わり (排他、累積 ticks)
    std::vector<int32_t> frameEnds;
    int32_t lengthTicks = 0; // Σticks。コマが無ければ 0

    // frames を読んで frameEnds / lengthTicks / nameHash を作り直す。frames を書き換えたら呼ぶ
    void RebuildDerived();
};

// 時刻 time (tick) に表示するコマの index。time は [0, lengthTicks - 1] へ丸める。コマが無ければ -1
int32_t SpriteAnimFrameAt(const SpriteAnim& anim, int32_t time);

// 列挙 1 件 (ピッカー / Asset Browser 用)
struct SpriteAnimEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .spriteanim.json の管理 (PhysMatLibrary 範型)。EngineLoop / HeadlessSim が所有し spriteanim:: で注入
class SpriteAnimLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path);              // 失敗時 0
    uint64_t Register(const std::wstring& path, SpriteAnim anim); // 返り値 = hash

    const SpriteAnim* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return anims_.find(hash) != anims_.end(); }
    std::vector<SpriteAnimEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    static nlohmann::json ToJson(const SpriteAnim& a);
    // 読み値は Sanitize 済み。スプライト列でない JSON ("spriteanim" キー無し) は false
    static bool FromJson(const nlohmann::json& j, SpriteAnim& out);

    // ticks を [1, kMaxSpriteAnimFrameTicks] へ、コマ数を kMaxSpriteAnimFrames 以下へ丸めて導出値を作り直す
    static void Sanitize(SpriteAnim& a);

private:
    std::map<uint64_t, SpriteAnim> anims_; // 順序つき: 列挙が読み順 / ハッシュ順に依存しない
};

// モジュール注入 (physmat:: と同じ流儀)。EngineLoop / HeadlessSim が起動時に Install し、終了時にライブラリ破棄前に外す
namespace spriteanim {
void Install(SpriteAnimLibrary* lib);
SpriteAnimLibrary* Library();           // 未接続 = nullptr
const SpriteAnim* Resolve(AssetID id);  // 未接続 / 未登録 / null ID = nullptr
} // namespace spriteanim

} // namespace mye
