#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Core/Hash.h"
#include "Engine/Engine/Physics/Shapes.h"

namespace mye {

class World;
struct PartBoundsComponent;

// 部位 (ソケット) の検索 API (M48f)。`PartComponent` を持つ子エンティティを
// 「名前パス」または「タグ」で引く。ゲームコードの本命ユースケースは
//   `Instantiate(effect, Parts::FindPart(w, enemy, "Hips/LegL"))`
// のような **アセット側が決めた場所へランタイムが物を付ける**動き。
//
// エディタにも Runtime にも要るので Engine 層に置く (Editor 層ではない)。
// タグ名 → ID の対応表はエディタ表示専用 (`Editor/PartTagNames.h`) で、
// **sim が触るのはハッシュ値だけ** = 決定論に無関係。
namespace Parts {

// タグ名 → タグ ID。`PartComponent::tag` と同じ値を返す唯一の定義。
// FNV-1a 64bit (Hash.h) — C# 側 (M48h) も同じ定数で実装する
constexpr uint64_t TagOf(std::string_view name) { return name.empty() ? 0ull : HashStr(name); }

// root サブツリーを '/' 区切りの**名前パス**で降下して引く ("Hips/LegL")。
// - 各セグメントは直子の名前と完全一致 (最初に一致したもの。M48b が兄弟名を一意にしている)
// - 空セグメント (先頭/末尾/連続の '/') は読み飛ばす。パスが空なら root 自身
// - 見つからなければ kNullEntity
// **PartComponent の有無は問わない** — 部位でない中間ノードを経由できないと使い物にならない
EntityID FindPart(World& world, EntityID root, std::string_view utf8Path);

// root サブツリーから tag 一致の PartComponent 持ちを **DFS 順** (root 先頭) に集める。
// ★入れ子プレハブの境界で止めない (フラット走査): 「ボス配下の全弱点」のような
//   使う側の視点を優先する。境界で止めたい要求が出たら flag を additive に足す
void FindPartsByTag(World& world, EntityID root, uint64_t tag, std::vector<EntityID>& out);

// 部位の骨供給元を解決する (M48i で 1 本化)。
//   explicitSource が生きていて SkinnedMesh を持つ → そのまま
//   そうでなければ **最も近い SkinnedMesh 祖先** (無ければ kNullEntity)
// ★PartFollowSystem (sim) と Inspector のジョイント一覧が**同じ答え**を見るための唯一の実装。
//   ここが 2 本あると「エディタで選べたジョイントに実行時は追従しない」が静かに起きる
EntityID ResolvePartSource(World& world, EntityID part, EntityID explicitSource);

// 部位の範囲 (M49) → ワールド ShapePose。**ワイヤ表示とレイ判定が同じ答えを見るための唯一の実装**
// (ResolvePartSource と同じ 1 本化原則)。規約は shapes::MakePoseFromMatrix の写し:
// 行ベクトル長 = スケール、正規化行 = 基底、無回転 fast-path。球の半径 =
// halfExtents.x × 最大軸スケール (ApplyScaledExtents と同じ max 規約)、箱は軸別。
// center はローカルオフセットとして wm でフル変換して位置に足す。
// ★shape 番号は PartBounds (0=箱 1=球) と ShapePose (0=球 1=箱) で**逆** — 変換はここだけ
ShapePose MakePartBoundsPose(const PartBoundsComponent& b, const DirectX::XMFLOAT4X4& wm);

// 部位ボリュームへのレイキャスト (M49)。root=kNullEntity でシーン全体、tag=0 で全部位。
// 対象は Part + PartBounds + WorldMatrix を**全部持つ**エンティティのみ (tag は Part 側)。
// dir は正規化済み前提 (shapes::Raycast の契約)。エディタのクリック選択も sim もこの 1 本。
// 決定論: entity.index 昇順走査 + 厳密 < のみ更新 = 同 t は低 index 勝ち (物理クエリの流儀)
struct PartRayHit {
    EntityID entity = kNullEntity;
    float distance = 0.0f;
    DirectX::XMFLOAT3 point = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 normal = { 0.0f, 0.0f, 0.0f };
};
bool RaycastParts(World& world, EntityID root, uint64_t tag, const DirectX::XMFLOAT3& origin,
                  const DirectX::XMFLOAT3& dir, float maxDist, PartRayHit& out);

// 部位の構造ロック (M48f)。「プレハブメンバ (PrefabLink 持ち) かつ Part 持ち」は
// **リネーム / 削除 / 再親化を禁止する**。部位はアセットが公開する API そのもので、
// インスタンス側で名前や場所を動かされると `FindPart("Hips/LegL")` が黙って壊れる。
// 値 (tag / joint / Transform) の上書きは従来どおり自由 (M48e の override リストに乗る)
bool IsStructureLocked(World& world, EntityID e);

} // namespace Parts
} // namespace mye
