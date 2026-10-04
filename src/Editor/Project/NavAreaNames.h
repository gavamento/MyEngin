//====================================================================================
//                          NavAreaNames.h
//  MyEngin/ 秋田蓮音                                                     10/04/2026
//                                          NavMesh エリア名 (project_settings.json、表示専用)
//====================================================================================
#pragma once
#include <string>

namespace mye {

// NavMesh のエリア名 (M82g)。assets\project_settings.json の "navAreas" 配列を読む。
// 表示専用 — sim はエリア番号しか見ないので決定論に無関係。0 = Walkable / 1 = Not Walkable / 2 = Jump は
// 固定名 (ファイルの値は無視)。他の未定義スロットは "Area N"。他キーを保存時に壊さない (PhysicsLayerNames と同じ流儀)
class NavAreaNames {
public:
    static constexpr int kCount = 16;
    static constexpr int kFixedCount = 3;   // 先頭の固定名のエリア数
    static constexpr int kNameCapacity = 32;

    NavAreaNames(); // 既定名で初期化 (Load 前でも Name() は非空)
    static NavAreaNames& Get(); // エディタ内シングルトン

    // 冪等 (同じ assetsRoot なら再読込しない)。保存後は force で
    void Load(const std::wstring& assetsRoot, bool force = false);
    bool Save(const std::wstring& assetsRoot) const;

    // 編集中の表が保存済みの内容と食い違うか (ディスクを読み直した表と比べる)。一度も Load していなければ false
    bool DiffersFromDisk() const;

    const char* Name(int i) const;                 // 表示名 (常に非空)
    char* EditBuffer(int i) { return names_[i]; }  // 固定名のスロットは編集させない
    static bool IsFixed(int i) { return i >= 0 && i < kFixedCount; }

private:
    char names_[kCount][kNameCapacity] = {};
    std::wstring loadedRoot_;
};

} // namespace mye
