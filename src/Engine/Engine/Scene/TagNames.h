//====================================================================================
//                          TagNames.h
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                              タグ名の表・RT のタグ設定・描画設定 (project_settings.json)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "Engine/Core/Ecs/Components.h"

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

// タグによる RT の一括 ON/OFF 規則 (project_settings.json の `"rayTracingTags"`)。
// 個別設定 (RayTracingComponent) が無い物にだけ効く。評価の規則は RtScope.h の ResolveRtScope。
//   scene*    = BVH に**入る**か (反射に映る / RT の影を落とす)
//   receiver* = RT の GI / 影 / 反射を**受ける**か
//   *On / *Off のどちらにも無いタグは規則なし。同じ物が両方に当たれば OFF が勝つ
// 保存はタグ番号の配列 ("sceneOn": [0, 3]) — 名前ではなく番号にするのは TagComponent と同じ理由
struct RtTagRules {
    uint64_t sceneOn = 0;
    uint64_t sceneOff = 0;
    uint64_t receiverOn = 0;
    uint64_t receiverOff = 0;
};
// 読めなかった / キーが無いときは規則なし (全部 0) = すべて既定 OFF
RtTagRules LoadRtTagRules(const std::wstring& assetsRoot);
bool SaveRtTagRules(const std::wstring& assetsRoot, const RtTagRules& rules);

// GPU オクルージョンカリングの ON/OFF (project_settings.json の `"rendering": {"occlusionCulling"}`)。
// キーが無い / 読めない = true。実効値はこの値 && CLI の --no-occlusion が無いこと (CLI は書き戻さない)
bool LoadOcclusionCullingSetting(const std::wstring& assetsRoot);
bool SaveOcclusionCullingSetting(const std::wstring& assetsRoot, bool enabled);
// 実効値 = ファイルの値 && CLI。EngineLoop と selftest が同じ式を使う
bool ResolveOcclusionCulling(const std::wstring& assetsRoot, bool cliOcclusionCulling);

// "0,3,5" のようなタグ番号のカンマ区切りをビット集合へ (CLI の --rt-receiver-tags 用)。
// 範囲外・数字以外を含むなら false (out は触らない)。空文字列は 0 = 制限なし
bool ParseTagIndexList(std::wstring_view text, uint64_t& out);

} // namespace mye
