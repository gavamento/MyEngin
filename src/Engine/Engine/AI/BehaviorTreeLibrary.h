//====================================================================================
//                          BehaviorTreeLibrary.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          ビヘイビアツリー資産 (.bt.json) とノード種類の表 (M85)
//====================================================================================
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

namespace mye {

constexpr int kBtMaxNodes = 1024;  // 1 アセットのノード数の上限
constexpr int kBtMaxDepth = 64;    // 根からの深さの上限 (実行器は再帰で降りるのでスタックを守る)
constexpr int kBtMaxStepsPerTick = 256; // 1 体 1 tick のノード訪問の手数の上限 (超えたらその tick はそこで止めて次の tick に続ける)
constexpr int kBtMaxTicksParam = 216000; // tick 数のパラメータの上限 (60 Hz で 1 時間)
constexpr int kBtMaxRepeatCount = 100000; // Repeat の回数の上限
constexpr int kBtMaxDecoratorsPerNode = 8; // 1 ノードに付けられる Decorator の数の上限
constexpr int kBtMaxStateSlots = kBtMaxNodes * (1 + kBtMaxDecoratorsPerNode); // 実行状態の欄の数の上限 (ノード 1 + Decorator 1 つにつき 1 欄)
constexpr int kBtMaxSubTreeDepth = 8;  // SubTree の入れ子の段数の上限 (9 段目の SubTree は Failure)

// ノードの種類。値は btNodeTypes の添字で、ファイルには名前で保存する (並べ替えても保存形式は変わらない)
enum class BtNodeKind : uint8_t {
    Selector,
    Sequence,
    SimpleParallel,
    Wait,
    MoveTo,
    RotateTo,
    SetBlackboard,
    ClearBlackboard,
    FindRandomPoint,
    FindNearestTarget,
    SearchArea,
    FindTarget,
    SendEvent,
    PlayAnimation,
    SubTree,
    Patrol,
    CppTask, // GameLogic.dll の REGISTER_BT_TASK で登録した C++ のタスク (BtTaskRegistry が名前で引く)
    Count,
};

// エディタのパレットの分類 (UE の Composites / Decorators / Tasks に相当)
enum class BtNodeCategory : uint8_t {
    Composite,
    Task,
    Ai, // 知覚・ナビメッシュを引いてブラックボードへ書くノード (FindRandomPoint など)
    Gameplay, // 外の仕組み (Animator・イベントキュー) へ働きかけるノード
    Tree,     // 別の木を取り込むノード (SubTree)
};

enum class BtParamType : uint8_t {
    Int,
    Float,
    Bool,
    Enum, // 値は enumNames の添字、ファイルには名前で保存する
    Guid, // アセット参照 (.navfilter.json など)。値は BtParamValue::u、ファイルには 16 桁の 16 進で保存する。0 = 参照なし
    Mask, // 64 ビットのビット集合 (タグの AND マスクなど)。値・保存形式は Guid と同じ (u / 16 桁の 16 進)。0 = 条件なし
    String, // 文字列 (イベント名など)。値は BtParamValue::s、kBbMaxNameBytes バイトまで。空 = 未指定
};

// ノードのパラメータ 1 つの記述。エディタ (パラメータ欄) と読み書き (範囲の丸め) が同じ表を引く
struct BtParamDesc {
    const char* name;
    BtParamType type;
    float defaultValue;                  // Enum は添字
    float minValue;                      // Int / Float の範囲
    float maxValue;
    const char* const* enumNames;        // Enum のときだけ
    int enumCount;
};

constexpr int kBtUnlimitedChildren = -1;

constexpr float kBtValueLimit = 1.0e9f;      // Int / Float パラメータの値の範囲 (int32 へ収まる)
constexpr int kBtMaxExtraBytesPerNode = 128; // 1 ノードの種類別の追加状態の上限 (CppTask が最大)
constexpr int kBtMaxExtraBytes = kBtMaxNodes * kBtMaxExtraBytesPerNode;

// MoveTo の追加状態 (BT 節に生バイトで入る。パディングを持たない 24 バイト)
namespace btmovetoflag {
enum : uint32_t {
    kNavFilterSwapped = 1u << 0, // Agent の navFilter を差し替えた (savedNavFilter が元の値)
};
} // namespace btmovetoflag
struct BtMoveToState {
    uint64_t savedNavFilter = 0;         // 差し替える前の navFilter
    float lastTarget[3] = {};            // 最後に目的地へ書いた目標の位置 (observeTarget の比較元)
    uint32_t flags = 0;
};
static_assert(sizeof(BtMoveToState) == 24, "BtMoveToState はパディングなしの 24 バイト (BT 節の生バイトに入る)");

// RotateTo の追加状態 (BT 節に生バイトで入る)
namespace btrotatetoflag {
enum : uint32_t {
    kSavedUpdateRotation = 1u << 0, // 入る前の Agent の updateRotation (終わったらこの値へ戻す)
    kAgentHandled = 1u << 1,        // 入るとき Agent が居て updateRotation を預かった (居なければ戻すものも無い)
};
} // namespace btrotatetoflag
struct BtRotateToState {
    uint32_t flags = 0;
};
static_assert(sizeof(BtRotateToState) == 4, "BtRotateToState はパディングなしの 4 バイト (BT 節の生バイトに入る)");

constexpr int kBtMaxSearchPoints = 32; // SearchArea が回る点の数の上限 (点は列で持たず 1 つずつ生成するので状態は固定長)

// SearchArea の追加状態 (BT 節に生バイトで入る。パディングを持たない 32 バイト)。点の列は持たない
namespace btsearchphase {
enum : uint32_t {
    kToOrigin = 0, // 起点へ向かっている
    kToPoint = 1,  // 起点の周りの点へ向かっている
};
} // namespace btsearchphase
struct BtSearchAreaState {
    float origin[3] = {};     // 吸着済みの起点
    float current[3] = {};    // 今向かっている点 (起点のときは origin と同じ)
    int32_t remaining = 0;    // まだ生成していない点の数
    uint32_t phase = btsearchphase::kToOrigin;
};
static_assert(sizeof(BtSearchAreaState) == 32, "BtSearchAreaState はパディングなしの 32 バイト (BT 節の生バイトに入る)");

// Patrol の追加状態 (BT 節に生バイトで入る。パディングを持たない 16 バイト)
namespace btpatrolphase {
enum : uint32_t {
    kMoving = 0,  // nextIndex の点へ向かっている
    kWaiting = 1, // nextIndex の点に着いて待っている
};
} // namespace btpatrolphase
struct BtPatrolState {
    int32_t nextIndex = 0;       // 今向かっている (待っている) 点
    int32_t direction = 1;       // PingPong の進む向き (+1 / -1)
    int32_t waitRemaining = 0;   // 待ちの残り tick (kWaiting のとき)
    uint32_t phase = btpatrolphase::kMoving;
};
static_assert(sizeof(BtPatrolState) == 16, "BtPatrolState はパディングなしの 16 バイト (BT 節の生バイトに入る)");

struct BtNodeTypeInfo {
    BtNodeKind kind;
    const char* name;                    // ファイルの "type"
    BtNodeCategory category;
    int minChildren;
    int maxChildren;                     // kBtUnlimitedChildren = 上限なし
    const BtParamDesc* params;
    int paramCount;
    const char* const* keyNames;         // ブラックボードのキー名を持つ欄の名前 ("keys" の項目名)。無ければ nullptr
    int keyCount;
    int extraStateBytes;                 // この種類が実行中に持つ追加状態のバイト数 (0 = BtNodeState だけ)。ノードごとに固定長の領域が付く
};

const BtNodeTypeInfo& BtNodeTypeOf(BtNodeKind kind);
const BtNodeTypeInfo* BtFindNodeType(const std::string& name); // 無ければ nullptr

// パラメータの値 1 つ。Int / Bool / Enum は i、Float は f
struct BtParamValue {
    int32_t i = 0;
    float f = 0.0f;
    uint64_t u = 0; // Guid / Mask
    std::string s;  // String
};

// MoveTo の params の並び
namespace btmoveparam {
enum : int {
    kAcceptanceRadius = 0, // この水平距離 (m) 以内で Success
    kObserveTarget = 1,    // 目標が動いたら目的地を書き直す
    kFailOnStuck = 2,      // Stuck になった tick に Failure (false なら Running のまま)
    kNavFilter = 3,        // 実行中だけ Agent の navFilter を差し替える (0 = Agent のまま)
};
} // namespace btmoveparam

// RotateTo の params の並び
namespace btrotateparam {
enum : int {
    kAngularSpeedDeg = 0, // 0 = Agent の値 (Agent も無ければ 360)
    kToleranceDeg = 1,
};
} // namespace btrotateparam

// SetBlackboard の params の並びと source
namespace btsetparam {
enum : int {
    kSource = 0,
    kBoolValue = 1,
    kIntValue = 2,
    kFloatValue = 3,
    kVectorX = 4,
    kVectorY = 5,
    kVectorZ = 6,
};
enum : int32_t {
    kConstant = 0, // 定数 (Bool / Int / Float / Vector)
    kSelf = 1,     // 自分 (Entity) / 自分のワールド位置 (Vector)
    kCopy = 2,     // sourceKey の値
};
} // namespace btsetparam

// FindRandomPoint の params / keys の並び
namespace btrandomparam {
enum : int {
    kRadius = 0,
};
} // namespace btrandomparam
namespace btrandomkey {
enum : int {
    kCenter = 0, // Vector。空 = 自分の位置
    kResult = 1, // Vector。書く先
};
} // namespace btrandomkey

// FindNearestTarget の params / keys の並び
namespace btnearestparam {
enum : int {
    kSight = 0,
    kHearing = 1,
    kDamage = 2,
    kTouch = 3,
    kCurrentOnly = 4, // 真なら今この tick に知覚している相手だけ
};
} // namespace btnearestparam
namespace btnearestkey {
enum : int {
    kTarget = 0,   // Entity。書く先
    kPosition = 1, // Vector。最後に知覚した位置を書く先 (任意)
};
} // namespace btnearestkey

// SearchArea の params / keys の並び
namespace btsearchparam {
enum : int {
    kUsePrediction = 0, // 真なら終了キーの相手の知覚の predictedPos を起点にする (無ければ origin キー)
    kRadius = 1,
    kPointCount = 2,
    kFailOnStuck = 3,   // Stuck になった tick にノード全体を Failure (false なら Running のまま。MoveTo と同じ意味)
};
} // namespace btsearchparam
namespace btsearchkey {
enum : int {
    kOrigin = 0, // Vector。起点
    kEndTarget = 1, // Entity。この相手が視覚で見えたら Success (空 = 見ない)
};
} // namespace btsearchkey

// FindTarget の params / keys の並び
namespace btfindtargetparam {
enum : int {
    kRadius = 0,
    kEnemies = 1,
    kNeutrals = 2,
    kFriendlies = 3,
    kTagMask = 4, // 0 = タグの条件なし。非 0 なら TagComponent.mask がどれか 1 ビットでも持つ相手だけ
};
} // namespace btfindtargetparam
namespace btfindtargetkey {
enum : int {
    kTarget = 0, // Entity。書く先
};
} // namespace btfindtargetkey

// SendEvent の params / keys の並び
namespace btsendparam {
enum : int {
    kEventName = 0, // String。空なら Failure
    kTarget = 1,    // btsendtarget
    kFloatValue = 2,
    kIntValue = 3,
};
} // namespace btsendparam
namespace btsendtarget {
enum : int32_t {
    kSelf = 0,   // 自分宛て
    kAll = 1,    // 全体宛て (target = null)
    kEntity = 2, // key "target" の Entity 宛て
};
} // namespace btsendtarget
namespace btsendkey {
enum : int {
    kTarget = 0, // Entity。宛先が Entity のとき
    kVector = 1, // Vector。ペイロード (空 = 0 ベクトル)
};
} // namespace btsendkey

// PlayAnimation の params の並び
namespace btplayparam {
enum : int {
    kState = 0,         // String。Animator Controller のステート名。空・見つからなければ Failure
    kDurationTicks = 1, // 遷移の長さ (tick)。0 = 即切り替え
    kWaitForEnd = 2,    // 真ならそのステートのクリップが 1 周するまで Running
};
} // namespace btplayparam

// SubTree の params の並び
namespace btsubtreeparam {
enum : int {
    kTree = 0, // Guid。取り込む .bt.json。0 = 参照なし (Failure)
};
} // namespace btsubtreeparam

// Patrol の params / keys の並び
namespace btpatrolparam {
enum : int {
    kAcceptanceRadius = 0, // この水平距離 (m) 以内で点に着いたとみなす
    kFailOnStuck = 1,      // Stuck になった tick にノード全体を Failure (false なら Running のまま。MoveTo と同じ意味)
};
} // namespace btpatrolparam
namespace btpatrolkey {
enum : int {
    kRoute = 0, // Entity。PatrolRouteComponent を持つエンティティ
};
} // namespace btpatrolkey

// CppTask の params の並び。タスク自身のフィールドは params ではなくノードの taskFields (JSON) に名前で持つ
namespace btcpptaskparam {
enum : int {
    kTask = 0, // String。REGISTER_BT_TASK のタスク名。空・登録に無い名前は Failure
};
} // namespace btcpptaskparam
constexpr size_t kBtMaxTaskFieldEntries = 32;   // taskFields のエントリ数の上限 (REGISTER_BT_TASK のフィールド数と同じ)
constexpr size_t kBtMaxTaskFieldTextBytes = 255; // taskFields の文字列値の長さの上限 (String256 に入る)

// ブラックボードのキーを持つノードの "keys" の並び
namespace btnodekey {
enum : int {
    kTarget = 0,    // MoveTo / RotateTo の目標、SetBlackboard / ClearBlackboard の書く先 (name は種類ごと)
    kSourceKey = 1, // SetBlackboard の source = Copy のときのコピー元
};
} // namespace btnodekey

// SimpleParallel の finishMode
namespace btparallelfinish {
enum : int32_t {
    kImmediate = 0, // メインが終わったら背景を Abort する
    kDelayed = 1,   // メインが終わっても背景が終わるまで待つ
};
} // namespace btparallelfinish

// Decorator の種類 (UE の Decorators に相当)。値は btDecoratorTypes の添字で、ファイルには名前で保存する
enum class BtDecoratorKind : uint8_t {
    BlackboardCondition,
    Invert,
    Cooldown,
    Repeat,
    Timeout,
    Count,
};

struct BtDecoratorTypeInfo {
    BtDecoratorKind kind;
    const char* name;                    // ファイルの "type"
    const BtParamDesc* params;
    int paramCount;
    bool hasKey;                         // ブラックボードのキー名を持つ (BlackboardCondition だけ)
};

const BtDecoratorTypeInfo& BtDecoratorTypeOf(BtDecoratorKind kind);
const BtDecoratorTypeInfo* BtFindDecoratorType(const std::string& name); // 無ければ nullptr

// BlackboardCondition の params の並び
namespace btbbparam {
enum : int {
    kQuery = 0,
    kIntValue = 1,   // Bool / Int のキーと比べる値
    kFloatValue = 2, // Float のキーと比べる値
    kAbort = 3,
};
} // namespace btbbparam

// BlackboardCondition の query
namespace btquery {
enum : int32_t {
    kIsSet,
    kIsNotSet,
    kEqual,
    kNotEqual,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
};
} // namespace btquery

// BlackboardCondition の abort (UE の Observer Aborts。OnResultChange のみ)
namespace btabort {
enum : int32_t {
    kNone,
    kSelf,
    kLowerPriority,
    kBoth,
};
} // namespace btabort

struct BtDecoratorDef {
    BtDecoratorKind kind = BtDecoratorKind::Invert;
    std::string key;                     // hasKey の種類だけ。ブラックボードのキー名
    std::vector<BtParamValue> params;    // 種類の params と同じ長さ・同じ並び

