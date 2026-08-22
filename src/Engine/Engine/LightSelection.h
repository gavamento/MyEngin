#pragma once
#include <cstdint>

#include <DirectXMath.h>

#include "Engine/Renderer/FrustumCull.h"
#include "Engine/Renderer/RenderTypes.h"

namespace mye {

// ライト選別 (M54b)。「どのライトを GPU へ送るか / どれが影を投げるか」を
// RenderSystem のライト収集から純関数として切り出したもの。
//
// なぜ切り出したのか:
//   ・M54c 以降のシャドウアトラスは「影を投げるライトの列」が frame 間で安定して
//     いることを前提に矩形を割り当てる。順序が揺れると影がポップする。
//     安定性を保証する場所は 1 箇所でないと守れない。
//   ・描画ロードマップ (M54〜M58) で唯一まともに selftest 化できる論理がここ。
//     ECS も D3D も触らないので、入力を作って出力を検査できる。
//
// カリングもソートも描画専用でワールドハッシュには一切関与しない (M16 と同じ立場)。

// 影を投げられる局所ライト (点/スポット) の上限。
// M54c のアトラス 4096^2 を 1024^2 タイルに割ると 16 枚 = 点光源 (6 面) 2 本 +
// スポット (1 面) 数本、で埋まる。実測は M54d — そこで見直す前提の暫定値
constexpr int kMaxShadowLights = 4;

// 選別の入力 1 件。ECS 走査の結果を純データへ落としたもの。
struct LightCandidate {
    GpuLight light = {};    // GPU 形へ変換済みの本体 (色はリニア、向きは正規化済み)
    uint32_t sortKey = 0;   // 決定論キーの第 2 位 = EntityID の index 部 (エンティティ毎に一意)
    int32_t castShadow = 0; // LightComponent::castShadow のミラー
};

// 選別の出力 1 件。
struct SelectedLight {
    GpuLight light = {};
    uint32_t sortKey = 0;    // 入力のキーをそのまま持ち回る (アトラス枠の同一性判定に使う)
    int32_t shadowSlot = -1; // 影の枠 0..maxShadowLights-1。投げないなら -1
};

// 選別の結果。lights[0..count) がそのまま SceneLightData へ流れる並び。
struct LightSelection {
    int count = 0;       // 有効件数
    int shadowCount = 0; // shadowSlot >= 0 の件数
    int culled = 0;      // 視錐台カリングで落ちた件数 (ログ / テスト用。上限で溢れた分は含まない)
    int overflow = 0;    // maxLights から溢れて捨てた件数 (同上)
    SelectedLight lights[kMaxLights] = {};
};

// 視錐台と球の交差 (保守的 = 「交差しない」と確信できるときだけ false)。
// ★BuildFrustum が返す平面は**正規化されていない** — AabbInFrustum は p-vertex の符号しか
//   見ないので不要だった。球は「平面までの距離」と半径を比べるので、法線長で割らないと
//   単位が合わず、遠くのライトが軒並み消えるか、逆に何も消えなくなる。
bool SphereInFrustum(const Frustum& f, const DirectX::XMFLOAT3& center, float radius);

// 範囲球 × 視錐台カリング → 決定論キー (type → sortKey 昇順) でソート → maxLights で切り詰め
// → 先頭から順に影スロットを割り当てる、までを一括で行う純関数。
//
// キーが type 優先なのは、上限で切り詰めたときに**平行光 (type=0) を必ず残す**ため。
// 従来の「登録順の先着 16 本」は、点光源が 16 本並んだシーンで太陽が落ちうる。
//
// frustum == nullptr でカリングを飛ばす (カメラ不在時。視錐台が単位行列由来のゴミになる)。
// 平行光は無限遠なのでカリング対象外。
//
// ★候補が 1 件も無いときだけ既定の平行光を 1 本補う (ライトの無いシーンでも見えるという
//   従来挙動)。「候補はあったが全部カリングで落ちた」ときに補ってはいけない —
//   画面外の点光源 1 本だけのシーンに、カメラを振った瞬間だけ太陽が湧く。
LightSelection SelectLights(const LightCandidate* cands, int count, const Frustum* frustum,
                            int maxLights = kMaxLights, int maxShadowLights = kMaxShadowLights);

} // namespace mye
