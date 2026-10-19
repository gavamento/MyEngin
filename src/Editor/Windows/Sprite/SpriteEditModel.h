//====================================================================================
//                          SpriteEditModel.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          スプライトエディタの編集モデル (スライスの純関数と Undo)
//====================================================================================
#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "Engine/Core/Asset/ImportMetaResolver.h"

namespace mye {
namespace spriteedit {

// 画素・左上原点の矩形
struct IRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool operator==(const IRect&) const = default;
};

enum class SliceMethod : int {
    DeleteExisting = 0, // 既存を全て捨てて置き換える
    Safe = 1,           // 既存を残し、既存と重なる新規は捨てる
};

constexpr int kPivotPresetCount = 9;
constexpr int kPivotCustom = kPivotPresetCount; // PivotPresetOf が一致しないときの値

// 並びは Center, TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight (Unity と同じ)。
// pivot は 0..1・左下原点
void PivotPreset(int preset, float& pivotX, float& pivotY);
// pivot がどのプリセットか。どれでもなければ kPivotCustom
int PivotPresetOf(float pivotX, float pivotY);

// 全セルを行優先 (上から下、左から右) で返す。keepEmpty = false かつ rgba (RGBA8、幅 texW) があれば、
// アルファが全て 0 のセルを捨てる。rgba が null なら keepEmpty に関係なく全部返す
std::vector<IRect> SliceGridByCellSize(int texW, int texH, int cellW, int cellH, int offX, int offY, int padX,
                                       int padY, bool keepEmpty, const uint8_t* rgba = nullptr);

// cols x rows 等分。セルの幅 = (texW - offX - padX * (cols - 1)) / cols (端数は捨てる)
std::vector<IRect> SliceGridByCellCount(int texW, int texH, int cols, int rows, int offX, int offY, int padX,
                                        int padY, bool keepEmpty, const uint8_t* rgba = nullptr);

// アルファ > 0 の 4 連結成分の外接矩形。幅か高さが minSize 未満のものは捨てる。
// 並びは各成分の最初の画素 (上から下、左から右) の順
std::vector<IRect> SliceAutomatic(const uint8_t* rgba, int texW, int texH, int minSize);

// 既存のスライスへ新しい矩形を適用した結果を返す。新規の id は firstId から、名前は "<namePrefix>_<n>" (既存と重複しない最小の n)
std::vector<importmeta::SpriteSlice> ApplySlices(const std::vector<importmeta::SpriteSlice>& existing,
                                                 const std::vector<IRect>& newRects, SliceMethod method,
                                                 float pivotX, float pivotY, const std::string& namePrefix,
                                                 uint32_t firstId);

bool RectsOverlap(const IRect& a, const IRect& b);

// 矩形をテクスチャ内に収める (最小 1x1)。texW / texH が 0 以下なら収めない
IRect ClampRectToTexture(IRect r, int texW, int texH);

} // namespace spriteedit

// スプライトエディタが編集中のスライス一覧。.meta の tex.sprites だけを編集し、
// filter / pixelsPerUnit など他の項目は Save のたびに .meta から読み直して保つ。
// Undo は BehaviorTreeEditModel と同じ流儀の独自 deque (シーンの UndoStack には載せない)
class SpriteEditModel {
public:
    static constexpr size_t kMaxUndo = 128;

    // .meta を読んで開く。.meta が無ければスライス 0 個。texW / texH はテクスチャの画素寸法
    bool Load(const std::wstring& texPath, int texW, int texH);
    void Clear();
    bool IsLoaded() const { return loaded_; }
    const std::wstring& Path() const { return path_; }
    int TexW() const { return texW_; }
    int TexH() const { return texH_; }
    const std::vector<importmeta::SpriteSlice>& Sprites() const { return sprites_; }
    // 新しいスライスに振る id (今の最大 + 1)
    uint32_t NextId() const;

    // 保存した (読み込んだ) 内容と違うか
    bool Dirty() const;

    // ---- 編集。値が変わらない呼び出しは何もせず false ----
    // 矩形を足して末尾の index を返す (テクスチャ内に収める)。name が空なら "sprite_<n>"
    int AddRect(const spriteedit::IRect& rect, float pivotX, float pivotY, const std::string& name);
    bool RemoveAt(int index);
    bool SetRect(int index, const spriteedit::IRect& rect);
    bool SetName(int index, const char* name);
    bool SetPivot(int index, float pivotX, float pivotY);
    bool SetBorder(int index, int left, int bottom, int right, int top);
    // 全スライスを置き換える (Apply)。1 段の Undo
    bool ReplaceAll(std::vector<importmeta::SpriteSlice> sprites);
    // 保存時点の状態へ戻す (Undo できる)
    bool RevertToSaved();

    // ---- Undo / Redo ----
    bool CanUndo() const { return !gesture_ && !undo_.empty(); }
    bool CanRedo() const { return !gesture_ && !redo_.empty(); }
    bool Undo();
    bool Redo();
    // ドラッグなどの連続変更を 1 段にまとめる。開いている間の最初の変更で 1 段だけ積む。戻り値 = 今開いたか
    bool BeginGesture();
    void EndGesture();
    bool InGesture() const { return gesture_; }
    size_t UndoDepth() const { return undo_.size(); }

    // ---- 保存 ----
    // .meta を読み直して tex.sprites だけ差し替えて書き、SpriteLibrary を登録し直す。
    // テクスチャ本体の再読込 (TextureLibrary::ReplaceFromFile) は呼び手の仕事
    bool Save();

private:
    using SpriteList = std::vector<importmeta::SpriteSlice>;

    void BeforeChange(); // 変更の直前に呼ぶ。Undo の 1 段を積む (ジェスチャ中は最初の 1 回だけ)
    static std::string Serialize(const SpriteList& sprites);

    bool loaded_ = false;
    std::wstring path_;
    int texW_ = 0;
    int texH_ = 0;
    SpriteList sprites_;
    SpriteList saved_;
    std::string savedKey_;
    std::deque<SpriteList> undo_;
    std::deque<SpriteList> redo_;
    bool gesture_ = false;
    bool gesturePushed_ = false;
    uint64_t revision_ = 0;
    mutable uint64_t dirtyRevision_ = ~0ull; // Dirty のキャッシュ
    mutable bool dirtyCache_ = false;
};

} // namespace mye
