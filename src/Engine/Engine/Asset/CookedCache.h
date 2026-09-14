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
//   srcPathKey 不一致 (ファイル移動) も再クック — blob は外部テクスチャを解決済み絶対パスで
//   持っているので、移動後に旧 blob を再生すると旧パスのテクスチャを読みに行く。
//   (サブアセット AssetID は .meta の GUID 由来なので、移動しても再クック結果のキーは変わらない)
//   deps (クック時に解決した外部テクスチャ実ファイル) は存在のみ検証 — 内容はリプレイ時も
//   実ファイルを読み直すので、編集は自動で反映される。
// kCookVersion bump で全キャッシュ無効化 (blob 形式を変えたら必ず上げる)。
// 2 = M67: Material に reflectionClass (int32) が末尾 append され
// **56 → 64 バイト** (60 + AssetID (uint64) の 8 バイト境界で足りない 4 バイトを明示パディング)。
// ★暗黙パディングのまま 60 → 64 に丸めさせないこと — 詰め物の中身が不定だと同じ入力でも
//   cooked ファイルのバイト列が run ごとに変わり、CookedCacheSelfTest の memcmp が不定になる。
// blob は Material を memcpy で書くので、bump しないと旧キャッシュを 8 バイト短く読んで
// 以降のフィールドが全てずれる (ModelCook.cpp の static_assert(sizeof(Material) == 64) が門番)
// 3 = M74a: サブアセットの登録名が "<正規化絶対パス>#..." から "guid://<16hex>#..." になった。
// blob はキー文字列を**そのまま**持ち (mesh / material / skin / 埋め込みテクスチャ) Replay が
// 再登録するので、bump しないと旧形式のキーが再生されて新形式のシーン参照が全部空振りする。
// ヘッダの検証はキーの中身を見ない = 版でしか弾けない。.mcvx (凸包表のキーも登録名) も同じ版で落ちる
inline constexpr uint32_t kCookVersion = 3;

// M51j: 封印マーカー。cooked ディレクトリにこの名前のファイルがあると「配布ビルドの
// 封印キャッシュ」として扱い、ReadValidated が srcPathKey / stat / 内容ハッシュ / deps の
// 検証を跳ばして再生する (magic / version / guid だけは見る)。
//
// 配布物は移設で pathKey / mtime が必ずずれ、DDS 一括後は元画像そのものが無い。封印キャッシュは
// 「クック時の登録列」をそのまま再生して、移設先での全ミス → 再パース (+ 元画像が無くて失敗) を
// 避ける (サブアセット ID は .meta の GUID 由来なので、ID の正しさは封印に依存しない)。
// 開発環境 (マーカー無し) の無効化判定は封印の影響を受けない。
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
