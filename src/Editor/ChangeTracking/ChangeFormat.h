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
#include "Editor/ChangeTracking/SceneChangeOverlay.h"
#include "Editor/ChangeTracking/TrackClient.h"

namespace mye {

// 一覧の節。attribution-rules.md §8: Unknown / Inferred は既定で別の節に分け、切り替えで混ぜられる。
// Editor の保存・復元で確定した変更 (actor = editor_user / restore) は AI の変更でも不明でもないので、さらに別の節 (既定は非表示)
enum class ChangeSection : uint8_t { Main, Uncertain, Editor };

// 行為者と帰属の区分から節を決める。Editor / 復元の変更は常に Editor。mixUncertain が真なら、不明 / 推定も Main
ChangeSection ClassifyChange(const std::string& actor, const std::string& attribution, bool mixUncertain);

// 一覧の行の添字を節ごとに振り分ける (元の順序を保つ)
struct ChangePartition {
    std::vector<size_t> main;
    std::vector<size_t> uncertain;
    std::vector<size_t> editor;
};
ChangePartition PartitionChanges(const std::vector<TrackChangeRow>& rows, bool mixUncertain);

// 文面は、呼んだ時点の言語 (Tr) で返す。未知の値は、値そのものを返す (表示から消さない)
std::string KindLabel(const std::string& changeType);
std::string AttributionLabel(const std::string& attribution);
// 帰属の区分の説明。Editor / 復元で確定した変更は、AI の「検証済み」と同じ文面にしない
std::string AttributionNote(const std::string& actor, const std::string& attribution);
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
    std::string attribution;   // Editor / 復元で確定した変更は、帰属の区分ではなく行為者の名前
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

// ---- シーンの変更 (M104m) ----

// 構造の変更の種類の文面 (要求 §7.2 の 11 種 + unparsed)。プレハブの 3 種 (本体 / インスタンスの参照元 / 上書き) は文面で区別する。
// 未知の値は値そのもの
std::string SceneKindLabel(const std::string& changeType);
// 種類の一覧 (11 種 + unparsed。文面の網羅の試験用)
const std::vector<std::string>& SceneChangeKinds();

// 値 (JSON の断片) を表示用に切る。改行・タブは空白にして、maxChars 文字 (UTF-8 の文字単位) を超えたら "..." を付ける
constexpr size_t kSceneValueMaxChars = 60;
std::string ClipValue(const std::string& fragment, size_t maxChars);

// scene_diff の 1 件の行
struct SceneChangeText {
    std::string kind;
    std::string target;    // 「名前 (#fileId)」。文書全体の変更は「(文書全体)」
    std::string component; // 空 = 無し
    std::string field;
    std::string before;    // 空 = 無し。大きすぎる値は「大きすぎる」の文面
    std::string after;
    std::string note;      // unparsed の理由
};
// entityName: ライブのシーンのエンティティ名 (分かるときだけ。空なら fileId だけ)
SceneChangeText FormatSceneChange(const TrackSceneChange& change, const std::string& entityName);
// unparsed の afterValue (理由の JSON 文字列) を平文にする。読めなければ元の文字列
std::string UnparsedReason(const std::string& afterValue);

// Hierarchy のバッジのツールチップ: 種類 + コンポーネント名・フィールド名を最大 3 つ。entity が null なら子孫の注記だけ
std::string OverlayBadgeTooltip(const OverlayEntity* entity, bool descendantOnly);

// ---- Inspector の印 (M104m2) ----

// Inspector のツールチップに出す値の長さ (UTF-8 の文字数)
constexpr size_t kInspectorValueMaxChars = 120;
// C# コンポーネントのツールチップに並べるフィールド名の最大数
constexpr size_t kInspectorMaxListedFields = 6;

// コンポーネントの印を引く (無ければ null)。name は ComponentRegistry の名前 (= シーン JSON のコンポーネントのキー)
const OverlayComponent* FindOverlayComponent(const OverlayEntity* entity, const char* componentName);
// フィールドの印を引く (無ければ null)。componentName / fieldName は JSON のキー
const OverlayField* FindOverlayField(const OverlayEntity* entity, const char* componentName, const char* fieldName);

// 変わったフィールドのツールチップ: 見出し (ファイルでの変更 / プレハブの上書きの変更) と、変更前・変更後の値 (字句のまま。長ければ切る)
std::string FieldMarkTooltip(const OverlayField& field);
// コンポーネントの印のツールチップ: 追加 / 変更。fieldsListed なら変わったフィールドの名前と変更前の値を並べる (C# コンポーネント用)
std::string ComponentMarkTooltip(const OverlayComponent& component, bool fieldsListed);

} // namespace mye
