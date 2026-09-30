#pragma once
#include <cstdint>

#include <DirectXMath.h>

namespace mye {

// カメラ操縦モードの状態 (エディタ全体で 1 個。ComponentClipboard と同じ流儀)。
//
// Inspector の Camera コンポーネントにあるボタンが立て、SceneView が消費する。
// 両者は互いを include しないので、間にこの小さなグローバルを置いている
// (Selection に相乗りさせないのは、ミニシーン編集 (M48k) が Selection の**中身ごと**
//  入れ替えるため — 編集モードへ出入りするたびに操縦対象が入れ替わってしまう)。
//
// ★保持するのは fileId だけ。EntityID を握るとシーンを読み直したあとに世代違いの
//   別実体を掴む。fileId でも「別シーンの同番」はありうるので、SceneView 側は毎フレーム
//   「その fileId が生きていて Camera を持っているか」まで確かめてから使う。
struct CameraPilotState {
    uint64_t fileId = 0; // 0 = 操縦していない

    bool Active() const { return fileId != 0; }
    void Stop() { fileId = 0; }
};

CameraPilotState& GetCameraPilot();

// ---- 操縦モードの姿勢更新 (純関数。CameraPilotSelfTest 対象) ----
constexpr float kPilotLookDegPerPixel = 0.25f; // エディタカメラの感度と同じ
// 真上/真下で裏返らない頭打ち。前方の y がこれを超えるピッチは捨てる
// (エディタカメラの ±89° と同じ効きを、角度へ分解しないまま作る)
constexpr float kPilotPitchLimit = 0.9998f;

// マウス移動量 (px) から新しい姿勢を返す。
//   ヨー = **ワールド上方向 (0,1,0)** まわり (地平が回らない)
//   ピッチ = **カメラ自身の右軸 (ローカル X)** まわり
// ★四元数を分解しないのが要点 — ロール (視線軸まわりの傾き) が保たれる。
//   yaw/pitch へ落として組み直す実装だとロールの受け皿が無く、操縦した瞬間に
//   水平へ戻る = ギズモで付けた傾きが黙って消える (静かなデータ損失)。
// 動かない入力 (両方 0 / ピッチが頭打ち) では rot をそのまま返す
DirectX::XMVECTOR PilotApplyLook(DirectX::FXMVECTOR rot, float dxPixels, float dyPixels);

} // namespace mye
