#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "nlohmann/json.hpp"

#include "Editor/SourceControl/CollabProtocol.h"

namespace mye {

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

    ~CollabClient();

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

    Unavailable State() const { return state_; }
    bool Ready() const { return state_ == Unavailable::None && handle_ != nullptr; }
    bool Loaded() const { return dll_ != nullptr; }
    // まだ応答が返っていない要求の数 (ゲートの OpInFlight 判定に使う)
    size_t PendingCount() const { return pending_.size(); }

    // 1 行の JSON を配る。Poll() が使う本体だが、**DLL 無しでも配線を検査できる**よう
    // 公開している (SourceControlSelfTest が偽の応答/通知を流し込む)
    void DispatchLine(const std::string& line);

    // テスト専用: 「id -> コールバック」の配線だけを DLL 無しで検査するために
    // 待ち行列へ直接積む (本番経路では Request() が積む)。戻り値は採番した id
    uint64_t AddPendingForTest(ResponseFn fn);

private:
    void* dll_ = nullptr;    // HMODULE (Windows.h を公開ヘッダへ持ち込まないため void*)
    void* handle_ = nullptr; // mye_collab_create のハンドル
    Unavailable state_ = Unavailable::NoService;
    uint64_t nextId_ = 1;
    std::unordered_map<uint64_t, ResponseFn> pending_;
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
