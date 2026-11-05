//====================================================================================
//                          TwoBoneIkAutoSetup.h
//  MyEngin/ 秋田蓮音                                                     11/05/2026
//                                          TwoBoneIK を付けたときに手足の鎖と手の目標を埋める
//====================================================================================
#pragma once

#include <cstdint>

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class Scene;
struct SkinnedModel;

// 自動設定が埋める手足。値はそのまま TwoBoneIK の鎖の番号 (鎖 0 / 1 = 手、2 / 3 = 足)
enum class IkLimb : int32_t {
    HandL = 0,
    HandR = 1,
    FootL = 2,
    FootR = 3,
    Count = 4,
};

// 手足の先端ジョイント (見つからない手足は -1)
struct IkLimbJoints {
    int32_t joint[static_cast<int>(IkLimb::Count)] = { -1, -1, -1, -1 };
};

// 骨の名前から手首・足首を探す。名前は ':' / '|' より前の接頭辞 (mixamorig: など) を落とし、
// 区切り・大小の切り替わり・数字で語に分けて、部位語 (hand / wrist、foot / ankle) と左右 (l / left、r / right) を
// ちょうど 1 つずつ含むものを候補にする。指・つま先・IK 補助・末端 (end / nub) の語を含む骨は除く。
// 2 ボーンの鎖が組めること (親と祖父母がある) を必須にし、候補が複数ならルートに最も近い骨 (同じ深さなら index の小さい方)。
// ★名前の手がかりが無い骨 (CesiumMan の leg_joint_L_5 等) は形だけで推し量らない — 誤った骨に IK を掛けるより、
//   空のまま手で選ばせるほうが原因を追いやすい
IkLimbJoints FindIkLimbJoints(const SkinnedModel& model);

struct TwoBoneIkAutoSetupResult {
    uint32_t filledMask = 0;     // 今回埋めた鎖 (bit = 鎖の番号)
    uint32_t missingMask = 0;    // 骨が見つからなかった手足 (bit = 鎖の番号)
    int32_t createdTargets = 0;  // 作った目標の子エンティティの数
};

// mesh (SkinnedMesh と TwoBoneIK を持つエンティティ) の鎖を model の骨から埋める。
//   - 足 (鎖 2 / 3): mode = 接地、重み 1。付けた時点で足が地面に乗る
//   - 手 (鎖 0 / 1): mode = 位置、重み 0 (待機)。目標として mesh の子を手首のバインド姿勢に作る。
//     重みを上げれば効く。最初から 1 にしないのは、アニメ中も手がバインド位置に張り付くため
// 先端ジョイントが既に入っている鎖と、同じ骨を既に指している鎖がある手足には触らない (再実行で手の設定を壊さない)。
// 子を作るのでエディタ操作からのみ呼ぶ (MakeUniqueSiblingName と同じ制約)。構造変更は中で確定させる。
// Undo は呼び出し側が mesh のサブツリーを Before / After で撮って囲む
TwoBoneIkAutoSetupResult ApplyTwoBoneIkAutoSetup(Scene& scene, EntityID mesh, const SkinnedModel& model);

} // namespace mye
