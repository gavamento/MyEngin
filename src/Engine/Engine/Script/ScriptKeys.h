#pragma once
#include <cstdint>

#include "Engine/Core/EntityID.h"
#include "Shared/MathPod.h" // MyeEntityId

namespace mye {

// エンジンの EntityID → スクリプト ABI の MyeEntityId。
// C++ スクリプトのホスト (ScriptHost)・C# のホスト (ManagedHost)・API 表 (EngineApiTable) が共有する
inline MyeEntityId ToShared(EntityID id)
{
    return { id.index, id.generation };
}

// Start 済みインスタンスの識別子 (M64b)。
// ★**エンティティ ID だけでは足りない**。同じエンティティに 2 つ目のスクリプトを
//   付けると、1 つ目が入れたキーで弾かれて 2 つ目の `Start()` が一度も呼ばれない、
//   という穴が M64a まで開いていた。`Update` / `LateUpdate` は無条件に回るので
//   「初期化だけ静かに効かない」という一番追いにくい形で出る。
// ★エンティティ側は index<<32|generation で 64bit を使い切っているので、
//   スクリプト型を同じ語に詰めることはできない。2 語持つ。
struct ScriptStartedKey {
    uint64_t entity = 0; // index<<32 | generation
    uint64_t script = 0; // そのスクリプト型の ComponentTypeId

    friend bool operator<(const ScriptStartedKey& a, const ScriptStartedKey& b)
    {
        return (a.entity != b.entity) ? (a.entity < b.entity) : (a.script < b.script);
    }
    friend bool operator==(const ScriptStartedKey& a, const ScriptStartedKey& b)
    {
        return a.entity == b.entity && a.script == b.script;
    }
};

// ★C++ と C# の**両ホストがこの 1 本で**キーを作る。M64b の修正は C++ 側にだけ入り、
//   C# 側はエンティティだけで引くコピーが残っていた (1 エンティティの 2 本目の C# スクリプトの
//   Start が呼ばれなかった)
inline ScriptStartedKey MakeScriptStartedKey(EntityID e, uint64_t scriptType)
{
    ScriptStartedKey k;
    k.entity = (static_cast<uint64_t>(e.index) << 32) | e.generation;
    k.script = scriptType;
    return k;
}

} // namespace mye
