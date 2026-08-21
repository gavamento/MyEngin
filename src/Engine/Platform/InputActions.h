#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Platform/Input.h"

namespace mye {

// パッド軸の選択 (InputAxisDef::padAxis)。JSON では "LX" 等の名前で書く
enum class PadAxis : uint8_t { None, LX, LY, RX, RY, LT, RT, Count };

// ActionState() の戻りビット。M51h の ABI GetActionState と同じ値にする (決定台帳 4)
enum : uint32_t {
    kActionHeld     = 1u << 0, // 今 tick 押下中
    kActionPressed  = 1u << 1, // この tick で押された (prev 上げ / cur 下げ)
    kActionReleased = 1u << 2, // この tick で離された
};

// デジタルアクション 1 本。keys / pad / mouse のいずれかが押下なら down (OR 合成)
struct InputActionDef {
    std::string name;
    uint64_t nameHash = 0;     // HashStr(name)。M51h の GetActionState(hash) の鍵
    std::vector<uint8_t> keys; // VK コード列
    uint16_t padMask = 0;      // MyePadButton (XINPUT_GAMEPAD_*) の論理和
    uint8_t mouseMask = 0;     // InputSnapshot::mouseButtons と同ビット (bit0:L bit1:R bit2:M ...)
};

// アナログ軸 1 本。値 = clamp(キー成分 (pos - neg) + パッド成分, -1, +1)
struct InputAxisDef {
    std::string name;
    uint64_t nameHash = 0;
    uint8_t posKey = 0;             // VK (0 = 割り当てなし)
    uint8_t negKey = 0;
    PadAxis padAxis = PadAxis::None;
    float deadzone = 0.24f;         // 正規化後 [-1,1] に対する遮断幅 (0.24 ≒ XInput 既定)
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
    // だけ — 評価そのものが (cur, prev) の純関数なので、レーン化しても record/verify
    // 透過という根拠は 1 ミリも変わらない。
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
    // player 省略時はレーン 0 = 従来の単一入力 (既存の呼び出し側は 1 文字も変わらない)
    uint32_t ActionState(uint64_t nameHash, uint32_t player = 0) const; // kAction* の論理和
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
