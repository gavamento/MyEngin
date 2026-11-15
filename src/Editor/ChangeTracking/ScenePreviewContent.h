//====================================================================================
//                          ScenePreviewContent.h
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After のシーンの内容の取得と、プレビュー専用 Scene への読み込み (宣言)
//====================================================================================
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Editor/ChangeTracking/RestoreFileIo.h"
#include "Editor/ChangeTracking/RestorePlan.h"

namespace mye {

class Scene;
class TrackClient;
class World;

// プレビューが読む JSON の上限。超えたら「大きすぎます」で止める (読み込みでフレームを止めない・メモリを食い潰さない)
constexpr size_t kPreviewMaxDocumentBytes = 32u * 1024u * 1024u;
// blob_get の 1 回の取得量 (サービスの上限)。大きい文書は応答ごとに次を取るので、自然に数フレームへ分かれる
constexpr uint64_t kPreviewBlobChunkBytes = 512u * 1024u;
// 今のファイルを 1 回の Tick で読む量
constexpr size_t kPreviewDiskChunkBytes = 1024u * 1024u;
// プレビューへ読むエンティティの上限。超えたら読まずに「大きすぎます」と出す (一部だけを読む案は採らない:
// 一部だけでは Before と After で映る範囲が食い違い、比較として嘘になる)
constexpr int kPreviewMaxEntities = 10000;

enum class PreviewFetchState : uint8_t { Idle, Loading, Ready, Failed };

enum class PreviewFailure : uint8_t {
    None,
    NoContent,     // その側が無い (追加の Before、削除の After)
    Unavailable,   // Blob が欠けている・除外・回収された
    TooLarge,      // kPreviewMaxDocumentBytes を超える
    NotConnected,  // 追跡サービスと話せていない
    ServiceError,  // blob_get の失敗・時間切れ・壊れた応答
    Mismatch,      // 取った内容のハッシュが記録と合わない
    DiskAbsent,    // 今のファイルが無い
    DiskError,     // 今のファイルを読めない・通常のファイルでない・書き先の検査に通らない
};

// Before / After の内容 (JSON の文字列) を取る。**ファイルは読むだけ**: サービスの blob_get (ディスクの内容の写し) か、
// RestoreFileIo で今のファイルを読む。Poll の外で待たない (応答で次を要求する・Tick で 1 回分だけ読む)
class PreviewContentFetch {
public:
    PreviewContentFetch();

    // 記録した面の内容を blob_get で取る。内容を持たない面はそれぞれの失敗で止まる
    void StartBlob(TrackClient& client, const RestoreSide& side);
    // 今のファイル (プロジェクトの中の相対パス)。io はこの取得が終わるまで生きていること
    void StartDisk(const std::wstring& projectRoot, const std::string& relPath, RestoreFileIo& io);
    // 今のファイルを kPreviewDiskChunkBytes だけ進める。blob の取得では何もしない
    void Tick();
    void Cancel();

    PreviewFetchState State() const;
    PreviewFailure Failure() const;
    // 取れたバイト数 / 総バイト数 (分からなければ 0)
    uint64_t Received() const;
    uint64_t Total() const;
    // Ready のときだけ中身がある
    const std::string& Text() const;
    // 応答が壊れた・ハッシュが合わないなど、利用者に見せる補足 (空のことがある)
    const std::string& Detail() const;

private:
    struct Data;
    static void RequestNext(TrackClient& client, const std::shared_ptr<Data>& data);
    std::shared_ptr<Data> data_;
};

enum class PreviewLoadStatus : uint8_t {
    Ok,
    BadJson,            // JSON として読めない
    NotAScene,          // シーンの文書の形ではない (entities が無い・壊れた要素)
    UnsupportedVersion, // このエンジンより新しい version
    TooManyEntities,    // kPreviewMaxEntities を超える
};

struct PreviewLoadInfo {
    PreviewLoadStatus status = PreviewLoadStatus::Ok;
    int entityCount = 0;
    int docVersion = 0;
    // 読み込まなかった C# コンポーネントの数 (インスタンス単位)
    int managedSkipped = 0;
    std::string detail;
};

// 名前のコンポーネントが C# スクリプトか (ManagedHost の問い合わせ。読むだけ)
using ManagedComponentFilter = std::function<bool(const std::string& componentName)>;

// text の JSON を、プレビュー専用の scene へ読み込む。scene は破棄してよい自前の Scene (本番の Scene を渡してはならない)。
// 本番と同じ SceneSerializer::LoadFromJson を通すが、**C# コンポーネントは JSON から取り除いてから渡す**
// (managed のインスタンスを作ると本番の C# の状態に触れうる)。読めないときは scene を空のままにして理由を返す (落ちない)
PreviewLoadInfo LoadPreviewScene(Scene& scene, const std::string& text, const ManagedComponentFilter& isManaged);

// GUID → プロジェクトからの相対パス (/ 区切り、UTF-8)。プロジェクトの外・組み込みで引けなければ空
using AssetPathResolver = std::function<std::string(uint64_t guid)>;

// シーンが参照するアセットの GUID (AssetRef フィールドの値。0 を除く)。昇順・重複なし。読むだけ
std::vector<uint64_t> CollectPreviewAssetRefs(World& world);

// Before の後に、シーンが参照するアセットへ記録された変更があるか。追跡の履歴 (list の path 絞り込み) で分かる範囲だけ。
// アセットの内容は今のものを描くので、その旨の注記の根拠になる
constexpr int kPreviewMaxAssetProbes = 64;
constexpr int kPreviewMaxAssetPathsShown = 8;

class PreviewAssetProbe {
public:
    PreviewAssetProbe();

    enum class State : uint8_t { Idle, Probing, Done, Unknown };

    // 各パスについて list(path, limit=1) を 1 回ずつ出し、revisionId > afterRevisionId の変更を数える。
    // paths は重複なしで、先頭の kPreviewMaxAssetProbes 件だけ調べる (残りは skipped)。接続できなければ Unknown
    void Start(TrackClient& client, std::vector<std::string> paths, int64_t afterRevisionId);
    void Cancel();

    State GetState() const;
    int Checked() const;
    int ChangedCount() const;
    int Skipped() const;
    // 変更のあったパス (先頭の kPreviewMaxAssetPathsShown 件。パスの昇順)
    const std::vector<std::string>& ChangedPaths() const;

private:
    struct Data;
    std::shared_ptr<Data> data_;
};

} // namespace mye
