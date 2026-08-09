#pragma once
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>

#include "Engine/Core/World.h"
#include "Engine/Engine/GameObject.h"

namespace mye {

// アクティブなワールドの所有者。M2 で JSON シリアライズ (保存/読込/Play スナップショット) が載る
class Scene {
public:
    // シーン文書の現行 version (SceneSerializer::SaveToJson が書く値)。
    //   v1: EntityRef を [index, generation] で保存 (M8 以前)
    //   v2: EntityRef を fileId で保存 + childIndex (M8)
    //   v3: 「キー不在 = ベース追随」をコンポーネント構造へ拡張 (M50c) —
    //       ベースにあり実体に無く "-Component" キーの無い comp は、ロード時に
    //       ベース値で追加してよい (v2 以前の文書では記録へのマージのみで実体は不変)
    static constexpr int kDocVersion = 3;
    GameObject CreateGameObject(std::string_view name)
    {
        return GameObject(&world_, world_.CreateEntity(name));
    }

    // エディタ経由の生成: fileId を即採番して付与する。
    // Undo/Redo の同一性キーになり、Play/Stop や再ロードで EntityID が変わっても追跡できる。
    // (シリアライザのロード経路は fileId をファイル値で設定するため CreateGameObject を使う)
    GameObject CreateGameObjectTracked(std::string_view name)
    {
        GameObject o = CreateGameObject(name);
        o.AddComponent<FileIdComponent>()->value = NextFileId();
        return o;
    }

    // 名前で線形検索 (最初に一致したもの)。見つからなければ無効な GameObject。
    // 重複名は「先勝ち」の意味論があるため索引化しない (M51a で据え置きを決定)
    GameObject Find(std::string_view name);

    // fileId で検索 (シーンリロードの差分適用 / Prefab::Instantiate / エディタ選択解決)。
    // M51a: ヒット時検証つきキャッシュ — ヒットしたら生存 + 値一致を確認し、stale なら
    // 破棄して線形走査にフォールバックして補修する。fileId はシーン内一意 (NextFileId
    // 単調採番) が前提。World::SimCacheEnabled()==false で従来の線形走査のみ
    GameObject FindByFileId(uint64_t fileId);

    // e に fileId が無ければ採番して返す (Undo/選択が同一性キーとして使う)。0 = 無効
    uint64_t EnsureFileId(EntityID e);

    // 全エンティティ破棄 (名前と nextFileId は保持)
    void Clear()
    {
        world_.Clear();
        overrides_.clear();
        fileIdCache_.clear();
    }

    World& GetWorld() { return world_; }
    const std::string& Name() const { return name_; }
    void SetName(std::string_view name) { name_ = name; }

    uint64_t NextFileId() { return nextFileId_++; }
    void SetNextFileId(uint64_t v) { nextFileId_ = v; }
    uint64_t PeekNextFileId() const { return nextFileId_; }

    // このシーンをロードした文書の version (LoadFromJson が設定する)。
    // RefreshNonOverridden が v3 の構造追随を掛けてよいかの判定に使う。
    // 既定は kDocVersion — メモリ上で組んだシーン (デモ構築等) は現行形式そのもの
    int LoadedVersion() const { return loadedVersion_; }
    void SetLoadedVersion(int v) { loadedVersion_ = v; }

    // ---- プレハブ override リスト (M48e) ----
    //
    // プレハブインスタンスのメンバごとに「ユーザーが上書きした葉フィールド」の集合を持つ。
    // キーは `"Component.field"`、エンティティ名だけは `"name"`。
    // **ECS の外に置き WorldHash には入れない** — シミュレーション状態ではなく編集メタデータで、
    // ロード後の値には差が出ないため (差が出るなら refresh のバグ)。
    //
    // 記録の有無そのものが意味を持つ:
    //   - 記録あり (空集合を含む) = 新形式。ロード時にベース最新値で非 override を更新してよい
    //   - 記録なし               = レガシー (M48d 以前に保存されたシーン)。ライブ diff に
    //                              フォールバックし、値には一切触らない (ビット不変ロード)
    // シリアライズはエンティティ JSON の `"overrides"` キー (SceneSerializer::WriteEntity)。
    using OverrideSet = std::set<std::string>; // ソート済み = 決定論的なファイル出力

    void SetOverrides(uint64_t fileId, OverrideSet keys)
    {
        if (fileId != 0) {
            overrides_[fileId] = std::move(keys); // 空集合でも「記録あり」として残す
        }
    }
    void ClearOverrides(uint64_t fileId) { overrides_.erase(fileId); }
    void MarkOverride(uint64_t fileId, std::string key)
    {
        if (fileId != 0) {
            overrides_[fileId].insert(std::move(key));
        }
    }
    void UnmarkOverride(uint64_t fileId, const std::string& key)
    {
        if (auto it = overrides_.find(fileId); it != overrides_.end()) {
            it->second.erase(key);
        }
    }
    const OverrideSet* GetOverrides(uint64_t fileId) const
    {
        auto it = overrides_.find(fileId);
        return (it != overrides_.end()) ? &it->second : nullptr;
    }
    bool HasOverrideRecord(uint64_t fileId) const { return overrides_.count(fileId) != 0; }

private:
    World world_;
    std::string name_ = "Untitled";
    uint64_t nextFileId_ = 1;
    int loadedVersion_ = kDocVersion; // LoadFromJson が文書の値で上書きする
    std::unordered_map<uint64_t, OverrideSet> overrides_; // fileId → 上書き済みキー集合
    // fileId → EntityID の検証つきキャッシュ (M51a)。ヒット時に生存 + 値一致を必ず確認
    // するため stale エントリは無害 (書込点の網羅は不要)。0 (未採番) は入れない
    std::unordered_map<uint64_t, EntityID> fileIdCache_;
};

} // namespace mye
