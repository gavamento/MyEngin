//====================================================================================
//                          TagNames.h
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          タグ名の表と RT のタグ設定 (project_settings.json)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "Engine/Core/Components.h"

namespace mye {

// タグ名の表。`assets\project_settings.json` の `"tags"` 配列 (最大 kMaxTags = 64 要素) を読む。
// 物理レイヤー (Editor/PhysicsLayerNames) と同じ read-modify-write で、保存時に他キーを壊さない。
// ★エディタ専用ではなく **Engine 層に置く** — スクリプトの ABI (TagIndex) が名前から番号を
//   引くため。sim が見るのは番号だけで、名前はその番号へ着く手段にすぎない
//   (名前を変えても既存シーンの TagComponent::mask は切れない)。
// ★名前の空欄は「未使用の番号」。TagIndex は空欄には一致しない (Display は "Tag N" を返す)
class TagNames {
public:
    static constexpr int kCount = kMaxTags;
    static constexpr int kNameCapacity = 32;

    static TagNames& Get(); // プロセス内シングルトン (PhysicsLayerNames 前例)

    // 冪等 (同じ assetsRoot なら再読込しない)。保存直後は force で撮り直す。
    // ★スクリプトからも呼ばれうるが、実際に書き換わるのは root が変わった最初の 1 回だけ —
    //   EngineLoop が起動時に先に読むので、tick 中に表が書き換わることはない
    void Load(const std::wstring& assetsRoot, bool force = false);
    bool Save(const std::wstring& assetsRoot) const;
    // 編集中の表が保存済みの内容と食い違うか (PhysicsLayerNames::DiffersFromDisk と同じ理屈)
    bool DiffersFromDisk() const;

    const char* Name(int i) const;    // 登録名そのもの (空欄なら "")。範囲外も ""
    const char* Display(int i) const; // 表示名 (空欄なら "Tag N")
    char* EditBuffer(int i);          // ProjectSettingsWindow の InputText 用 (kNameCapacity バイト)
    // 名前 → 番号。空文字列 / 未登録は -1。大文字小文字は区別する (Unity の Tag と同じ)
    int32_t IndexOf(std::string_view name) const;

private:
    char names_[kCount][kNameCapacity] = {};
    std::wstring loadedRoot_;
};

// RT をどのタグに適用するか (project_settings.json の `"rayTracingTags"`)。
//   receiverMask = RT の反射 / GI / 影を**受ける**面のタグ。0 = 全部の面 (従来どおり)
//   sceneMask    = BVH に**入る**物のタグ (反射に映る / 影を落とす)。0 = 全部の物 (従来どおり)
// 保存はタグ番号の配列 ("receivers": [0, 3]) — 名前ではなく番号にするのは TagComponent と同じ理由
struct RtTagSettings {
    uint64_t receiverMask = 0;
    uint64_t sceneMask = 0;
};
// 読めなかった / キーが無いときは既定 (両方 0) を返す = 従来の挙動
RtTagSettings LoadRtTagSettings(const std::wstring& assetsRoot);
bool SaveRtTagSettings(const std::wstring& assetsRoot, const RtTagSettings& settings);

// "0,3,5" のようなタグ番号のカンマ区切りをビット集合へ (CLI の --rt-receiver-tags 用)。
// 範囲外・数字以外を含むなら false (out は触らない)。空文字列は 0 = 制限なし
bool ParseTagIndexList(std::wstring_view text, uint64_t& out);

} // namespace mye
