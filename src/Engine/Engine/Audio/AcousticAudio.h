//====================================================================================
//                          AcousticAudio.h
//  MyEngine/ 秋田蓮音                                                      09/06/2026
//                                          リスナー場（Dial の 3 本目）と遮蔽・回折の整形
//====================================================================================
#pragma once
#include <cstdint>
#include <vector>

#include "Engine/Core/Components.h"
#include "Engine/Core/EntityID.h"
#include "Engine/Core/Random.h"
#include "Engine/Engine/Acoustic/AcousticField.h"
#include "Engine/Engine/Acoustic/AcousticGrid.h"
#include "Engine/Engine/Audio/AudioSystem.h"
#include "Engine/Engine/Audio/SpatialMath.h"

namespace mye {

// M68b: 一発再生の組み立てで参照するだけ。**SoundAsset.h を include しない** —
// あちらは nlohmann/json.hpp を引き込むので、音響のヘッダが JSON に依存してしまう
class SoundLibrary;

// 音響伝播 × 実オーディオ (M68)。**ここは全部「出力レーン」** — sim 状態は 1 バイトも
// 持たないし、World にも書き戻さない。AcousticField からは読むだけ (HasVolume / Grid /
// IsSolid / StaticSignature の 4 本しか触らない)。
//
// ★include の向きは **Engine/Audio → Engine/Acoustic の一方向**。逆向き
//   (AcousticField.h から AcousticAudio.h) を作ると「sim の場がオーディオを知っている」
//   形になり、決定論レーンの境界が型で守れなくなる。
//
// ★**Dial の 3 本目**。1 本目 = AcousticField::AdvanceWaveOneRing (波、増分)、
//   2 本目 = AcousticNav::BuildDistance (航法、毎 tick 全再計算)、3 本目 = ここ
//   (リスナー場、一気に完走)。3 本とも **同じ 26 近傍表・同じ重み・同じ「閉セルは
//   訪れない / 中間セルは見ない」規則**でなければならない — 見える波・敵が聞く波・
//   耳に届く音の減衰が同じ距離場から出ている、というのが M65+M68 の主張そのものだから。
//   AdvanceWaveOneRing に手を入れずに写しを増やしているのは、あちらが sim 状態
//   (WriteShell = 残光) を巻き込んでいて再利用できないため。

// 経路の分類。**log とセルフテストが読む唯一の観測点**なので値を動かさないこと
// (0..3 が summary の "classes D/T/O/B" の並びそのもの)
enum class AcousticPathClass : int32_t {
    Direct = 0,   // 直線とほぼ同じ長さで届く = 遮蔽なし
    Detour = 1,   // 回り込んで届く = 仮想発音位置 + 回折 LPF
    Occluded = 2, // 届かない (密閉 / 経路上限超え) = 一律の減衰
    Bypass = 3,   // 場が無い / リスナーがグリッド外 = **何もしない**
};

// log と Inspector に出す名前。表の並びは enum と 1:1
const char* AcousticPathClassName(AcousticPathClass c);

// リスナー場が確保してよいセル数の上限。超えたら probeMaxRing を半分ずつ下げて収める。
// ★262144 = 64^3 = 3 配列で ~1.3MB。既定ボリューム (52x6x52 = 16k) なら常に箱 =
//   グリッド全体なので、この予算が効くのは 256^3 級のグリッドだけ
inline constexpr int64_t kProbeCellBudget = 262144;

// 自由空間 (占有を無視) のチャンファ距離の閉形式。
// a >= b >= c を |dx|,|dy|,|dz| の降順として 11(a-b) + 16(b-c) + 19c。
// ★開放度の**分母**に使う。Dial を占有無しでもう 1 本回す代わりの純関数で、
//   「自由空間なら Dial と厳密に一致する」ことをセルフテスト T11 が固定する
uint32_t ChamferClosedForm(int32_t dx, int32_t dy, int32_t dz);

// リスナーを原点にした距離場 (= 「耳から見た世界の遠さ」)。
// ★波の場 (AcousticField::WaveField) を**そのまま流用**する。同じ箱・同じ dist・
//   同じ parentDir の意味なので、T2/T3 が「波の場と probe が一致する」を memcmp で
//   書ける (別の型にすると比較のために変換が要り、その変換自体がバグの置き場になる)。
struct AcousticProbe {
    AcousticField::WaveField waveField; // 箱 + dist + parentDir + バケット
    int32_t ox = 0, oy = 0, oz = 0;     // 原点セル (= リスナーの居るセル)
    int32_t maxRing = 0;                // 実際に使ったリング (予算で下がることがある)
    int32_t requestRing = 0;            // 要求値 (再構築契機の比較用)
    bool valid = false;                 // false = 場が無い / リスナーがグリッド外
    uint64_t signature = 0;             // AcousticField::StaticSignature() の写し
    AcousticGridDesc grid;              // 焼いたときのグリッド (SameGrid で比較)
    float openness = 0.0f;              // 開放度 0..1 (M68b の残響が読む)
    bool budgetWarned = false;          // 予算警告は 1 回だけ

