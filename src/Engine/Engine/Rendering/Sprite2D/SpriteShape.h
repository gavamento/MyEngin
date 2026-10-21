//====================================================================================
//                          SpriteShape.h
//  MyEngin/ 秋田蓮音                                                       10/22/2026
//                                          SpriteShape2D のプロファイル登録表と輪郭・縁・塗りの純関数
//====================================================================================
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "nlohmann/json.hpp"

#include "Engine/Core/Ecs/EntityID.h"

namespace mye {

// プロファイルの縁の数と、1 区間あたりの分割数 (Catmull-Rom)
inline constexpr size_t kMaxSpriteShapeEdges = 16;
inline constexpr int kSpriteShapeSubdivisions = 8;
inline constexpr float kDefaultSpriteShapeHeight = 1.0f;
inline constexpr float kMinSpriteShapeHeight = 0.001f;
inline constexpr float kMaxSpriteShapeHeight = 100.0f;
// 衝突の辺の箱: 半厚 (線の両側へ) と z 方向の全厚。輪郭の線そのものが当たり面の中心
inline constexpr float kSpriteShapeColliderHalfThickness = 0.1f;
inline constexpr float kSpriteShapeColliderDepth = 1.0f;

// 縁 1 種。輪郭の区間の外向き法線の角度 (度。+Y が 0、反時計回りが正) が [angleFrom, angleTo] に入る区間にこのスプライトを敷く
struct SpriteShapeEdge {
    float angleFrom = -45.0f;
    float angleTo = 45.0f;
    uint64_t sprite = 0; // SpriteLibrary の AssetID
};

// .spriteshape.json 1 件。fill は閉じた輪郭の内側を埋めるスプライト (0 = 塗らない)。
// 描画専用の資産で、sim は引かない (衝突は輪郭の点列だけから作る)
struct SpriteShapeProfile {
    uint64_t hash = 0; // = GUID (SpriteShapeLibrary のキー)
    std::string name;
    std::wstring path;
    uint64_t fill = 0;
    std::vector<SpriteShapeEdge> edges; // 配列順が優先順 (角度が重なったら先勝ち)
    float height = kDefaultSpriteShapeHeight; // 縁の帯の厚み (ワールド単位)
};

struct SpriteShapeEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .spriteshape.json の管理 (SpriteAnimLibrary と同じ範型)。EngineLoop / HeadlessSim が所有し spriteshape:: で注入
class SpriteShapeLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    uint64_t LoadFromFile(const std::wstring& path); // 失敗時 0
    uint64_t Register(const std::wstring& path, SpriteShapeProfile profile);

    const SpriteShapeProfile* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return profiles_.find(hash) != profiles_.end(); }
    std::vector<SpriteShapeEntry> Enumerate() const; // 名前昇順 (反復順を表に出さない)

    static nlohmann::json ToJson(const SpriteShapeProfile& p);
    // 読み値は Sanitize 済み。プロファイルでない JSON ("spriteshape" キー無し) は false
    static bool FromJson(const nlohmann::json& j, SpriteShapeProfile& out);

    // 縁の数と height を範囲へ丸める (非有限の角度・高さは既定へ)
    static void Sanitize(SpriteShapeProfile& p);

private:
    std::map<uint64_t, SpriteShapeProfile> profiles_; // 順序つき: 列挙がハッシュの反復順に依存しない
};

