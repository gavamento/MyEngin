//====================================================================================
//                          Shape2DLibrary.h
//  MyEngin/ 秋田蓮音                                                       10/20/2026
//                                          2D 形状 (.shape2d.json) と衝突データの登録表
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"
#include "Engine/Engine/Physics/Collider/ConvexHull.h"
#include "Engine/Engine/Physics/Collider/MeshColliderLibrary.h"

namespace mye {

// 点の数の上限。耳刈りが最悪 O(n^3) なので、編集中の毎フレーム再構築でも固まらない範囲に抑える
inline constexpr size_t kMaxShape2DPoints = 512;
inline constexpr float kDefaultShape2DEdgeRadius = 0.02f;
inline constexpr float kDefaultShape2DDepth = 1.0f;

// 2D 形状 1 件。点列は形状のローカル XY 平面、z 方向は depth の厚みを持つ (中心 z = 0)。
// Collider.shape = kPolygon2D は点列を閉じた多角形として、kEdge2D は線分の列として使う。
// 衝突用の導出データは登録時に作る (静的 = プリズムの三角形スープ、動的 = プリズムの凸包)。
// 決定論: 導出は points / depth だけの純関数。ワールドハッシュ非対象 (「再生時に同じ資産がある」前提)
struct Shape2D {
    uint64_t hash = 0; // = GUID (ShapeLibrary2D のキー)
    std::string name;
    std::wstring path;
    std::vector<DirectX::XMFLOAT2> points;
    bool closed = true;                                // kEdge2D だけが見る。多角形は常に閉じる
    float edgeRadius = kDefaultShape2DEdgeRadius;      // kEdge2D の線分の半厚
    float depth = kDefaultShape2DDepth;                // z 方向の全厚

    MeshColliderData soup; // 静的多角形: 前後面 + 側面。点が 3 つ未満なら空
    ConvexHullData hull;   // 動的多角形: プリズム頂点の凸包。点が 3 つ未満なら空

    // points / depth から soup / hull を作り直す。points を書き換えたら呼ぶ
    void RebuildDerived();

    // 線分の数 (kEdge2D)。開いた列は points - 1、閉じた列は points (3 点以上のときだけ閉じる)
    int32_t SegmentCount() const;
    // 線分 i の端点 (SegmentCount() 未満の i)
    void Segment(int32_t i, DirectX::XMFLOAT2& a, DirectX::XMFLOAT2& b) const;
};

// 多角形を耳刈りで三角形に分ける。out は points への添字 3 つ組で、全て反時計回り (入力が時計回りでも揃える)。
// 凹も可。共線の頂点は面積 0 の三角形を出さずに落とす。耳が見つからない入力 (自己交差など) でも必ず終わる。
// 3 点未満は false
bool TriangulatePolygon2D(const std::vector<DirectX::XMFLOAT2>& points, std::vector<uint32_t>& outTris);

// 列挙 1 件 (ピッカー / Asset Browser 用)
struct Shape2DEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .shape2d.json の管理 (SpriteAnimLibrary と同じ範型)。EngineLoop / HeadlessSim が所有し shape2d:: で注入
class ShapeLibrary2D {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path);          // 失敗時 0
    uint64_t Register(const std::wstring& path, Shape2D shape); // 返り値 = hash

    const Shape2D* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return shapes_.find(hash) != shapes_.end(); }
    std::vector<Shape2DEntry> Enumerate() const; // 名前昇順 (反復順を表に出さない)

    static nlohmann::json ToJson(const Shape2D& s);
    // 読み値は Sanitize 済み。形状でない JSON ("shape2d" キー無し) は false
    static bool FromJson(const nlohmann::json& j, Shape2D& out);

    // 非有限の点を落とし、点数・半厚・厚みを範囲へ丸めて導出データを作り直す
    static void Sanitize(Shape2D& s);

private:
    std::map<uint64_t, Shape2D> shapes_; // 順序つき: 列挙がハッシュの反復順に依存しない
};

// モジュール注入 (spriteanim:: と同じ流儀)。EngineLoop / HeadlessSim が起動時に Install し、
// 終了時にライブラリ破棄前に外す。メインスレッド専用
namespace shape2d {
void Install(ShapeLibrary2D* lib);
ShapeLibrary2D* Library();                          // 未接続 = nullptr
const Shape2D* Resolve(AssetID id);                 // 未接続 / 未登録 / null ID = nullptr
const MeshColliderData* ResolveSoup(AssetID id);    // 点が 3 つ未満 / 未登録 = nullptr
const ConvexHullData* ResolveHull(AssetID id);      // 同上
} // namespace shape2d

} // namespace mye
