#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/HotReload/ReloadHub.h" // BatchChange::Kind (変更の種別の語彙を 1 つに保つ)

namespace mye {

// git が working tree を書き換えたあと「エディタをどこまで作り直すか」の判定 (M66d)。
// spec §4.1「段階分類 (決定 10 の確定版)」の表がそのまま入っている。
//
// **全部が純関数**にしてある。段階を 1 段軽く判定すると
// 「絵は変わったのにシーンの参照だけ古い」という、画面を見ても気付けない壊れ方をする —
// つまり実機で確かめるのが最も難しい種類の誤りなので、表を単体で検査できる形に切り出す。

// 適用段階。**値の大小がそのまま「重さ」**で、混在は max を採る (spec §4.1)
enum class ApplyStage : uint8_t {
    A = 0, // その場で差し替え (ReloadHub の EndBatch だけ)
    B = 1, // 開いている文書を読み直す (LoadSceneFromPath / CompileScripts)
    C = 2, // エディタを再起動する
};

// 1 回の一括適用に載せてよい件数の上限 (spec §4.1、R2 で確定した初期値)。
// ★これを超える規模は「別ブランチへ移った」に等しく、1 件ずつ差し替えるより
//   再起動した方が速くて確実 (差し替え漏れの可能性も消える)
constexpr int kCollabMaxBatchApply = 200;

// 変更集合の 1 件。パスは **toplevel 相対・'/' 区切り** (git が返す形そのまま)
struct StageChange {
    std::string path;
    BatchChange::Kind kind = BatchChange::Kind::Modified;
    std::string oldPath; // Renamed のときの旧パス
    // `.meta` の guid が実行の前後で変わったか。呼び手 (GitTransaction) が
    // 実行前後のディスクを読んで埋める。
    // ★guid が変わる = シーンが持っている参照が全部別物を指す。差し替えでは直せない
    bool metaGuidChanged = false;
};

struct StageInputs {
    std::vector<StageChange> changes;
    // 今開いているシーンの toplevel 相対パス ('/' 区切り、空 = 無し / リポジトリ外)
    std::string activeScene;
    int maxBatchApply = kCollabMaxBatchApply;
};

// 重い方を返す
ApplyStage CombineStage(ApplyStage a, ApplyStage b);
// 1 件だけの判定 (集合の規則 — 件数上限と actor+scene 同居 — は Classify 側)
ApplyStage ClassifyChange(const StageChange& change, const std::string& activeScene);
// 変更集合 → 最も重い段階
ApplyStage Classify(const StageInputs& in);

// ---- 分類に使う述語 (どれも綴りだけを見る純関数) ----

// ReloadHub が「その場で差し替えられる」拡張子か (spec §4.1 の A の一覧)
bool IsReloadableAsset(const std::string& path);
// `assets/` 配下か
bool IsInsideAssets(const std::string& path);
// `assets/schemas/` 配下か (動的コンポーネント定義 = TypeId が動くので必ず C)
bool IsSchemaPath(const std::string& path);
// `src/GameLogic/Scripts/` 配下の C++ ソース (spec §2 の S3: `assets/scripts` ではない)
bool IsCppScript(const std::string& path);
// `.cs` (C# スクリプト)
bool IsCsScript(const std::string& path);
// `.meta` サイドカーそのものか
bool IsMetaSidecar(const std::string& path);

} // namespace mye
