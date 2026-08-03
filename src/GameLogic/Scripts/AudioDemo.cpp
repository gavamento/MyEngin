// v8 オーディオ API (M45g) のデモスクリプト。
// エンティティに付けて Play すると、キー入力で 3D 再生 / BGM クロスフェード /
// バス音量操作を一通り試せる。
//
//   1 / 2 : BGM を bgm_calm / bgm_drive へクロスフェード (fadeSeconds 秒)
//   3     : BGM 停止 (フェードアウト)
//   Space : 自分の位置で 3D ワンショット再生 (ハンドルを覚える)
//   X     : 直前のワンショットを 0.5 秒でフェードアウト停止
//   Q / E : SE バスの音量を下げ / 上げ (ミキサー窓のフェーダが動くのが見える)
//   R     : 自分に付いている AudioSource を鳴らし直す
//   T     : 3D リスナーを自分に固定する (以後この位置で聴こえる)
//
// orbitRadius > 0 なら自分を円軌道で動かす — 定位・減衰・ドップラーの確認用。
// **移動は sim 状態 (SetLocalPosition) なのでリプレイ対象**。音の側は全て非決定論レーンで、
// スクリプトは書くだけ (再生位置や再生中判定を読む API は存在しない)。
#include <math.h>

#include "Shared/ScriptAPI.h"

namespace {
// <Windows.h> を引き込まないため VK コードを直接定義
constexpr uint8_t kVkSpace = 0x20;
constexpr uint8_t kVk1 = 0x31;
constexpr uint8_t kVk2 = 0x32;
constexpr uint8_t kVk3 = 0x33;
constexpr uint8_t kVkE = 0x45;
constexpr uint8_t kVkQ = 0x51;
constexpr uint8_t kVkR = 0x52;
constexpr uint8_t kVkT = 0x54;
constexpr uint8_t kVkX = 0x58;

constexpr float kPi = 3.14159265358979323846f;

// 押した瞬間だけ true。押下状態は 1 つの int32 にビットで畳んである
// (登録フィールドは最大 16 個なので、キー毎に 1 フィールド持つと入らない)
bool Pressed(const MyeUpdateContext& ctx, uint8_t vk, int32_t bit, int32_t& prevBits)
{
    const bool down = ctx.api->KeyDown(ctx.api->engine, vk) != 0;
    const bool was = (prevBits & bit) != 0;
    prevBits = down ? (prevBits | bit) : (prevBits & ~bit);
    return down && !was;
}
} // namespace

struct AudioDemo : Script<AudioDemo> {
    // ---- Inspector で編集できる設定 ----
    float orbitRadius = 4.0f; // 0 以下で円軌道オフ
    float orbitSpeed = 1.2f;  // rad/s
    float fadeSeconds = 1.5f; // BGM クロスフェード長
    float seVolume = 1.0f;    // SE バスの現在音量 (Q/E で 0..1 を動かす)

    // ---- 内部状態 (sim = リプレイ対象) ----
    float angle = 0.0f;
    float centerX = 0.0f;
    float centerZ = 0.0f;
    int32_t initialized = 0;
    int32_t prevKeys = 0; // キー押下状態のビットマスク (エッジ検出用)
    // ★voice ハンドルは 64bit だが登録フィールドは 32bit 系しか無いので上下に割って持つ。
    //   採番は呼出時に予約される単調増加値なのでリプレイでも同じ値になる
    int32_t voiceLo = 0;
    int32_t voiceHi = 0;

