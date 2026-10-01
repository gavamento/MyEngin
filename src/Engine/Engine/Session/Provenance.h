//====================================================================================
//                          Provenance.h
//  MyEngin/ 秋田蓮音                                                     10/02/2026
//                                          出自情報 (SimProvenance) の算出と照合
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Engine/Session/SessionTypes.h"

namespace mye {

// 「同じものを走らせているか」の指紋を作り、照合する場所 (M81c)。
// ★sim 側 (Net/ を include しない)。P2P の NetIdentity も、将来のサーバ/クライアントの Hello も、
//   照合項目はここの CompareProvenance 1 本から導く (項目を 2 か所で管理しない)。
//   Debug と Release で同じ値にならなければならない項目 (engine / content / schema) は
//   構成名・コンパイラ・最適化に依存させないこと。

// ---- engineVersion (ビルド ID) ----

// ビルド時に焼かれた git 記述子 (build\Common.props の MyeBuildInfo の MYE_GIT_HASH。"unknown" あり)
std::string_view EngineBuildGit();
// 記述子 → engineVersion。"unknown" / 空は 0 (= 不明)、それ以外は 64bit ハッシュ (0 にはしない)
uint64_t EngineVersionFromGit(std::string_view git);
// 記述子が未コミットの差分を含むか ("-dirty" で終わる)。dirty 同士は中身が違っても一致してしまう
bool IsDirtyGit(std::string_view git);

// ---- gameVersion ----

// ファイルのバイト列の 64bit ハッシュ。読めなければ 0
uint64_t HashFileBytes(const std::wstring& path);

// ---- schemaVersion ----

// kSimSnapshotVersion と sizeof(InputSnapshot) と kReplayFileVersion を畳んだ値
uint32_t ComputeSchemaVersion();

// ---- contentHash ----

// 1 ファイル分の記録。path は assets ルート相対・'/' 区切り・小文字
struct ContentEntry {
    std::string path;
    uint64_t size = 0;
    uint64_t hash = 0;
};

// contentHash の対象外にする拡張子か (描画・音声専用の資産。小文字比較。ドット付き ".png")。
// 除外は明示リスト方式: 新しい資産種別は既定で対象に入る (多めに含めるのは安全側)
bool IsContentExcludedExtension(std::wstring_view ext);

// assets ルート直下に置く manifest のファイル名 (これ自身は対象から外す)
inline constexpr const wchar_t* kContentManifestName = L"content_manifest.json";

// 対象ファイルを列挙して (正規化パス昇順に) 中身のハッシュを取る。読めないファイルがあれば false
bool CollectContentEntries(const std::wstring& assetsRoot, std::vector<ContentEntry>& out);
// エントリ列 (整列済み) → contentHash
uint64_t FoldContentEntries(const std::vector<ContentEntry>& entries);

// manifest (JSON) の書出し。成功したら outHash に全体ハッシュ
bool WriteContentManifest(const std::wstring& manifestPath, const std::wstring& assetsRoot,
                          uint64_t* outHash = nullptr);
// manifest の読込。エントリを畳み直して記録済みの全体ハッシュと一致したときだけ true
bool ReadContentManifest(const std::wstring& manifestPath, uint64_t& outHash, size_t* outFileCount = nullptr);

// 起動時の contentHash: assets ルート直下に manifest があればそれを読み、無ければ計算する
// (所要時間をログに出す)。失敗は 0
uint64_t ResolveContentHash(const std::wstring& assetsRoot);

// ---- 組み立てと照合 ----

// この実行の出自。dllHash = 実際にロードした GameLogic.dll のバイト列ハッシュ (無ければ 0)。
// protocolVersion は呼び出し側 (Net 層の版) が渡す。initialSnapshotHash は 0 のまま返す
SimProvenance MakeSimProvenance(uint64_t dllHash, uint64_t contentHash, uint32_t protocolVersion);

// 起動ログ 1 行: [provenance] engine=... game=... content=...
std::string FormatProvenance(const SimProvenance& p);

enum class ProvenanceMismatch : uint32_t {
    None = 0,
    ProtocolVersion,
    ApiVersion,
    ReplayVersion,
    SchemaVersion,
    EngineVersion,
    GameVersion,
    ContentHash,
    InitialSnapshot,
};
const char* ProvenanceMismatchName(ProvenanceMismatch m);

// 最初に食い違った項目を返す (全部並べるより原因が 1 行で読める)。
// engineVersion / contentHash は双方 0 (不明) なら WARN 付きで一致扱い、片方だけ 0 は不一致。
// allowGameMismatch (--allow-game-mismatch) のときは gameVersion の不一致だけ WARN に落とす
ProvenanceMismatch CompareProvenance(const SimProvenance& a, const SimProvenance& b,
                                     bool allowGameMismatch = false);

} // namespace mye
