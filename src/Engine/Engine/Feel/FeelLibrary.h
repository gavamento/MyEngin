//====================================================================================
//                          FeelLibrary.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          手触りのプリセット資産 (.feel.json、M94d、ADR-031) の読み書きと登録
//====================================================================================
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"
#include "Shared/FeelMath.h"

namespace mye {

// ---- 手触りのプリセット資産 (.feel.json) ----
// 1 ファイル = MyeFeelDesc 1 個 (ヒット・着地・被弾…)。スクリプトはファイル名 (拡張子抜き) の
// 名前ハッシュで、FeelTrigger は AssetRef (GUID) で引く。
//
// 決定論: ヒットストップ / スローの tick 数は sim を変える = physmat と同じ「再生時に同じ資産がある」
// 前提の資産クラス。値そのものは発火時に FeelState へコピーされるので、ワールドハッシュには
// 「鳴らした結果」として入る (資産の中身を別にハッシュする必要は無い)。
// 読み書きは起動走査 / ReloadHub / エディタのメインスレッドのみ。
struct FeelPreset {
    uint64_t hash = 0;     // AssetID (.meta の GUID、未移動なら正規化パスのハッシュ)
    uint64_t nameHash = 0; // 名前の FNV-1a (MyeNameHash / HashStr と同値) — PlayFeel(名前) の鍵
    std::string name;      // "HeavyHit" (ファイル名から ".feel.json" を除いたもの)
    std::wstring path;
    MyeFeelDesc desc;
};

struct FeelPresetEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .feel.json の管理 (PhysMatLibrary 範型)。EngineLoop / HeadlessSim が所有し feellib:: で注入
class FeelLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);
    static std::string NameFromPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, const MyeFeelDesc& desc); // 返り値 = hash

    const FeelPreset* Get(uint64_t hash) const;
    // 名前ハッシュで引く。同名が複数あるときはパスの序数比較で最小のもの (走査順に依らない)
    const FeelPreset* FindByName(uint64_t nameHash) const;
    bool Contains(uint64_t hash) const { return presets_.find(hash) != presets_.end(); }
    std::vector<FeelPresetEntry> Enumerate() const; // 名前昇順
    void Clear() { presets_.clear(); }

    static nlohmann::json ToJson(const MyeFeelDesc& d);
    // 読み値は SanitizeFeelDesc 済みで返す。手触りでない JSON ("feel" キー無し) は false。
    // 欠けたキーは既定値 (前方互換の contains 読み)
    static bool FromJson(const nlohmann::json& j, MyeFeelDesc& out);
    static nlohmann::json CurveToJson(const MyeCurve& c);
    static void CurveFromJson(const nlohmann::json& j, MyeCurve& out); // 無い / 壊れた欄は out のまま

private:
    std::unordered_map<uint64_t, FeelPreset> presets_;
};

// モジュール注入 (physmat:: と同じ流儀。EngineContext を汚さない)。メインスレッド専用
namespace feellib {
void Install(FeelLibrary* lib);
FeelLibrary* Library();                        // 未接続 = nullptr
const FeelPreset* Resolve(AssetID id);         // 未接続 / 未登録 / null ID = nullptr
const FeelPreset* ResolveName(uint64_t nameHash);
} // namespace feellib

} // namespace mye
