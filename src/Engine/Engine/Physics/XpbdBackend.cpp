//====================================================================================
//                          XpbdBackend.cpp
//  MyEngine/ 秋田蓮音                                                      08/27/2026
//                                          XPBD 変形体の粒子池の同期（M60'b は器のみ）
//====================================================================================
#include "Engine/Engine/Physics/XpbdBackend.h"

namespace mye {

// M60'b 時点では変形体コンポーネント (Rope/Cloth/SoftBody) がまだ存在しないので
// 何もしない。M60'c がここに「コンポーネント走査 → 池の生成/破棄 (owner.index 昇順)」を
// 実装する。呼び出し点 (PhysicsSystem::Update の先頭) だけを先に配線してある —
// 後から呼び忘れる型の事故を器の段階で潰すため
void XpbdBackend::Sync(World& world)
{
    (void)world;
}

} // namespace mye
