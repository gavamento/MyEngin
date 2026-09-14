#include "Engine/Platform/Input.h"

#include <cstring>

#include <Windows.h>
#include <windowsx.h>

#include <Xinput.h>
// XInput9_1_0 は Win7+ で常在 (再頒布 DLL 不要)。Engine.lib 経由で exe のリンクに伝播する
#pragma comment(lib, "Xinput9_1_0.lib")

namespace mye {

void Input::SetKey(uint8_t vk, bool down)
{
    if (down) {
        keys_[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7));
    } else {
        keys_[vk >> 3] &= static_cast<uint8_t>(~(1u << (vk & 7)));
    }
}

bool Input::HandleMessage(void* hwnd, uint32_t msg, uint64_t wparam, int64_t lparam, int64_t& result)
{
    (void)hwnd;
    (void)result;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        SetKey(static_cast<uint8_t>(wparam & 0xFF), true);
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        SetKey(static_cast<uint8_t>(wparam & 0xFF), false);
        break;
    case WM_MOUSEMOVE:
        mouseX_ = GET_X_LPARAM(lparam);
        mouseY_ = GET_Y_LPARAM(lparam);
        break;
    case WM_INPUT: {
        // 生マウスデルタ (M64a)。AttachRawInput 済みのときだけ届く。
        // ★ここで mouseX_/mouseY_ は触らない — 絶対座標の正本は今も WM_MOUSEMOVE で、
        //   生デルタは**別レーン**として積む。混ぜるとカーソルロック中に
        //   「見えないカーソルの位置」で UI のヒットテストが動いてしまう
        RAWINPUT raw = {};
        UINT size = sizeof(raw);
        const HRAWINPUT h = reinterpret_cast<HRAWINPUT>(static_cast<intptr_t>(lparam));
        if (GetRawInputData(h, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER))
            == static_cast<UINT>(-1)) {
            break;
        }
        if (raw.header.dwType != RIM_TYPEMOUSE) {
            break;
        }
        const RAWMOUSE& m = raw.data.mouse;
        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            // リモートデスクトップ / タブレット / 一部の仮想機は正規化**絶対**座標を寄越す。
            // 前回値との差を取る。基準が無い初回は 0 (視点を跳ねさせない)
            if (rawAbsValid_) {
                mouseDeltaX_ += static_cast<int32_t>(m.lLastX) - rawAbsX_;
                mouseDeltaY_ += static_cast<int32_t>(m.lLastY) - rawAbsY_;
            }
            rawAbsX_ = static_cast<int32_t>(m.lLastX);
            rawAbsY_ = static_cast<int32_t>(m.lLastY);
            rawAbsValid_ = true;
        } else {
            mouseDeltaX_ += static_cast<int32_t>(m.lLastX);
            mouseDeltaY_ += static_cast<int32_t>(m.lLastY);
            rawAbsValid_ = false;
        }
        break;
    }
    case WM_LBUTTONDOWN: buttons_ |= 1u << 0; break;
    case WM_LBUTTONUP:   buttons_ &= static_cast<uint8_t>(~(1u << 0)); break;
    case WM_RBUTTONDOWN: buttons_ |= 1u << 1; break;
    case WM_RBUTTONUP:   buttons_ &= static_cast<uint8_t>(~(1u << 1)); break;
    case WM_MBUTTONDOWN: buttons_ |= 1u << 2; break;
    case WM_MBUTTONUP:   buttons_ &= static_cast<uint8_t>(~(1u << 2)); break;
    case WM_XBUTTONDOWN:
        buttons_ |= static_cast<uint8_t>((GET_XBUTTON_WPARAM(wparam) == XBUTTON1) ? (1u << 3) : (1u << 4));
        break;
    case WM_XBUTTONUP:
        buttons_ &= static_cast<uint8_t>(~((GET_XBUTTON_WPARAM(wparam) == XBUTTON1) ? (1u << 3) : (1u << 4)));
        break;
    case WM_MOUSEWHEEL:
        wheelAccum_ += GET_WHEEL_DELTA_WPARAM(wparam);
        break;
    case WM_CHAR: {
        // 文字入力 (M75b)。Win32Window のポンプの TranslateMessage が WM_KEYDOWN から作る
        // (窓は W 系で作ってあるので wparam は UTF-16 のコード単位 1 個)。v1 は BMP の可視文字だけ:
        //   - 制御文字 (<0x20 = Backspace / Tab / Enter / Ctrl+英字、0x7F = Ctrl+Backspace) は
        //     keys のエッジで読む側の仕事。文字としても積むと InputField で二重に効く
        //   - サロゲート (0xD800..0xDFFF) は対で来るが、片割れだけ積まれると InputField の
        //     String256 に壊れた UTF-16 が残るので両方捨てる (絵文字は非対応)
        // 溢れた分は捨てる (1 tick に 8 文字を超えて打つことは無い)
        const uint32_t cu = static_cast<uint32_t>(wparam & 0xFFFFu);
        const bool control = cu < 0x20u || cu == 0x7Fu;
        const bool surrogate = cu >= 0xD800u && cu <= 0xDFFFu;
        if (!control && !surrogate && charCount_ < sizeof(chars_) / sizeof(chars_[0])) {
            chars_[charCount_] = static_cast<uint16_t>(cu);
            ++charCount_;
        }
        break;
    }
    case WM_KILLFOCUS:
        // フォーカス喪失中の KEYUP は届かないため全解除 (キー押しっぱなし防止)
        memset(keys_, 0, sizeof(keys_));
        buttons_ = 0;
        // M64a: 溜まっていた生デルタも捨てる。裏で動かしたぶんが復帰した瞬間に
        // 1 tick でまとめて入ると視点が飛ぶ。絶対値モードの基準も無効化する
        mouseDeltaX_ = 0;
        mouseDeltaY_ = 0;
        rawAbsValid_ = false;
        break;
    default:
        break;
    }
    return false; // 消費しない (ImGui など他のハンドラにも流す)
}

