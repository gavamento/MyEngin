#pragma once
#include <cstdint>
#include <string>

namespace mye {

// 部位タグ名の表 (M48f)。`assets\project_settings.json` の `"partTags"` 配列を読む
// **エディタ表示専用**テーブル — sim が見るのは `PartComponent::tag` (= 名前のハッシュ) だけ
// なので決定論には無関係。`PhysicsLayerNames` と同じ read-modify-write パターンで、
// 保存時に他キー (particle 設定 / physicsLayers) を壊さない。
//
// ★物理レイヤーとの決定的な違い: レイヤーは **番号** が実体で名前は飾りだが、
//   部位タグは **名前のハッシュ** が実体。つまりタグ名を書き換えると ID が変わり、
//   既にその ID を持っているシーンの参照が切れる。UI にもその注意書きを出す
class PartTagNames {
public:
    static constexpr int kMaxTags = 64;
    static constexpr int kNameCapacity = 32;

    static PartTagNames& Get(); // エディタ内シングルトン (PhysicsLayerNames 前例)

    // 冪等 (同じ assetsRoot なら再読込しない)。保存直後は force で撮り直す
    void Load(const std::wstring& assetsRoot, bool force = false);
    bool Save(const std::wstring& assetsRoot) const;

    int Count() const { return count_; }
    void SetCount(int n); // 増やした分は空文字列。Save 時に空の要素は落ちる

    const char* Name(int i) const;   // 範囲外は ""
    uint64_t Id(int i) const;        // HashStr(Name(i))。空名は 0
    char* EditBuffer(int i);         // ProjectSettingsWindow の InputText 用 (kNameCapacity バイト)

    // タグ ID → 登録名。未登録なら nullptr (Inspector が「(unknown)」を出す用)
    const char* NameOf(uint64_t tag) const;

private:
    char names_[kMaxTags][kNameCapacity] = {};
    int count_ = 0;
    std::wstring loadedRoot_;
};

} // namespace mye
