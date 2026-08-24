#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Physics/ConvexHull.h"

namespace mye {

struct RenderResources;

// ---- 凸包コライダーのキャッシュ + クック (M60f、Collider.shape=5) ----
// MeshLibrary が持つ CPU 頂点から凸包を作ってキャッシュする。MeshColliderLibrary (shape=3)
// と同じ「AssetID → 形状データの lazy 構築」だが、**こちらはクックが乗る**:
//
//   `.mmdl` / `.mpcm` に続く CookedCache の第 3 の種 = `.mcvx`。1 ファイルに
//   「そのモデルファイル由来の凸包」を key (= メッシュ登録名) つきで並べた表を置き、
//   要求されたメッシュの分だけ**遅延で書き足していく**。モデルのロード時に全メッシュの
//   凸包を先に焼く設計にしないのは、「どのメッシュを凸包として使うか」を知っているのが
//   モデルではなくシーン (Collider.shape=5) だから — 先に焼くと使わない分まで焼ける。
//
// 元ファイルのパスは **AssetID の逆引き**で得る。モデル由来のメッシュ登録名は
// "<正規化絶対パス>#mesh0#prim0" なので、'#' の手前がそのままクックのソースパスになる。
// 手続き生成メッシュ (builtin:// / 地形チャンク / selftest) は '#' を持たないか実ファイルが
// 無いので、その場生成だけになる (CookedCache 側が stat に失敗して無効化されるため、
// 特別扱いのコードは要らない)。
//
// 決定論: 生成は入力頂点順に依らず (ConvexHull.h)、blob は生値なので
// **クックから読んだ凸包とフレッシュ生成した凸包はビット同一**。`.mmdl` と同じ契約で、
// これが崩れると「キャッシュの有無でワールドハッシュが変わる」= 最悪の壊れ方をする。
class ConvexColliderLibrary {
public:
    void Init(RenderResources* resources) { resources_ = resources; }
    // 未登録メッシュ / CPU 頂点なしは nullptr (呼び出し側は shape=5 を無視する)
    const ConvexHullData* Get(AssetID meshAsset);
    // 任意データの直接登録 (selftest / 手続き生成メッシュ用)。同 ID は差し替え
    void Register(AssetID id, ConvexHullData data);
    void Clear();

private:
    using CookTable = std::vector<std::pair<std::string, ConvexHullData>>; // key 昇順

    // srcPath の `.mcvx` を読み込む (未読なら 1 回だけ)。戻り値は表への参照
    CookTable& LoadTable(const std::wstring& srcPath);
    void SaveTable(const std::wstring& srcPath, const CookTable& table);

    RenderResources* resources_ = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<ConvexHullData>> cache_;
    std::unordered_map<std::wstring, CookTable> tables_;
};

// モジュール注入 (meshcol:: / terraincol:: と同じ流儀)。EngineLoop が起動時に Install し
// 終了時に外す。PhysicsSystem / クエリ / トリガーの pose 構築サイトが shape=5 の実体解決に
// 使う。メインスレッド専用
namespace convexcol {
void Install(ConvexColliderLibrary* lib);
const ConvexHullData* Resolve(AssetID meshAsset); // 未接続/未登録 = nullptr
} // namespace convexcol

// `.mcvx` の表 ⇄ blob (selftest から直接叩けるように公開する)
void SerializeConvexTable(const std::vector<std::pair<std::string, ConvexHullData>>& table,
                          std::vector<uint8_t>& out);
bool DeserializeConvexTable(const std::vector<uint8_t>& in,
                            std::vector<std::pair<std::string, ConvexHullData>>& out);

// メッシュ登録名からクック元ファイルのパスを切り出す ("<path>#mesh0#prim0" → "<path>")。
// '#' が無い = 手続き生成なので空を返す
std::wstring ConvexCookSourcePath(const std::string& meshName);

} // namespace mye
