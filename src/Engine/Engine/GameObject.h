#pragma once
#include "Engine/Core/Components.h"
#include "Engine/Core/World.h"

namespace mye {

// Unity 風の軽量ハンドル (engine_spec.md 4.1/4.3)。実体は EntityID + World*。
// - operator bool で生存確認 (Destroy 後の Unity 風 null チェックを再現)
// - GetComponent が返すポインタは当該 tick 内のみ有効 (アーキタイプ移動で無効化)。
//   tick を跨ぐ参照は GameObject (= EntityID) を保持すること
class GameObject {
public:
    GameObject() = default;
    GameObject(World* world, EntityID id) : world_(world), id_(id) {}

    explicit operator bool() const { return world_ != nullptr && world_->IsAlive(id_); }

    EntityID Id() const { return id_; }
    World* GetWorld() const { return world_; }
    const char* Name() const { return world_ ? world_->GetName(id_) : ""; }

    template <typename T> T* AddComponent() { return world_ ? world_->AddComponent<T>(id_) : nullptr; }
    template <typename T> T* GetComponent() { return world_ ? world_->GetComponent<T>(id_) : nullptr; }
    template <typename T> void RemoveComponent() { if (world_) { world_->RemoveComponent<T>(id_); } }

    void Destroy() { if (world_) { world_->DestroyEntity(id_); } } // 実削除は tick 末 (spec 4.3)
    void SetParent(GameObject parent) { if (world_) { world_->SetParent(id_, parent.id_); } }

    // ---- Transform ユーティリティ ----
    void SetLocalPosition(float x, float y, float z)
    {
        if (auto* t = GetComponent<LocalTransform>()) {
            t->position = { x, y, z };
        }
    }
    void SetLocalScale(float x, float y, float z)
    {
        if (auto* t = GetComponent<LocalTransform>()) {
            t->scale = { x, y, z };
        }
    }
    // オイラー角 (度、pitch/yaw/roll) からクォータニオンを設定
    void SetLocalRotationEuler(float pitchDeg, float yawDeg, float rollDeg)
    {
        if (auto* t = GetComponent<LocalTransform>()) {
            const float d2r = 3.14159265358979323846f / 180.0f;
            const DirectX::XMVECTOR q =
                DirectX::XMQuaternionRotationRollPitchYaw(pitchDeg * d2r, yawDeg * d2r, rollDeg * d2r);
            DirectX::XMStoreFloat4(&t->rotation, q);
        }
    }

private:
    World* world_ = nullptr;
    EntityID id_ = kNullEntity;
};

} // namespace mye
