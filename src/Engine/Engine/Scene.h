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

    // エディタ経由の生成: fileId を即採番して付与する。
    // Undo/Redo の同一性キーになり、Play/Stop や再ロードで EntityID が変わっても追跡できる。
    // (シリアライザのロード経路は fileId をファイル値で設定するため CreateGameObject を使う)
    GameObject CreateGameObjectTracked(std::string_view name)
    {
        GameObject o = CreateGameObject(name);
        o.AddComponent<FileIdComponent>()->value = NextFileId();
        return o;
    }

    // 名前で線形検索 (最初に一致したもの)。見つからなければ無効な GameObject
    GameObject Find(std::string_view name);

    // fileId で線形検索 (シーンリロードの差分適用用)
    GameObject FindByFileId(uint64_t fileId);

    // e に fileId が無ければ採番して返す (Undo/選択が同一性キーとして使う)。0 = 無効
    uint64_t EnsureFileId(EntityID e);

    // 全エンティティ破棄 (名前と nextFileId は保持)
    void Clear() { world_.Clear(); }

    World& GetWorld() { return world_; }
    const std::string& Name() const { return name_; }
    void SetName(std::string_view name) { name_ = name; }

    uint64_t NextFileId() { return nextFileId_++; }
    void SetNextFileId(uint64_t v) { nextFileId_ = v; }
    uint64_t PeekNextFileId() const { return nextFileId_; }

private:
    World world_;
    std::string name_ = "Untitled";
    uint64_t nextFileId_ = 1;
};

} // namespace mye
