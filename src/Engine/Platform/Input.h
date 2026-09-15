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

// このフレームのゲーム面 (M75b)。記録するのは換算前のゲーム面 px と面の寸法だけで、
// キャンバスへの換算は sim 側の uilayout::CanvasOfInput / SurfaceToCanvas (描画側と同じ関数) がやる。
// ★ここでキャンバス座標へ正規化してはいけない — キャンバスは複数あり (M75c)
//   **倍率がキャンバスごとに違う**ので、「正規化済みの座標 1 個」は記録できない。
// w/h <= 0 は「未確定」の予約値で、CaptureSnapshot は面の寸法欄を 0 のままにする
struct InputSurface {
    int32_t w = 0; // ゲーム面の幅 (実 px。Runtime ではクライアント矩形 = バックバッファ)
    int32_t h = 0;
};

// ゲームがマウスのクリックを受け取ってよい範囲 (メインウィンドウのクライアント px、2026-09-14)。
// エディタの Game ビューの画像がこれ — エディタのマウス座標はエディタ全体のクライアント px なので、
// 範囲を持たないと停止ボタンやインスペクタのクリックまでゲームの「画面クリック」になる。
// Runtime は範囲を持たない (IEngineApp::GameMouseArea が false = クライアント全体)。
// w/h <= 0 は「どこも受け取らない」(Game ビューが見えていない)
struct InputRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t w = 0;
    int32_t h = 0;
    // 右端 / 下端は含まない (半開区間 = 隣り合う矩形で 1 px を二重に取らない)
    bool Contains(int32_t px, int32_t py) const
    {
        return w > 0 && h > 0 && px >= x && py >= y && px < x + w && py < y + h;
    }
};

// 1 tick 分の入力状態。リプレイ記録の最小単位 (spec 11.3)。
// - POD であること (このビットパターンがそのまま .rep に保存され、ハッシュされる)
// - レイアウトを変更すると過去のリプレイと互換が壊れるため、変更時は
//   ReplayFile のバージョンを上げること (M6)
struct InputSnapshot {
    uint8_t keys[32];      // VK コード 256bit ビットセット
    int32_t mouseX;        // クライアント座標 px
    int32_t mouseY;
    // 生マウスデルタ (M64a)。**WM_INPUT (Raw Input) の生カウント**であって
    // mouseX/mouseY の差分ではない。差分にしないのは、カーソルロック中は絶対座標が
    // 動かない (クライアント矩形の端や中央に張り付く) ため — 一人称の視点は
    // ロック中こそ回り続けなければならない。CaptureSnapshot が消費して 0 に戻し、
    // そのフレームで回った**最初の tick だけ**が読む (wheelDelta と同じ規約、PointerDeltaCarry)。
    // ★ポインタ加速の掛からない生カウントなので OS のマウス設定には依存しないが、
    //   **DPI は機種依存**。感度は必ずプロジェクト側の調整値で割ること
    int32_t mouseDeltaX;
    int32_t mouseDeltaY;
    int32_t wheelDelta;    // この tick へ渡す累積の生値 (WHEEL_DELTA=120 単位、PointerDeltaCarry)
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
    // ---- ゲーム面 (M75b)。**レーン 0 だけが持つ** ----
    // なぜ入力に載せるのか: UI のヒットテストとフォーカスナビは sim レーンにあり、
    // 面の寸法は**ウィンドウの大きさという機種依存の値**。実解像度をその場で読ませると
    // 2 台/2 回の実行でズレるが、UIElement は kComponentNoHash なので
    // **ワールドハッシュには 1 ビットも出ない** = replay も desync 検出も助けてくれない
    // (「最悪の壊れ方」)。ここへ載せて .rep に記録すれば、再生は窓の大きさに依らず一致する。
    // 記録するのは**換算前のゲーム面 px** (InputSurface の解説)。
    // ★消費側は必ず ctx.Input() (レーン 0) を読み、uilayout::CanvasOfInput を通すこと。
    //   surfW/H == 0 は「まだ確定していない」(ヘッドレス / 旧い記録) で、読み手が基準解像度へ倒す
    float mouseSurfX;         // ゲーム面 px のマウス位置 (Runtime では float(mouseX))
    float mouseSurfY;
    int32_t surfW;            // ゲーム面の寸法 (実 px)
    int32_t surfH;
    // ---- 文字入力 (M75b、InputField 用)。**レーン 0 だけが持つ** ----
    // WM_CHAR の UTF-16 コード単位を、**それを消費する tick 1 本ぶん**だけ載せる
    // (Input::ConsumeChars の解説)。載るのは BMP の可視文字だけ — 制御文字 (<0x20 / 0x7F) と
    // サロゲートは捨てる。IME は非対応 (確定文字が WM_CHAR で来る分は載る)。
    // Backspace / Enter / 矢印は文字ではなく keys のエッジで読む。
    // ★chars[charCount..] は必ず 0 (.rep とハッシュに未初期化バイトを載せない)
    uint16_t chars[8];
    uint8_t charCount;        // 0..8。溢れた分は捨てる (60Hz で 1 tick 8 文字 = 480 字/秒)
    uint8_t pad3[7];          // 明示パディング

