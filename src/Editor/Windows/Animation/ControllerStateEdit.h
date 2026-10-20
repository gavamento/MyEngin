//====================================================================================
//                          ControllerStateEdit.h
//  MyEngin/ 秋田蓮音                                                       10/08/2026
//                                  コントローラ窓のステート編集 (純関数)
//====================================================================================
#pragma once
#include <string>
#include <vector>

#include "Engine/Engine/Animation/AnimatorController.h"

namespace mye {

struct SkinnedModel;

// ステートが骨をどう駆動するか (M89g)。アセット上は blendType と skelClip の組で表され、この型は持たない。
// プロパティクリップ (clipHash) とは独立 — どの種類でもプロパティクリップを併用できる
enum class StateSkelKind : int32_t {
    None = 0,    // 骨を駆動しない (skelClip が空、blendType == None)
    Clip = 1,    // 骨クリップ 1 本 (skelClip)
    Blend1D = 2,
    Blend2D = 3,
    Sprite = 4,  // スプライト列クリップ (M94、spriteHash)。骨の駆動とは排他
};

StateSkelKind GetStateSkelKind(const ControllerState& state);

// 種類を切り替える。窓のコンボを行き来しても編集中の値を失わないよう、使わなくなる側も消さない
// (blendChildren は blendType == None の間は読まれず、保存もされない)。ただし None は skelClip を空にする
// — skelClip が残っていると骨を駆動してしまうため。
// - Clip へ: skelClip が空なら、最初の名前付きの子 → fallbackClip の順で埋める
// - Blend1D / Blend2D へ: 子が無ければ skelClip (空なら fallbackClip) の子を 1 本作る
// - Sprite へ: skelClip を空にし、spriteHash が 0 なら fallbackSprite で埋める (候補が無ければ None のまま)
// - Sprite 以外へ: spriteHash を 0 にする (読み込みは sprite を優先するので、残すと骨の指定が捨てられる)
void SetStateSkelKind(ControllerState& state, StateSkelKind kind, const std::string& fallbackClip,
                      uint64_t fallbackSprite = 0);

// 骨クリップの名前を書き、ハッシュを揃える (ハッシュだけ古いと別のクリップを駆動する)
void SetStateSkelClip(ControllerState& state, const std::string& clip);
void SetBlendChildClip(ControllerBlendChild& child, const std::string& clip);

// 子を末尾に足す。位置は最後の子の隣 (1D は閾値 +1、2D は x +1) にして、既存の子と重ならないようにする
void AddBlendChild(ControllerState& state, const std::string& clip);

// モデルの名前付きの骨クリップ (並びはモデルの index 順)。無名のクリップは名前で引けないので出さない
std::vector<std::string> NamedSkeletalClips(const SkinnedModel* model);

} // namespace mye
