#pragma once
#include <d3d11.h>

#include <deque>
#include <vector>

#include "Engine/Renderer/PostProcess.h"
#include "Engine/Renderer/RenderTypes.h"
#include "Engine/Renderer/ShadowPass.h"

namespace mye {

class World;
class GraphicsDevice;
class IRenderPath;
class ShaderManager;
class ParticleSystem;
class VfxRenderer;
struct RenderResources;

// このフレームの描画先
struct FrameTarget {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    int width = 0;
    int height = 0;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
};

// エディタカメラ等でシーンカメラを上書きするためのビュー指定
struct CameraOverride {
    DirectX::XMFLOAT4X4 view = {};
    DirectX::XMFLOAT3 position = { 0, 0, 0 };
    float fovYDeg = 60.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;
};

// 環境コンポーネント収集 (M29d): 最初 (entity.index 最小) の active な Skybox/Fog を
// view に反映する。無ければ view の既定値 (-1 = 無効) のまま。ヘッドレス selftest 対象。
void CollectEnvironment(World& world, RenderView& view);

// ECS から描画アイテムを収集し、ソートして RenderPath に提出する (spec 5.1 システム層 / 6.3)。
// カメラ: isPrimary の CameraComponent (無ければ最初のカメラ)。override 指定時はそれを優先。
// ライト: 最初の LightComponent (向き = エンティティの +Z)
class RenderSystem {
public:
    // 戻り値: カメラが見つかった (または override があった) か。
    // particles を渡すとシーン描画後に Forward 後段としてパーティクルを重ねる
    // (M6.5 の Deferred でも共通の後段 — spec 7 章 / 6.1)。
    // vfx (M29c) は Sprite/Trail/TextMesh をメッシュ後・パーティクル前に重ねる
    bool Render(World& world, GraphicsDevice& device, IRenderPath& path, ShaderManager& shaders,
                RenderResources& resources, const FrameTarget& target,
                const CameraOverride* cameraOverride = nullptr,
                ParticleSystem* particles = nullptr, VfxRenderer* vfx = nullptr);

    // ポストプロセス設定 (M16)。config / エディタから書き換え可能。全ビューポート共通。
    PostProcess::Settings postFxSettings;
    bool enablePostFx = true; // false で HDR 配管を丸ごとバイパス (従来の直描き)
    bool enableShadows = true; // false で平行光シャドウを無効 (M17)

private:
    RenderQueue queue_;     // フレーム毎に再利用 (アロケーション回避)
    PostProcess postFx_;    // HDR 中間 + トーンマップ (遅延 Init)
    ShadowPass shadowPass_; // 平行光シャドウマップ (遅延 Init)
    // スキンメッシュのボーンパレット (M18)。フレーム毎に再構築。deque = push_back で
    // 既存要素の .data() ポインタが無効化されない (RenderItem.bones が参照する)
    std::deque<std::vector<DirectX::XMFLOAT4X4>> skinPalettes_;
};

} // namespace mye