    bool KeyDown(uint8_t vk) const { return ((keys[vk >> 3] >> (vk & 7)) & 1) != 0; }
    bool MouseDown(int button) const { return ((mouseButtons >> button) & 1) != 0; }
    bool PadButton(uint16_t mask) const { return (padButtons & mask) != 0; }
};
// ★レイアウトを変えたら kReplayFileVersion / kSimSnapshotVersion / kNetProtoVersion を
// 同時に上げること (この 3 つがこのビット列をそのまま持ち回る)
static_assert(sizeof(InputSnapshot) == 112, "InputSnapshot layout is part of the replay format");

// Win32 メッセージを蓄積し、フレーム頭でスナップショットを確定する。
class Input {
public:
    // Win32Window の MsgHandler として登録する。消費はしない (常に false)
    bool HandleMessage(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam, int64_t& result);

    // フレーム頭 (spec 5.3 フェーズ 1) で呼ぶ。wheel 累積はここでリセットされる。
    //
    // M52g のレーン規約 (ローカルマルチプレイ):
    //   lane 0   … キーボード + マウス + XInput スロット 0
    //   lane n>0 … XInput スロット n **だけ**。キーボード/マウスは載せない
    // キーボードを 2 人で分割しないのは、割り当てがアクションマップ (プロジェクト共有の
    // 1 本) に無く、レーンごとの別マップを持つ設計は M52 の範囲外だから。
    // ★つまりローカル 2P には物理パッドが 2 本要る。パッド無しでレーンを動かす手段は
    //   検証用の合成入力 (SynthLaneInput / --synth-input) 側に寄せてある
    //
    // surface = このフレームのゲーム面 (M75b)。実解像度が sim へ入る口はこの 1 箇所。
    // 換算前の px と面の寸法を記録し、キャンバスへの換算は sim 側がやる (InputSurface の解説)。
    // レーン n>0 は面も文字も持たない (0 のまま) ので引数は無視される
    InputSnapshot CaptureSnapshot(uint32_t lane, const InputSurface& surface);

    // 文字キューの先頭 count 個を捨てる (M75b)。**tick が 1 本回った後に EngineLoop が呼ぶ**。
    // wheelDelta と違って CaptureSnapshot では消費しない — CaptureSnapshot はフレーム頭に
    // 毎フレーム呼ばれるので、そこで消費すると **tick の回らないフレーム (fps が 60 を超えると
    // 過半) に打った文字が消える**。count はそのフレームで写した charCount — 写した後に届いた
    // 文字 (フレーム途中のポンプ) を巻き込んで捨てないよう、全消去ではなく先頭だけ落とす
    void ConsumeChars(uint8_t count);

    // パッド振動を適用する (M51h、XInput パッド 0、値 0..1)。**出力レーン専用** —
    // sim から振動状態を読み返す API は作らない。実際の XInputSetState は
    // 量子化後の値が前回から変わったときだけ発行する (毎フレーム呼んで良い)
    void ApplyVibration(float left, float right);

