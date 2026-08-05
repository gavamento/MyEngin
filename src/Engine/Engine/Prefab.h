#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "nlohmann/json.hpp"

#include "Engine/Core/EntityID.h"

namespace mye {

class Scene;
class World;
struct FieldDesc;

// 構成アセット (M13 プレハブ → M48d で .actor.json に拡張)。
// シーンと同形式のフラット展開サブツリー + ローカル fileId (単一ルートなら root=1)。
// エディタとランタイム (M15) の両方から使うため Engine 層に置く (Editor 層ではない)。
//
// **.prefab.json は .actor.json の部分集合** (単一ルート・部位なし・overrides なし) として
// 読み込み互換。上書き機構・ライブラリ・タグはすべて共通で、違いは宣言キーと拡張子だけ
struct PrefabAsset {
    uint64_t hash = 0;       // パスハッシュ (PrefabLibrary キー = PrefabInstanceComponent.prefabHash)
    std::string name;        // 表示名 (拡張子なしファイル名)
    std::wstring path;       // 絶対パス
    nlohmann::json entities; // ローカルエンティティ配列 (SubtreeToJson 形式、fileId はローカル)
    // 宣言キー: true = "actor":1 / false = "prefab":1。**書き戻しで維持する** —
    // 既存 .prefab.json を勝手に新形式へ移行させない (強制移行はユーザーの diff を汚す)
    bool actorFormat = false;
};

// 列挙 1 件 (Asset Browser / インスタンス化 UI 用)
struct PrefabEntry {
    uint64_t hash = 0;
    std::string name;
    std::wstring path;
};

// 登録済みプレハブの管理 (Mesh/Material ライブラリと同じ役割の Engine 層アセット)
class PrefabLibrary {
public:
    // 新規作成の既定拡張子 (M48d)。既存 .prefab.json は読み書きとも据え置き
    static constexpr const wchar_t* kActorSuffix = L".actor.json";
    static constexpr const wchar_t* kPrefabSuffix = L".prefab.json";

    // path が構成アセット (.actor.json / .prefab.json) か。大文字小文字は無視する。
    // **suffix 判定はここ 1 箇所に集約する** — 種別が増えるたびに各所へ書き足すと必ず漏れる
    // (登録は通るがホットリロードだけ効かない、といった片肺の壊れ方をする)
    static bool IsComposePath(const std::wstring& path);

    // パスからハッシュを計算 (正規化パスのハッシュ)。ロード有無に関わらず同じ値
    static uint64_t HashForPath(const std::wstring& path);

    // ファイルを読み込み登録する (既存は置き換え)。失敗時 0。返り値 = hash
    uint64_t LoadFromFile(const std::wstring& path);
    // メモリ上の entities を登録/更新する (Create / Apply 用)。返り値 = hash。
    // actorFormat は拡張子から推定される — ファイル由来の宣言キーを保つ場合は
    // 続けて SetActorFormat を呼ぶこと (LoadFromFile / ApplyInstance がそうしている)
    uint64_t Register(const std::wstring& path, std::string name, nlohmann::json entities);

    // 登録済みアセットの宣言キーを設定する (書き戻し時に維持するため)
    void SetActorFormat(uint64_t hash, bool actorFormat);

    const PrefabAsset* Get(uint64_t hash) const;
    bool Contains(uint64_t hash) const { return assets_.find(hash) != assets_.end(); }

