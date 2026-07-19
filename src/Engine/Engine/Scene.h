#pragma once
#include <string>
#include <string_view>

#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"

namespace mye {

// アクティブなワールドの所有者。M2 で JSON シリアライズ (保存/読込/Play スナップショット) が載る
class Scene {
public:
    GameObject CreateGameObject(std::string_view name)
    {
        return GameObject(&world_, world_.CreateEntity(name));
    }

    // 名前で線形検索 (最初に一致したもの)。見つからなければ無効な GameObject
    GameObject Find(std::string_view name);

    World& GetWorld() { return world_; }
    const std::string& Name() const { return name_; }
    void SetName(std::string_view name) { name_ = name; }

private:
    World world_;
    std::string name_ = "Untitled";
};

} // namespace mye
