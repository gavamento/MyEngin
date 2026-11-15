//====================================================================================
//                          ChangeTrackingSession.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Change Tracking の持ち主 (宣言)
//====================================================================================
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/TrackClient.h"

namespace mye {

// 保存の知らせのためにハッシュを取るファイルの上限。これを超えるものは知らせない (その保存は不明 / 推定に落ちるだけ)。
// 1 回の Poll で扱う件数の上限と合わせて、フレームを止めない大きさに抑える
constexpr uint64_t kTrackSavedHashMaxBytes = 8ull * 1024 * 1024;
constexpr int kTrackSavedPerPoll = 2;
// 知らせの順番待ちの上限 (溢れた分は捨てる)
constexpr size_t kTrackSavedMaxPending = 256;

// ファイルの SHA-256 (小文字 16 進)。読めない・大きすぎるときは false
using SavedFileHasher = std::function<bool(const std::wstring& absPath, std::string& hexOut)>;
// 既定のハッシュ関数 (BCrypt。kTrackSavedHashMaxBytes を超える・通常のファイルでないものは false)
bool HashFileForSavedNotice(const std::wstring& absPath, std::string& hexOut);
// エディタ 1 プロセス分の AI Change Tracking の持ち主 (SourceControlSession の形)。
// 窓ではなく状態: 窓が閉じていても接続と通知の受信は続き、開いたときに取り直す。
// **Shutdown は接続を閉じるだけでサービスを止めない** (Editor を閉じても記録が続く。architecture.md §3)
class ChangeTrackingSession {
public:
    ChangeTrackingSession() = default;
    ChangeTrackingSession(const ChangeTrackingSession&) = delete;
    ChangeTrackingSession& operator=(const ChangeTrackingSession&) = delete;

    // 起動時に 1 回。enabled が偽 (設定で無効 / 非対話起動) なら I/O の口を 1 回も呼ばず Disabled、
    // projectRoot が空なら NoProject。exeDir は mye_track.exe を探す場所 (Editor の exe の隣)
    void Start(const std::wstring& exeDir, const std::wstring& projectRoot, bool enabled);
    // 試験用: I/O を差し替えて始める (Start と同じ条件の判定)
    void StartWithIo(std::unique_ptr<TrackIo> io, const std::wstring& projectRoot, bool enabled);

    // 毎フレーム 1 回 (OnImGui の先頭)。待たない
    void Poll();
    void Shutdown();

    // Editor が absPath を保存した / 作った / 移した直後に呼ぶ口 (Editor 全体で 1 か所。呼び元は EditorApp::NoteFileSaved)。
    // 接続中 (Ready) で、プロジェクトの中 (.mye の外) のときだけ順番待ちに積む。それ以外ではハッシュも計算しない。
    // 実際のハッシュ計算と送信は次の Poll 以降 (保存を遅らせない・失敗させない)。切断・時間切れで送れなければ何もしない
    void NoteSaved(const std::wstring& absPath);
    // 試験用: ハッシュの取り方を差し替える
    void SetSavedFileHasher(SavedFileHasher hasher) { hasher_ = std::move(hasher); }
    size_t PendingSavedCount() const { return pendingSaved_.size(); }

    TrackState State() const { return client_.State(); }
    TrackClient& Client() { return client_; }
    ChangeModel& Model() { return model_; }
    const ChangeModel& Model() const { return model_; }

private:
    void Wire();
    // 順番待ちのパスを、1 回の Poll につき kTrackSavedPerPoll 件までハッシュして editor_saved で送る
    void DrainSaved();

    // ★宣言順に意味がある: client_ のデストラクタが保留中の要求を失敗で返し、そのコールバックが model_ に触る。
    //   model_ を先に宣言して、後に壊れるようにする
    ChangeModel model_;
    TrackClient client_;
    uint32_t seenConnects_ = 0;
    std::wstring projectRoot_;
    SavedFileHasher hasher_ = HashFileForSavedNotice;
    // 正規化したキー (小文字・\ 区切り) → 絶対パス。同じファイルの保存は 1 件にまとめ、順序はキー順で決める
    std::map<std::wstring, std::wstring> pendingSaved_;
};

} // namespace mye
