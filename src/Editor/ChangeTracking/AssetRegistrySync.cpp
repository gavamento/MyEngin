//====================================================================================
//                          AssetRegistrySync.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          復元が書いたファイルを、アセットの登録へ反映する
//====================================================================================
#include "Editor/ChangeTracking/AssetRegistrySync.h"

#include "Engine/Engine/Asset/AssetDatabase.h"
#include "Engine/Platform/PathUtil.h"

namespace mye {

namespace {

bool IsUnderAssets(const std::wstring& projectRoot, const std::wstring& path)
{
    const std::wstring prefix = NormalizePathKey(projectRoot) + L"\\assets\\";
    const std::wstring key = NormalizePathKey(path);
    return key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

int SyncAssetRegistry(AssetDatabase& db, const std::wstring& projectRoot, const std::vector<BatchChange>& changes)
{
    int synced = 0;
    for (const BatchChange& change : changes) {
        if (AssetDatabase::IsMetaPath(change.path) || !IsUnderAssets(projectRoot, change.path)) {
            continue;
        }
        switch (change.kind) {
        case BatchChange::Kind::Deleted:
            db.MoveAsset(change.path, change.path); // 旧キーを外すだけ (新パスは無いので再登録は起きない)
            ++synced;
            break;
        case BatchChange::Kind::Added:
            if (AssetDatabase::ClassifyPath(change.path) != AssetType::Unknown) {
                db.GuidForPath(change.path, true); // 同伴の .meta があればその GUID を採る
                ++synced;
            }
            break;
        case BatchChange::Kind::Renamed:
            if (!change.oldPath.empty()) {
                db.MoveAsset(change.oldPath, change.path);
                ++synced;
            }
            break;
        case BatchChange::Kind::Modified: break; // 登録は変わらない (ReloadHub が読み直す)
        }
    }
    return synced;
}

} // namespace mye
