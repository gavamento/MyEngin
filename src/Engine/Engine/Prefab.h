#pragma once
#include <set>
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

// プレハブ操作 (Engine 層の自由関数)。
//
// ---- オーバーライドの保存モデル (M48e) ----
// 上書きは Scene の override リスト (`Scene::GetOverrides`) に **保存型** で持ち、
// エンティティ JSON の `"overrides"` キーで往復する。ライブ diff (現在値 != ベース値) は
// 記録の無いレガシーシーンのフォールバックとしてのみ残る。
//
// 記録の更新はライブ diff の**スナップショット**として行う。ライブ diff が真になるのは
// 「ベースが現行と一致していると分かっているタイミング」だけなので、そこで撮って保存する:
//   - 編集直後 (UndoStack::CaptureAfter → RecordOverridesSubtree)
//   - ベース更新の直後 (PropagateBaseChange / ApplyInstance)
//   - ロード直後のレガシー移行 (RefreshNonOverridden)
// 逆に「シーンを閉じている間にベースが変わった」場合はライブ diff が誤判定するため、
// ロード時は保存済みリストだけを信じて非 override をベース最新値へ更新する
// (これが M13 のライブ diff 方式にあった欠陥の修理そのもの)。
//
// v1 の制限: 値が偶然ベースと一致する上書きは記録されない (ライブ diff 由来の性質)。
//
// ---- 構造上書き = コンポーネントの追加/削除 (M50c) ----
// override キーに `"+Component"` / `"-Component"` を追加し、コンポーネント単位の構造変更を
// 上書きとして追跡する (エンティティ = 子の増減の追跡は v2 のまま非対応)。
//   - キーはライブ diff からの**純導出** (OverridesAgainstBase) — RecordOverrides の
//     全置換スナップショット方式とそのまま両立する
//   - 追跡対象 = ReadEntityComponents の removeMissing 除外集合のミラー
//     (NoSerialize / Hidden / Name / LocalTransform 以外。C# スクリプト状態は含む)
//   - PropagateBaseChange: "-C" 付き欠落は復活させない / "+C" 付き既存はベース値で
//     クロバーしない。レコード無し (レガシー) は従来どおり復活
//   - シーン文書 version 2→3: v3 は「キー不在 = ベース追随」が構造にも及ぶ契約 —
//     ベースにあり実体に無く "-C" の無い comp はロード時にベース値で追加される。
//     v2 以前の文書はロード時に構造キーをレコードへマージするだけで実体は不変
//     (v2 では削除と閉間ベース成長が区別できず、どちらも "-C" になる — 一度きりの制限)
//   - 旧エンジンは "+/-" キーを素通しで保存・復元するだけ (不活性)。ReplayFile /
//     WorldHash / ABI の bump は無し (レコードはハッシュ対象外)
// 制限: ベースに追加された C# comp は伝播/復元で既定値のまま生える (フィールドは
// managed 側が持つため充填できない — 既存ギャップ、スコープ外)
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

// rootFileId のインスタンスをプレハブから切り離す (Unity の Unpack 相当。M50b)。
// 直属メンバの PrefabLink とルートの PrefabInstance を除去し (= 部位の構造ロックも外れる)、
// override 記録も消す。内側インスタンスは無傷 — タグを温存し outerLocalId=0 (シーン直接
// 配置扱い) へ。**祖先にインスタンスルートがある場合は false** (外側の Apply でこの枝が
// 新ベースから落ちるため。外側から順に Unpack すれば解ける)。
// 呼び出し後は World::ApplyStructuralChanges を回すこと
bool UnpackInstance(Scene& scene, uint64_t rootFileId);

// e の 1 フィールドがプレハブベースと異なるか (= オーバーライド)。
// EntityRef は remap 判定が不確実なため常に false。プレハブ非所属なら false
bool IsFieldOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                       const FieldDesc& field);
bool IsNameOverridden(Scene& scene, const PrefabLibrary& lib, EntityID e);

// e の 1 フィールドをプレハブベース値へ戻す
void RevertField(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName,
                 const FieldDesc& field);

// ---- 構造上書き (M50c) ----

// コンポーネント単位の構造上書き状態
enum class CompOverride {
    None,    // ベースと構造一致 (または非対象)
    Added,   // インスタンスで追加された comp ("+Component")
    Removed, // インスタンスで削除された comp ("-Component")
};

// e の 1 コンポーネントの構造上書き状態。保存済みレコードが一次情報、
// 無ければライブ diff (レガシー) に落ちる。プレハブ非所属なら None
CompOverride ComponentOverrideState(Scene& scene, const PrefabLibrary& lib, EntityID e,
                                    const char* compName);

// e でインスタンス削除されたベース comp 名の一覧 (名前順)。Inspector の
// 「Removed prefab components [Restore]」節が使う。実体・ベースと突き合わせ済みなので
// Restore (RevertComponent) が no-op になるエントリは含まれない
std::vector<std::string> RemovedPrefabComponents(Scene& scene, const PrefabLibrary& lib, EntityID e);

