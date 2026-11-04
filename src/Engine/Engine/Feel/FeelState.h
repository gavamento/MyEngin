//====================================================================================
//                          FeelState.h
//  MyEngin/ 秋田蓮音                                                       11/04/2026
//                                          再生中の手触り (M94c、ADR-031) の sim 状態と、描画専用の出力の評価
//====================================================================================
#pragma once

#include <cstdint>

#include "Shared/FeelMath.h"

namespace mye {

struct TimeControl;

// 同時に再生できる手触りの数。満杯で鳴らすと「残り時間が最短のもの」を置き換える
inline constexpr uint32_t kMaxFeelInstances = 16;
// 1 チャネルの持続の上限 (10 秒)。desc の *Ticks はここへクランプされる
inline constexpr int32_t kMaxFeelChannelTicks = 600;
// PlayFeel の戻り値: 時間効果だけで描画チャネルを持たない desc (止める対象が無い)
inline constexpr int32_t kFeelHandleTimeOnly = 0;
// PlayFeel の戻り値: 失敗 (プリセットが見つからない等)
inline constexpr int32_t kFeelHandleInvalid = -1;

// 再生中の 1 件。発火時に desc を**コピー**して持つ — 後でアセットを編集しても再生中の結果は変わらず、
// この状態だけで揺れ・振動・画面効果を再現できる (タイムトラベル / ロールバックで正しく巻き戻る)。
// 末尾に整列の詰め物があるので、ハッシュとスナップショットは生バイトでなく欄ごとに畳む / 書く
struct FeelInstance {
    uint32_t active = 0;      // 0 = 空きスロット (空きは全欄が既定値 = 正規形)
    uint32_t serial = 0;      // 発火の通し番号 (ハンドルの世代、値ノイズの種)
    int32_t ageTicks = 0;     // 発火した tick の末で 0、以後 tick ごとに +1
    float intensity = 1.0f;   // 描画チャネルの振幅に掛ける強さ
    uint64_t sourceKey = 0;   // プリセット名のハッシュ (0 = コードで組んだ desc)。モニタの表示用
    MyeFeelDesc desc;
};

// 再生中の手触りの表。Scene が持つ sim 状態で WorldHash 対象 (内容ゲート)・SimSnapshot の Scene 節に入る。
// 鳴らす (Play) のはスクリプト層 (フェーズ 3) と FeelTrigger、進める (Advance) のは tick 頭の 1 箇所だけ
class FeelState {
public:
    // desc を鳴らす。時間効果 (ヒットストップ / スロー) は time へ要求し、描画チャネルがあれば表に載せる。
    // 戻り値はハンドル (>0)、描画チャネルが無ければ kFeelHandleTimeOnly。desc は正規化してから使う
    int32_t Play(const MyeFeelDesc& desc, float intensity, uint64_t sourceKey, TimeControl& time);
    // ハンドルの 1 件を止める。kFeelHandleInvalid (-1) なら全部止める。古いハンドルは無視する
    void Stop(int32_t handle);
    // tick 頭に 1 回: 経過を進め、全チャネルが終わったものを空ける
    void Advance();
    // シーン遷移・Play/Stop で全消去 (通し番号も 1 へ戻す)
    void Clear();

    // 何か鳴った後の状態か (= ハッシュに節を畳むか)。一度も鳴らしていない間は偽
    bool HasHashableState() const;
    uint32_t NextSerial() const { return nextSerial_; }
    void SetNextSerial(uint32_t s) { nextSerial_ = s; }
    const FeelInstance& Slot(uint32_t i) const { return slots_[i]; }
    FeelInstance& Slot(uint32_t i) { return slots_[i]; }
    uint32_t ActiveCount() const;

private:
    FeelInstance slots_[kMaxFeelInstances] = {};
    uint32_t nextSerial_ = 1;
};

// desc の正規化 (tick のクランプ、非有限値の 0 化、カーブの制御点の整列)。
// FeelLibrary の読み込みと Play の入口の両方が通す
void SanitizeFeelDesc(MyeFeelDesc& desc);
void SanitizeFeelCurve(MyeCurve& curve);
// 描画チャネルの持続の最大 (= 表に載る tick 数)。0 なら時間効果だけ
int32_t FeelPresentationTicks(const MyeFeelDesc& desc);

// ---- 描画専用の出力 (状態と経過時間の純関数) ----

// 利用者の出力倍率 (アクセシビリティ、出力レーン)。sim は書くだけで読み返さない
enum : uint32_t {
    kFeelChannelCamera = 0, // 揺れ + キック
    kFeelChannelRumble = 1,
    kFeelChannelFlash = 2,
    kFeelChannelCount = 3,
};
struct FeelOutputScale {
    float scale[kFeelChannelCount] = { 1.0f, 1.0f, 1.0f };
};

struct FeelFrameOutput {
    bool cameraActive = false;
    float camPos[3] = { 0.0f, 0.0f, 0.0f };    // カメラ局所の平行移動 [m]
    float camRotDeg[3] = { 0.0f, 0.0f, 0.0f }; // pitch / yaw / roll [度]
    float fovDeg = 0.0f;                       // 画角の差分 [度]
    float rumbleLow = 0.0f;                    // [0,1]
    float rumbleHigh = 0.0f;
    bool flashActive = false;
    float vignette = 0.0f;
    float chromAberration = 0.0f;
    float exposure = 0.0f;   // 露出倍率への加算
    float saturation = 0.0f; // 彩度への加算
    float flashColor[3] = { 1.0f, 1.0f, 1.0f };
    float flashWeight = 0.0f; // 乗算フィルタを flashColor へ寄せる重み [0,1]
};

// 表の全件をフレームの時刻で評価する。interpAlpha は描画補間の係数 (1 = 最新 tick)。
// 最新 tick で発火した件は α に依らず経過 0 から始まる (押した瞬間に最大の反応を出す)。
// preview はエディタのプレビュー用の描画専用の表 (sim 状態ではない、EngineLoop 所有)。足し合わせて評価する
FeelFrameOutput EvaluateFeel(const FeelState& state, float interpAlpha, const FeelOutputScale& scale,
                             const FeelState* preview = nullptr);
// desc から時間効果 (ヒットストップ / スロー) を外す。描画専用のプレビュー表へ鳴らすときに使う
// (時間効果は sim を変えるので、録画・再シミュレーションに乗らない経路からは鳴らさない)
MyeFeelDesc PresentationOnly(const MyeFeelDesc& desc);

} // namespace mye
