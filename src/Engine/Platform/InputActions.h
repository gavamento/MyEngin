#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Platform/Input.h"
#include "Shared/FeelMath.h"

namespace mye {

// パッド軸の選択 (InputAxisDef::padAxis)。JSON では "LX" 等の名前で書く
enum class PadAxis : uint8_t { None, LX, LY, RX, RY, LT, RT, Count };

// ActionState() の戻りビット。M51h の ABI GetActionState と同じ値にする (決定台帳 4)
enum : uint32_t {
    kActionHeld     = 1u << 0, // 今 tick 押下中
    kActionPressed  = 1u << 1, // この tick で押された (prev 上げ / cur 下げ)
    kActionReleased = 1u << 2, // この tick で離された
    // M94f: 先行受付の窓の中 (bufferTicks 以内に押され、まだ ConsumeBuffered されていない)。
    // bufferTicks が 0 のアクションには立たない = 既存のアクションの戻り値は変わらない
    kActionBuffered = 1u << 3,
};

// ---- 先行入力 (M94f、ADR-031) ----
// 先行受付を持てるアクションは定義順の先頭 32 本 (PlayerInputComponent のビットと同じ上限)
inline constexpr uint32_t kMaxBufferedActions = 32;
inline constexpr uint8_t kInputBufferNone = 0xFFu; // 窓の外 (押されていない / 消費した / 窓を過ぎた)

// レーン × アクションごとの「押されてからの tick 数」。**sim 状態** (Scene 保持、WorldHash の内容ゲート、
// SimSnapshot の Scene 節)。InputActions::UpdateBuffers だけが進め、ConsumeBuffered だけが消す
struct InputBufferState {
    uint8_t pressAge[kMaxPlayers][kMaxBufferedActions];
    InputBufferState() { Clear(); }
    void Clear()
    {
        for (auto& lane : pressAge) {
            for (uint8_t& a : lane) {
                a = kInputBufferNone;
            }
        }
    }
    // 窓の中のものが 1 つでもあるか (= ハッシュに節を畳むか)。先行受付を使わない間は常に偽
    bool HasHashableState() const
    {
        for (const auto& lane : pressAge) {
            for (uint8_t a : lane) {
                if (a != kInputBufferNone) {
                    return true;
                }
            }
        }
        return false;
    }
};

// デジタルアクション 1 本。keys / pad / mouse のいずれかが押下なら down (OR 合成)
struct InputActionDef {
    std::string name;
    uint64_t nameHash = 0;     // HashStr(name)。M51h の GetActionState(hash) の鍵
    std::vector<uint8_t> keys; // VK コード列
    uint16_t padMask = 0;      // MyePadButton (XINPUT_GAMEPAD_*) の論理和
    uint8_t mouseMask = 0;     // InputSnapshot::mouseButtons と同ビット (bit0:L bit1:R bit2:M ...)
    // M94f: 先行受付の窓 [tick]。押した tick から数えてこの tick 数のあいだ kActionBuffered が立つ。0 = 先行受付なし
    uint8_t bufferTicks = 0;
};
inline constexpr uint8_t kMaxInputBufferTicks = 120;

// アナログ軸 1 本。値 = clamp(キー成分 (pos - neg) + パッド成分, -1, +1)
struct InputAxisDef {
    std::string name;
    uint64_t nameHash = 0;
    uint8_t posKey = 0;             // VK (0 = 割り当てなし)
    uint8_t negKey = 0;
    PadAxis padAxis = PadAxis::None;
    float deadzone = 0.24f;         // 正規化後 [-1,1] に対する遮断幅 (0.24 ≒ XInput 既定)
    // M94f: パッド成分の外側の遮断 (|v| >= 1 - outerDeadzone で 1 に張り付く) と応答カーブ (|v| → |v'|、符号は保つ)。
    // 既定 (0 と恒等カーブ) なら評価を飛ばす = 従来の値と 1 ビットも違わない。キー成分 (±1) には掛けない
    float outerDeadzone = 0.0f;
    MyeCurve response = MyeMakeCurve(MYE_EASE_LINEAR, 0.0f, 1.0f);
};

// 入力アクションマッピング (M51d、決定台帳 4)。
// assets\input\actions.json を実行時マップに読み、毎 tick Evaluate で状態を確定する。
//
// 決定論の根拠: Evaluate は (cur, prev) 2 枚の**記録済み InputSnapshot の純関数** —
// ライブデバイスや時刻を一切読まないため、record/verify では記録入力から同じ状態列が
// 再現される。マップ定義 (JSON) はアセットと同じ扱いで、verify は record 時と同内容の
// actions.json で行うのが前提 (モデル等の他アセットと同じ規約)。
// ファイル不在 = 空マップ = 完全 no-op、破損 = WARN + 空マップ (エンジンは継続)。
class InputActions {
public:
    // <assetsRoot>\input\actions.json を読む。冪等 (同 root の再呼出しは no-op)。
    // ProjectSettings の保存後は force = true で読み直す (保存ホットリロード)
    void Load(const std::wstring& assetsRoot, bool force = false);
    // 編集 UI の保存。input\ フォルダは無ければ作る。成功で true
    bool Save(const std::wstring& assetsRoot) const;

    // JSON 文字列から構築 (Load の実体。selftest の注入点)。
    // パース失敗は false + 空マップ。個別エントリの不備 (不明キー名など) は WARN + スキップ
    bool LoadFromJsonText(const std::string& text);
    std::string ToJsonText() const; // Save の実体 (LoadFromJsonText と往復可能)