    // 登録済みプレハブを名前順で列挙 (エディタ UI 用)
    std::vector<PrefabEntry> Enumerate() const;

private:
    std::unordered_map<uint64_t, PrefabAsset> assets_;
};

// プレハブ操作 (Engine 層の自由関数)。オーバーライドは「現在値がベース値と異なるか」の
// ライブ判定で、別途保存はしない (シーンには実体を持つのでロードは通常経路 = 決定論安全)
//
// ---- 入れ子インスタンスの ID ドメイン (M48c) ----
// プレハブは **展開保存** される (シーンにもアセットにも実体が丸ごと入る) ので、入れ子は
// 「再帰インスタンス化」ではなく「境界を尊重したタグ付け」で成立する。規約は 3 つ:
//   1. `PrefabLink.localId` は **自分が直接所属するインスタンスのベース** のドメインの値。
//      内側インスタンスのメンバは内側ベースの番号を保つ (参照ではなくデータなので remap 不要)
//   2. 内側ルートは `PrefabInstance{prefabHash=内側, outerLocalId=外側ベースでの位置}` と
//      `PrefabLink{localId=1}` (内側ドメイン) を同時に持つ
//   3. 所属境界は `FindInstanceRoot` (最近祖先) が唯一の定義。列挙 (CollectInstanceMembers)・
//      抽出・タグ付けはすべてこの境界に一致させる — 内側のメンバは外側のメンバではない
// v1 の制限: 外側ベースが内側ルートに掛けた変更は再伝播しない (3 層マージ非対応)
namespace Prefab {

// scene のサブツリー root を「プレハブローカル形式」へ変換する:
//   fileId=1..N (DFS 順、root=1) / 集合外への親・EntityRef は除去 /
//   **自レベル** のプレハブタグは除去 (内側インスタンスのタグは保持し outerLocalId を付け替え)
nlohmann::json ExtractLocal(Scene& scene, EntityID root);

// root サブツリーを path に .prefab.json として保存し library に登録、root をインスタンス化する。
// 返り値 = prefabHash (失敗時 0)
uint64_t CreateAsset(Scene& scene, PrefabLibrary& lib, const std::wstring& path, EntityID root);

// localEntities (ExtractLocal 形式) をシーンへインスタンス化する低レベル版。
// parentFileId!=0 ならルートをその子に。返り値 = 生成したルートの新 fileId (失敗時 0)。
// forcedRootFileId!=0 ならルートに新規採番せずその ID を使う (v7 Instantiate の予約方式 —
// 呼出側が Scene::NextFileId() で確保済みであること。M37)。
// **複数ルート (ミニシーン型 .actor.json) は wrapperName のグループで包む** (M48d)。
// ラッパーは `PrefabLink.localId = 0` = ベース対応物なしで、diff/Revert/伝播から外れる
uint64_t InstantiateEntities(Scene& scene, const nlohmann::json& localEntities, uint64_t prefabHash,
                             uint64_t parentFileId = 0, uint64_t forcedRootFileId = 0,
                             const char* wrapperName = nullptr);

// prefabHash のプレハブをシーンへインスタンス化する。返り値 = 新ルート fileId (失敗時 0)
uint64_t Instantiate(Scene& scene, const PrefabLibrary& lib, uint64_t prefabHash,
                     uint64_t parentFileId = 0, uint64_t forcedRootFileId = 0);

// e (またはその祖先) が属するインスタンスのルート。無ければ kNullEntity
EntityID FindInstanceRoot(World& world, EntityID e);

// root インスタンスに **直接所属する** メンバ (PrefabLink 持ち) を DFS 順に収集する。
// root 以外で PrefabInstanceComponent を持つノード = 入れ子インスタンスのルートに達したら、
// **そのノード自身も含めずに降下を止める** (= FindInstanceRoot(e)==root と厳密に等価)。
// innerRoots が非 null なら、そこで止めたノードを DFS 順に受け取る
void CollectInstanceMembers(World& world, EntityID root, std::vector<EntityID>& out,
                            std::vector<EntityID>* innerRoots = nullptr);

// e の 1 フィールドがプレハブベースと異なるか (= オーバーライド)。
// EntityRef は remap 判定が不確実なため常に false。プレハブ非所属なら false
bool IsFieldOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                       const FieldDesc& field);
bool IsNameOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e);

// e の 1 フィールドをプレハブベース値へ戻す
void RevertField(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                 const FieldDesc& field);
// rootFileId のインスタンス全体をベース値へ戻す (全フィールド + 名前。EntityRef は除く)。
// 入れ子インスタンスは**境界を越えない** — 内側は内側の Revert で戻す (M48c)
void RevertInstance(Scene& scene, const PrefabLibrary& lib, uint64_t rootFileId);

// rootFileId のインスタンスの現在値を新ベースとして .prefab.json に書き戻し、
// 同一プレハブの他インスタンスの「非オーバーライド」フィールドへ伝播する
bool ApplyInstance(Scene& scene, PrefabLibrary& lib, uint64_t rootFileId);

// prefabHash のベース変更 (oldBase→newBase) を全インスタンスの非オーバーライドへ伝播 (リロード用)。
// 非オーバーライド = 現在値が oldBase と一致するフィールド。
// 入れ子インスタンスは境界を越えない (内側は内側のハッシュで別途伝播される。M48c)
void PropagateBaseChange(Scene& scene, const nlohmann::json& oldBase, const nlohmann::json& newBase,
                         uint64_t prefabHash);

} // namespace Prefab
} // namespace mye