    // ---- BtLinkAsset が作る導出値 ----
    int32_t slot = -1;                   // 実行状態 (BtInstance::nodes) の欄の添字
};

struct BtNodeDef {
    int32_t id = 0;                      // ファイル内で一意 (>= 0)。ライブ表示と ABI の「実行中ノード」はこの値
    BtNodeKind kind = BtNodeKind::Selector;
    std::vector<BtParamValue> params;    // 種類の params と同じ長さ・同じ並び
    std::vector<std::string> keys;       // 種類の keyNames と同じ長さ・同じ並び。空文字 = 未指定 (実行時は Failure)
    std::vector<BtDecoratorDef> decorators; // 上から順に評価する (最初が一番外側)
    nlohmann::json taskFields;           // CppTask だけ: タスクのフィールドの値 ({ "<名前>": 値 })。入るたびに既定値へ重ねる。空 = 全部既定
    std::vector<int32_t> childIds;       // 左から右 = 優先順
    float pos[2] = {};                   // エディタの表示位置 (実行には使わない)

    // ---- BtLinkAsset が作る導出値 ----
    std::vector<int32_t> children;       // nodes の添字
    int32_t parent = -1;                 // nodes の添字。根と、どこにもつながっていないノードは -1
    int32_t extraOffset = 0;             // BtInstance::extra の中のこのノードの追加状態の先頭 (extraStateBytes が 0 の種類は使わない)
    // SubTree の展開 (BtExpandSubTrees) が写したノードだけ: 写し元の木の GUID と、その木の中での元の id。0 = この木自身のノード
    // (id が元の id)。ライブ表示が実行木の id を元の木へ戻すための表で、BT 節・ハッシュ・ファイルには入れない
    uint64_t originTree = 0;
    int32_t originId = -1;
};

// ビヘイビアツリー 1 本。値はワールドハッシュに入れない (ファイルの中身は provenance の contentHash が守る。
// 実行状態は BehaviorTreeSystem が持つ)。読み書きは起動走査 / ReloadHub / エディタのメインスレッドのみ
struct BehaviorTreeAsset {
    uint64_t hash = 0;                   // = GUID (BehaviorTreeLibrary のキー)
    std::string name;
    std::wstring path;
    uint64_t blackboard = 0;             // 使う .bb.json の GUID。0 = ブラックボードなし
    int32_t rootId = -1;                 // -1 = 根なし (何も実行しない)
    std::vector<BtNodeDef> nodes;        // ファイルの順 (エディタの往復で並びを変えない)

