//====================================================================================
//                          ScenePreviewFormat.cpp
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の比較の画面に出す文面 (実装)
//====================================================================================
#include "Editor/ChangeTracking/ScenePreviewFormat.h"

#include "Engine/Core/Localization/Localization.h"

namespace mye {

namespace {

std::string WithNumber(const char* label, int64_t number)
{
    return std::string(label) + " " + std::to_string(number);
}

} // namespace

const char* PreviewFailureText(PreviewFailure failure)
{
    switch (failure) {
    case PreviewFailure::NoContent: return Tr(StrId::ScPv_FailNoContent);
    case PreviewFailure::Unavailable: return Tr(StrId::ScPv_FailUnavailable);
    case PreviewFailure::TooLarge: return Tr(StrId::ScPv_FailTooLarge);
    case PreviewFailure::NotConnected: return Tr(StrId::ScPv_FailNotConnected);
    case PreviewFailure::ServiceError: return Tr(StrId::ScPv_FailService);
    case PreviewFailure::Mismatch: return Tr(StrId::ScPv_FailMismatch);
    case PreviewFailure::DiskAbsent: return Tr(StrId::ScPv_FailDiskAbsent);
    case PreviewFailure::DiskError: return Tr(StrId::ScPv_FailDiskError);
    case PreviewFailure::None: break;
    }
    return "";
}

std::string PreviewSideText(const ScenePreviewSession::Side& side)
{
    switch (side.state) {
    case ScenePreviewSession::SideState::Idle: return std::string();
    case ScenePreviewSession::SideState::Fetching: {
        std::string text = Tr(StrId::ScPv_Loading);
        if (side.fetch.Total() > 0) {
            text += " (" + std::to_string(side.fetch.Received() / 1024) + " / " + std::to_string(side.fetch.Total() / 1024) + " KiB)";
        }
        return text;
    }
    case ScenePreviewSession::SideState::Loaded: return WithNumber(Tr(StrId::ScPv_Entities), side.load.entityCount);
    case ScenePreviewSession::SideState::Failed: break;
    }
    switch (side.load.status) {
    case PreviewLoadStatus::BadJson: return Tr(StrId::ScPv_LoadBadJson);
    case PreviewLoadStatus::NotAScene: return Tr(StrId::ScPv_LoadNotScene);
    case PreviewLoadStatus::UnsupportedVersion: return WithNumber(Tr(StrId::ScPv_LoadVersion), side.load.docVersion);
    case PreviewLoadStatus::TooManyEntities:
        return std::string(Tr(StrId::ScPv_LoadTooMany)) + " " + std::to_string(side.load.entityCount) + " / " + std::to_string(kPreviewMaxEntities);
    case PreviewLoadStatus::Ok: break;
    }
    return PreviewFailureText(side.failure);
}

ScenePreviewScreen BuildScenePreviewScreen(const ScenePreviewSession& session)
{
    ScenePreviewScreen screen;
    const RestoreRevision& revision = session.Revision();
    screen.target = revision.path + "  #" + std::to_string(revision.revisionId);
    screen.beforeLine = std::string(Tr(StrId::ScPv_LabelBefore)) + " " + PreviewSideText(session.Before());
    screen.afterLine = std::string(Tr(StrId::ScPv_LabelAfter)) + " " + PreviewSideText(session.After());
    screen.busy = session.Before().state == ScenePreviewSession::SideState::Fetching || session.After().state == ScenePreviewSession::SideState::Fetching;

    const bool anyLoaded = session.Before().state == ScenePreviewSession::SideState::Loaded || session.After().state == ScenePreviewSession::SideState::Loaded;
    if (anyLoaded) {
        // アセットは今の内容で描く。常に出す
        screen.notes.push_back(Tr(StrId::ScPv_NoteAssets));
        const PreviewAssetProbe& assets = session.Assets();
        switch (assets.GetState()) {
        case PreviewAssetProbe::State::Probing: screen.notes.push_back(Tr(StrId::ScPv_NoteAssetsChecking)); break;
        case PreviewAssetProbe::State::Unknown: screen.notes.push_back(Tr(StrId::ScPv_NoteAssetsUnknown)); break;
        case PreviewAssetProbe::State::Done: {
            if (assets.ChangedCount() > 0) {
                std::string text = WithNumber(Tr(StrId::ScPv_NoteAssetsChanged), assets.ChangedCount());
                for (const std::string& path : assets.ChangedPaths()) {
                    text += "\n  " + path;
                }
                if (assets.ChangedCount() > static_cast<int>(assets.ChangedPaths().size())) {
                    text += "\n  ...";
                }
                screen.notes.push_back(std::move(text));
            }
            if (assets.Skipped() > 0) {
                screen.notes.push_back(WithNumber(Tr(StrId::ScPv_NoteAssetsSkipped), assets.Skipped()));
            }
            break;
        }
        case PreviewAssetProbe::State::Idle: break;
        }
        if (session.ManagedSkipped() > 0) {
            screen.notes.push_back(WithNumber(Tr(StrId::ScPv_NoteManaged), session.ManagedSkipped()));
        }
        screen.notes.push_back(Tr(StrId::ScPv_NoteRemoved));
        if (session.MarkersOmitted() > 0) {
            screen.notes.push_back(WithNumber(Tr(StrId::ScPv_NoteMarkersOmitted), session.MarkersOmitted()));
        }
    }
    // 比べる対象の範囲は、何が読めたかに関係なく常に出す
    screen.notes.push_back(Tr(StrId::ScPv_NoteUnsaved));
    return screen;
}

} // namespace mye