    // 今 tick の状態を確定する。(cur, prev) 以外を読まないこと — record/verify 透過の根拠。
    // 呼出しは EngineLoop の tick 頭 (verify の入力置換の後)。tick 0 の prev はゼロ値。
    //
    // M52g: cur / prev は **kMaxPlayers 本のレーン配列**で、playerCount 本だけを評価する。
    // マップ定義 (actions.json) は全レーン共通で、レーン間で違うのは入力スナップショット
    // だけ — 評価そのものが (cur, prev) の純関数なので、レーン配列でも record/verify 透過。
    // ★playerCount 以降のレーンは**明示的にゼロへ落とす** (前回の残骸を残さない)。
    //   未接続レーンを読んだスクリプトが「前の tick の値」を拾うのが一番たちが悪い
    void Evaluate(const InputSnapshot* cur, const InputSnapshot* prev, uint32_t playerCount);
    // 単一レーンの糖衣 (レーンを意識しない呼び出し側とセルフテスト用)。
    // レーン 1 以降はゼロ入力で評価される = 「1 人ぶんしか無い」状態そのもの
    void Evaluate(const InputSnapshot& cur, const InputSnapshot& prev)
    {
        Evaluate(&cur, &prev, 1);
    }

    // 名前ハッシュ (HashStr) で引く。未定義は 0。線形走査 (アクションは高々数十本)。
    // player 省略時はレーン 0
    uint32_t ActionState(uint64_t nameHash, uint32_t player = 0) const; // kAction* の論理和

    // M94f: 先行入力。Evaluate の**直後**に 1 回呼び、state (sim 状態) を進めて kActionBuffered を立てる。
    // 呼ばない tick は窓が進まない (エディタの編集中)。決定論: (今 tick の評価結果, state) の純関数
    void UpdateBuffers(InputBufferState& state);
    // 窓の中なら消費して true (同じ押下で 2 回跳ばない)。この tick の kActionBuffered も下ろす
    bool ConsumeBuffered(uint64_t nameHash, uint32_t player, InputBufferState& state);
    // パッド成分 (内側の遮断の後) に外側の遮断と応答カーブを掛ける。Evaluate の実体を selftest から直接検査するために公開する
    static float ShapePadValue(const InputAxisDef& a, float v);
    float AxisValue(uint64_t nameHash, uint32_t player = 0) const;      // [-1, +1]

    // 編集 UI 用 (Project Settings)。書き換えたら Save → Load(force) で正規形に確定する。
    // 定義列を伸縮した場合、次の Evaluate までライブ状態 (At 系) は 0 を返す
    std::vector<InputActionDef>& Actions() { return actions_; }
    std::vector<InputAxisDef>& Axes() { return axes_; }
    const std::vector<InputActionDef>& Actions() const { return actions_; }
    const std::vector<InputAxisDef>& Axes() const { return axes_; }
    uint32_t ActionStateAt(size_t i, uint32_t player = 0) const; // ライブ表示用 (範囲外 = 0)
    float AxisValueAt(size_t i, uint32_t player = 0) const;

    // ---- 名前テーブル (VkNameTable.inl / MyePadButton) ----
    static const char* VkNameInTable(uint8_t vk);   // 未収載は nullptr
    static std::string VkNameStr(uint8_t vk);       // 未収載は "0xNN" (往復可能)
    static uint8_t VkFromName(std::string_view name); // 不明は 0 ("0xNN" 16 進名も受ける)
    static const char* PadButtonName(uint16_t singleBitMask); // 単一ビットの名前。不明は nullptr
    static uint16_t PadMaskFromName(std::string_view name);   // 不明は 0
    static const char* PadAxisName(PadAxis a);                // None は "None"
    static PadAxis PadAxisFromName(std::string_view name);    // 不明は None
    static const char* MouseButtonName(int bit);              // 0..4 = Left/Right/Middle/X1/X2
    static int MouseBitFromName(std::string_view name);       // 不明は -1

    // 正規化済み値 [-1,1] へのデッドゾーン適用 (残域を 0..1 に再スケール)。
    // Evaluate の実体を selftest から直接検査するために公開する
    static float ApplyDeadzone(float v, float deadzone);
    // アクション 1 本がスナップショット 1 枚で押下か (keys / pad / mouse の OR)。Evaluate の実体。
    // M73b: Timeline の入力帯が**過去 tick の記録済み entry** を見るために公開する
    // (ActionState 系は Evaluate 済みのライブ状態しか返さない)
    static bool ActionDown(const InputActionDef& a, const InputSnapshot& s);
    // 定義済みアクションのどれかが押されているか。定義が無ければ常に false
    bool AnyActionDown(const InputSnapshot& s) const;

private:
    std::vector<InputActionDef> actions_;
    std::vector<InputAxisDef> axes_;
    // Evaluate の結果。**レーン major** で並べる: [player * 定義数 + index]。
    // 長さは常に 定義数 * kMaxPlayers (playerCount で伸縮させない — 伸縮させると
    // 「レーンを増やした tick だけ添字がずれる」種類の事故が入る)。ロードで 0 クリア
    std::vector<uint8_t> actionBits_;
    std::vector<float> axisValues_;
    std::wstring loadedRoot_; // Load の冪等判定 (PhysicsLayerNames と同型)
};

} // namespace mye
