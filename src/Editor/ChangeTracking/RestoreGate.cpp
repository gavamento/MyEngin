//====================================================================================
//                          RestoreGate.cpp
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ファイル・Hunk の復元を止める理由の判定 (ゲート。純関数)
//====================================================================================
#include "Editor/ChangeTracking/RestoreGate.h"

#include <algorithm>

#include "Editor/ChangeTracking/PathGuard.h"

namespace mye {

namespace {

// 対象のうち、文書 (document) と同じパスのもの (大文字小文字を区別しない)。無ければ空
std::string FirstTargetIn(const std::vector<std::string>& targets, const std::vector<std::string>& documents)
{
    for (const std::string& target : targets) {
        const std::string targetKey = RestorePathKey(target);
        for (const std::string& document : documents) {
            if (!document.empty() && RestorePathKey(document) == targetKey) {
                return target;
            }
        }
    }
    return std::string();
}

// 窓の文書の未保存: 文書が分かる窓は対象がその文書のときだけ止め、分からない窓は止める (安全側)
void BlockForWindow(std::vector<RestoreBlock>& out, GateBlocker gate, const EditedDocument& document, const std::vector<std::string>& targets)
{
    RestoreBlock block;
    block.kind = RestoreBlockKind::Gate;
    block.gate = gate;
    if (!document.known) {
        out.push_back(block);
        return;
    }
    block.path = FirstTargetIn(targets, document.paths);
    if (!block.path.empty()) {
        out.push_back(block);
    }
}

} // namespace

std::vector<RestoreBlock> EvaluateRestoreGate(const RestoreGateInput& in)
{
    std::vector<RestoreBlock> out;
    for (const GateBlocker gate : ComputeBlockers(in.gate)) {
        if (in.target == RestoreGateTarget::LiveScene) {
            // World を書き換えるので、Play 中・ネットセッション中・ミニシーンの編集中 (ctx.scene が差し替わっている) だけ止める。
            // ビルド・git の書き込み・未保存のシーン・各窓の未保存は、ファイルを書かないので止めない
            if (gate == GateBlocker::Playing || gate == GateBlocker::NetActive || gate == GateBlocker::ActorEdit) {
                RestoreBlock block;
                block.gate = gate;
                if (gate == GateBlocker::ActorEdit) {
                    block.path = in.documents.openScene;
                }
                out.push_back(block);
            }
            continue;
        }
        switch (gate) {
        case GateBlocker::Playing:
        case GateBlocker::NetActive:
        case GateBlocker::BuildRunning:
        case GateBlocker::ScriptBuildRunning:
        case GateBlocker::OpInFlight:
        case GateBlocker::MergeInProgress: {
            RestoreBlock block;
            block.gate = gate;
            out.push_back(block);
            break;
        }
        case GateBlocker::SceneDirty: {
            // 未保存のシーンを書き換えるのは、対象が開いているシーンのファイルのときだけ
            RestoreBlock block;
            block.gate = gate;
            block.path = FirstTargetIn(in.targetPaths, { in.documents.openScene });
            if (!block.path.empty()) {
                out.push_back(block);
            }
            break;
        }
        case GateBlocker::ActorEdit: BlockForWindow(out, gate, in.documents.actor, in.targetPaths); break;
        case GateBlocker::AnimationDirty: BlockForWindow(out, gate, in.documents.animation, in.targetPaths); break;
        case GateBlocker::ControllerDirty: BlockForWindow(out, gate, in.documents.controller, in.targetPaths); break;
        case GateBlocker::MixerDirty: BlockForWindow(out, gate, in.documents.mixer, in.targetPaths); break;
        case GateBlocker::BehaviorTreeDirty: BlockForWindow(out, gate, in.documents.behaviorTree, in.targetPaths); break;
        case GateBlocker::ProjectSettingsDirty: BlockForWindow(out, gate, in.documents.projectSettings, in.targetPaths); break;
        case GateBlocker::SpriteEditorDirty: BlockForWindow(out, gate, in.documents.spriteEditor, in.targetPaths); break;
        case GateBlocker::TilePaletteDirty: BlockForWindow(out, gate, in.documents.tilePalette, in.targetPaths); break;
        case GateBlocker::ServiceUnavailable: break; // collab (git) が使えなくても止めない。復元は git を使わない
        case GateBlocker::Count: break;
        }
    }
    auto tracking = [&](RestoreBlockKind kind) {
        RestoreBlock block;
        block.kind = kind;
        out.push_back(block);
    };
    if (!in.trackConnected) {
        tracking(RestoreBlockKind::TrackUnavailable);
    }
    if (in.journalPending) {
        tracking(RestoreBlockKind::JournalPending);
    }
    if (in.blobMissing) {
        tracking(RestoreBlockKind::BlobMissing);
    }
    if (in.target == RestoreGateTarget::LiveScene && in.externalChangePending) {
        tracking(RestoreBlockKind::ExternalChangePending);
    }
    return out;
}

} // namespace mye