InputSnapshot Input::CaptureSnapshot(uint32_t lane, const InputSurface& surface)
{
    InputSnapshot s = {};
    if (lane == 0) {
        // キーボード/マウスはレーン 0 だけが受け取る (Input.h のレーン規約)。
        // ★wheel の累積リセットもここでしかしない — レーンの数だけ呼ばれるので、
        //   どのレーンでも消費する作りにすると 2 人目以降でホイールが消える
        memcpy(s.keys, keys_, sizeof(s.keys));
        s.mouseX = mouseX_;
        s.mouseY = mouseY_;
        s.mouseDeltaX = mouseDeltaX_;
        s.mouseDeltaY = mouseDeltaY_;
        s.wheelDelta = wheelAccum_;
        s.mouseButtons = buttons_;
        wheelAccum_ = 0;
        mouseDeltaX_ = 0; // M64a: wheel と同じ「1 tick で消費」規約
        mouseDeltaY_ = 0;
        // M75b: 実解像度が sim へ入る唯一の口。記録するのは換算前のゲーム面 px と
        // 面の寸法で、キャンバスへの換算は sim 側 (uilayout::CanvasOfInput) がやる。
        // 位置は面が未確定でも書く (float にするだけ) — 未確定の読み手は基準解像度 + scale 1 へ倒れる。
        // 寸法が退化 (最小化など) なら 0 のまま = 「まだ確定していない」の予約値
        s.mouseSurfX = static_cast<float>(s.mouseX);
        s.mouseSurfY = static_cast<float>(s.mouseY);
        if (surface.w > 0 && surface.h > 0) {
            s.surfW = surface.w;
            s.surfH = surface.h;
        }
        // M75b: 文字は写すだけ。消費は tick が回った後の ConsumeChars (Input.h の解説)
        memcpy(s.chars, chars_, sizeof(s.chars));
        s.charCount = charCount_;
    }

    // gamepad (XInput、スロット = レーン番号)。verify 中は記録値が上書きするので透過 (spec 11.3)
    XINPUT_STATE xs = {};
    if (lane < kMaxPlayers && XInputGetState(lane, &xs) == ERROR_SUCCESS) {
        s.padConnected = 1;
        s.padButtons = xs.Gamepad.wButtons;
        s.padLeftTrigger = xs.Gamepad.bLeftTrigger;
        s.padRightTrigger = xs.Gamepad.bRightTrigger;
        s.padLX = xs.Gamepad.sThumbLX;
        s.padLY = xs.Gamepad.sThumbLY;
        s.padRX = xs.Gamepad.sThumbRX;
        s.padRY = xs.Gamepad.sThumbRY;
    }
    return s;
}

void Input::ConsumeChars(uint8_t count)
{
    const uint8_t n = (count < charCount_) ? count : charCount_;
    const uint8_t rest = static_cast<uint8_t>(charCount_ - n);
    // 写した後に届いた分を先頭へ詰め、空いた末尾は 0 に戻す
    // (次の CaptureSnapshot が chars[charCount..] == 0 の規約をそのまま写せるように)
    for (uint8_t i = 0; i < rest; ++i) {
        chars_[i] = chars_[n + i];
    }
    for (uint8_t i = rest; i < charCount_; ++i) {
        chars_[i] = 0;
    }
    charCount_ = rest;
}

