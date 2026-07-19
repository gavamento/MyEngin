#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace mye {

// エディタ内の選択状態 (Hierarchy と Inspector で共有)。
// 選択は **fileId** で保持する — Undo/Redo・Play/Stop・シーン再ロードで EntityID が
// 作り直されても選択を追跡できるようにするため (M8)。EntityID への解決は
// Scene::FindByFileId で各フレーム行う。
struct Selection {
    std::vector<uint64_t> ids; // 選択中の fileId 群 (選択順)
    uint64_t primary = 0;      // アクティブ = Inspector 表示対象 / 範囲選択の起点 (0 = 無し)

    bool Empty() const { return ids.empty(); }
    bool Contains(uint64_t fid) const
    {
        return fid != 0 && std::find(ids.begin(), ids.end(), fid) != ids.end();
    }

    void Clear()
    {
        ids.clear();
        primary = 0;
    }

    // 単一選択 (通常クリック)
    void SelectOnly(uint64_t fid)
    {
        ids.clear();
        if (fid != 0) {
            ids.push_back(fid);
        }
        primary = fid;
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
    }
};

} // namespace mye
