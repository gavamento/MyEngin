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
// 競合の後始末 (中止 / 解決を完了 / ours / theirs) を止める理由だけを残す (M66g)。
// ★`MergeInProgress` は**除く** — それを解消するための操作なので、含めると
//   「競合中は競合を解決できない」という手詰まりになる。他の理由 (未保存 /
//   再生中 / ビルド中) は競合の解決でも同じように効かせる
std::vector<GateBlocker> BlockersForConflictOps(const std::vector<GateBlocker>& all);

// 実行中モーダルに回復案内を出し始めるまでの実時間 (M66k、spec §4.4)。
constexpr double kStuckHintSec = 15.0;

// 実行中モーダルに「エディタを終了して回復してください」を出すか (M66k)。
//
// なぜ「しきい値つき」か: 書き込み系にキャンセルは作らない (打ち切っても git は
// 走り続け、working tree が半端に書き換わったままエディタだけが『やめた』と思い込む)
// という v1 の決定は動かさない。一方でチームリポの `pre-commit` / `pre-push` hook は
// git が同期実行するので、**返らない git は想定内の事故**であり、そのとき
// Phase::Running のモーダルはボタンが 1 つも無い = エディタ全体が固まる。
// ただし常時案内すると 200 ms で終わる通常の commit でも毎回「終了して回復」が出て
// 警告として読まれなくなるので、`kStuckHintSec` を超えたときだけ出す。
// ★時間の分岐は目で確かめる手段が無いので純関数にしてセルフテストで固定する。
bool ShouldShowStuckHint(double elapsedSec);

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

    // ---- pull (M66f) ----
    // upstream から取り込む。checkout と同じ流れ (予測 → 確認 → 実行 → 段階 A/B/C) を
    // 通る — 降ってくるファイルの扱いは「どの git で降ってきたか」に依存しない。
    // ★予測の向きは `HEAD..@{u}` (fetch 済みの追跡ブランチとの差)。fetch していなければ
    //   予測は空になるが、実行後の names で必ず分類し直すので安全側
    void RequestPull();

    // ---- 競合 (M66g) ----
    // マージを中止して pull 前へ戻す / 解決済みのマージを 1 コミットで閉じる。
    // `known` = 「競合 + マージ済み」の集合 (SourceControlSession::ConflictChangeSet)。
    // ★これは**段階の事前予測**にしか使わない。実行後は必ず応答の names で分類し直す —
    //   予測に頼ると、外部ツールで解決された分を見落とす
    void RequestMergeAbort(std::vector<StageChange> known);
    void RequestMergeContinue(std::vector<StageChange> known);

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
        Pull,
        MergeAbort,    // M66g: マージの中止
        MergeContinue, // M66g: 解決を完了 (マージコミット)
    };

    // 変更集合を「git が返した names」から採る op か (= revert 以外)。
    // revert だけは実行前後のディスクを見て組む (選んだパスが正本)
    static bool IsTreeOp(OpKind k) { return k != OpKind::Revert; }

    enum class Phase {
        Idle,
        Predict, // checkout: diff_names の応答待ち (段階を出すため)
        Confirm, // 確認モーダル
        Running, // git 実行中 (応答待ち)
        Applying,// 応答が返った (次の OnImGui で後処理する)
        // M66g: 競合で止まった pull の後始末。**一括モードのまま** conflicts を聞き、
        // 「競合せずにマージ済みのファイル」だけを適用するために 1 往復挟む。
        // ★ここを飛ばして EndBatch({}) すると、競合しなかったファイル (相手の
        //   テクスチャなど) がディスクだけ新しくエディタに反映されないまま残る
        ConflictScan,
        Report,  // 結果 / エラーの表示
        Restart, // 段階 C: 再起動の確認
    };

    void BeginOp(EngineContext& ctx, SourceControlSession& scm);
    // 競合の後始末 (中止 / 完了) の共通の入口。既知の集合で段階を見積もって確認へ
    void BeginConflictOp(OpKind kind, std::vector<StageChange> known);
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
    // Phase::Running に入った時刻 (ImGui::GetTime() = 実時間の秒)。回復案内のしきい値用。
    // ★ImGui の時計を使う: git は DLL の worker が回すので**フレームは進み続ける**
    //   (モーダルが入力を止めているだけ)。sim は 1 バイトも触らないので決定論に無関係
    double runningSince_ = 0.0;

    // ---- checkout (M66e) / pull (M66f) ----
    std::string target_;                          // 切り替え先のブランチ名 (pull では空)
    bool predictSent_ = false;                    // diff_names を投げたか
    std::vector<StageChange> checkoutChanges_;    // 応答が返した実際の変更集合
    std::vector<std::string> reportPaths_;        // 失敗モーダルに出す一覧
    // pull がマージを許すか (非 ff で拒否された後、ユーザーが選んだときだけ true)。
    // ★既定を true にしない。黙ってマージコミットを作ると、後から見た人には
    //   「誰も押していないコミット」が履歴に残る
    bool allowMerge_ = false;
    // ---- 競合 (M66g) ----
    bool conflictScanSent_ = false; // conflicts を投げたか (ConflictScan で 1 回だけ)
    // マージ済みの適用が終わったら「競合しました」を出す (成功の後処理と
    // 失敗の報告が **1 回の pull で両方起きる** のはこの経路だけ)
    bool conflictReport_ = false;

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