void Input::ApplyVibration(float left, float right)
{
    const auto quantize = [](float v) -> uint16_t {
        const float c = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<uint16_t>(c * 65535.0f + 0.5f);
    };
    const uint16_t l = quantize(left);
    const uint16_t r = quantize(right);
    if (l == lastVibLeft_ && r == lastVibRight_) {
        return; // 値が変わらない限りドライバへ再送しない
    }
    lastVibLeft_ = l;
    lastVibRight_ = r;
    XINPUT_VIBRATION vib = {};
    vib.wLeftMotorSpeed = l;
    vib.wRightMotorSpeed = r;
    XInputSetState(0, &vib);
}

void Input::AttachRawInput(void* hwnd)
{
    if (hwnd == nullptr) {
        return; // ヘッドレス (--selftest 等) はウィンドウを作らない
    }
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01; // Generic Desktop Controls
    rid.usUsage = 0x02;     // Mouse
    // dwFlags = 0: 従来のマウスメッセージを**殺さない**。RIDEV_NOLEGACY を付けると
    // WM_MOUSEMOVE / WM_LBUTTONDOWN が止まり、ImGui もエディタのヒットテストも死ぬ。
    // RIDEV_INPUTSINK も付けない — 非フォアグラウンド時にまで視点を回す必要は無い
    rid.dwFlags = 0;
    rid.hwndTarget = static_cast<HWND>(hwnd);
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        // 失敗しても致命ではない (デルタが常に 0 になるだけ) ので続行する
        OutputDebugStringW(L"[input] RegisterRawInputDevices failed; mouse delta will be 0\n");
    }
}

void Input::ApplyCursorLock(void* hwnd, bool locked, const InputRect* area)
{
    const HWND h = static_cast<HWND>(hwnd);
    if (locked && h != nullptr) {
        // ★毎フレーム打ち直す。ClipCursor はスクリーン座標の矩形なので、
        //   ウィンドウを動かす / サイズを変えると前の矩形は無関係な場所に残る
        RECT rc = {};
        GetClientRect(h, &rc);
        if (area != nullptr && area->w > 0 && area->h > 0) {
            // エディタは Game ビューの画像の中へ閉じ込める (クライアント全体だとパネルの上へ出る)
            rc = { area->x, area->y, area->x + area->w, area->y + area->h };
        }
        POINT tl = { rc.left, rc.top };
        POINT br = { rc.right, rc.bottom };
        ClientToScreen(h, &tl);
        ClientToScreen(h, &br);
        const RECT screenRect = { tl.x, tl.y, br.x, br.y };
        ClipCursor(&screenRect);
        // ★毎フレーム中央へ戻す (生デルタは WM_INPUT なので視点には効かない)。
        //   掴んだ瞬間だけ寄せる方式だと、見えないカーソルが矩形の中を流れて端に張り付く。
        //   エディタでは MaskMouseOutside が位置でクリックを選り分けるので、端にいると
        //   1 px の誤差で投擲のクリックが捨てられうる — 中央に置けば常に Game ビューの中
        SetCursorPos((tl.x + br.x) / 2, (tl.y + br.y) / 2);
    } else if (cursorLocked_) {
        ClipCursor(nullptr);
    }
    if (locked != cursorLocked_) {
        // ★ShowCursor は**内部カウンタ**。同じ向きに 2 回呼ぶとカーソルが戻らなくなる。
        //   状態が変わったときだけ 1 回呼ぶ (だから現状態を持っている)
        ShowCursor(locked ? FALSE : TRUE);
        cursorLocked_ = locked;
    }
}

void Input::MaskMouseOutside(InputSnapshot& s, const InputRect& area)
{
    if (area.Contains(s.mouseX, s.mouseY)) {
        return;
    }
    s.mouseButtons = 0;
    s.wheelDelta = 0;
}

namespace {

// SplitMix64 (整数四則とシフトのみ = /fp:precise 以前に浮動小数を触らない)。
// PCG32 (エンジンの sim 乱数) を使わないのは、合成入力が **sim の外**で作られる値で、
// ワールド RNG の列を 1 歩でも進めてはいけないため
uint64_t SplitMix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

} // namespace