// e の 1 コンポーネントの構造上書きを戻す (双方向):
//   - ベースにあり実体に無い ("-C") → ベース値で再生成 (EntityRef は null のまま)
//   - ベースに無く実体にある ("+C") → 除去
// 構造が一致しているときは false (値の Revert は RevertField)。レコードのキーも消す
bool RevertComponent(Scene& scene, const PrefabLibrary& lib, EntityID e, const char* compName);
// rootFileId のインスタンス全体をベース値へ戻す (全フィールド + 名前。EntityRef は除く)。
// 構造も戻す (M50c): 削除された comp をベース値で再生成し、追加された comp を除去する。
// 入れ子インスタンスは**境界を越えない** — 内側は内側の Revert で戻す (M48c)
void RevertInstance(Scene& scene, const PrefabLibrary& lib, uint64_t rootFileId);

// rootFileId のインスタンスの現在値を新ベースとして .prefab.json に書き戻し、
// 同一プレハブの他インスタンスの「非オーバーライド」フィールドへ伝播する
bool ApplyInstance(Scene& scene, PrefabLibrary& lib, uint64_t rootFileId);

// prefabHash のベース変更 (oldBase→newBase) を全インスタンスの非オーバーライドへ伝播 (リロード用)。
// 非オーバーライド = 現在値が oldBase と一致するフィールド。伝播後は override リストを取り直す。
// 入れ子インスタンスは境界を越えない (内側は内側のハッシュで別途伝播される。M48c)
void PropagateBaseChange(Scene& scene, const nlohmann::json& oldBase, const nlohmann::json& newBase,
                         uint64_t prefabHash);

// ---- override リストの記録 (M48e) ----

// e の現在値をベースと突き合わせた上書きキー集合 (ライブ diff)。プレハブ非メンバなら空。
// キーは "Component.field" / 名前は "name"。**ベースに存在するフィールドだけ**を見る
// (ベースに無いものはロード時の更新対象でもないので記録する意味がない)
std::set<std::string> ComputeOverrides(Scene& scene, const PrefabLibrary& lib, EntityID e);

// e の override リストを現在値から記録し直す。プレハブ非メンバになったら記録を消す。
// ベースが未登録 (アセット欠落) のときは既存の記録を保持する — 消すと復帰時に上書きを失う
void RecordOverrides(Scene& scene, const PrefabLibrary& lib, EntityID e);

// root サブツリー全体に RecordOverrides を適用する (エディタ編集直後のフック)。
// **UndoStack::CaptureAfter がこれを呼ぶ** = 全エディタ編集経路を 1 箇所で被覆する
void RecordOverridesSubtree(Scene& scene, const PrefabLibrary& lib, EntityID root);

// **ロード直後に 1 回だけ**呼ぶ: 全インスタンスの「上書きされていない」フィールドと名前を
// ベースの最新値へ更新する。シーンを閉じている間にプレハブが更新されていても追随する。
//   - 記録のあるメンバ : リストに無いフィールドをベース値で更新。文書が v3 なら構造も
//     追随する ("-C" の無い欠落 comp をベース値で追加。M50c)。v2 以前は構造キーを
//     ライブ diff からレコードへマージするだけで実体は不変
//   - 記録の無いメンバ : レガシー。**値には触らず**ライブ diff から記録だけ作る (移行)
// 順序は fileId 昇順 → localId 昇順の固定順で、入力 (シーン JSON / アセット JSON) だけに
// 依存する純関数 = 決定論的。Play/Stop の LoadFromJson では**呼ばない** (往復で値が動かない)
void RefreshNonOverridden(Scene& scene, const PrefabLibrary& lib);

// ---- ミニシーン編集モード (M48k) ----
//
// アセットそのものを専用の Scene に展開して編集し、書き戻す経路。M13 で見送った
// 「分離ステージのプレハブ編集」の回収で、Unity の Prefab Mode に相当する。
//
// ★localId (= ミニシーンの fileId) は開いても保存しても**振り直さない**。
//   配置済みインスタンスの `PrefabLink.localId` がこの番号でベースを指しているので、
//   振り直すと全インスタンスの上書き・伝播が別メンバに掛かる (静かなデータ破壊)

// アセットの entities を LoadFromJson へ渡せるシーン文書に仕立てる。
// 要点は **nextFileId = max(fileId)+1**: 既定の 1 のままだと編集中に足した
// エンティティが既存 localId と衝突する
nlohmann::json MakeEditDocument(const PrefabAsset& asset);

// ミニシーンの現在内容をアセットへ書き戻し、ライブラリにも登録し直す。
// 配置済みインスタンスへの伝播は ReloadHub (ファイル監視) が拾う既存経路に任せる
bool SaveEdited(Scene& miniScene, PrefabLibrary& lib, const std::wstring& path,
                const std::string& name, bool actorFormat);

} // namespace Prefab
} // namespace mye
