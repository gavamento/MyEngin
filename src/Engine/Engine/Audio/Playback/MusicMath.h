#pragma once
#include <cmath>
#include <cstdint>

// BGM ストリーミング (M45f) の純関数だけを置くヘッダ。XAudio2 にもスレッドにもファイル I/O にも
// 触れないので、ヘッドレス selftest がリング演算・ループ点・クロスフェードの規則をそのまま
// 検証できる (Audio/SpatialMath.h・Audio/VoicePolicy.h と同じ流儀)。
//
// **ここは決定論レーンの外** — sim はこの結果を一切読まない。

namespace mye {

// リングは「1 ブロック = kMusicBlockMs ミリ秒」× kMusicRingBlocks 枚。
// 4 × 0.2 秒 = 0.8 秒ぶん先読みするので、メインスレッドが 0.8 秒止まっても音が途切れない
// (C++ スクリプトの MSBuild リビルドやシーンロードで実際に止まる。これがワーカースレッドを
//  1 本持つ理由そのもの)。XAUDIO2_MAX_QUEUED_BUFFERS は 64 なので 4 枚は十分に余裕がある。
inline constexpr int kMusicRingBlocks = 4;
inline constexpr int kMusicBlockMs = 200;

// ループ区間 (フレーム単位・end は排他)
struct MusicLoop {
    int64_t start = 0;
    int64_t end = 0;
};

// .sound.json の loopStartSample / loopEndSample を実フレーム数へ正規化する。
// end <= start は「末尾まで」の意味。壊れた値 (負 / 総フレーム超過 / 逆転) でも
// 無音や 0 長ループにならないよう、必ず全体ループへ落とす
inline MusicLoop ResolveMusicLoop(int64_t loopStart, int64_t loopEnd, int64_t totalFrames)
{
    MusicLoop r;
    if (totalFrames <= 0) {
        return r; // 0..0 (呼び出し側は再生自体をあきらめる)
    }
    r.start = loopStart > 0 ? loopStart : 0;
    if (r.start >= totalFrames) {
        r.start = 0;
    }
    r.end = (loopEnd > 0 && loopEnd <= totalFrames) ? loopEnd : totalFrames;
    if (r.end <= r.start) {
        r.start = 0;
        r.end = totalFrames;
    }
    return r;
}

// cursor から want フレーム読むときに、ループ点 / 終端に当たるまでに読めるフレーム数。
// **戻り値 0 = ここでループ (looping) するか終端 (非 looping)**。
// cursor がループ区間の手前 (イントロ) にあっても正しい — loop.end に達して初めて
// loop.start へ巻き戻るので、「イントロ + ループ」の素材がそのまま鳴る
inline int64_t MusicChunkFrames(int64_t cursor, int64_t want, const MusicLoop& loop, bool looping,
                                int64_t totalFrames)
{
    if (want <= 0) {
        return 0;
    }
    const int64_t limit = looping ? loop.end : totalFrames;
    const int64_t avail = limit - cursor;
    if (avail <= 0) {
        return 0;
    }
    return avail < want ? avail : want;
}

// リングへ追加投入してよいブロック数。queued は GetState().BuffersQueued。
// **queued < blocks の間だけ**埋める — queued == blocks の状態で書くと、XAudio2 がまだ
// 読んでいるブロックを上書きしてノイズになる (リングの唯一の安全条件)
inline int MusicRefillCount(int queued, int blocks)
{
    const int q = queued > 0 ? queued : 0;
    return q < blocks ? blocks - q : 0;
}

// 次に書くブロック index (round-robin)。投入順が厳密に round-robin である限り、
// queued < blocks なら「次のブロック」は必ず再生済み = 上書きしてよい
inline int MusicNextBlock(int last, int blocks)
{
    return blocks > 0 ? (last + 1) % blocks : 0;
}

// クロスフェードの 2 ゲイン (旧曲 / 新曲)
struct MusicFade {
    float from = 0.0f;
    float to = 1.0f;
};

// クロスフェードのゲイン。**等パワー** (cos/sin) — 線形補間だと互いに無相関な 2 曲が
// 中間で約 -3dB へこんで「谷」として聞こえる。
// duration <= 0 は即時切替。elapsed >= duration で完了 (from=0 / to=1)。
// **elapsed は「開始からの絶対経過時間」** — 毎フレームゲインを掛け足す実装にすると
// 6500fps では丸め誤差が積もってフェード長が変わる
inline MusicFade MusicCrossfadeGains(double elapsed, double duration)
{
    MusicFade g;
    if (duration <= 0.0 || elapsed >= duration) {
        return g;
    }
    const double t = elapsed > 0.0 ? elapsed / duration : 0.0;
    constexpr double kHalfPi = 1.5707963267948966;
    g.from = static_cast<float>(std::cos(t * kHalfPi));
    g.to = static_cast<float>(std::sin(t * kHalfPi));
    return g;
}

// 「今ゲイン gain の曲を、これからフェードアウトさせる」ときに fadeElapsed へ入れる値。
// ★フェード中の曲をさらに別の曲で上書きしたとき、経過 0 から始めるとゲインが 1.0 へ
//   飛び上がって**プツッとクリックノイズが出る**。等パワーの逆関数で「今のゲインに
//   相当する経過時間」から継ぎ足すことで連続にする
inline double MusicFadeOutElapsedFor(float gain, double duration)
{
    if (duration <= 0.0) {
        return 0.0;
    }
    const double g = gain < 0.0f ? 0.0 : (gain > 1.0f ? 1.0 : static_cast<double>(gain));
    constexpr double kHalfPi = 1.5707963267948966;
    return (std::acos(g) / kHalfPi) * duration;
}

} // namespace mye