    uint64_t Voice() const
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(voiceHi)) << 32)
               | static_cast<uint32_t>(voiceLo);
    }
    void SetVoice(uint64_t h)
    {
        voiceLo = static_cast<int32_t>(static_cast<uint32_t>(h & 0xFFFFFFFFull));
        voiceHi = static_cast<int32_t>(static_cast<uint32_t>(h >> 32));
    }

    void Start(MyeUpdateContext& ctx)
    {
        const MyeVec3 p = MyeSelf(ctx).GetLocalPosition();
        centerX = p.x;
        centerZ = p.z;
        initialized = 1;
        MyeLogf(ctx, "AudioDemo: 1/2=BGM 3=stop Space=play X=stop Q/E=SE bus R=source T=listener");
    }

    void Update(MyeUpdateContext& ctx)
    {
        if (!initialized) { // DLL リロード直後に Start より先に来た場合の保険
            Start(ctx);
        }

        // ---- 円軌道 (定位 / 減衰 / ドップラーの確認用) ----
        if (orbitRadius > 0.0f) {
            angle += orbitSpeed * ctx.dt;
            if (angle > 2.0f * kPi) {
                angle -= 2.0f * kPi;
            }
            MyeVec3 p = MyeSelf(ctx).GetLocalPosition();
            p.x = centerX + cosf(angle) * orbitRadius;
            p.z = centerZ + sinf(angle) * orbitRadius;
            MyeSelf(ctx).SetLocalPosition(p);
        }

        // ---- BGM (クロスフェード) ----
        if (Pressed(ctx, kVk1, 1 << 0, prevKeys)) {
            MyePlayMusic(ctx, "bgm_calm", fadeSeconds);
            MyeLogf(ctx, "music -> bgm_calm (%.1fs)", fadeSeconds);
        }
        if (Pressed(ctx, kVk2, 1 << 1, prevKeys)) {
            MyePlayMusic(ctx, "bgm_drive", fadeSeconds);
            MyeLogf(ctx, "music -> bgm_drive (%.1fs)", fadeSeconds);
        }
        if (Pressed(ctx, kVk3, 1 << 2, prevKeys)) {
            MyeStopMusic(ctx, fadeSeconds);
            MyeLogf(ctx, "music stop (%.1fs)", fadeSeconds);
        }

        // ---- 3D ワンショット ----
        if (Pressed(ctx, kVkSpace, 1 << 3, prevKeys)) {
            // 自分の位置で鳴らす。2D 設定の .sound.json でも 3D に載る
            SetVoice(MyePlaySoundHere(ctx, "beep"));
            MyeLogf(ctx, "one-shot at self (handle %llu)",
                    static_cast<unsigned long long>(Voice()));
        }
        if (Pressed(ctx, kVkX, 1 << 4, prevKeys)) {
            // 鳴り終わったハンドルへの指定は黙って無視される (エラーにならない)
            MyeStopVoice(ctx, Voice(), 0.5f);
            MyeLogf(ctx, "one-shot stop (fade 0.5s)");
        }

        // ---- バス音量 ----
        const bool down = Pressed(ctx, kVkQ, 1 << 5, prevKeys);
        const bool up = Pressed(ctx, kVkE, 1 << 6, prevKeys);
        if (down || up) {
            seVolume += up ? 0.1f : -0.1f;
            seVolume = seVolume < 0.0f ? 0.0f : (seVolume > 1.0f ? 1.0f : seVolume);
            MyeSetBusVolume(ctx, "SE", seVolume);
            MyeLogf(ctx, "bus SE volume = %.2f", seVolume);
        }

        // ---- AudioSource / リスナー ----
        if (Pressed(ctx, kVkR, 1 << 7, prevKeys)) {
            const int ok = ctx.api->PlayAudioSource(ctx.api->engine, ctx.self);
            MyeLogf(ctx, "PlayAudioSource -> %s", ok ? "ok" : "no AudioSource on self");
        }
        if (Pressed(ctx, kVkT, 1 << 8, prevKeys)) {
            ctx.api->SetListenerEntity(ctx.api->engine, ctx.self);
            MyeLogf(ctx, "listener fixed to self");
        }
    }
};
REGISTER_SCRIPT(AudioDemo, FIELDS(orbitRadius, orbitSpeed, fadeSeconds, seVolume, angle, centerX,
                                  centerZ, initialized, prevKeys, voiceLo, voiceHi));
