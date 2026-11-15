//====================================================================================
//                          SceneRestorePlan.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ライブのシーンの復元の計画 (照合して何を戻すか決める。World を書かない、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeModel.h"

namespace mye {

// 今エディタで開いているシーンの、Field / Component / Entity / Prefab Override の復元の判定。
// 値は字句どうしでは比べない: 記録の After を、そのフィールドの型で読み (FieldFromJson) 書き直した正規形と、ライブの値の正規形を比べる。
// 正本: docs\ai-change-tracking\restore-safety.md §3.3

// 戻す範囲 (狭い順。レビューの範囲と同じ並び)
enum class SceneRestoreScope : uint8_t { Field, Component, Entity, Revision };

// 戻す行の上限 (restore_record が 1 つの対象に載せられる行の数と同じ)
constexpr size_t kSceneRestoreMaxRows = 4096;

// ライブのシーンの読み取り口。計画は World を書かない。試験は偽の実装 (JSON の表) を渡す
class SceneReadView {
public:
    virtual ~SceneReadView() = default;
    // エンティティの JSON (SceneSerializer::EntityToJson と同じ形: fileId / name / parent / childIndex / tagMask / components / overrides)。居なければ false
    virtual bool Entity(uint64_t fileId, nlohmann::json& out) const = 0;
    // 子の fileId (兄弟順)。fileId の無い子は 0 で返す (計画は「After に無い子孫」として扱う)
    virtual std::vector<uint64_t> Children(uint64_t fileId) const = 0;
    // C# スクリプトのコンポーネントか (フィールドは managed が持つので、値は managed の出力どうしで比べる)
    virtual bool IsManagedComponent(const std::string& componentName) const = 0;
};

// 構造の変更の行の種類 (戻し方が違う)
enum class SceneRowKind : uint8_t {
    Field,            // コンポーネントのフィールド (プレハブの上書き済みの値の行を含む)
    EntityKey,        // エンティティ直下の name / tagMask
    Hierarchy,        // parent / childIndex
    ComponentAdded,
    ComponentRemoved,
    EntityAdded,
    EntityRemoved,
    OverrideKey,      // プレハブの上書きの印 (overrides の要素)。値の行を戻すと連動して変わる
    Unsupported,      // 文書全体・プレハブの付け替え・解析不可など
};

SceneRowKind ClassifySceneRow(const TrackSceneChange& row);

enum class SceneRestoreIssue : uint8_t {
    None,
    // 戻せない行 (その行は戻せない)
    ActorNotRestorable,  // 行為者が ai / unknown でない
    NotOpenDocument,     // 今開いているシーンの変更ではない
    NoRows,              // 範囲に行が無い
    TooManyRows,         // 行が多すぎる
    RowUnsupported,      // 文書全体・プレハブの付け替え・解析不可・コンポーネント全体の値
    OverrideKeyOnly,     // 上書きの印だけで、値の行が無い
    ValueOversize,       // 値が大きすぎて保持していない
    NoBefore,            // 戻す先の値が無い (C# のフィールドを「無い」へ戻す)
    ValueUnreadable,     // 記録の値を JSON として読めない・フィールドの型に合わない
    UnknownComponent,    // 今のエンジンに無いコンポーネント
    UnknownField,        // 今のエンジンに無いフィールド
    ProtectedComponent,  // 戻せないコンポーネント (必須の LocalTransform・隠しコンポーネント)
    // 競合・依存の欠落 (今の状態が記録と合わない。何も当てない)
    EntityMissing,       // エンティティがいない
    ComponentMissing,    // コンポーネントがない
    ValueChanged,        // 今の値が After と違う
    ComponentChanged,    // コンポーネントの内容が After と違う
    ComponentExists,     // 削除の取り消しだが、同じコンポーネントがある
    EntityChanged,       // エンティティが After と違う
    ExtraDescendant,     // After に無い子孫がいる
    EntityExists,        // 削除の取り消しだが、その fileId が使われている
    ParentMissing,       // 親がいない
    ReferenceMissing,    // 参照しているエンティティがいない (依存の欠落)
    ParentCycle,         // 戻すと親子が輪になる
};

enum class SceneRestoreVerdict : uint8_t {
    Ready,           // 照合に通った。戻せる
    AlreadyRestored, // 今の値が既に Before (何もしない)
    Conflict,        // 今の状態が記録と合わない (自動では戻さない)
    NotRestorable,   // 戻せない (理由は issue)
};

struct SceneRestoreRowResult {
    int64_t sceneChangeId = 0;
    int64_t fileId = 0;
    std::string component;
    std::string field;
    SceneRowKind kind = SceneRowKind::Unsupported;
    SceneRestoreVerdict verdict = SceneRestoreVerdict::NotRestorable;
    SceneRestoreIssue issue = SceneRestoreIssue::None;
    std::string detail; // 理由に関わる fileId など
    bool derived = false; // 値の行に連動して変わる行 (上書きの印)。集約では数えない
};

// 戻すときに World へ行う操作 1 件
struct SceneRestoreOp {
    enum class Kind : uint8_t {
        SetField,         // コンポーネントのフィールドを書く
        SetEntityKey,     // name / tagMask
        SetHierarchy,     // parent / childIndex
        AddComponent,
        RemoveComponent,
        CreateEntities,   // 消されたエンティティ (と、同じ変更で消えた子孫) を作る。fileId = 根、value = JSON の配列
        DestroyEntities,  // 追加されたエンティティのサブツリーを消す。fileId = 根、subtree = 消える fileId
    };
    Kind kind = Kind::SetField;
    uint64_t fileId = 0;
    std::string component;
    std::string field;
    nlohmann::json value;   // 戻す値 (Field / Key / Hierarchy は正規形)。AddComponent はコンポーネントの JSON
    nlohmann::json current; // 今の値 (表示用)
    std::vector<uint64_t> subtree;
    std::vector<int64_t> sceneChangeIds;
    std::string entityName; // 表示用 (比べるものではない)
    bool operator==(const SceneRestoreOp& other) const
    {
        return kind == other.kind && fileId == other.fileId && component == other.component && field == other.field && value == other.value
               && subtree == other.subtree && sceneChangeIds == other.sceneChangeIds;
    }
};

struct SceneRestorePlan {
    SceneRestoreVerdict verdict = SceneRestoreVerdict::NotRestorable;
    SceneRestoreIssue issue = SceneRestoreIssue::None; // 計画全体の理由 (最初の戻せない行 / 競合した行のもの)
    std::vector<SceneRestoreRowResult> rows;           // 入力の行と同じ並び
    std::vector<SceneRestoreOp> ops;                   // Ready のときだけ
};

struct SceneRestoreInput {
    std::vector<TrackSceneChange> rows; // 戻す行
    std::string actor;                  // その Revision の行為者
    std::string openDocumentId;         // 今開いているシーンの文書 ID (引き継ぎをたどった ID)。空 = 開いていない
};

// 計画を作る。**純関数**: view の読み取りだけで、World にも DB にも触れない
SceneRestorePlan BuildSceneRestorePlan(const SceneRestoreInput& input, const SceneReadView& view);

// anchor を含む範囲の行を選ぶ (all = 1 つの Revision の全行)
std::vector<TrackSceneChange> SelectSceneRows(const std::vector<TrackSceneChange>& all, const TrackSceneChange& anchor, SceneRestoreScope scope);

// 範囲を選べるか (Field は field のある行、Component は component のある行、Entity は fileId のある行だけ)
bool SceneRestoreScopeAvailable(const TrackSceneChange& anchor, SceneRestoreScope scope);

// ---- 正規形 (試験が直接叩く) ----

enum class CanonStatus : uint8_t { Ok, UnknownComponent, UnknownField, Unreadable };

// フィールド 1 つの値を正規形にする。通常のコンポーネントは、その型で読んで書き直した値 (EntityRef は fileId の整数)。
// managed = C# コンポーネントなら、型を引けないので JSON のまま (数値の書き方の違いだけを同一視する)
CanonStatus CanonicalFieldJson(const std::string& component, const std::string& field, const nlohmann::json& raw, bool managed, nlohmann::json& canon);
// フィールドの既定値 (キーが無いときにロードが使う値)。C# は不明なので false
bool DefaultFieldJson(const std::string& component, const std::string& field, bool managed, nlohmann::json& out);

} // namespace mye
