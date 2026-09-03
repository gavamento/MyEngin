#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "Editor/SourceControl/SourceControlState.h"
#include "Editor/SourceControl/StageClassifier.h"
#include "Engine/Core/Localization.h" // StrId (阻害要因の文言)
#include "Engine/Core/Log.h"          // LogLevel (トーストの重み)

namespace mye {

struct EngineContext;

// working tree を書き換える git 操作の**入口を 1 本にする**仕組み (M66d)。
//
// なぜ要るか: pull / checkout / revert は「エディタが開いている文書の下から
// ファイルを差し替える」操作で、素直にやると
//   * 未保存の編集が黙って消える
//   * ホットリロードが半分だけ走って中間状態になる
//   * 再生中の sim が別のアセットを掴む
// のどれかを必ず踏む。だから
//   「実行してよい状態か (ゲート)」→「実行前処理」→「git」→「段階に応じた後処理」
// を 1 か所に閉じ込め、**この経路以外から書き込み系 op を投げない**という規約にする。

// ゲートの阻害要因 (spec §4.1)。**全件を列挙して返す** — 最初の 1 件で止めると
// 「保存したのにまだ押せない」を何度も繰り返すことになる
enum class GateBlocker : uint8_t {
    SceneDirty,           // シーン (またはミニシーン編集中のアセット) が未保存
    ActorEdit,            // ミニシーン編集モード中 (開いている文書がシーンではない)
    AnimationDirty,       // Animation 窓の編集が未保存
    ControllerDirty,      // Animator Controller 窓の編集が未保存
    MixerDirty,           // Audio Mixer 窓の編集が未保存
    ProjectSettingsDirty, // Project Settings の編集が未保存
    Playing,              // 再生中 / ポーズ中
    NetActive,            // ネットセッション中
    BuildRunning,         // Build Settings のパイプラインが進行中
    ScriptBuildRunning,   // GameLogic のスクリプトビルドが走っている
    OpInFlight,           // 別の書き込み系 op の応答待ち
    MergeInProgress,      // マージ / リベースの途中
    ServiceUnavailable,   // サービスが使えない (DLL 無し / リポジトリ外 / panic)
    Count,
};

// ComputeBlockers の入力。**bool だけ**にしてあるのは、判定そのものを純関数にして
// セルフテストから 13 種を 1 つずつ立てられるようにするため
struct GateInputs {
    bool sceneDirty = false;
    bool actorEdit = false;
    bool animationDirty = false;
    bool controllerDirty = false;
    bool mixerDirty = false;
    bool projectSettingsDirty = false;
    bool playing = false;
    bool netActive = false;
    bool buildRunning = false;
    bool scriptBuildRunning = false;
    bool opInFlight = false;
    bool mergeInProgress = false;
    bool serviceUnavailable = false;
};

// 立っている阻害要因を**列挙順**で全部返す。**純関数**
std::vector<GateBlocker> ComputeBlockers(const GateInputs& in);
// 阻害要因 → ツールチップの文言
StrId GateBlockerText(GateBlocker b);

// 4 窓の未保存判定 (直列化してディスクと比較する = 高い)。
// GitTransaction が 500 ms キャッシュする
struct DocumentDirty {
    bool animation = false;
    bool controller = false;
    bool mixer = false;
    bool projectSettings = false;
};

class GitTransaction {
public:
    // EditorApp が握っているものへの後処理の口。**GitTransaction に EngineContext 以外の
    // エディタ状態を持ち込まない**ため、必要なものだけ関数で受け取る
    struct Hooks {
        // シーンを開き直す (EditorApp::LoadSceneFromPath)。戻り値 = 実際に読めたか
        std::function<bool(const std::wstring&)> loadScene;
        // アクティブシーンがブランチ側で消えた -> 空シーンへ (EditorApp の NewScene 相当)
        std::function<void()> newScene;
        std::function<void(LogLevel, const std::string&)> toast;
        std::function<void()> compileCs;  // C# の再コンパイル
        // 自分を再起動して終了する (段階 C)。**成功したかを返すこと** —
        // 失敗を握り潰すとモーダルが閉じて「あとで」と同じ状態になる
        std::function<bool()> relaunch;
        // 今開いているシーンの絶対パス (空 = 無し)
        std::function<std::wstring()> activeScenePath;
    };
    void SetHooks(Hooks hooks) { hooks_ = std::move(hooks); }

    // 4 窓の直列化は毎フレームやらない (500 ms キャッシュ)。probe は
    // キャッシュが切れたときだけ呼ばれる
    using DirtyProbeFn = std::function<DocumentDirty()>;
    const DocumentDirty& CachedDirty(const DirtyProbeFn& probe);

    // 書き込み系 op を実行してよいか。blockers には立っている理由を全件入れる
    bool CanRunGitWriteOp(const GateInputs& in, std::vector<GateBlocker>& blockers) const;

