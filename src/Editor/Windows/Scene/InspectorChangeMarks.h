//====================================================================================
//                          InspectorChangeMarks.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Inspector に出す AI の変更の印 (Field 差分・コンポーネントの印、宣言)
//====================================================================================
#pragma once
#include <cstdint>

#include "imgui.h"

namespace mye {

class SceneChangeOverlay;
struct OverlayEntity;

// Inspector の見出しとフィールド行の右に、変更の印 (小さい点) と、ホバーで Before / After を出す。
// 描画専用: World にも Undo にも触れず、直前のアイテムの状態を読まない (呼ぶ側がフィールドの編集の処理を済ませてから呼ぶ)
class InspectorChangeMarks {
public:
    void SetOverlay(const SceneChangeOverlay* overlay) { overlay_ = overlay; }

    // 毎フレーム 1 回、Inspector を描く前に。数を 0 に戻す
    void BeginFrame()
    {
        fieldMarks_ = 0;
        componentMarks_ = 0;
        entity_ = nullptr;
    }
    // 表示するエンティティ (primary の fileId)。強調が切・表に無いときは何も出さない
    void SetEntity(uint64_t fileId);

    // コンポーネントの見出しの右端 (直前のアイテムが見出し)。offsetFromRight = 右端から点までの余白
    void DrawComponentMark(const char* componentName, bool fieldsListedInTooltip, float offsetFromRight);
    // フィールド行の直後に、行の右へ (直前のアイテムの右へ SameLine)
    void DrawFieldMark(const char* componentName, const char* fieldName);

    // 試験用
    int FieldMarks() const { return fieldMarks_; }
    int ComponentMarks() const { return componentMarks_; }

private:
    const SceneChangeOverlay* overlay_ = nullptr;
    const OverlayEntity* entity_ = nullptr;
    int fieldMarks_ = 0;
    int componentMarks_ = 0;
};

} // namespace mye