InputSnapshot SynthLaneInput(uint64_t tick, uint32_t lane)
{
    InputSnapshot s = {};
    // レーンごとにブロック長を変える = 同じ tick でもレーン間で位相も内容も揃わない。
    // 「全レーンに同じ入力を配ってしまった」実装ミスがここで必ず値の差として出る
    const uint64_t block = 11ull + static_cast<uint64_t>(lane) * 5ull;
    const uint64_t h = SplitMix64((tick / block) * 4ull + lane + 1ull);

    const auto press = [&s](uint8_t vk) { s.keys[vk >> 3] |= static_cast<uint8_t>(1u << (vk & 7)); };
    // actions.json の既定マップ (MoveX = A/D、MoveY = W/S、Jump = Space) を叩く。
    // 左右/上下は排他にする — 同時押しは軸が 0 になるだけで動きが死ぬ
    if (h & 1u) {
        press('A');
    } else if (h & 2u) {
        press('D');
    }
    if (h & 4u) {
        press('W');
    } else if (h & 8u) {
        press('S');
    }
    if (((h >> 4) & 7u) == 0u) {
        press(VK_SPACE); // Jump は疎に (pressed/released のエッジを作るのが目的)
    }

    // パッド成分も埋める: アクションマップのデッドゾーン適用とパッドボタンの経路、
    // および「レーン n はパッド n を見る」という規約そのものを被覆に入れる
    s.padConnected = 1;
    s.padLX = static_cast<int16_t>((static_cast<int32_t>((h >> 8) & 0xFFFFu) - 32768) / 2);
    s.padLY = static_cast<int16_t>((static_cast<int32_t>((h >> 24) & 0xFFFFu) - 32768) / 2);
    s.padButtons = static_cast<uint16_t>((h >> 40) & 0x1000u); // A ボタン (= UINavSubmit)
    // M70c: D-Pad も疎に押す。これが無いと UI のフォーカス移動 (UINav*) が
    // replay で 1 度も動かず、「エンジンがフォーカスを持つ」配線が被覆から漏れる。
    // 4 方向のうち 1 つだけを押す (同時押しは意味論が曖昧になるので作らない) —
    // 8 通りのうち 4 通りが「どれも押さない」= ブロック境界でだけ押される疎な列になる
    const uint32_t dpad = static_cast<uint32_t>((h >> 53) & 7u);
    if (dpad < 4u) {
        s.padButtons |= static_cast<uint16_t>(1u << dpad); // DPadUp/Down/Left/Right
    }

    // ★マウスの**位置**は動かさない。動かすとエディタの GameView ヒットテストと UI が
    //   合成入力で誤爆し、「検証フラグを足した途端に UI が勝手に操作される」ことになる。
    // 一方 **生デルタ (M64a) は載せる** — どのヒットテストにも入らない純粋な視点入力で、
    // これを流して初めて新フィールドが .rep と SimSnapshot の往復照合に被覆される。
    //
    // ★**平均 0 になる作り方をすること**。`((h >> n) & 15) - 7` のような非対称な範囲だと
    //   1 カウントぶんの直流バイアスが残り、**デルタを積分する側 (視点角) がクランプに
    //   張り付く** — 実測で 1400 tick 後にピッチが上限 80 度に到達し、合成入力の実行が
    //   「ずっと真下を向いて歩くだけ」になった (診断としても絵としても読めない)。
    //   独立な 3bit を 2 本引いて差を取れば、範囲 [-7,+7] で平均が厳密に 0 になる
    const auto span3 = [h](int shift) { return static_cast<int32_t>((h >> shift) & 7u); };
    s.mouseDeltaX = span3(40) - span3(43);
    s.mouseDeltaY = span3(46) - span3(49);

    // ゲーム面 (M75b)。**基準解像度で固定する** — 合成入力は「(tick, lane) だけの
    // 純関数」なので、ここに実ウィンドウの寸法を混ぜたら決定論が壊れる。
    // レーン 0 だけが持つのは実キャプチャと同じ規約 (Input.h)。
    // ★マウス位置を動かさない規約は据え置きなので mouseSurfX/Y は 0 のまま。
    //   0 も正当な座標 (左上) で、被覆としては surfW/H が
    //   .rep / SimSnapshot の往復にレイアウトごと載ることに意味がある
    if (lane == 0) {
        s.surfW = 1920; // = uilayout::kCanvasRefW (Engine 層なので直接は引けない)
        s.surfH = 1080; // = uilayout::kCanvasRefH
        // 文字キュー (M75b)。37 tick に 1 回、固定の文字列から 1〜3 文字を順に載せる。
        // 狙いは .rep / SimSnapshot の prevTickInput / ネットのパケットに文字の列が実際に
        // 載ること (M75h の InputField が読むまでハッシュには出ない)。ブロック量子化の外に
        // 置くのは、文字には「押しっぱなし」の意味論が無いから (毎 tick 積むと連打になる)。
        // 可視 ASCII だけ = WM_CHAR の取り込み規則 (制御文字を捨てる) と揃える
        constexpr uint64_t kCharPeriod = 37;
        if (tick % kCharPeriod == 0u) {
            static constexpr char kText[] = "MyEngine uGUI 75b";
            const uint64_t len = sizeof(kText) - 1u;
            const uint64_t k = tick / kCharPeriod;
            const uint8_t count = static_cast<uint8_t>(1u + k % 3u);
            for (uint8_t i = 0; i < count; ++i) {
                s.chars[i] = static_cast<uint16_t>(static_cast<unsigned char>(kText[(k + i) % len]));
            }
            s.charCount = count;
        }
    }
    return s;
}

} // namespace mye
