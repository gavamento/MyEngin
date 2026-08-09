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

// M51j: 封印マーカー。cooked ディレクトリにこの名前のファイルがあると「配布ビルドの
// 封印キャッシュ」として扱い、ReadValidated が srcPathKey / stat / 内容ハッシュ / deps の
// 検証を跳ばして再生する (magic / version / guid だけは見る)。
//
// これは性能ではなく**正しさ**の仕組み: モデルのサブアセット AssetID は正規化した絶対
// パス由来なので、配布物のシーンはクック元 (パッケージ元) のパスから導出された ID を
// 参照している。移設先で pathKey 不一致→再クック (フレッシュパース) すると移設先パス由来の
// 別 ID が登録され、**シーンのメッシュ/マテリアル参照が全部空振りする**。封印キャッシュが
// 「クック時の登録列」をそのまま再生することで、配布物はどこに置いても同じ ID が揃う。
// 開発環境 (マーカー無し) の無効化判定は従来どおり 1 ビットも変わらない。
// マーカーは BuildSettings のパッケージ段が dist 側にだけ書く — リポジトリ/プロジェクトの
// cooked ディレクトリには決して置かないこと
inline constexpr const wchar_t* kSealedMarker = L".sealed";

// 起動形態で 1 回だけ設定する (EngineLoop)。プロジェクト起動 = <project>\cache\cooked、
// レガシー起動/配布 = <exeDir>\cache\cooked (分岐は projectRoot の有無 — 二経路規則)。
// 未設定 (selftest 等) は全 API が no-op = キャッシュ無効と同じ。
void Configure(const std::wstring& cookedDir, bool enabled);
bool Enabled();
bool Sealed(); // 封印キャッシュとして動作中か (Configure がマーカーの有無で判定)
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
