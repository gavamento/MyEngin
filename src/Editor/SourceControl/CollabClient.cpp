#include "Editor/SourceControl/CollabClient.h"

#include <utility>

#include <Windows.h>

#include "Engine/Core/Log.h"
#include "Engine/Platform/PathUtil.h" // WideToUtf8 (ログは UTF-8 の narrow で出す)

namespace mye {

namespace {

// DLL 名は構成 (Debug/Release) で分けない — Rust 側は release を 1 本だけ作り、
// build_collab.bat が bin\x64\Debug\ と bin\x64\Release\ の両方へ置く。
// ★ログは UTF-8 の narrow 版を使う (ログは printf 系なので %ls を混ぜると
//   ロケール依存でパスが化ける。既存コードも WideToUtf8 で揃えている)
constexpr const wchar_t* kCollabDllNameW = L"MyeCollab.dll";
constexpr const char* kCollabDllName = "MyeCollab.dll";

template <typename Fn>
bool Bind(HMODULE dll, const char* name, Fn& out)
{
    out = reinterpret_cast<Fn>(::GetProcAddress(dll, name));
    if (!out) {
        MYE_LOG_WARN("[collab] %s is missing from %s", name, kCollabDllName);
        return false;
    }
    return true;
}

} // namespace

CollabClient::~CollabClient()
{
    Shutdown();
}

bool CollabClient::Load(const std::wstring& exeDir)
{
    if (dll_) {
        return Ready();
    }
    const std::wstring path = exeDir + L"\\" + kCollabDllNameW;
    // フルパスで読む: 探索順に任せると、たまたま PATH 上にある別版を掴みうる
    HMODULE dll = ::LoadLibraryW(path.c_str());
    if (!dll) {
        // **これは異常ではない** — rustup を入れていない環境では正常な縮退。
        // ログは INFO で 1 行だけ (WARN にするとトーストへ昇格して毎起動うるさい)
        MYE_LOG_INFO("[collab] %s not found (source control disabled)",
                     WideToUtf8(path).c_str());
        state_ = Unavailable::NoService;
        return false;
    }
    dll_ = dll;

    const bool bound = Bind(dll, "mye_collab_proto_version", protoVersionFn_)
        && Bind(dll, "mye_collab_create", createFn_) && Bind(dll, "mye_collab_request", requestFn_)
        && Bind(dll, "mye_collab_poll", pollFn_) && Bind(dll, "mye_collab_free", freeFn_)
        && Bind(dll, "mye_collab_destroy", destroyFn_);
    if (!bound) {
        // ロードはできたのに export が足りない = 古い DLL。版違いとして扱う
        MYE_LOG_ERROR("[collab] %s does not export the expected C ABI", kCollabDllName);
        state_ = Unavailable::ProtoMismatch;
        return false;
    }

    const uint32_t dllProto = protoVersionFn_();
    if (dllProto != static_cast<uint32_t>(kCollabProtoVersion)) {
        MYE_LOG_ERROR("[collab] proto mismatch: dll=%u editor=%d (rebuild with tools\\build_collab.bat)",
                      dllProto, kCollabProtoVersion);
        state_ = Unavailable::ProtoMismatch;
        return false;
    }
    state_ = Unavailable::None;
    MYE_LOG_INFO("[collab] %s loaded (proto %d)", kCollabDllName, kCollabProtoVersion);
    return true;
}

bool CollabClient::Create(const std::string& rootUtf8)
{
    if (state_ != Unavailable::None || !createFn_) {
        return false;
    }
    if (handle_) {
        return true;
    }
    handle_ = createFn_(rootUtf8.c_str());
    if (!handle_) {
        MYE_LOG_ERROR("[collab] mye_collab_create failed for %s", rootUtf8.c_str());
        state_ = Unavailable::ServiceDied;
        return false;
    }
    return true;
}

uint64_t CollabClient::Request(const char* op, const nlohmann::json& args, ResponseFn onDone)
{
    if (!Ready() || !requestFn_) {
        // 呼ぶ前に Ready() を見るのが窓側の作法。ここでは黙って 0 を返す
        // (コールバックを登録しないので「永久に待つ」状態は作らない)
        return 0;
    }
    const uint64_t id = nextId_++;
    nlohmann::json req;
    req["id"] = id;
    req["op"] = op;
    req["args"] = args.is_null() ? nlohmann::json::object() : args;
    if (onDone) {
        pending_.emplace(id, std::move(onDone));
    }
    requestFn_(handle_, req.dump().c_str());
    return id;
}

uint64_t CollabClient::AddPendingForTest(ResponseFn fn)
{
    const uint64_t id = nextId_++;
    pending_.emplace(id, std::move(fn));
    return id;
}

void CollabClient::Poll()
{
    if (!handle_ || !pollFn_ || !freeFn_) {
        return;
    }
    // NULL まで drain する。1 フレーム 1 件にすると、大きな status の後に
    // 通知が数フレーム遅れて届く = 表示がちらつく
    while (char* line = pollFn_(handle_)) {
        std::string owned(line);
        freeFn_(line); // ★Rust のアロケータへ返す。ここを free/delete にすると即死
        DispatchLine(owned);
    }
}

void CollabClient::DispatchLine(const std::string& line)
{
    nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) {
        MYE_LOG_ERROR("[collab] malformed message from service: %s", line.c_str());
        return;
    }
    if (msg.contains("event")) {
        const std::string ev = msg.value("event", std::string());
        if (ev == "service_error") {
            // worker が panic した。以後の要求は service_dead が返るだけなので
            // 窓は「サービス死亡」を出して読み取り専用に落ちる (spec §4.0)
            MYE_LOG_ERROR("[collab] service_error: %s", msg.value("detail", std::string()).c_str());
            state_ = Unavailable::ServiceDied;
        }
        if (onEvent_) {
            onEvent_(msg);
        }
        return;
    }
    if (!msg.contains("id")) {
        MYE_LOG_ERROR("[collab] message without id or event: %s", line.c_str());
        return;
    }
    const uint64_t id = msg.value("id", static_cast<uint64_t>(0));
    auto it = pending_.find(id);
    if (it == pending_.end()) {
        // 応答が二重に来た / 誰も待っていない id。捨ててよいが黙らない
        MYE_LOG_WARN("[collab] response for unknown id %llu", static_cast<unsigned long long>(id));
        return;
    }
    ResponseFn fn = std::move(it->second);
    pending_.erase(it);
    if (fn) {
        fn(msg);
    }
}

void CollabClient::Shutdown()
{
    if (handle_ && destroyFn_) {
        destroyFn_(handle_); // worker を join してから返る
    }
    handle_ = nullptr;
    pending_.clear();
    if (dll_) {
        // ★destroy の後で FreeLibrary する。逆順だと走行中のコードごとアンロードされる
        ::FreeLibrary(static_cast<HMODULE>(dll_));
        dll_ = nullptr;
    }
    protoVersionFn_ = nullptr;
    createFn_ = nullptr;
    requestFn_ = nullptr;
    pollFn_ = nullptr;
    freeFn_ = nullptr;
    destroyFn_ = nullptr;
    state_ = Unavailable::NoService;
}

} // namespace mye
