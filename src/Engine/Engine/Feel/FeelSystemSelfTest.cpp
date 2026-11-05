//====================================================================================
//                          FeelSystemSelfTest.cpp
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          再生中の手触り (FeelState、M94c) の回帰テスト実装
//====================================================================================
#include "Engine/Engine/Feel/FeelSystemSelfTest.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "Engine/Core/Diagnostics/Log.h"
#include "Engine/Engine/Feel/FeelState.h"
#include "Engine/Engine/Loop/GameFlow.h"
#include "Engine/Core/Ecs/World.h"
#include "Engine/Engine/Replay/WorldHasher.h"

namespace mye {

namespace {

MyeFeelDesc KickDesc(int32_t ticks, float z)
{
    MyeFeelDesc d;
    d.kickTicks = ticks;
    d.kickOffset.z = z;
    d.kickCurve = MyeMakeCurve(MYE_EASE_LINEAR, 1.0f, 0.0f);
    return d;
}

} // namespace

bool RunFeelSystemSelfTest()
{
    MYE_LOG_INFO("==== FeelSystem self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 時間効果だけの desc は表に載らない ----
    {
        FeelState fs;
        TimeControl tc;
        MyeFeelDesc d;
        d.hitStopTicks = 6;
        d.slowPercent = 40;
        d.slowHoldTicks = 10;
        const int32_t h = fs.Play(d, 3.0f, 0, tc);
        check(h == kFeelHandleTimeOnly && fs.ActiveCount() == 0, "a time-only feel returns kFeelHandleTimeOnly");
        check(tc.freezeTicks == 6 && tc.slowPercent == 40 && tc.slowHoldTicks == 10,
              "time effects are requested regardless of intensity");
        check(fs.HasHashableState() == false, "a time-only feel leaves the table in its default state");
    }

    // ---- 経過と満了 ----
    {
        FeelState fs;
        TimeControl tc;
        const int32_t h = fs.Play(KickDesc(10, 0.5f), 1.0f, 0x42ull, tc);
        check(h > 0 && fs.ActiveCount() == 1 && fs.Slot(0).ageTicks == 0, "a presentation feel occupies a slot at age 0");
        for (int i = 0; i < 9; ++i) {
            fs.Advance();
        }
        check(fs.ActiveCount() == 1 && fs.Slot(0).ageTicks == 9, "it stays for its duration");
        fs.Advance();
        check(fs.ActiveCount() == 0 && fs.Slot(0).serial == 0, "it expires at its duration and the slot is reset");
        check(fs.HasHashableState(), "the serial counter keeps the table hashable after a play");
    }

    // ---- 満杯時の置換とハンドル ----
    {
        FeelState fs;
        TimeControl tc;
        int32_t handles[kMaxFeelInstances] = {};
        for (uint32_t i = 0; i < kMaxFeelInstances; ++i) {
            handles[i] = fs.Play(KickDesc(100 + static_cast<int32_t>(i), 1.0f), 1.0f, 0, tc);
        }
        fs.Advance();
        // 残り時間: slot0 = 99、slot5 = 104 …。最短 (slot0) が置き換わる
        const int32_t h = fs.Play(KickDesc(50, 1.0f), 1.0f, 0, tc);
        check(fs.ActiveCount() == kMaxFeelInstances && fs.Slot(0).desc.kickTicks == 50 && (h & 0xF) == 0,
              "a full table replaces the instance with the least remaining time");
        fs.Stop(handles[0]);
        check(fs.Slot(0).active == 1, "a stale handle does not stop the replacement");
        fs.Stop(handles[3]);
        check(fs.Slot(3).active == 0 && fs.ActiveCount() == kMaxFeelInstances - 1, "a live handle stops its instance");
        fs.Stop(kFeelHandleInvalid);
        check(fs.ActiveCount() == 0, "Stop(-1) stops everything");
        fs.Clear();
        check(!fs.HasHashableState() && fs.NextSerial() == 1, "Clear returns to the default state");
    }

    // ---- 描画出力の評価 ----
    {
        FeelState fs;
        TimeControl tc;
        fs.Play(KickDesc(10, 0.5f), 2.0f, 0, tc);
        const FeelOutputScale full;
        FeelFrameOutput o = EvaluateFeel(fs, 0.0f, full);
        check(o.cameraActive && std::fabs(o.camPos[2] - 1.0f) < 1e-6f,
              "a feel played on the latest tick shows its full kick at any alpha (0.5 m x intensity 2)");
        fs.Advance();
        fs.Advance();
        fs.Advance();
        fs.Advance();
        fs.Advance(); // age 5
        o = EvaluateFeel(fs, 1.0f, full);
        check(std::fabs(o.camPos[2] - 0.5f) < 1e-6f, "a linear kick curve halves at half its duration");
        FeelOutputScale none;
        none.scale[kFeelChannelCamera] = 0.0f;
        o = EvaluateFeel(fs, 1.0f, none);
        check(!o.cameraActive && o.camPos[2] == 0.0f, "camera output scale 0 removes the kick");

        FeelState rs;
        MyeFeelDesc r1;
        r1.rumbleTicks = 20;
        r1.rumbleLow = 0.6f;
        r1.rumbleHigh = 0.1f;
        r1.rumbleCurve = MyeMakeCurve(MYE_EASE_LINEAR, 1.0f, 1.0f);
        MyeFeelDesc r2 = r1;
        r2.rumbleLow = 0.3f;
        r2.rumbleHigh = 0.8f;
        rs.Play(r1, 1.0f, 0, tc);
        rs.Play(r2, 1.0f, 0, tc);
        o = EvaluateFeel(rs, 1.0f, full);
        check(std::fabs(o.rumbleLow - 0.6f) < 1e-6f && std::fabs(o.rumbleHigh - 0.8f) < 1e-6f,
              "overlapping rumbles combine per motor with max");

        FeelState ss;
        MyeFeelDesc sh;
        sh.shakeTicks = 30;
        sh.shakePosAmp = 0.3f;
        sh.shakeRotAmpDeg = 2.0f;
        ss.Play(sh, 1.0f, 0, tc);
        ss.Advance();
        ss.Advance();
        const FeelFrameOutput a = EvaluateFeel(ss, 0.5f, full);
        const FeelFrameOutput b = EvaluateFeel(ss, 0.5f, full);
        check(a.cameraActive && std::memcmp(a.camPos, b.camPos, sizeof(a.camPos)) == 0
                  && std::memcmp(a.camRotDeg, b.camRotDeg, sizeof(a.camRotDeg)) == 0,
              "shake is a pure function of the state and alpha");
        bool bounded = true;
        for (int i = 0; i < 28; ++i) {
            const FeelFrameOutput q = EvaluateFeel(ss, 1.0f, full);
            for (int ax = 0; ax < 3; ++ax) {
                bounded = bounded && std::fabs(q.camPos[ax]) <= 0.3f + 1e-6f && std::fabs(q.camRotDeg[ax]) <= 2.0f + 1e-6f;
            }
            ss.Advance();
        }
        check(bounded, "shake stays within its amplitudes");

        FeelState fl;
        MyeFeelDesc f;
        f.flashTicks = 10;
        f.flashExposure = 1.0f;
        f.flashColor = { 1.0f, 0.0f, 0.0f, 0.5f };
        f.flashCurve = MyeMakeCurve(MYE_EASE_LINEAR, 1.0f, 1.0f);
        fl.Play(f, 1.0f, 0, tc);
        o = EvaluateFeel(fl, 1.0f, full);
        check(o.flashActive && std::fabs(o.exposure - 1.0f) < 1e-6f && std::fabs(o.flashWeight - 0.5f) < 1e-6f
                  && o.flashColor[0] == 1.0f && o.flashColor[1] == 0.0f,
              "flash pulses report exposure, colour and weight");
    }

    // ---- 正規化 ----
    {
        MyeFeelDesc d;
        d.shakeTicks = 100000;
        d.shakePosAmp = std::numeric_limits<float>::quiet_NaN();
        d.rumbleLow = 7.0f;
        d.slowRecoverEase = 999;
        d.flashCurve.mode = MYE_CURVE_CUSTOM;
        d.flashCurve.keyCount = 3;
        d.flashCurve.keys[0] = { 0.9f, 1.0f, 0.0f, 0.0f };
        d.flashCurve.keys[1] = { 0.1f, 2.0f, 0.0f, 0.0f };
        d.flashCurve.keys[2] = { 0.5f, 3.0f, 0.0f, 0.0f };
        d.flashCurve.keys[5] = { 0.3f, 9.0f, 0.0f, 0.0f }; // keyCount の外 = 既定値へ戻る
        SanitizeFeelDesc(d);
        check(d.shakeTicks == kMaxFeelChannelTicks && d.shakePosAmp == 0.0f && d.rumbleLow == 1.0f
                  && d.slowRecoverEase == MYE_EASE_LINEAR,
              "sanitize clamps ticks, zeroes NaN, clamps rumble and unknown eases");
        check(d.flashCurve.keys[0].t == 0.1f && d.flashCurve.keys[1].t == 0.5f && d.flashCurve.keys[2].t == 0.9f
                  && d.flashCurve.keys[5].value == 0.0f,
              "sanitize sorts curve keys by t and resets unused keys");
    }

    // ---- ワールドハッシュの内容ゲート ----
    {
        World w;
        TimeControl tc;
        FeelState fs;
        SimSources without;
        without.time = &tc;
        SimSources with = without;
        with.feel = &fs;
        check(HashWorld(w, without) == HashWorld(w, with), "an unused feel table folds nothing (pre-M94 hash)");
        fs.Play(KickDesc(3, 0.1f), 1.0f, 0, tc);
        const uint64_t playing = HashWorld(w, with);
        check(playing != HashWorld(w, without), "a playing feel is hash-covered");
        fs.Advance();
        check(HashWorld(w, with) != playing, "the age of a playing feel is hash-covered");
    }

    return failCount == 0;
}

} // namespace mye
