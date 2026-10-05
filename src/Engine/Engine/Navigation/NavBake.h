//====================================================================================
//                          NavBake.h
//  MyEngin/ 秋田蓮音                                                     10/03/2026
//                                          ナビメッシュのベイク (三角形 -> タイルの層 -> .mnav の中身)
//====================================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/Navigation/NavBakeInput.h"
#include "Engine/Engine/Navigation/NavMeshAsset.h"

namespace mye {

class World;

// エディタのワーカーとの受け渡し。ベイクは進捗を書き、キャンセルを読むだけ
struct NavBakeControl {
    std::atomic<bool> cancel{false};
    std::atomic<int> tilesDone{0};
    std::atomic<int> tilesTotal{0};
};

enum class NavBakeStatus : uint8_t {
    Ok,
    Empty,     // 歩ける面が 1 枚も無い (入力の三角形が無い、または全て範囲外・急すぎる)
    Cancelled,
    Failed,    // Recast / TileCache の失敗、タイル数や ref のビット配分の超過
};

struct NavBakeOutput {
    NavBakeStatus status = NavBakeStatus::Failed;
    std::string message; // 失敗・空の理由 (英語。UI 側で表示する)
    NavMeshAsset::Data data;
    int polyCount = 0;   // 結果の要約 (タイル 1 枚あたりの最大は maxPolysPerTile の元)
    int tileCount = 0;   // 層が 1 枚以上あるタイルの数
    int linkCount = 0;   // 自動生成した Link の数 (M84e)
};

// 障害物の最大数 (dtTileCacheParams::maxObstacles)。Surface ごと。NavMeshObstacle が使う
inline constexpr int kNavMaxObstacles = 128;

// Surface グループ 1 つのベイクに必要な入力。World から取り出し済みなので、以降は World に触れずに焼ける
struct NavBakeInputs {
    NavBakeConfig config;
    NavTriangleSoup soup;
    std::vector<NavBakeClipBox> clipBoxes; // グループの Surface の範囲 (エンティティキー順)。歩けるのはこの中だけ
};

// Surface が属するグループ (同じ agentTypeId の有効な Surface 全部、M84b) の設定・範囲・三角形を集める。
// セル・タイル・寸法は代表 (NavFindSurfaceGroup の leader) の Surface から、範囲は全 Surface の AABB を合わせたもの。
// 三角形は Surface ごとの collectLayerMask で、その Surface の範囲と重なるコライダーから集める (1 つのコライダーは 1 回)。
// メインスレッド専用。エディタの Bake も、Editor 無しの SelfTest も、将来の実行時再ベイクも、ここから始める。
// Surface コンポーネントが無ければ false
bool NavPrepareBakeInputs(World& world, EntityID surface, NavBakeInputs& out);

// 入力ハッシュ (保存名の 16 桁)。設定・三角形・範囲の箱・ベイク方式の版から決まる
uint64_t NavComputeInputHash(const NavBakeConfig& config, const NavTriangleSoup& soup,
                             const std::vector<NavBakeClipBox>& clipBoxes);

// タイル 1 枚分: soup の中からこのタイルの範囲 (余白込み) に触れる三角形だけを選んで層にする。
// clipBoxes が空でなければ、その外の歩行面を歩けなくする。実行時の再ベイクもこの関数を呼ぶ (M82 spec 4.4 F2)
bool NavBakeTile(const NavBakeConfig& config, const NavTriangleSoup& soup, const std::vector<NavBakeClipBox>& clipBoxes,
                 int tx, int ty, std::vector<std::vector<uint8_t>>& outLayers);

// 全タイルを NavBakeTile で回して .mnav の中身を作る。config.generateLinks なら、組んだナビメッシュから Link を生成して
// data.links に入れる (NavGenerateLinks)。World には触れない (ワーカースレッドから呼べる)
NavBakeOutput NavBakeAsset(const NavBakeConfig& config, const NavTriangleSoup& soup,
                           const std::vector<NavBakeClipBox>& clipBoxes, NavBakeControl* control);

} // namespace mye
