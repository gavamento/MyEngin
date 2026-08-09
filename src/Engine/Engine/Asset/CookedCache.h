#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye::CookedCache {

// アセットクックキャッシュ (M51b、spec §10)。起動毎の FBX/glTF フルパースと .ogg デコードを
// <cache>\cooked\<guid 16hex>.mmdl / .mpcm へ保存して 2 回目以降の起動を短縮する。
// クック blob は「パース結果の生バイト」— リプレイ登録はフレッシュパースとビット同一が契約。
//
// 無効化はヘッダで判定する:
//   size 不一致 → 再クック / mtime 一致 → 即有効 /
//   mtime 不一致 → コンテンツハッシュ一致なら mtime だけ自己修復して有効、不一致で再クック。
//   srcPathKey 不一致 (ファイル移動) も再クック — モデルのサブアセット AssetID は正規化パス由来
//   なので、移動後のフレッシュパースは別キーを登録する。クックだけ旧キーを再生すると意味論が割れる。
//   deps (クック時に解決した外部テクスチャ実ファイル) は存在のみ検証 — 内容はリプレイ時も
//   実ファイルを読み直すので、編集は自動で反映される。
// kCookVersion bump で全キャッシュ無効化 (blob 形式を変えたら必ず上げる)。
inline constexpr uint32_t kCookVersion = 1;

// 起動形態で 1 回だけ設定する (EngineLoop)。プロジェクト起動 = <project>\cache\cooked、
// レガシー起動/配布 = <exeDir>\cache\cooked (分岐は projectRoot の有無 — 二経路規則)。
// 未設定 (selftest 等) は全 API が no-op = キャッシュ無効と同じ。
void Configure(const std::wstring& cookedDir, bool enabled);
bool Enabled();
const std::wstring& Dir();

// srcPath に対応するクックファイルの絶対パス (guid 16hex + ext)。無効時/解決不能は空
std::wstring PathFor(const std::wstring& srcPath, const wchar_t* ext);

// ヘッダを検証して payload を返す。false = キャッシュ無し/無効 (呼び出し側がフレッシュパース)
bool ReadValidated(const std::wstring& srcPath, const wchar_t* ext,
                   std::vector<uint8_t>& payloadOut);

// srcPath の現在の stat + コンテンツハッシュでヘッダを書き、payload を保存する。
// deps = クック時に解決した外部依存ファイル (ReadValidated が存在検証する)
bool Write(const std::wstring& srcPath, const wchar_t* ext, const void* payload,
           size_t payloadSize, const std::vector<std::wstring>& deps = {});

} // namespace mye::CookedCache
