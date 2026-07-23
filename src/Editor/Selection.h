#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// エディタ内の選択状態 (Hierarchy と Inspector で共有)。
// 選択は **fileId** で保持する — Undo/Redo・Play/Stop・シーン再ロードで EntityID が
// 作り直されても選択を追跡できるようにするため (M8)。EntityID への解決は
// Scene::FindByFileId で各フレーム行う。
// M40c: アセット選択 (assetPath) を追加 — エンティティ選択と排他 (Unity と同じ)。
// エンティティを選ぶとアセット選択は解除され、アセットを選ぶとエンティティ選択が解除される
struct Selection {
    std::vector<uint64_t> ids; // 選択中の fileId 群 (選択順)
    uint64_t primary = 0;      // アクティブ = Inspector 表示対象 / 範囲選択の起点 (0 = 無し)
    std::wstring assetPath;    // 選択中アセットの絶対パス (空 = アセット非選択、M40c)

    bool Empty() const { return ids.empty(); }
    bool HasAsset() const { return !assetPath.empty(); }
    bool Contains(uint64_t fid) const
    {
        return fid != 0 && std::find(ids.begin(), ids.end(), fid) != ids.end();
    }

    void Clear()
    {
        ids.clear();
        primary = 0;
        assetPath.clear();
    }

    // アセット単一選択 (AssetBrowser タイルクリック、M40c)
    void SelectAsset(std::wstring path)
    {
        ids.clear();
        primary = 0;
        assetPath = std::move(path);
    }

    // 単一選択 (通常クリック)
    void SelectOnly(uint64_t fid)
    {
        ids.clear();
        if (fid != 0) {
            ids.push_back(fid);
        }
        primary = fid;
        assetPath.clear();
    }

    // 追加選択 (Ctrl+クリック)。既にあれば primary だけ更新
    void Add(uint64_t fid)
    {
        if (fid == 0) {
            return;
        }
        if (!Contains(fid)) {
            ids.push_back(fid);
        }
        primary = fid;
        assetPath.clear();
    }

    void Remove(uint64_t fid)
    {
        ids.erase(std::remove(ids.begin(), ids.end(), fid), ids.end());
        if (primary == fid) {
            primary = ids.empty() ? 0 : ids.back();
        }
    }

    // トグル (Ctrl+クリック)
    void Toggle(uint64_t fid)
    {
        if (Contains(fid)) {
            Remove(fid);
        } else {
            Add(fid);
        }
    }

    // Undo/Redo 用の一括設定
    void Set(std::vector<uint64_t> newIds, uint64_t newPrimary)
    {
        ids = std::move(newIds);
        primary = newPrimary;
        assetPath.clear();
    }
};

} // namespace mye