    // 生マウスデルタの受け口を hwnd へ登録する (M64a)。ウィンドウ生成後に 1 回呼ぶ。
    // ★**RIDEV_NOLEGACY は付けない** — 従来の WM_MOUSEMOVE / ボタンメッセージを止めると
    //   ImGui と mouseX/mouseY (エディタのヒットテスト) が同時に死ぬ。生デルタは
    //   WM_INPUT で**追加で**受け取るだけにしてある
    void AttachRawInput(void* hwnd);

    // カーソルを矩形の中央へ固定して隠す / 解除する (M64a、中央固定は 2026-09-14)。
    // 矩形は area (エディタの Game ビュー) があればそれ、無ければクライアント全体。
    // **出力レーン専用** — ApplyVibration と同じ扱いで、sim から状態を読み返す口は作らない。
    // record/verify 中・フォーカス喪失中は呼び出し側が false を渡す (ゲートは EngineLoop)。
    // ロック中は毎フレーム呼ぶこと (ウィンドウ移動に追従するため矩形と中央を打ち直している)
    void ApplyCursorLock(void* hwnd, bool locked, const InputRect* area = nullptr);

    // area の外にあるマウスのボタンとホイールを捨てる (2026-09-14、エディタの Game ビュー用)。
    // キー・パッド・位置・生デルタは触らない — 視点 (生デルタ) は範囲に関係なく回ってよく、
    // 位置を消すと UI の hovered が「どこも指さない」値と区別できなくなる。
    // ★EngineLoop が CaptureSnapshot の**直後**に呼ぶ = .rep の記録と verify / synth の置換より前
    static void MaskMouseOutside(InputSnapshot& s, const InputRect& area);

private:
    void SetKey(uint8_t vk, bool down);

    uint8_t keys_[32] = {};
    int32_t mouseX_ = 0;
    int32_t mouseY_ = 0;
    int32_t wheelAccum_ = 0;
    int32_t mouseDeltaX_ = 0;   // M64a: WM_INPUT で積む生デルタ (CaptureSnapshot が消費)
    int32_t mouseDeltaY_ = 0;
    int32_t rawAbsX_ = 0;       // MOUSE_MOVE_ABSOLUTE 機 (RDP/タブレット) の前回絶対値
    int32_t rawAbsY_ = 0;
    bool rawAbsValid_ = false;  // 上の基準が有効か (初回とフォーカス喪失で落とす)
    bool cursorLocked_ = false; // ShowCursor の内部カウンタを二重に進めないための現状態
    uint8_t buttons_ = 0;
    uint16_t chars_[8] = {};    // M75b: WM_CHAR のキュー (ConsumeChars が先頭から捨てる)
    uint8_t charCount_ = 0;
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

// 生マウスデルタとホイールを「実際に回った tick」へ 1 回だけ渡す (2026-09-15)。
// CaptureSnapshot はフレーム頭に 1 回だけ呼ばれ、同じ ctx.inputs でそのフレームの tick を 0〜N 本回す。
// 量を素通しすると、
//   - tick の回らないフレームで動かした分が消える (fps が 60 を超えると過半。180Hz なら 3 回に 2 回)
//   - 1 フレームで 2 本以上回ると、同じ量が本数ぶん足される
// の両方が起きる。視点は「たまにがくがく」になり、感度も fps で変わる (三校で踏んだ)。
// 文字の ConsumeChars と同じ問題だが、こちらは量なので「持ち越して足す」で解く:
//   フレーム頭 (写した直後)  AddTo(ctx.inputs[0])          … 前のフレームで回らなかった分を足す
//   tick を 1 本回した後      ClearAfterTick(ctx.inputs[0]) … 同じフレームの次の tick へ渡さない
//   tick ループの後           EndFrame(ctx.inputs[0], drop) … 残っていれば次のフレームへ持ち越す
// ★ワールドに入るのは tick が読んだ値だけで、それは .rep に載る = 記録と照合の対称は崩れない
struct PointerDeltaCarry {
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel = 0;

    void AddTo(InputSnapshot& s) const;
    static void ClearAfterTick(InputSnapshot& s);
    // drop = 持ち越さずに捨てる (スクラブ中 / フォーカス喪失。Input の WM_KILLFOCUS と同じ理由で、
    // 止まっていた間や裏で動かした分を再開の 1 tick にまとめて入れない)
    void EndFrame(const InputSnapshot& s, bool drop);
};

} // namespace mye
