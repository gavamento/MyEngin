#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Engine/Engine/DebugDraw.h"
#include "Engine/Platform/Input.h"
#include "Shared/EngineAPI.h"

namespace mye {

class Scene;
class InputActions;

// パッド振動の目標値 (v12、M51h)。スロットはここへ書くだけで、実際の XInputSetState は
// EngineLoop がフレーム末 (出力レーン) に適用する — record/verify 中とフォーカス喪失中は
// 0 に落とし、終了時も 0 リセットする (オーディオの suspend と同型)
struct PadVibrationState {
    float left = 0.0f;  // 低周波モーター 0..1
    float right = 0.0f; // 高周波モーター 0..1
};

// スクリプトが tick 内で積むオーディオ操作 (M19 の再生イベントを v8 でタグ付きに拡張)。
// ハッシュ後に EngineLoop が drain して AudioSystem へ流す。
// **POD で持つ** — 毎 tick clear() されるので std::string を含めるとヒープが暴れる。
// 文字列キーは push 時に 64bit へ潰す (name-key ハッシュ or AssetID)。
enum class ScriptAudioOp : uint32_t {
    // key は「.sound.json の名前キー → その GUID → 生クリップ」の順で解決される
    // (ResolveSoundKey が唯一の実装)。volume/pitch はアセット既定への**乗算**
    PlayOneShot = 0, // key=sound, a=volume, b=pitch, handle=予約ハンドル (= voice の tag)
    PlayAtPoint,     // + pos (ワールド座標。2D 設定の音でも 3D に載せる)
    StopVoice,       // handle, a=fadeSeconds
    SetVoiceVolume,  // handle, a=volume
    SetVoicePitch,   // handle, b=pitch
    PlaySource,      // entity (AudioSource を持つエンティティ)
    StopSource,      // entity, a=fadeSeconds
    SetBusVolume,    // key=AudioSystem::HashBusName(バス名), a=volume
    PlayMusic,       // key=sound, a=fadeSeconds, i0=loop
    StopMusic,       // a=fadeSeconds
    SetListener,     // entity (null id = 自動 = AudioListener → primary カメラ)
};

struct ScriptAudioEvent {
    ScriptAudioOp op = ScriptAudioOp::PlayOneShot;
    uint64_t handle = 0; // 予約済み再生ハンドル (0 = 不要な op)
    uint64_t key = 0;    // clip の AssetID / name-key ハッシュ / バス名ハッシュ
    MyeEntityId entity = {};
    MyeVec3 pos = {};
    float a = 0.0f;  // op 依存: volume / fadeSeconds
    float b = 1.0f;  // op 依存: pitch
    int32_t i0 = 0;  // op 依存: loop フラグ等
};

// スクリプトが tick 内で積むエフェクト/汎用生成要求 (M32f、v7 Instantiate も共用)。
// tick 末 (ハッシュ前) に EngineLoop がプレハブをインスタンス化する。
// sim 状態なので record/verify とも実行される。
struct EffectSpawnRequest {
    std::string prefabKey; // .prefab.json、assets 相対 (省略サフィックス可)
    MyeVec3 pos = {};      // ルートの LocalTransform.position (parent 有効ならローカル)
    MyeEntityId parent = {};
    // v7 Instantiate (M37): 呼出時に予約したルート fileId (0 = PlayEffect = 予約なし)。
    // 予約は Scene::NextFileId() の消費 = 決定論点で行われ record/verify で同一列になる
    uint64_t reservedRootFid = 0;
};

// MyeEngineApi の engine 不透明ポインタが指すコンテキスト。
// C++ の ScriptHost と C# の ManagedHost が同じ C ABI テーブルを共有するために使う
// (Transform / Log / Input / Random は両ホストで同一実装)。
struct NetRuntimeInfo;

struct ScriptApiContext {
    Scene* scene = nullptr;
    InputSnapshot input = {};
    uint64_t tickIndex = 0;
    float dt = 1.0f / 60.0f;
    // v3 (M19) のスロット。EngineLoop が毎 tick セットする。null 時は該当 API が no-op。
    std::vector<ScriptAudioEvent>* audioQueue = nullptr; // PlaySound の積み先
    std::wstring* pendingScene = nullptr;                // LoadScene の書き先 (tick 末に消費)
    void* physics = nullptr;                             // Raycast 用 (M20 で PhysicsWorld*)
    std::vector<EffectSpawnRequest>* effectQueue = nullptr; // PlayEffect の積み先 (M32f、tick 末消費)
    std::vector<DebugLineCmd>* debugLines = nullptr; // DebugDrawLine の積み先 (v7。tick 頭クリア)
    // v8 (M45): 再生ハンドルの予約カウンタ。**採番はここ (push 側) で行う** —
    // drain はゲートされるがこのカウンタは常に進むので、スクリプトが受け取る値が
    // 記録/検証で一致する (v7 Instantiate の fileId 予約と同型)。null 時は 0 を返す
    uint64_t* audioHandleSeq = nullptr;
    // v12 (M51h): アクションマップ (EngineLoop 所有、tick 頭に評価済み)。null 時は状態 0
    const InputActions* inputActions = nullptr;
    // v12 (M51h): SaveGame/LoadGame のスロット要求の書き先 (-1 = なし、tick 末に消費)。
    // pendingScene と同じ「書くだけ」パターン。null 時は該当 API が no-op
    int* pendingSaveSlot = nullptr;
    int* pendingLoadSlot = nullptr;
    // v12 (M51h): パッド振動の目標値の書き先。適用は EngineLoop (出力レーン)
    PadVibrationState* padVibration = nullptr;
    // v13 (M52i): ネットセッションの状態 (EngineLoop が毎フレーム書く読み取り専用 POD)。
    // null = ネットを張っていない → Net* スロットは既定値を返す
    const NetRuntimeInfo* net = nullptr;
};

// out に MyeEngineApi (engine = ctx) を構築する。ctx の生存は呼び出し側が管理する。
void BuildEngineApi(MyeEngineApi& out, ScriptApiContext* ctx);

} // namespace mye
