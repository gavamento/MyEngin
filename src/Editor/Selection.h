#pragma once
#include "Engine/Core/EntityID.h"

namespace mye {

// エディタ内の選択状態 (Hierarchy と Inspector で共有)
struct Selection {
    EntityID entity = kNullEntity;
};

} // namespace mye