namespace spriteshape {

// ---- モジュール注入 (spriteanim:: と同じ流儀)。メインスレッド専用 ----
void Install(SpriteShapeLibrary* lib);
SpriteShapeLibrary* Library();               // 未接続 = nullptr
const SpriteShapeProfile* Resolve(AssetID id); // 未接続 / 未登録 / null ID = nullptr

// ---- 純関数 (GPU にも World にも依らない) ----

// 点列が「閉じた輪郭」として扱われるか。closed でも 3 点未満は開いた線のまま
inline bool IsClosedOutline(int count, bool closed)
{
    return closed && count >= 3;
}

// 点列 points[0..count) を折れ線に評価する。smooth = false は点そのまま。smooth = true は Catmull-Rom で、
// 1 区間を kSpriteShapeSubdivisions 分割する (開いた線の端は端点を複製して挟む)。
// 閉じた輪郭 (IsClosedOutline) では閉じる区間も含め、最初の点を末尾に重ねない。
// 戻る点数: 開 smooth = (count - 1) * 8 + 1 / 閉 smooth = count * 8 / smooth 無し = count。count <= 0 は空。
// sim (衝突) が使うので、三角関数を通さず float の四則だけで決める
void EvaluateSpline(const DirectX::XMFLOAT2* points, int count, bool closed, bool smooth,
                    std::vector<DirectX::XMFLOAT2>& out);

// 縁の選択。angleDeg (任意の実数) を [-180, 180] に畳んで、edges を先頭から見て最初に角度範囲へ入る縁の添字を返す。
// 範囲は ±360 度ずらした値も試すので、180 度をまたぐ範囲 (135..225) も書ける。angleFrom > angleTo は入れ替える。無ければ -1
int SelectEdge(const std::vector<SpriteShapeEdge>& edges, float angleDeg);

// 外向き法線 (nx, ny) の角度 (度)。+Y が 0、反時計回り (+X 側が負、-X 側が正)
float OutwardAngleDeg(float nx, float ny);

// 縁スプライト 1 種の解決済みの寸法と UV ((u0, v0) が左上)。width <= 0 は「描けない」(縁なし扱い)
struct EdgeSpriteInfo {
    float width = 0.0f; // スプライト 1 枚のワールド幅 (rect.w / ppu)
    float u0 = 0.0f;
    float v0 = 0.0f;
    float u1 = 1.0f;
    float v1 = 1.0f;
};

// 縁のクアッド 1 枚。頂点順は 左上, 右上, 右下, 左下 (上 = 外側)。edge = SpriteShapeProfile::edges の添字
struct EdgeQuad {
    int edge = 0;
    float x[4] = {};
    float y[4] = {};
    float u[4] = {};
    float v[4] = {};
};

// 輪郭の折れ線 (任意の座標系。ワールド XY を渡す) に沿って、縁のクアッド列を作る。
//  - 重複点はまとめる。閉じた輪郭は時計回りに揃える (外側が進行方向の左)。開いた線は進行方向の左が外側 = スプライトの上
//  - 区間ごとに外向き法線の角度で縁を選び (SelectEdge)、帯は輪郭の線を中心に height の厚み
//  - u は区間の長さをスプライト幅で割って繰り返し、端数は UV を切る。同じ縁が続く区間はスプライトの位相を引き継ぎ、
//    継ぎ目は法線の平均 (マイター) で隙間を作らない。縁が変わる / 縁なしの区間で位相は 0 に戻る
//  - sprites は edges と同じ並び (足りない縁は描けない扱い)
//  - クアッドが maxQuads を超えたら打ち切り、true を返す
bool BuildEdgeQuads(const std::vector<DirectX::XMFLOAT2>& polyline, bool closed, const std::vector<SpriteShapeEdge>& edges,
                    const std::vector<EdgeSpriteInfo>& sprites, float height, size_t maxQuads,
                    std::vector<EdgeQuad>& out);

// 塗りの頂点 (3 頂点 1 組の三角形リスト)。u / v はスプライトの UV 範囲へ写した値
struct FillVertex {
    float x = 0.0f;
    float y = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

// 閉じた輪郭 (3 点以上) を耳刈りで三角形に割り、座標系の原点を基準にした tileW x tileH の格子で切って、
// 各格子の中でスプライトの UV 範囲 (u0, v0)-(u1, v1) を 1 回敷く (UV = 座標 / スプライト寸法の繰り返しと同じ。
// wrap サンプラが無いので頂点で繰り返す)。頂点が maxVertices を超えたら打ち切って true
bool BuildFill(const std::vector<DirectX::XMFLOAT2>& polyline, float tileW, float tileH, float u0, float v0, float u1,
               float v1, size_t maxVertices, std::vector<FillVertex>& out);

} // namespace spriteshape
} // namespace mye