    // ---- BtLinkAsset が作る導出値 ----
    int32_t rootIndex = -1;              // nodes の添字
    int32_t stateSlotCount = 0;          // 実行状態 (BtNodeState) の欄の数。先頭がノードごと (nodes と同じ添字)、続いて Decorator ごと
    int32_t extraStateBytes = 0;         // 種類別の追加状態 (BtInstance::extra) の合計バイト数

    // id からノードの添字。無ければ -1
    int FindNode(int32_t id) const;

    // 展開後の木 (BtExpansion::tree) のノードが、元のどの木のどの id か。展開していない木・源の木自身のノードは (hash, id)
    uint64_t OriginTreeOf(const BtNodeDef& node) const { return node.originTree != 0 ? node.originTree : hash; }
    int32_t OriginIdOf(const BtNodeDef& node) const { return node.originTree != 0 ? node.originId : node.id; }
    // nodes[index] を displayedTree の木のノード id へ戻す。displayedTree の木に属さないノードは、その木の中の一番近い祖先
    // (= 部分木を取り込んでいる SubTree ノード) へ寄せる。どの祖先も属さなければ -1。ライブ表示が実行木の id を窓の木へ戻すのに使う
    int32_t DisplayedIdOf(int32_t index, uint64_t displayedTree) const;
};

// childIds から children / parent / rootIndex を作り、木として成り立つか検査する。
// 成り立たない (id の重複・存在しない子・子の数の違反・親が 2 つ・循環・深すぎ・根に親がある) なら false。
// BehaviorTreeLibrary::FromJson とエディタの保存前の検査が使う
bool BtLinkAsset(BehaviorTreeAsset& asset);

class BehaviorTreeLibrary;

// 取り込み元の木 1 つの参照。登録が置き換わった・新しく登録された・消えたことを shared_ptr の同一性で検出する (null = 未登録だった)
struct BtSubTreeDep {
    uint64_t guid = 0;
    std::shared_ptr<const BehaviorTreeAsset> asset;
};

// 実行用に SubTree を取り込み終えた木。実行器は SubTree の部分木を呼び出し側の木のノード表へ平らに写して 1 本の木として動かす
// (欄は 1 つの BtInstance に収まり、部分木の Decorator も呼び出し側の Abort の監視にそのまま加わる)
struct BtExpansion {
    std::shared_ptr<const BehaviorTreeAsset> source; // 展開前の木 (登録されているもの)
    std::shared_ptr<const BehaviorTreeAsset> tree;   // 実行する木。SubTree が 1 つも無ければ source そのもの
    std::vector<BtSubTreeDep> deps;                  // 引いた部分木 (引いた順)

