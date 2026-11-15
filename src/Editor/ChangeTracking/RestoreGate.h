//====================================================================================
//                          RestoreGate.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          ファイル・Hunk の復元を止める理由の判定 (ゲート。純関数、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Editor/SourceControl/GitTransaction.h" // GateBlocker / GateInputs / ComputeBlockers (ゲートの型と純関数だけ。GitTransaction 自体は使わない)

namespace mye {

// restore-safety.md §2 の表の「ファイル・Hunk の復元」の列。立っている理由は**全部**並べる (1 件目で止めない)。

// ある窓が今編集している文書。known が偽 (窓が文書を教えられない) のとき、その窓の「未保存」が立っていれば対象に関係なく止める (安全側)
struct EditedDocument {
    bool known = false;
    std::vector<std::string> paths; // プロジェクトからの相対 (/ 区切り)
};

// Editor が渡す「どの窓がどの文書を編集中か」
struct EditedDocuments {
    // 今開いているシーン (ミニシーン編集中はそのアセット)。空 = まだパスの無い新しいシーン
    std::string openScene;
    EditedDocument actor;
    EditedDocument animation;
    EditedDocument controller;
    EditedDocument mixer;
    EditedDocument behaviorTree;
    EditedDocument projectSettings;
    EditedDocument spriteEditor;
    EditedDocument tilePalette;
};

// 止める理由の種類。Gate は GateBlocker、残りは追跡に固有の条件 (restore-safety.md §2 の下の 3 つ)
enum class RestoreBlockKind : uint8_t {
    Gate,
    TrackUnavailable, // 追跡サービスに接続できていない (Before の内容を取れない)
    JournalPending,   // 進行中・回復待ちの復元がある
    BlobMissing,      // 対象の Blob が欠けている
};

struct RestoreBlock {
    RestoreBlockKind kind = RestoreBlockKind::Gate;
    GateBlocker gate = GateBlocker::SceneDirty; // kind == Gate のとき
    // 文書が理由のとき (SceneDirty / ActorEdit / 各窓の未保存) の対象のパス。窓が文書を教えられず安全側で止めたときは空
    std::string path;
    bool operator==(const RestoreBlock&) const = default;
};

struct RestoreGateInput {
    GateInputs gate;            // EditorApp::BuildGateInputs の結果
    EditedDocuments documents;
    std::vector<std::string> targetPaths; // 復元が触るパス (プロジェクトからの相対)
    bool trackConnected = false;
    bool journalPending = false;
    bool blobMissing = false;
};

// 止める理由を、ゲートの列挙順 → 追跡に固有の順で全部返す。**純関数**。空 = 止めない
std::vector<RestoreBlock> EvaluateRestoreGate(const RestoreGateInput& input);

} // namespace mye
