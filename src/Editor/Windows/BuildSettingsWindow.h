#pragma once
#include <string>
#include <vector>

#include "Engine/Core/Localization.h"
#include "Engine/Engine/EngineLoop.h"

namespace mye {

// Build Settings ウィンドウ (engine_spec.md 9 章、M15 → M51j で段階化)。
// 「ビルド」1 ボタンで次の段を順に実行し、各段の結果を一覧表示する:
//   1) スクリプト再ビルド (C++ GameLogic + C# Roslyn。opt-out 可)
//   2) アセットクック温め (モデルの .mmdl を確保 — 起動済みセッションではほぼ即時)
//   3) パッケージコピー (Runtime.exe + GameLogic.dll + C# ホスト + assets\ + ブートシーン
//      + cache\cooked\ 同梱 + 封印マーカー .sealed — 移設先でクック再生が正しさを担保する)
//   4) DDS 一括クック (opt-in。パッケージ内の画像を .dds 化して元画像を除去)
//   5) zip 圧縮 (opt-in)
// スクリプトビルドと zip は子プロセスを毎フレームポーリングする (UI は固まらない)。
// それ以外はワンフレーム内で同期実行 (Release ビルド自体は VS / MSBuild で行う前提)
class BuildSettingsWindow {
public:
    bool open = false;
    void OnImGui(EngineContext& ctx);

    // M51j: CLI (--package <dir>) から GUI と同じパイプラインを開始する (検証/CI 用)。
    // 進行は OnImGui 冒頭の AdvancePipeline が毎フレーム回す (ウィンドウが閉じていても進む)
    void StartCliPackage(const std::wstring& outDir, bool dds, bool zip,
                         const std::string& bootScene = {});
    bool PipelineFinished() const { return stage_ == Stage::Done; }
    bool PipelineSucceeded() const; // Done かつ全段 OK (スキップは成功扱い)

    // ---- M66d: 書き込み系 git 操作のゲート ----
    // 子プロセス (スクリプトビルド / zip) が生きているか。
    // ★`StartGameLogicBuild` の呼び出し元は**この窓の Stage::Scripts 1 箇所だけ**
    //   (2026-09-03 に全文検索で確認)。Asset Browser の [Rebuild Scripts] は
    //   `AssetOps::RebuildGameLogic` = ShellExecuteW の fire-and-forget で
    //   **ハンドルを持たない**ため、そちらが走っているかはエディタから観測できない
    //   (spec の未決事項に対する回答: 同期ビルド経路は無い / 観測不能な経路が 1 本ある)
    bool IsRunning() const { return proc_ != nullptr; }
    // 段階パイプライン全体が進行中か (dist\ を書き換えている間 = git を通さない)
    bool IsPipelineRunning() const { return stage_ != Stage::Idle && stage_ != Stage::Done; }
    // GameLogic のビルドが走っている段 (bin\ と cache\ を書き換える)
    bool IsScriptBuildRunning() const
    {
        return (stage_ == Stage::Scripts && proc_ != nullptr) || stage_ == Stage::CompileCs;
    }

private:
    enum class Stage { Idle, Scripts, CompileCs, CookWarm, Copy, Dds, Zip, Done };
    struct StageResult {
        StrId name;
        bool ok = false;
        bool skipped = false;
        std::string detail; // 件数/パス等の技術情報 (ログと同じ扱いで非翻訳)
    };

    void AdvancePipeline(EngineContext& ctx);
    void FinishStage(StrId name, bool ok, bool skipped, std::string detail);
    bool StageCookWarm(EngineContext& ctx, std::string& detail);
    bool StageCopy(EngineContext& ctx, std::string& detail); // 旧 DoPackage + cooked 同梱
    bool StageDds(EngineContext& ctx, std::string& detail);

    char outputDir_[512] = {};
    std::string bootScene_ = "main.scene.json";
    bool init_ = false;
    bool bundleDotnet_ = true;   // .NET ランタイムを同梱して自己完結配布にする
    bool rebuildScripts_ = true; // 段 1 を実行するか
    bool ddsCook_ = false;       // 段 4 (opt-in)
    bool zipOutput_ = false;     // 段 5 (opt-in)

    Stage stage_ = Stage::Idle;
    std::vector<StageResult> results_;
    void* proc_ = nullptr; // Scripts/Zip の子プロセス (HANDLE)。ポーリングで回収
    std::wstring procLog_;
    std::string status_;
};

} // namespace mye