    int64_t BoxCells() const
    {
        return static_cast<int64_t>(waveField.sx) * waveField.sy * waveField.sz;
    }
    // 箱の外と未到達は kUnreached。**グリッド座標**で引く (箱ローカルではない)
    uint16_t DistAt(int32_t cx, int32_t cy, int32_t cz) const;
    uint8_t ParentAt(int32_t cx, int32_t cy, int32_t cz) const;
};

// 整形の平滑化状態 (voice 1 本ぶん)。**コンポーネントには持たせない** —
// SourceState::vel と同じ「側テーブルに置く非決定論レーンの状態」
struct AcousticShapeState {
    bool valid = false; // false = 次の整形で目標へスナップする
    float gain = 1.0f;
    float lpf = 1.0f;
    AudioVec3 position;
};

// 1 回の整形の観測値 (log / セルフテスト用)。**整形の結果は全部ここに出る**ので、
// 耳を使わずに配管を検査できる
struct AcousticShapeInfo {
    AcousticPathClass cls = AcousticPathClass::Bypass;
    float dPath = -1.0f; // 経路長 [m]。Occluded / Bypass は -1
    float dLine = -1.0f; // セル中心間の直線 [m]。同上
    float dReal = 0.0f;  // 実座標間の直線 [m] (量子化の影響を受けない参照値)
    float lpf = 1.0f;    // 平滑化後の lpfCoefficient
    float gain = 1.0f;   // 平滑化後 + spatialBlend 込みの音量倍率
};

// Profiler と終了時 summary が読む統計。**POD** (窓は AudioSourceSystem に問うだけ)
struct AcousticAudioStats {
    uint64_t ticks = 0;      // 整形が走った tick 数
    int32_t rebuilds = 0;    // リスナー場を焼き直した回数
    int32_t boxCells = 0;    // 直近の箱のセル数
    float probeMsTotal = 0.0f; // 焼き直しの実測合計 [ms] (**run-to-run 比較から除く**)
    float probeMsLast = 0.0f;
    int32_t shaped = 0;      // 整形した voice / shot の延べ本数
    int32_t classCount[4] = {}; // Direct / Detour / Occluded / Bypass
    float openness = 0.0f;
    bool active = false;     // 有効な AcousticAudio と有効な probe があるか
    // ---- 鳴る波 (M68b) ----
    int32_t shots = 0;       // 実際に Play まで行った一発再生
    int32_t shotsSkipped = 0; // minWaveVolume 未満 / stream で捨てた
    int32_t shotsUnknownKey = 0; // tone → .sound.json が引けなかった
    int32_t shotsDropped = 0; // キュー上限 (kMaxPendingShots) 超過で捨てた
    // ★Play が無効ハンドルを返した回数 (クリップ未ロード / suspend / voice 枯渇)。
    //   これが 0 でないと「shots 本 Play を呼んだ」が「shots 本 voice が立った」に
    //   ならない — ログだけ緑で音が出ていない状態を機械で捕まえる唯一の口
    int32_t shotsPlayFailed = 0;
    float roomT = 0.0f;      // 平滑化後の部屋補間パラメータ (0 = 狭い / 1 = 広い)

