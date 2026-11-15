//====================================================================================
//                          SceneChangeWireBoxes.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          Scene View に枠を出す変更済みエンティティの選び方 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include <DirectXMath.h>

#include "Editor/ChangeTracking/OverlayPalette.h"
#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

class Scene;
class SceneChangeOverlay;

// 1 フレームに描くワイヤ箱の上限 (変更が数千あっても線を数万本積まない。要求 §14.1)
constexpr int kMaxChangeWireBoxes = 256;

// 画面の端からどれだけはみ出した中心まで「見えている」と数えるか (大きい箱の中心が外でも枠が見えることがある)
constexpr float kChangeBoxVisibleMargin = 1.5f;

struct ChangeBoxCandidate {
    uint64_t fileId = 0;
    EntityID entity = kNullEntity;
    DirectX::XMFLOAT3 position = {};
    OverlayColorId color = OverlayColorId::Modified;
    bool visible = false;
    float depth = 0.0f; // カメラ空間の前方への距離 (近いほど優先)
    bool uncertain = false; // 帰属が未確認 (Scene View が小さい印を足す)
};

// 点が画面に見えているか (画面の端から kChangeBoxVisibleMargin 倍までは見えているとみなす)。depth = カメラ空間の前方への距離。
// ワイヤ箱とゴーストの候補が同じ判定を使う
bool ProjectChangePoint(const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj, float& depth);

struct ChangeBoxSelection {
    std::vector<ChangeBoxCandidate> chosen; // 描くもの (近い順)
    int omitted = 0;                        // 見えているのに上限で描かなかった数
};

// 開いているシーンの変更済み (追加・更新) エンティティのうち、ライブのシーンに居るものを候補にする。
// **World は読むだけ** (FindByFileId の内部の索引を温める以外に状態を変えない。fileId を採番しない)。
// 強調が切なら空。走査順は不定なので、順序に使う側は SelectChangeBoxes で並べ直す
void CollectChangeBoxCandidates(Scene& scene, const SceneChangeOverlay& overlay, const DirectX::XMFLOAT4X4& view,
                                const DirectX::XMFLOAT4X4& proj, std::vector<ChangeBoxCandidate>& out);

// 見えているものだけを、近い順 (同じ距離なら fileId の昇順 = 決定的) に maxBoxes 件まで選ぶ。
// 見えていないものは描かない (画面外の線は積んでも見えない)
ChangeBoxSelection SelectChangeBoxes(std::vector<ChangeBoxCandidate> candidates, int maxBoxes);

} // namespace mye
