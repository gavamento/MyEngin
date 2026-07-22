#pragma once
#include <string>

namespace mye {

// 物理レイヤー名 (M36a)。assets\project_settings.json の "physicsLayers" 配列を読む
// エディタ表示専用テーブル — sim はレイヤー index しか見ないので決定論に無関係。
// 未定義スロットは "Layer N"。ParticleSystem と同じ read-override パターンで
// 他キーを保存時に破壊しない。
class PhysicsLayerNames {
public:
    static constexpr int kCount = 32;

    static PhysicsLayerNames& Get(); // エディタ内シングルトン (ComponentRegistry 前例)

    // 冪等 (同じ assetsRoot なら再読込しない)。ProjectSettingsWindow の保存後は force で
    void Load(const std::wstring& assetsRoot, bool force = false);
    bool Save(const std::wstring& assetsRoot) const;

    const char* Name(int i) const;          // 表示名 (常に非空)
    char* EditBuffer(int i) { return names_[i]; } // ProjectSettingsWindow の InputText 用
    // Combo 用の連結ラベル配列 (毎フレーム呼んで良い軽さ)
    void BuildComboLabels(const char* out[kCount]) const;

private:
    char names_[kCount][32] = {};
    std::wstring loadedRoot_;
};

} // namespace mye
