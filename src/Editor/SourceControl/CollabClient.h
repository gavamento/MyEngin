#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "nlohmann/json.hpp"

#include "Editor/SourceControl/CollabProtocol.h"

namespace mye {

// SourceControlSelfTest.h と同じ宣言 (friend にするために前方宣言が要る)
bool RunSourceControlSelfTest();

// MyeCollab.dll (Rust cdylib) の in-process クライアント (M66a)。
//
// 設計の要点:
//   * **C++ 側にスレッドを作らない**。git の直列実行も定期 fetch も DLL の中の
//     worker スレッド 1 本が持つ。こちらは毎フレーム Poll() を NULL まで drain して
//     応答をコールバックへ配るだけ = ロックも条件変数も Editor 層に持ち込まない。
//   * DLL が返した文字列は **mye_collab_free でしか解放しない** (Rust のアロケータで
//     確保されているので delete/free で返すとヒープが違って即クラッシュする)。
//   * Shutdown は destroy -> FreeLibrary の順。逆にすると走行中の worker のコードごと
//     アンロードされる。
//   * DLL が無い / 版が違うのは**異常ではない** (rustup 未導入の同僚がいる)。
//     State() が理由を返し、エディタの他機能は一切影響を受けない。
class CollabClient {
public:
    using ResponseFn = std::function<void(const nlohmann::json&)>; // 応答 1 件
    using EventFn = std::function<void(const nlohmann::json&)>;    // 通知 1 件

    CollabClient() = default;
    ~CollabClient();
    // ★コピー禁止。dll_ / handle_ は所有権のある生ポインタで、デストラクタが
    //   Shutdown (destroy + FreeLibrary) を呼ぶ = 複製すると二重解放になる
    CollabClient(const CollabClient&) = delete;
    CollabClient& operator=(const CollabClient&) = delete;

    // <exeDir>\MyeCollab.dll をロードして版を照合する。失敗しても例外は投げない
    // (理由は State() に載る)。二重呼び出しは無視
    bool Load(const std::wstring& exeDir);

    // リポジトリのルート (UTF-8) でサービスハンドルを作る。Load 成功後に 1 回
    bool Create(const std::string& rootUtf8);

    // 要求を投げる。戻り値は採番した id (0 = 送れなかった)。
    // onDone は Poll() の中から**同じスレッドで**呼ばれる
    uint64_t Request(const char* op, const nlohmann::json& args, ResponseFn onDone);

    // 応答/通知を NULL まで drain する。毎フレーム 1 回呼ぶ
    void Poll();

    // destroy + FreeLibrary。以後 State() は NoService
    void Shutdown();

    void SetEventHandler(EventFn fn) { onEvent_ = std::move(fn); }

    // 状態を外から落とす (repo_check の結果や hello の error.code を受けて
    // 窓が NotRepo / NoGit などへ遷移させる。逆向きに None へ戻すのは禁止 —
    // ProtoMismatch / ServiceDied は復帰しない)
    void SetUnavailable(Unavailable reason);

    Unavailable State() const { return state_; }
    bool Ready() const { return state_ == Unavailable::None && handle_ != nullptr; }
    bool Loaded() const { return dll_ != nullptr; }
    // まだ応答が返っていない要求の数 (ゲートの OpInFlight 判定に使う)
    size_t PendingCount() const { return pending_.size(); }
    // 未応答の**書き込み系**があるか (spec §4.1 の GateBlocker::OpInFlight)。
    // 読み取り系 (status の自動更新) で書き込みボタンを塞ぐと、監視が動くたびに
    // ボタンがちらついて押せなくなる
    bool OpInFlight() const;

    // 1 行の JSON を配る。Poll() が使う本体だが、**DLL 無しでも配線を検査できる**よう
    // 公開している (SourceControlSelfTest が偽の応答/通知を流し込む)
    void DispatchLine(const std::string& line);

private:
    // テスト専用: 「id -> コールバック」の配線だけを DLL 無しで検査するために
    // 待ち行列へ直接積む (本番経路では Request() が積む)。戻り値は採番した id。
    // op を渡すとタイムアウト分類も本番と同じになる。
    // ★private + friend にしてある — public に置くと「DLL 無しでも要求を積める」
    //   ように読めてしまい、窓側が誤って使うと**応答が永久に来ない**待ち行列ができる
    friend bool RunSourceControlSelfTest();
    uint64_t AddPendingForTest(ResponseFn fn, const char* op = "status");

    // 送信済みで応答待ちの 1 件
    struct Pending {
        ResponseFn fn;
        std::chrono::steady_clock::time_point sentAt;
        int timeoutMs = 0; // 0 = 無期限 (書き込み系)
        std::string op;
        CollabOpKind kind = CollabOpKind::Read;
    };

    // 期限切れの要求にエラー応答を合成して配る (Poll から毎フレーム)
    void ExpireTimedOut();

    void* dll_ = nullptr;    // HMODULE (Windows.h を公開ヘッダへ持ち込まないため void*)
    void* handle_ = nullptr; // mye_collab_create のハンドル
    Unavailable state_ = Unavailable::NoService;
    uint64_t nextId_ = 1;
    // ★unordered_map ではなく map。走査順が id 昇順 = 送信順に固定されるので、
    //   複数が同時に期限切れになったときのコールバック順が機種依存にならない
    //   (sim には載らないが、UI のトースト順が環境で変わるのは同じ理由で嫌う)
    std::map<uint64_t, Pending> pending_;
    EventFn onEvent_;

    // DLL の export (spec §4.0 の 6 本)
    uint32_t (*protoVersionFn_)() = nullptr;
    void* (*createFn_)(const char*) = nullptr;
    void (*requestFn_)(void*, const char*) = nullptr;
    char* (*pollFn_)(void*) = nullptr;
    void (*freeFn_)(char*) = nullptr;
    void (*destroyFn_)(void*) = nullptr;
};

} // namespace mye