    // source と deps が今のライブラリの登録と全部同じなら true (false なら作り直す)
    bool IsCurrent(const BehaviorTreeLibrary& library, const std::shared_ptr<const BehaviorTreeAsset>& registered) const;
};

// source の SubTree ノードへ部分木を取り込む。取り込めない SubTree (未指定・未登録・BB 違い・入れ子が kBtMaxSubTreeDepth を超える・
// 根なし) は子なしのまま残し (実行時 Failure)、理由を 1 回警告する。取り込み後がノード数・深さの上限を超えるときは
// 全部を取り込まずに source そのまま (全 SubTree が Failure) にする。
// 取り込んだノードの id は source の最大 id より後ろへ連番で振り直す (部分木の元の id は実行木には残らない)。
// 結果は assets の中身だけで決まる (決定論)
BtExpansion BtExpandSubTrees(const BehaviorTreeLibrary& library, std::shared_ptr<const BehaviorTreeAsset> source);

struct BehaviorTreeEntry {
    uint64_t hash = 0;
    std::string name;
};

// 登録済み .bt.json の管理 (NavFilterLibrary と同じ形)。EngineLoop / HeadlessSim が所有し behaviortree:: で注入
class BehaviorTreeLibrary {
public:
    static uint64_t HashForPath(const std::wstring& path);