    float ProbeMsAvg() const
    {
        return rebuilds > 0 ? probeMsTotal / static_cast<float>(rebuilds) : 0.0f;
    }
};

// 「この tick に生まれた波」を音として鳴らすための POD (M68b で push/drain する)。
// ★tick 側で積んでフレーム側で流す形にするのは、kMaxTicksPerFrame = 5 のフレームで
//   4 tick しか生きない衝撃波を取りこぼさないため。逆に drain をフレーム側に置くのは、
//   **その同じフレームで焼き直したリスナー場**で整形して鳴らせるから
struct PendingWaveShot {
    int32_t ox = 0, oy = 0, oz = 0;
    uint32_t tone = 0;
    float amplitude = 0.0f;
    uint32_t maxRing = 0;
    uint64_t bornTick = 0;
    EntityID source = kNullEntity;
    // ImpactSynth: 積む側 (ResolveWaveShotSound) が決めた鳴らす音。
    // soundKey = 名前キーのハッシュ (0 = AcousticAudio の tone マップに従う)。
    // mute = 1 なら鳴らさない (発音元が WaveSound を空で持っている)
    uint64_t soundKey = 0;
    uint8_t mute = 0;
};

class World;

// 波 1 本の「鳴らす音」を決めて shot へ書く (**規則はこの 1 本だけ**):
//   1. 発音元が生きていて WaveSoundComponent を持つ → その名前 (空文字 = mute)
//   2. materialHint (AcousticField::WaveSoundHint = 床材の PhysMat) の acousticSound が非空 → それ
//   3. どちらでもなければ soundKey = 0 (tone マップ)
// 呼ぶのは TickRunner の !resim ブロック (出力レーン)。World は読むだけ
void ResolveWaveShotSound(World& world, EntityID source, uint64_t materialHint,
                          PendingWaveShot& shot);

// リスナー場を必要なら焼き直す。戻り値 = 焼き直した (統計を進めるのは呼び出し側)。
//
// 再構築の契機は 4 つ + 1: probe が無効 / 原点セルが動いた / 静的署名が変わった /
// グリッドが変わった / **probeMaxRing が変わった** (Inspector で実行中に触れるため)。
// ★`!HasVolume()` またはリスナーがグリッド外なら valid = false にして**配列は保持**する
//   (部屋を出入りするたびに数 MB の確保を往復させない)
bool UpdateAcousticProbe(const AcousticField& field, const AcousticAudioComponent& comp,
                         AudioVec3 listenerPos, AcousticProbe& io);

// 音源 1 つぶんの spatial を「遮蔽・回折」で整形する **唯一の関数**。
// per-voice の毎フレーム更新と、波の一発再生 (M68b) の**両方がこの 1 本**を呼ぶ
// (MakeSourcePlay の「規則は必ずこの 1 本だけ」と同型 — 二重実装すると
//  「セルフテストが見ている規則と実際に鳴らしている規則が別物」になる)。
//
// io: 入出力。Direct/Bypass では position 以外に触れない。Detour は position を
//     **仮想発音位置**へ、dopplerScale を 0 へ書き替える (仮想位置は瞬間移動するので
//     ドップラーを載せるとピッチが飛ぶ)。lpfCoefficient は全 class で書く。
// gainOut: desc.volume に掛ける倍率 (spatialBlend でブレンド済み)。
// smooth: 非 null なら gain/lpf/position を半減期 smoothTicks で平滑化する。
//         null (一発再生) は目標をそのまま書く。
// dTicks: 前回整形からの tick 差 (最小 1)。**フレームではなく tick 基準**なのは
//         kVelocityHalfLifeTicks と同じ理由 (Runtime は数千 fps 回る)。
// info: 非 null なら観測値を書く (log / セルフテスト)。
//
// ★spec §4.1.2 の署名に対して **field と dTicks の 2 つを足してある**。
//   field は「音源セルが壁の中のときの 26 近傍 nudge」に IsSolid が要るため、
//   dTicks は平滑化の半減期が tick 基準だから。どちらも probe に隠し持たせるより
//   引数で受けるほうが依存が見える (probe に AcousticField* を持たせると寿命の罠になる)。
void ShapeAcousticSpatial(const AcousticField& field, const AcousticProbe& probe,
                          const AcousticAudioComponent& comp, AudioVec3 listenerPos,
                          AudioVec3 sourcePos, AudioSpatial& io, float& gainOut,
                          AcousticShapeState* smooth, float dTicks, AcousticShapeInfo* info);

// ---- 部屋の残響 (M68b) ----

// I3DL2 の 13 パラメータを線形補間する **純関数**。
// ★mB (Room / RoomHF / Reflections / Reverb) は既に対数域の整数なので、線形に混ぜると
//   dB が線形に動く = 耳には「連続に広くなる」と聞こえる。整数は float で混ぜてから四捨五入。
// ★端点は**厳密に**一致させる必要がある (t=0 で a、t=1 で b)。`a + (b-a)*t` は
//   t=1 で 1ulp ずれることがあるので、必ず `(1-t)*a + t*b` の形で書く —
//   ずれると「上書きは掛かっているのにプリセットと 1 ビット違う」というテストできない
//   状態が生まれる
AudioReverbParams LerpReverbParams(const AudioReverbParams& a, const AudioReverbParams& b, float t);

// 開放度 → 2 プリセット間の補間パラメータ (smoothstep)。
// ★段差なく変わることが企画の要求 (廊下と部屋で響きが**切り替わる**と、
//   境目で「カチッ」と鳴って世界が嘘になる)。smoothstep にしてあるのは端点で
//   微分が 0 になるから — 線形だと閾値をまたぐ瞬間に速度が不連続に見える
float RoomBlend(float openness, float openSmall, float openLarge);

// ---- 鳴る波 (M68b) ----

// 一発再生の組み立て結果。**Played 以外は 1 音も鳴らない** (summary が内訳を数える)
enum class WaveShotResult : int32_t {
    Played = 0,     // outDesc / outSpatial が埋まった (呼び出し側が Play する)
    BelowMin = 1,   // amplitude * waveVolume < minWaveVolume (呼吸などの微音)
    UnknownKey = 2, // tone / soundKey → .sound.json / 生クリップが引けなかった
    Stream = 3,     // BGM (stream) は一発再生の対象外
    Muted = 4,      // 発音元の WaveSound が空 = 意図して鳴らさない (ImpactSynth)
};

// 波 1 本 → 一発再生の PlayDesc + AudioSpatial を組み立てる **純関数**
// (`AudioSystem::Play` は呼ばない = デバイス無しのセルフテストが全部検査できる)。
//
// ★spatial は **波から**取る (音源コンポーネントを持たない音なので当然だが、要点は
//   `maxDistance = maxRing * cellSize` にすること)。RolloffGain は全カーブで
//   `d >= maxDistance` を厳密 0 にするので、これで「波が届く所でだけ聞こえる」が
//   減衰式の性質として保証される — 距離判定を別に書かない。
// ★rolloff の既定が 0 (Logarithmic) なのは spec S2。EnergyAt は**エネルギー**で
//   XAudio2 の volume は**振幅**なので、逆二乗をそのまま振幅に掛けると 10m で -52dB になる。
// ★rng は AudioSourceSystem 所有の Pcg32。**world.Rng() も audioScriptRng も使わない**
//   (前者は sim が壊れ、後者はスクリプトの一発再生の乱数列が動く)。
//
// field/probe/listenerPos は ShapeAcousticSpatial へそのまま渡す (遮蔽・回折は
// 一発再生も per-voice とまったく同じ 1 本を通る)。info は非 null なら log 用の観測値。
WaveShotResult MakeWaveShotPlay(const AcousticField& field, const AcousticProbe& probe,
                                const AcousticAudioComponent& comp, const PendingWaveShot& shot,
                                AudioVec3 listenerPos, const AudioSystem& audio,
                                const SoundLibrary& sounds, Pcg32& rng, PlayDesc& outDesc,
                                AudioSpatial& outSpatial, AcousticShapeInfo* info);

} // namespace mye
