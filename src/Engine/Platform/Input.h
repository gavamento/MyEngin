#pragma once
#include <cstdint>

namespace mye {

// 入力レーンの上限 (M52g)。.rep の playerCount / EngineContext::inputs /
// TimeTravel のリング / CrashRing の .rep イメージ / PlayerInputComponent が
// この 1 個の定数を共有する。
// 4 なのは XInput のスロット数がそのまま上限だから — ローカル 2P はその部分集合で、
// ネット対戦 (M52h/i) も「2 台がそれぞれ 1 レーンを埋める」形でここに乗る。
// ★増やすと SimSnapshot の blob レイアウトと TimeTravel の 1 エントリ長が変わる
//   (kSimSnapshotVersion の bump が要る)
inline constexpr uint32_t kMaxPlayers = 4;

// 1 tick 分の入力状態。リプレイ記録の最小単位 (spec 11.3)。
// - POD であること (このビットパターンがそのまま .rep に保存され、ハッシュされる)
// - レイアウトを変更すると過去のリプレイと互換が壊れるため、変更時は
//   ReplayFile のバージョンを上げること (M6)
struct InputSnapshot {
    uint8_t keys[32];      // VK コード 256bit ビットセット
    int32_t mouseX;        // クライアント座標 px
    int32_t mouseY;
    int32_t wheelDelta;    // このフレームに累積した生値 (WHEEL_DELTA=120 単位)
    uint8_t mouseButtons;  // bit0:L bit1:R bit2:M bit3:X1 bit4:X2
    uint8_t pad[3];        // 明示パディング (未初期化バイト混入防止, spec 11.2-3)
    // ---- gamepad (XInput、M19)。record/verify では記録値が live poll を上書きするので透過 ----
    uint16_t padButtons;      // XINPUT_GAMEPAD_* ビットマスク (A/B/X/Y/DPad/LB/RB/Start/Back/...)
    uint8_t  padLeftTrigger;  // 0..255
    uint8_t  padRightTrigger; // 0..255
    int16_t  padLX;           // 左スティック X (-32768..32767)
    int16_t  padLY;           // 左スティック Y
    int16_t  padRX;           // 右スティック X
    int16_t  padRY;           // 右スティック Y
    uint8_t  padConnected;    // 0=未接続 1=接続
    uint8_t  pad2[3];         // 明示パディング

    bool KeyDown(uint8_t vk) const { return ((keys[vk >> 3] >> (vk & 7)) & 1) != 0; }
    bool MouseDown(int button) const { return ((mouseButtons >> button) & 1) != 0; }
    bool PadButton(uint16_t mask) const { return (padButtons & mask) != 0; }
};
static_assert(sizeof(InputSnapshot) == 64, "InputSnapshot layout is part of the replay format");

// Win32 メッセージを蓄積し、フレーム頭でスナップショットを確定する。
class Input {
public:
    // Win32Window の MsgHandler として登録する。消費はしない (常に false)
    bool HandleMessage(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam, int64_t& result);

    // フレーム頭 (spec 5.3 フェーズ 1) で呼ぶ。wheel 累積はここでリセットされる。
    //
    // M52g のレーン規約 (ローカルマルチプレイ):
    //   lane 0   … キーボード + マウス + XInput スロット 0 (従来と 1 バイトも変わらない)
    //   lane n>0 … XInput スロット n **だけ**。キーボード/マウスは載せない
    // キーボードを 2 人で分割しないのは、割り当てがアクションマップ (プロジェクト共有の
    // 1 本) に無く、レーンごとの別マップを持つ設計は M52 の範囲外だから。
    // ★つまりローカル 2P には物理パッドが 2 本要る。パッド無しでレーンを動かす手段は
    //   検証用の合成入力 (SynthLaneInput / --synth-input) 側に寄せてある
    InputSnapshot CaptureSnapshot(uint32_t lane = 0);

    // パッド振動を適用する (M51h、XInput パッド 0、値 0..1)。**出力レーン専用** —
    // sim から振動状態を読み返す API は作らない。実際の XInputSetState は
    // 量子化後の値が前回から変わったときだけ発行する (毎フレーム呼んで良い)
    void ApplyVibration(float left, float right);

private:
    void SetKey(uint8_t vk, bool down);

    uint8_t keys_[32] = {};
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    int32_t wheelAccum_ = 0;
    uint8_t buttons_ = 0;
    uint16_t lastVibLeft_ = 0;  // 最後に XInput へ送った量子化値 (重複送信の抑止)
    uint16_t lastVibRight_ = 0;
};

// 合成入力 (M52g、--synth-input)。**(tick, lane) だけの純関数** — ライブデバイスも
// 実時間も読まないので、同じ引数なら常に同じビット列を返す。
//
// なぜ要るか: レーンを足しただけでは replay_verify は何も証明しない。ヘッドレス実行の
// 実入力は全レーン恒常ゼロで、「レーン 1 がレーン 0 を読んでいる」ような配線ミスが
// 記録側と検証側で**対称に**起きてハッシュが一致してしまう。レーンごとに違う入力を
// 流し込み、それを PlayerInputComponent 経由でワールドハッシュに載せて初めて、
// 4 ペア目が配線を検査する試験になる (M49 の「probe は書き戻さないと被覆にならない」と同じ)。
//
// tick ごとに撹拌するのではなく **レーンごとに長さの違うブロックへ量子化してから**
// 撹拌する: 毎 tick 変えると pressed/released が全 tick で立ち、絵としても診断としても
// 読めなくなる (押しっぱなしの区間があるほうが実入力に近い)
InputSnapshot SynthLaneInput(uint64_t tick, uint32_t lane);

} // namespace mye