    // 失敗時 0。outUnchanged を渡すと、登録済みと内容が同じなら置き換えず *outUnchanged = true
    // (走っている木の無用なやり直しを避ける。ReloadHub が使う)
    uint64_t LoadFromFile(const std::wstring& path, bool* outUnchanged = nullptr);
    uint64_t Register(const std::wstring& path, BehaviorTreeAsset asset); // 返り値 = hash。同じ GUID なら置き換える

    // 置き換わった後も、実行中のエンティティが古い木を持ち続けて後始末できるよう shared_ptr で渡す。
    // 実行器は「持っている木 != 今の登録」で読み直しを検出する
    std::shared_ptr<const BehaviorTreeAsset> GetShared(uint64_t hash) const;
    const BehaviorTreeAsset* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return assets_.find(hash) != assets_.end(); }
    std::vector<BehaviorTreeEntry> Enumerate() const; // 名前昇順 (ハッシュの反復順を表に出さない)

    static nlohmann::json ToJson(const BehaviorTreeAsset& asset);
    // 読み値は Sanitize 済みで返す。木でない JSON ("behaviortree" キー無し)・未知の type・壊れた参照・循環は false
    static bool FromJson(const nlohmann::json& j, BehaviorTreeAsset& out);

private:
    std::unordered_map<uint64_t, std::shared_ptr<const BehaviorTreeAsset>> assets_;
};

// モジュール注入 (navfilter:: と同じ流儀)。所有者が起動時に Install し、終了時にライブラリ破棄前に外す。メインスレッド専用
namespace behaviortree {
void Install(BehaviorTreeLibrary* lib);
BehaviorTreeLibrary* Library();              // 未接続 = nullptr
} // namespace behaviortree

} // namespace mye
