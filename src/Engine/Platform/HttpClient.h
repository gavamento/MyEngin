//====================================================================================
//                          HttpClient.h
//  MyEngine/ 秋田蓮音                                                      11/10/2026
//                                          WinHTTP による HTTPS の取得とダウンロード
//====================================================================================
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mye {

// 更新確認と Launcher のダウンロード (M98) 用。シミュレーションからは使わない
// (結果の届く時刻が実行ごとに違うので、決定性の必要な状態へ持ち込まない)。
// https:// だけを受け付ける。リダイレクトは追従する (GitHub の asset URL は別ドメインへ飛ぶ)
struct HttpOptions
{
    std::string userAgent = "MyEngine";
    std::vector<std::string> headers; // "Accept: application/json" の形
    uint32_t connectTimeoutMs = 15000;
    uint32_t receiveTimeoutMs = 30000;
};

struct HttpResult
{
    bool ok = false;
    bool cancelled = false;
    int status = 0;    // HTTP ステータス (接続できなかったときは 0)
    std::string error; // 失敗時の説明 (ログと画面に出す)
};

// done = 受け取ったバイト数、total = Content-Length (分からなければ 0)
using HttpProgressFn = std::function<void(uint64_t done, uint64_t total)>;

// 本文をメモリに受け取る。maxBytes を超えたら失敗にする (API 応答の上限)
HttpResult HttpGetText(const std::string& url, const HttpOptions& options, std::string& body,
                       size_t maxBytes, const std::atomic<bool>* cancel);

// 本文を path へ書く。失敗・取り消し時は書きかけのファイルを消す
HttpResult HttpDownloadFile(const std::string& url, const std::wstring& path, const HttpOptions& options,
                            const HttpProgressFn& progress, const std::atomic<bool>* cancel);

} // namespace mye
