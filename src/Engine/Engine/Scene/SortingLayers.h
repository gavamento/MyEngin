//====================================================================================
//                          SortingLayers.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          2D スプライトのソート層の表 (project_settings.json)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// ソート層 1 件。id は Sprite2D などが保存する値で、改名・並べ替えでは変わらない
struct SortingLayer {
    int32_t id = 0;
    char name[32] = {};
};

// ソート層の表。`assets\project_settings.json` の `"sortingLayers": [{"id":0,"name":"Default"}, ...]`。
// 配列の順 = 描画順 (先頭が奥)。id は 0..31 の未使用最小値を割り当て、削除で空く。
// 上限 32 は 2D ライトの targetLayerMask (uint32、bit = id) のため。id 0 (Default) は消せない。
// 描画専用で sim の入力ではない。他キーを保存時に壊さない (TagNames と同じ流儀)。メインスレッド専用
class SortingLayers {
public:
    static constexpr int kMaxLayers = 32;
    static constexpr int32_t kDefaultId = 0;

    SortingLayers(); // Default だけで初期化
    static SortingLayers& Get();

    // 冪等 (同じ assetsRoot なら再読込しない)。保存後は force で
    void Load(const std::wstring& assetsRoot, bool force = false);
    bool Save(const std::wstring& assetsRoot) const;
    // 編集中の表が保存済みの内容と食い違うか。一度も Load していなければ false
    bool DiffersFromDisk() const;

    int Count() const { return static_cast<int>(layers_.size()); }
    const SortingLayer& At(int index) const { return layers_[index]; }
    SortingLayer& EditAt(int index) { return layers_[index]; } // ProjectSettingsWindow の InputText 用

    int IndexOfId(int32_t id) const; // 無ければ -1
    // 名前 → id。無ければ -1 (大文字小文字を区別する)
    int32_t IdOf(const char* name) const;

    // 末尾に追加して id を返す。満杯なら -1
    int32_t Add(const char* name);
    // id 0 と存在しない id は false
    bool Remove(int32_t id);
    bool Rename(int32_t id, const char* name);
    // 添字 from の層を添字 to へ動かす。範囲外は false
    bool Move(int from, int to);

    // 描画順の添字。表に無い id は Default として扱う (参照先の消えたスプライトを描き続けるため)
    int OrderOf(int32_t id) const;

private:
    std::vector<SortingLayer> layers_;
    std::wstring loadedRoot_;
};

} // namespace mye
