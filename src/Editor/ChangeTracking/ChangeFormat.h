//====================================================================================
//                          ChangeFormat.h
//  MyEngine/ 秋田蓮音                                                      11/15/2026
//                                          AI Changes 窓の文面の整形 (ImGui を呼ばない純関数、宣言)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeModel.h"
#include "Editor/ChangeTracking/TrackClient.h"

namespace mye {

// 一覧の節。attribution-rules.md §8: Unknown / Inferred は既定で別の節に分け、切り替えで混ぜられる
enum class ChangeSection : uint8_t { Main, Uncertain };

// 帰属の区分から節を決める。mixUncertain が真なら、すべて Main (1 つの一覧)
ChangeSection ClassifyChange(const std::string& attribution, bool mixUncertain);

// 一覧の行の添字を節ごとに振り分ける (元の順序を保つ)
struct ChangePartition {
    std::vector<size_t> main;
    std::vector<size_t> uncertain;
};
ChangePartition PartitionChanges(const std::vector<TrackChangeRow>& rows, bool mixUncertain);

// 文面は、呼んだ時点の言語 (Tr) で返す。未知の値は、値そのものを返す (表示から消さない)
std::string KindLabel(const std::string& changeType);
std::string AttributionLabel(const std::string& attribution);
std::string AttributionNote(const std::string& attribution);
std::string ActorLabel(const std::string& actor);
std::string CompletenessLabel(const std::string& completeness);
// complete 以外の意味を 1 行で。complete は空。evidenceSource は describe の observed の根拠にある source (verify_gap など)
std::string CompletenessNote(const std::string& completeness, const std::string& evidenceSource);
std::string SessionStatusLabel(const std::string& status);
std::string DiffModeLabel(const std::string& diffMode);
// lines 以外の Hunk が無い (または全体 1 つの) 理由。lines は空
std::string DiffModeNote(const std::string& diffMode);

// 根拠 1 件 (describe の evidence[] の 1 要素) の文面。detail.source があれば、見つけ方も添える
std::string EvidenceText(const nlohmann::json& evidence);
// 根拠の kind の一覧の順 (文面の網羅の試験用)
const std::vector<std::string>& EvidenceKinds();

// observed の根拠にある source (watch / startup_gap / overflow_gap / verify_gap)。無ければ空
std::string EvidenceSource(const nlohmann::json& detail);

// 一覧の 1 行
struct ChangeRowText {
    std::string path;          // リネームは「旧 -> 新」
    std::string kind;
    std::string attribution;
    std::string completeness;  // 空 = complete
    std::string superseded;    // 空 = 引き継がれていない
    std::string time;
    std::string size;          // After の大きさ。無ければ空
};
ChangeRowText FormatChangeRow(const TrackChangeRow& row);

struct SessionRowText {
    std::string title;  // provider
    std::string status;
    std::string time;
    std::string count;  // 「N 件の変更」
};
SessionRowText FormatSessionRow(const TrackSessionRow& row);

// UTC ミリ秒 → 現地時刻の "YYYY-MM-DD HH:MM:SS"。0 以下は空
std::string FormatTimeMs(int64_t utcMs);
// 1024 進の大きさ ("512 B" / "1.5 MiB")
std::string FormatBytes(int64_t bytes);

// プロジェクトの相対パス (UTF-8、区切りは / ) を絶対パスへ。".." を含む・絶対パス・空は空文字列 (プロジェクトの外を開かない)
std::wstring ProjectFilePath(const std::wstring& projectRoot, const std::string& relPath);

// 状態の帯の文面 (状態ごとに 1 つ)。Disabled のとき、allowed が偽 (自動実行の起動) ならその旨、真なら設定で無効の旨
std::string StateBannerText(TrackState state, bool allowed, const std::string& dbReason);

// 動作中のときの要約の行 (ベースラインの進捗・追跡中のファイル・Blob の総量・欠落区間・監視の縮退)
std::vector<std::string> StatusLines(const TrackStatusInfo& status);

} // namespace mye