    // ---- revert (M66d) ----
    // paths は toplevel 相対 '/' 区切り。untracked = そのうち「削除される」件数
    void RequestRevert(std::vector<std::string> paths, int untrackedCount);

    // ---- checkout (M66e) ----
    // ブランチを切り替える。**押した瞬間には何もしない** — まず
    // `diff_names(HEAD, target)` で「何が降ってくるか」を聞き、段階 (A/B/C) と
    // 件数を確認モーダルに出してから実行する (spec §4.1「実行前に予測して確認」)。
    // ★段階 C (再起動が要る) をユーザーが止められるのは**実行前だけ**なので、
    //   予測を挟まずに走らせてはいけない
    void RequestCheckout(std::string target);

    // 毎フレーム 1 回。応答の消化とモーダルの描画。
    // ★scm_.Poll() より**後**に呼ぶこと (応答は Poll の中でフラグになる)
    void OnImGui(EngineContext& ctx, SourceControlSession& scm);

    // 確認中 / 実行中 / 結果表示中。true の間は他の入力を止める
    bool Busy() const { return phase_ != Phase::Idle; }
    // 実行中 (git が走っている) — ボタンの表示を変えるため
    bool Running() const { return phase_ == Phase::Running; }

private:
    // どの op を通しているか (M66e で 2 種になった)。
    // ★`BeginOp` / `ApplyResult` / 後処理は op に依存しない。違うのは
    //   「変更集合をどう決めるか」だけ — revert は実行前後のディスク、
    //   checkout は git が返した `names`
    enum class OpKind : uint8_t {
        Revert,
        Checkout,
    };

    enum class Phase {
        Idle,
        Predict, // checkout: diff_names の応答待ち (段階を出すため)
        Confirm, // 確認モーダル
        Running, // git 実行中 (応答待ち)
        Applying,// 応答が返った (次の OnImGui で後処理する)
        Report,  // 結果 / エラーの表示
        Restart, // 段階 C: 再起動の確認
    };

    void BeginOp(EngineContext& ctx, SourceControlSession& scm);
    void ApplyResult(EngineContext& ctx, SourceControlSession& scm);
    // checkout の事前予測 (diff_names) を 1 回だけ投げる
    void SendPredict(SourceControlSession& scm);
    // 実行前後のディスクを見て変更集合を組む (kind と .meta の guid 変化)
    std::vector<StageChange> BuildChangeSet(EngineContext& ctx) const;
    // checkout: git が返した集合に「`.meta` の guid が変わったか」を足す。
    // ★guid の変化は**実行前に控えた値との比較でしか**分からない (spec §4.1 C 行)
    void ResolveMetaGuidChanges(EngineContext& ctx, std::vector<StageChange>& changes) const;
    // 「今開いている文書」の toplevel 相対パスを取り直す (段階 B の判定に効く)
    void UpdateActiveSceneRel();
    // 段階 A: 新しく現れた資産を AssetDatabase へ登録する (spec S5)
    void RegisterAdded(EngineContext& ctx, const std::vector<StageChange>& changes) const;
    // 段階 B: ライブラリのキャッシュを捨ててから開き直す
    void ApplyStageB(EngineContext& ctx, const std::vector<StageChange>& changes);
    std::wstring AbsolutePathOf(EngineContext& ctx, const std::string& rel) const;

    Hooks hooks_;
    Phase phase_ = Phase::Idle;
    OpKind op_ = OpKind::Revert;

    std::vector<std::string> paths_;   // 対象 (toplevel 相対)。checkout では予測された集合
    int untrackedCount_ = 0;
    ApplyStage predicted_ = ApplyStage::A;
    ApplyStage applied_ = ApplyStage::A;
    // 実行前のディスクの様子 (paths_ と同じ並び)
    std::vector<bool> existedBefore_;
    std::vector<uint64_t> metaGuidBefore_;
    std::string activeSceneRel_; // 「今開いているシーン」(予測時にも実行時にも取り直す)
    std::wstring projectRoot_;   // OnImGui で毎フレーム控える (予測は ctx を持たない)

    // ---- checkout (M66e) ----
    std::string target_;                          // 切り替え先のブランチ名
    bool predictSent_ = false;                    // diff_names を投げたか
    std::vector<StageChange> checkoutChanges_;    // 応答が返した実際の変更集合
    std::vector<std::string> reportPaths_;        // 失敗モーダルに出す一覧

    bool responseOk_ = false;
    std::string errorCode_;
    std::string errorDetail_;
    std::string reportText_;
    bool restartFailed_ = false; // 段階 C の再起動に失敗した (モーダルを閉じない)

    DocumentDirty dirty_;
    std::chrono::steady_clock::time_point dirtyAt_{};
    bool dirtyValid_ = false;
};

} // namespace mye
