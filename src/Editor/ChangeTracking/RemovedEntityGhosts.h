//====================================================================================
//                          RemovedEntityGhosts.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          削除されたエンティティのゴーストと Before の読み取り専用の表示 (宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include "nlohmann/json.hpp"

namespace mye {

class Scene;
class SceneChangeOverlay;

// 削除されたエンティティを、消す前の JSON (Before) だけから Scene View に重ねる (arch §7)。
// **World にエンティティを作らない**: ワールド行列は Before の LocalTransform と親の連鎖から合成し、読むだけで済ませる。
// ここは「何を・どの行列で・どの色で」の一覧を作る側。GPU の型には触れず、描くのは SceneViewWindow (規則 16)。

// 1 フレームに描くゴーストの上限 (半透明メッシュは線より重いので、ワイヤ箱の上限より低い)
constexpr int kMaxRemovedGhosts = 64;
// 半透明メッシュの不透明度 (0x66 = 40%。分岐のゴーストと同じ)
constexpr uint32_t kGhostMeshAlpha = 0x66u;
// Scene View でゴーストを選ぶ半径 (画面の px。原点の投影位置からの距離)
constexpr float kGhostPickRadiusPx = 18.0f;
// 親をたどる段数の上限 (壊れた Before の輪を止める)
constexpr int kMaxGhostParentChain = 256;

enum class GhostShape : uint8_t {
    Mesh,   // 今読み込まれているメッシュの半透明
    Box,    // ワイヤ箱 (メッシュが引けない・スキンメッシュ・スプライト・パーティクル・ライト・UI など)
    Marker, // 位置の印だけ (描画のコンポーネントが無い)
};

// 描いたゴーストに付ける注記 (AI Changes 窓の Before の表示が文面にする)
enum GhostNote : uint32_t {
    kGhostNoteParentUnknown = 1u << 0, // 親の位置が分からない (ルートとして、ローカルの位置で描いた)
    kGhostNoteNoTransform = 1u << 1,   // LocalTransform が無い (原点で描いた)
    kGhostNoteMeshCurrent = 1u << 2,   // メッシュは今の内容 (削除の時点の内容とは限らない)
};

// Before の JSON (SceneSerializer の 1 エンティティ分) から、ゴーストに要る部分だけ読んだもの
struct ParsedBefore {
    uint64_t fileId = 0;
    std::string name;
    bool hasParent = false;
    uint64_t parentFileId = 0;
    bool hasTransform = false;
    DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT3 scale = { 1.0f, 1.0f, 1.0f };
    bool hasMeshRenderer = false;
    uint64_t meshAsset = 0;
    bool hasOtherRenderable = false; // SkinnedMesh / SpriteRenderer / ParticleEmitter / Light / UIElement
};

// 読めたら true。JSON でない・オブジェクトでない・fileId が無いときは false (その行は「表示できない」になる)。
// LocalTransform や MeshRenderer が無い・読めない値は、無いものとして扱う (位置だけ・描けないに落ちる)
bool ParseRemovedBefore(const std::string& fragment, ParsedBefore& out);

// LocalTransform から行列を作る。TransformSystem の ComputeWorldFrom (XMMatrixAffineTransformation) と同じ式。
// 試験が、同じエンティティがライブにいたときの WorldMatrix と一致することを確かめる
DirectX::XMFLOAT4X4 GhostLocalMatrix(const ParsedBefore& before);

// ライブのシーンの、いま居るエンティティのワールド行列を引く口 (読むだけ。居なければ false)
using LiveWorldLookup = std::function<bool(uint64_t fileId, DirectX::XMFLOAT4X4& world)>;

// fileId のワールド行列を、親の連鎖をたどって合成する (子 → 親の順に local を掛ける)。
// 親は (a) 同じ表の削除済み (removed の Before)、(b) ライブに今いるもの (live の今の行列)、の順で探す。
// どちらにも居ない・輪になっている・段数が多すぎるときは、そこで打ち切ってルートとして扱い、notes に kGhostNoteParentUnknown を立てる。
// removed に fileId が無ければ false
bool ComposeGhostWorld(uint64_t fileId, const std::unordered_map<uint64_t, ParsedBefore>& removed, const LiveWorldLookup& live,
                       DirectX::XMFLOAT4X4& world, uint32_t& notes);

struct RemovedGhostCandidate {
    uint64_t fileId = 0;
    std::string name;
    GhostShape shape = GhostShape::Marker;
    uint64_t meshAsset = 0; // shape == Mesh のとき、引くメッシュ
    DirectX::XMFLOAT4X4 world = {};
    DirectX::XMFLOAT3 position = {};
    bool uncertain = false;
    uint32_t notes = 0;
    bool visible = false;
    float depth = 0.0f; // カメラ空間の前方への距離 (近いほど優先)
};

struct RemovedGhostScan {
    std::vector<RemovedGhostCandidate> candidates;
    int undrawable = 0; // Before が無い・大きすぎて保持していない・読めない (位置が分からない)
};

// メッシュが今読み込まれていて描けるか。読むだけで、新しく読み込まない
using MeshAvailable = std::function<bool(uint64_t meshAsset)>;

// 開いているシーンの、削除されたエンティティ (overlay の status = removed) を候補にする。
// **World は読むだけ** (FindByFileId の内部の索引を温める以外に状態を変えない。fileId を採番しない)。
// 強調か、ゴーストの表示が切のとき・追跡サービスと話せていないときは空。候補は fileId の昇順 (決定的)
void CollectRemovedGhostCandidates(Scene& scene, const SceneChangeOverlay& overlay, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj,
                                   const MeshAvailable& meshAvailable, RemovedGhostScan& out);

struct RemovedGhostSelection {
    std::vector<RemovedGhostCandidate> chosen; // 描くもの (近い順)
    int omitted = 0;                           // 見えているのに上限で描かなかった数
};

// 見えているものだけを、近い順 (同じ距離なら fileId の昇順) に maxGhosts 件まで選ぶ
RemovedGhostSelection SelectRemovedGhosts(std::vector<RemovedGhostCandidate> candidates, int maxGhosts);

struct GhostPickTarget {
    uint64_t fileId = 0;
    DirectX::XMFLOAT3 position = {};
};

// 画面上の (mouseX, mouseY) [画像の左上が原点の px] に最も近いゴーストの fileId。radiusPx より遠いものしか無ければ 0。
// Scene View のピックが、ライブのエンティティに当たらなかったときにだけ呼ぶ
uint64_t HitTestRemovedGhosts(const std::vector<GhostPickTarget>& targets, const DirectX::XMFLOAT4X4& view, const DirectX::XMFLOAT4X4& proj, float width,
                              float height, float mouseX, float mouseY, float radiusPx);

// ---- Before の読み取り専用の表示 ----

enum class BeforeProblem : uint8_t {
    None,
    NoBefore,   // Before が来ていない
    Oversize,   // 大きすぎて保持していない
    Unreadable, // JSON として読めない
};

struct BeforeField {
    std::string name;
    std::string value; // JSON の値を 1 行にして、長ければ切ったもの
};

struct BeforeComponent {
    std::string name;
    std::vector<BeforeField> fields;
};

// 削除されたエンティティの Before の中身 (名前・親・コンポーネント・フィールドの値)。表示するだけで、編集の口は無い
struct RemovedBeforeView {
    bool shown = false; // 何かを出す (problem != None でも、理由を出す)
    BeforeProblem problem = BeforeProblem::None;
    int64_t fileId = 0;
    int64_t revisionId = 0; // 削除した変更。0 = 分からない
    std::string name;
    bool hasParent = false;
    int64_t parentFileId = 0;
    std::vector<BeforeField> entityFields; // childIndex / tagMask など、エンティティ直下の値
    std::vector<BeforeComponent> components;
};

// Before の断片から表示の中身を作る。World にも GPU にも触れない
RemovedBeforeView BuildRemovedBeforeView(int64_t fileId, int64_t revisionId, bool hasBefore, bool oversize, const std::string& fragment);

} // namespace mye
