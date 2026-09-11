#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Engine/EngineLoop.h"

namespace mye {

class PlayModeController;
class TimeTravel;

// 分岐レーンの色 (M72c)。0xRRGGBBAA — EditorLinePass (SceneView のゴースト、M72e) と
// Timeline のレーン帯が**同じ表**を引く = 窓の中の色と画面の中の色が必ず一致する。
// ライブは themeColor::PlayAccent (再生中の意味色) で、この表は分岐 id 用
inline constexpr uint32_t kBranchLaneColors[8] = {
    0x40E0FFFFu, 0xFF60D0FFu, 0xA0FF60FFu, 0xFFB040FFu,
    0xC080FFFFu, 0x40FFC0FFu, 0xFF8080FFu, 0xE0E060FFu,
};
inline constexpr uint32_t BranchLaneColor(uint32_t branchId)
{
    return kBranchLaneColors[(branchId == 0 ? 0u : branchId - 1u) % 8u];
}

// タイムライン (M52e): Play 中のワールドを過去 tick へシークして観察する。
//
// ★この窓は**要求を出すだけ** — Restore と再シムは EngineLoop が tick 境界で行う。
//   ImGui の途中で世界を差し替えると、その後のウィンドウが破棄済み EntityID を掴む。
// ★スクラブすると自動でポーズする (見たい瞬間で世界が止まっていないと観察できない)。
//   再生を再開するとそこから**分岐**する。M72a から記録済みの未来は捨てられず
//   分岐レーンとして残り、この窓で切り替えて行き来できる。
// ★M73: 一時停止はホールド (tick 番号も止まる)。Pause / Resume / Step の規則は
//   PlayModeController が TimeTravel と束ねて 1 か所で決めるので、この窓のボタンは
//   Controller を呼ぶだけ (EndScrub / Hold をここで直接叩かない)。
class TimelineWindow {
public:
    bool open = false; // 既定は非表示 (Play しない限り中身が無いため)
    void OnImGui(EngineContext& ctx, PlayModeController& playMode);

private:
    // トランスポート (⏮ -30 -1 ⏸/▶ step +1 +30 ⏭) + 状態語 / tick / タイムコード (M73b)
    void DrawTransport(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode);
    // レーン帯 (ライブ + 分岐 + 入力 + 上書き) を tick 軸に並べて描く。現在 tick に縦線。
    // クリック / ドラッグでシーク、ホイールで ±1 (Shift で ±30) (M73b)
    void DrawLaneStrip(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode);
    // 分岐の一覧 (範囲 / ライブとの乖離 / 切替 / 削除)
    void DrawBranchTable(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode);
    // 入力の上書き (M72f): アクション / 軸を [from, from+len) の間押し続ける項目の編集
    void DrawOverrides(EngineContext& ctx, TimeTravel& tt);
    // フィールド差分 (M72g): 直前の RequestDiff の結果を表で
    void DrawDiff(EngineContext& ctx, TimeTravel& tt);
    // シーク要求のこの窓での唯一の入口。必ずポーズを伴い、ホールド中の現在 tick へは出さない
    void Seek(EngineContext& ctx, TimeTravel& tt, PlayModeController& playMode, uint64_t tick);
    // 入力帯の tick 単位キャッシュを増分更新する (毎フレーム 3600 entry を舐めない)
    void UpdateInputCache(const EngineContext& ctx, const TimeTravel& tt);

    int ovrSel_ = 0;      // 0..A-1 = アクション、A.. = 軸
    int ovrLane_ = 0;
    int ovrFrom_ = -1;    // < 0 = 追加した時点の現在 tick
    int ovrLen_ = 60;
    float ovrValue_ = 1.0f;

    // スライダーを掴んでいる間の表示位置。ドラッグ中は要求済みの目標を出しておかないと、
    // シークが 1 フレーム遅れて効くせいでつまみが手元へ戻ってしまう
    long long pendingPos_ = -1;
    // 帯のドラッグ中に直前に要求した tick (同じ tick を毎フレーム要求し直さない)。-1 = 掴んでいない
    long long dragTick_ = -1;

    // 入力帯 (M73b): [i] = tick (inputAnyFirst_ + i) でどれかのレーンにアクション押下があったか。
    // 末尾 entry の hashAfter を控えておき、レーン切替でライブの中身が入れ替わったら作り直す
    std::vector<uint8_t> inputAny_;
    uint64_t inputAnyFirst_ = 0;
    uint64_t inputAnyEnd_ = 0;
    uint64_t inputAnyLastHash_ = 0;

    // 差分の見出しを「新しい結果が来たフレーム」で自動展開するために控える直前の結果
    uint64_t diffSeenTick_ = 0;
    double diffSeenMs_ = -1.0;
};

} // namespace mye
