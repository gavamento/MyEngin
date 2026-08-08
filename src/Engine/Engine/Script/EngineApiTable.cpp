#include "Engine/Engine/Script/EngineApiTable.h"

#include <cstddef>
#include <cstring>

#include "Engine/Core/Components.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/World.h"
#include "Engine/Engine/Audio/AudioSystem.h" // HashBusName (バス名ハッシュの規則は 1 本だけ)
#include "Engine/Engine/EffectSystem.h"
#include "Engine/Engine/GameObject.h"
#include "Engine/Engine/Parts.h" // v9 部位クエリ (M48h)
#include "Engine/Engine/Physics/PhysicsSystem.h"
#include "Engine/Engine/Scene.h"
#include "Engine/Engine/UI/UINav.h"      // v7 UIFocusNav (M37)
#include "Engine/Engine/UI/UIRenderer.h" // ResolveAnchor (基準解像度でのナビ矩形解決)
#include "Engine/Platform/PathUtil.h"

namespace mye {
namespace {

// Shared 側 POD とエンジン型のバイナリ互換 (DLL / CLR 境界の前提)
static_assert(sizeof(MyeEntityId) == sizeof(EntityID));
static_assert(offsetof(MyeEntityId, index) == offsetof(EntityID, index));
static_assert(offsetof(MyeEntityId, generation) == offsetof(EntityID, generation));

EntityID ToEngine(MyeEntityId id) { return { id.index, id.generation }; }
MyeEntityId ToShared(EntityID id) { return { id.index, id.generation }; }

ScriptApiContext* Ctx(void* engine) { return static_cast<ScriptApiContext*>(engine); }
Scene* Sc(void* engine) { return Ctx(engine)->scene; }

// v8 (M45): 再生ハンドルを 1 つ予約する。**採番は push 側 = 記録/検証でもゲートされない**
// ので、同じスクリプト呼出順なら常に同じ値が返る (v7 Instantiate の fileId 予約と同型)。
uint64_t ReserveAudioHandle(ScriptApiContext* c)
{
    if (c->audioHandleSeq == nullptr) {
        return 0; // キュー未接続 = 失敗
    }
    return ++(*c->audioHandleSeq);
}

// handle + float 1 つだけの op を積む (StopVoice / SetVoiceVolume / StopMusic)
void PushAudioOp(ScriptApiContext* c, ScriptAudioOp op, uint64_t handle, float a)
{
    if (c->audioQueue == nullptr) {
        return;
    }
    ScriptAudioEvent e;
    e.op = op;
    e.handle = handle;
    e.a = a;
    c->audioQueue->push_back(e);
}

} // namespace

// engine ポインタは常に ScriptApiContext*。キャプチャなしラムダ → 関数ポインタ変換で
// extern "C" スタイルのテーブルを埋める (C++ ScriptHost / C# ManagedHost が共用)
void BuildEngineApi(MyeEngineApi& out, ScriptApiContext* ctx)
{
    out = {};
    out.version = MYE_API_VERSION;
    out.engine = ctx;

    out.Log = [](void* engine, int level, const char* msg) {
        (void)engine;
        logging::Write(static_cast<LogLevel>(level & 3), "[script] %s", msg);
    };
    out.KeyDown = [](void* engine, uint8_t vk) -> int {
        return Ctx(engine)->input.KeyDown(vk) ? 1 : 0;
    };
    out.MouseButton = [](void* engine, int button) -> int {
        return Ctx(engine)->input.MouseDown(button) ? 1 : 0;
    };
    out.MousePos = [](void* engine, int32_t* x, int32_t* y) {
        if (x) { *x = Ctx(engine)->input.mouseX; }
        if (y) { *y = Ctx(engine)->input.mouseY; }
    };
    out.CreateGameObject = [](void* engine, const char* name) -> MyeEntityId {
        return ToShared(Sc(engine)->GetWorld().CreateEntity(name ? name : "GameObject"));
    };
    out.DestroyGameObject = [](void* engine, MyeEntityId id) {
        Sc(engine)->GetWorld().DestroyEntity(ToEngine(id));
    };
    out.IsAlive = [](void* engine, MyeEntityId id) -> int {
        return Sc(engine)->GetWorld().IsAlive(ToEngine(id)) ? 1 : 0;
    };
    out.FindByName = [](void* engine, const char* name) -> MyeEntityId {
        return ToShared(Sc(engine)->Find(name ? name : "").Id());
    };
    out.SetParent = [](void* engine, MyeEntityId child, MyeEntityId parent) {
        Sc(engine)->GetWorld().SetParent(ToEngine(child), ToEngine(parent));
    };
    out.GetLocalPosition = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !o) { return 0; }
        *o = { t->position.x, t->position.y, t->position.z };
        return 1;
    };
    out.SetLocalPosition = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->position = { v.x, v.y, v.z };
        return 1;
    };
    out.GetLocalRotation = [](void* engine, MyeEntityId id, MyeQuat* o) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !o) { return 0; }
        *o = { t->rotation.x, t->rotation.y, t->rotation.z, t->rotation.w };
        return 1;
    };
    out.SetLocalRotation = [](void* engine, MyeEntityId id, MyeQuat q) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->rotation = { q.x, q.y, q.z, q.w };
        return 1;
    };
    out.GetLocalScale = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t || !o) { return 0; }
        *o = { t->scale.x, t->scale.y, t->scale.z };
        return 1;
    };
    out.SetLocalScale = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* t = Sc(engine)->GetWorld().GetComponent<LocalTransform>(ToEngine(id));
        if (!t) { return 0; }
        t->scale = { v.x, v.y, v.z };
        return 1;
    };
    out.RandomFloat01 = [](void* engine) -> float {
        return Sc(engine)->GetWorld().Rng().NextFloat01();
    };
    out.RandomRange = [](void* engine, float lo, float hi) -> float {
        return Sc(engine)->GetWorld().Rng().Range(lo, hi);
    };
    out.AddComponentByName = [](void* engine, MyeEntityId id, const char* name) -> int {
        if (!name) { return 0; }
        const ComponentTypeId t = ComponentRegistry::Get().FindByName(name);
        if (t == kInvalidComponentType) { return 0; }
        return Sc(engine)->GetWorld().AddComponentRaw(ToEngine(id), t) ? 1 : 0;
    };
    out.SetMeshRenderer = [](void* engine, MyeEntityId id, const char* meshKey,
                             const char* materialKey) -> int {
        World& world = Sc(engine)->GetWorld();
        auto* mr = static_cast<MeshRendererComponent*>(
            world.AddComponentRaw(ToEngine(id), MeshRendererComponent::sTypeId));
        if (!mr) { return 0; }
        if (meshKey) { mr->mesh = AssetID{ HashStr(meshKey) }; }
        if (materialKey) { mr->material = AssetID{ HashStr(materialKey) }; }
        return 1;
    };

    // ---- gamepad (v3、M19)。context.input の gamepad フィールドから読む ----
    out.PadConnected = [](void* engine) -> int {
        return Ctx(engine)->input.padConnected ? 1 : 0;
    };
    out.PadButton = [](void* engine, uint16_t mask) -> int {
        return Ctx(engine)->input.PadButton(mask) ? 1 : 0;
    };
    out.PadSticks = [](void* engine, MyeVec2* left, MyeVec2* right) {
        const InputSnapshot& in = Ctx(engine)->input;
        constexpr float kInv = 1.0f / 32767.0f;
        if (left) { *left = { in.padLX * kInv, in.padLY * kInv }; }
        if (right) { *right = { in.padRX * kInv, in.padRY * kInv }; }
    };
    out.PadTriggers = [](void* engine, float* left, float* right) {
        const InputSnapshot& in = Ctx(engine)->input;
        if (left) { *left = in.padLeftTrigger / 255.0f; }
        if (right) { *right = in.padRightTrigger / 255.0f; }
    };

    // ---- 物理 Raycast (v3 で予約、M20 で実装)。ワールドの全コライダーに対する最近ヒット ----
    out.Raycast = [](void* engine, MyeVec3 origin, MyeVec3 dir, float maxDist,
                     MyeRaycastHit* outHit) -> int {
        return RaycastWorld(Sc(engine)->GetWorld(), origin, dir, maxDist, outHit);
    };

    // ---- オーディオ (v3 で予約、M19.3 で drain 実装)。tick 内で決定論順に積む ----
    out.PlaySound = [](void* engine, const char* soundKey, float volume) -> int {
        ScriptApiContext* c = Ctx(engine);
        if (!c->audioQueue || !soundKey) { return 0; }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::PlayOneShot;
        e.key = HashStr(soundKey);
        e.a = volume;
        e.handle = ReserveAudioHandle(c);
        c->audioQueue->push_back(e);
        // v3 の戻り値は int なので下位 31bit だけ返す。**tick を跨いで一意** になったので
        // 旧実装 (キュー index) の衝突バグは解消しているが、停止には v8 の
        // StopVoice(uint64) を使うこと (int へ潰すと 2^31 再生目以降で衝突しうる)
        return static_cast<int>(e.handle & 0x7FFFFFFFull);
    };
    out.StopSound = [](void* engine, int voice) {
        (void)engine;
        (void)voice; // v8 で非推奨。停止は StopVoice(uint64 ハンドル) を使う
    };

    // ---- シーン遷移 (v3 で予約、M19.4 で消費)。tick 末に遅延ロード ----
    out.LoadScene = [](void* engine, const char* scenePath) {
        auto* pending = Ctx(engine)->pendingScene;
        if (pending && scenePath) {
            *pending = Utf8ToWide(scenePath); // tick 末に EngineLoop が消費する (M19.4)
        }
    };

    // ---- 剛体操作 (v4、M28a)。velocity は hash 対象の sim 状態だが、スクリプト実行順は
    // 決定論なので直接更新してよい (SetLocalPosition と同格)。蓄積フィールドは持たない ----
    out.AddForce = [](void* engine, MyeEntityId id, MyeVec3 f) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb || rb->isKinematic) { return 0; }
        // 固定 tick (EngineLoop kFixedDt = 1/60) 前提の 1 tick 分加速。毎 tick 呼べば連続力
        constexpr float kFixedDt = 1.0f / 60.0f;
        const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
        const float s = kFixedDt / mass;
        rb->velocity.x += f.x * s;
        rb->velocity.y += f.y * s;
        rb->velocity.z += f.z * s;
        return 1;
    };
    out.AddImpulse = [](void* engine, MyeEntityId id, MyeVec3 imp) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb || rb->isKinematic) { return 0; }
        const float mass = (rb->mass > 0.0f) ? rb->mass : 1.0f;
        rb->velocity.x += imp.x / mass;
        rb->velocity.y += imp.y / mass;
        rb->velocity.z += imp.z / mass;
        return 1;
    };
    out.AddTorque = [](void* engine, MyeEntityId id, MyeVec3 torque) -> int {
        // ω += I⁻¹·τ·dt を即時適用 (M28b)。AddForce と同じ「毎 tick 呼べば連続トルク」方式
        constexpr float kFixedDt = 1.0f / 60.0f;
        return ApplyTorqueWorld(Sc(engine)->GetWorld(), ToEngine(id), torque, kFixedDt);
    };
    out.GetVelocity = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb || !o) { return 0; }
        *o = { rb->velocity.x, rb->velocity.y, rb->velocity.z };
        return 1;
    };
    out.SetVelocity = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* rb = Sc(engine)->GetWorld().GetComponent<RigidbodyComponent>(ToEngine(id));
        if (!rb) { return 0; }
        rb->velocity = { v.x, v.y, v.z };
        return 1;
    };

    // ---- 空間クエリ (v4 で予約、M28c で実装)。実装本体は PhysicsQueries.cpp ----
    out.OverlapSphere = [](void* engine, MyeVec3 center, float radius, MyeEntityId* outEntities,
                           int maxCount) -> int {
        return OverlapSphereWorld(Sc(engine)->GetWorld(), center, radius, outEntities, maxCount);
    };
    out.OverlapBox = [](void* engine, MyeVec3 center, MyeVec3 half, MyeQuat rot,
                        MyeEntityId* outEntities, int maxCount) -> int {
        return OverlapBoxWorld(Sc(engine)->GetWorld(), center, half, rot, outEntities, maxCount);
    };
    out.SphereCast = [](void* engine, MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                        MyeRaycastHit* outHit) -> int {
        return SphereCastWorld(Sc(engine)->GetWorld(), origin, dir, radius, maxDist, outHit);
    };

    // ---- キャラクターコントローラ (v5、M29b)。sim 入力フィールドへの直接書き込み
    //      (SetLocalPosition と同格 — スクリプト実行順は決定論なのでハッシュ安全) ----
    out.CharacterMove = [](void* engine, MyeEntityId id, MyeVec3 v) -> int {
        auto* cc =
            Sc(engine)->GetWorld().GetComponent<CharacterControllerComponent>(ToEngine(id));
        if (!cc) { return 0; }
        cc->moveInput = { v.x, v.y, v.z }; // y は物理側で無視される
        return 1;
    };
    out.CharacterJump = [](void* engine, MyeEntityId id, float speed) -> int {
        auto* cc =
            Sc(engine)->GetWorld().GetComponent<CharacterControllerComponent>(ToEngine(id));
        if (!cc) { return 0; }
        cc->jumpSpeed = speed;
        return 1;
    };
    out.CharacterIsGrounded = [](void* engine, MyeEntityId id) -> int {
        auto* cc =
            Sc(engine)->GetWorld().GetComponent<CharacterControllerComponent>(ToEngine(id));
        return (cc && cc->isGrounded != 0) ? 1 : 0;
    };
    out.CharacterGetVelocity = [](void* engine, MyeEntityId id, MyeVec3* o) -> int {
        auto* cc =
            Sc(engine)->GetWorld().GetComponent<CharacterControllerComponent>(ToEngine(id));
        if (!cc || !o) { return 0; }
        *o = { cc->velocity.x, cc->velocity.y, cc->velocity.z };
        return 1;
    };

    // ---- UI テキスト (v5 で予約、M29c で実装)。TextMesh は NoHash の描画状態なので
    //      スクリプトから毎 tick 書いても sim/リプレイに影響しない ----
    out.SetTextMeshText = [](void* engine, MyeEntityId id, const char* text) -> int {
        auto* tm = Sc(engine)->GetWorld().GetComponent<TextMeshComponent>(ToEngine(id));
        if (!tm || !text) { return 0; }
        size_t n = 0;
        while (text[n] != '\0' && n < sizeof(tm->text) - 1) {
            tm->text[n] = text[n];
            ++n;
        }
        tm->text[n] = '\0';
        return 1;
    };

    // ---- エフェクト制御 (v6、M32f)。sim 入力/状態フィールドへの直接書込 (SetLocalPosition と同格) ----
    out.EmitterBurst = [](void* engine, MyeEntityId id, int count) -> int {
        auto* em = Sc(engine)->GetWorld().GetComponent<ParticleEmitterComponent>(ToEngine(id));
        if (!em || count <= 0) { return 0; }
        em->pendingBurst += count; // 次の粒子 Update で放出、ParticleSystem が tick 末に 0 クリア
        return 1;
    };
    out.SetEmitterPlaying = [](void* engine, MyeEntityId id, int playing) -> int {
        auto* em = Sc(engine)->GetWorld().GetComponent<ParticleEmitterComponent>(ToEngine(id));
        if (!em) { return 0; }
        em->playing = playing ? 1 : 0;
        return 1;
    };
    out.RestartEffect = [](void* engine, MyeEntityId id) -> int {
        World& w = Sc(engine)->GetWorld();
        if (!w.GetComponent<EffectComponent>(ToEngine(id))) { return 0; }
        EffectSystem::RestartEffect(w, ToEngine(id));
        return 1;
    };
    // PlayEffect: tick 末の spawn キューに積む (Prefab::Instantiate は内部で構造変更を確定するため
    // イテレーション中には呼べない。EngineLoop が ApplyStructuralChanges 直前に drain する)
    out.PlayEffect = [](void* engine, const char* prefabKey, MyeVec3 pos, MyeEntityId parent) {
        auto* q = Ctx(engine)->effectQueue;
        if (q && prefabKey) {
            EffectSpawnRequest r;
            r.prefabKey = prefabKey;
            r.pos = pos;
            r.parent = parent;
            q->push_back(std::move(r));
        }
    };

    // ---- v7 (M37) ----
    // Instantiate: PlayEffect の予約付き版。呼出時 (tick 内の決定論点) に NextFileId を
    // 消費してルート fileId を確保 → drain 側の Prefab::Instantiate へ強制 ID として渡る
    out.Instantiate = [](void* engine, const char* prefabKey, MyeVec3 pos,
                         MyeEntityId parent) -> uint64_t {
        auto* q = Ctx(engine)->effectQueue;
        Scene* s = Sc(engine);
        if (!q || !s || !prefabKey) {
            return 0;
        }
        EffectSpawnRequest r;
        r.prefabKey = prefabKey;
        r.pos = pos;
        r.parent = parent;
        r.reservedRootFid = s->NextFileId();
        const uint64_t fid = r.reservedRootFid;
        q->push_back(std::move(r));
        return fid;
    };
    out.FindByFileId = [](void* engine, uint64_t fileId) -> MyeEntityId {
        GameObject g = Sc(engine)->FindByFileId(fileId);
        return g ? ToShared(g.Id()) : MyeEntityId{};
    };

    // ---- Animator Controller パラメータ (v7)。hash 対象への決定論的書込 (SetLocalPosition と同格) ----
    out.SetAnimatorParam = [](void* engine, MyeEntityId id, int index, int value) -> int {
        auto* ac = Sc(engine)->GetWorld().GetComponent<AnimatorControllerComponent>(ToEngine(id));
        if (!ac || index < 0 || index >= 4) { return 0; }
        ac->params[index] = value;
        return 1;
    };
    out.GetAnimatorParam = [](void* engine, MyeEntityId id, int index, int* outValue) -> int {
        auto* ac = Sc(engine)->GetWorld().GetComponent<AnimatorControllerComponent>(ToEngine(id));
        if (!ac || index < 0 || index >= 4 || !outValue) { return 0; }
        *outValue = ac->params[index];
        return 1;
    };

    // ---- 動的 UI (v7)。UIElement は NoHash → 毎 tick 書いても sim/リプレイに無関係 ----
    out.SetUIText = [](void* engine, MyeEntityId id, const char* utf8) -> int {
        auto* el = Sc(engine)->GetWorld().GetComponent<UIElementComponent>(ToEngine(id));
        if (!el || !utf8) { return 0; }
        size_t n = 0;
        while (utf8[n] != '\0' && n < sizeof(el->text) - 1) {
            el->text[n] = utf8[n];
            ++n;
        }
        el->text[n] = '\0'; // 多バイト途中切れは描画側の U+FFFD 耐性で安全 (M34)
        return 1;
    };
    out.SetUIFill = [](void* engine, MyeEntityId id, float amount) -> int {
        auto* el = Sc(engine)->GetWorld().GetComponent<UIElementComponent>(ToEngine(id));
        if (!el) { return 0; }
        el->fillAmount = amount;
        return 1;
    };
    out.SetUIColor = [](void* engine, MyeEntityId id, MyeColor color) -> int {
        auto* el = Sc(engine)->GetWorld().GetComponent<UIElementComponent>(ToEngine(id));
        if (!el) { return 0; }
        el->color = { color.r, color.g, color.b, color.a };
        return 1;
    };
    out.SetUIFocused = [](void* engine, MyeEntityId id, int focused) -> int {
        auto* el = Sc(engine)->GetWorld().GetComponent<UIElementComponent>(ToEngine(id));
        if (!el) { return 0; }
        el->focused = focused;
        return 1;
    };
    // フォーカスナビ: 基準解像度 1920x1080 でアンカー解決 (ウィンドウ実寸非依存 = 決定論)
    out.UIFocusNav = [](void* engine, MyeEntityId current, int dir) -> MyeEntityId {
        static constexpr int kRefW = 1920; // 内側ラムダから ODR 非使用で参照 (C4189 回避に static)
        static constexpr int kRefH = 1080;
        World& w = Sc(engine)->GetWorld();
        std::vector<uinav::NavRect> rects;
        std::vector<uint32_t> gens;
        uinav::NavRect cur = {};
        bool haveCur = false;
        const ComponentTypeId req[] = { UIElementComponent::sTypeId };
        w.ForEachArchetype(req, [&](Archetype& arch) {
            const int ci = arch.FindTypeIndex(UIElementComponent::sTypeId);
            for (uint32_t row = 0; row < arch.Count(); ++row) {
                const EntityID e = arch.EntityAt(row);
                if (!IsEntityActive(w, e)) {
                    continue;
                }
                const auto* el = static_cast<const UIElementComponent*>(arch.GetPtr(ci, row));
                if (el->focusable == 0) {
                    continue;
                }
                uinav::NavRect r;
                UIRenderer::ResolveAnchor(el->anchor, el->x, el->y, el->w, el->h, kRefW, kRefH,
                                          r.x, r.y);
                r.w = el->w;
                r.h = el->h;
                r.index = e.index;
                if (e.index == current.index) {
                    cur = r;
                    haveCur = true;
                }
                rects.push_back(r);
                gens.push_back(e.generation);
            }
        });
        if (!haveCur || rects.empty()) {
            return current; // 現フォーカスが候補に無ければ維持
        }
        const uint32_t next =
            uinav::FindNext(rects.data(), static_cast<int>(rects.size()), cur, dir);
        for (size_t i = 0; i < rects.size(); ++i) {
            if (rects[i].index == next) {
                return { next, gens[i] };
            }
        }
        return current;
    };

    // ---- デバッグ描画 (v7)。描画レーンのキューに積むだけ (audioQueue パターン) ----
    out.DebugDrawLine = [](void* engine, MyeVec3 a, MyeVec3 b, MyeColor color) {
        auto* q = Ctx(engine)->debugLines;
        if (!q) {
            return;
        }
        auto pack = [](float v) -> uint32_t {
            const float c = (v < 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
            return static_cast<uint32_t>(c * 255.0f + 0.5f);
        };
        DebugLineCmd cmd;
        cmd.ax = a.x; cmd.ay = a.y; cmd.az = a.z;
        cmd.bx = b.x; cmd.by = b.y; cmd.bz = b.z;
        cmd.rgba = (pack(color.r) << 24) | (pack(color.g) << 16) | (pack(color.b) << 8)
                   | pack(color.a);
        q->push_back(cmd);
    };

    // ---- マスク付き空間クエリ (v7、M36a) ----
    out.RaycastMasked = [](void* engine, MyeVec3 origin, MyeVec3 dir, float maxDist, uint32_t mask,
                           MyeRaycastHit* outHit) -> int {
        return RaycastWorld(Sc(engine)->GetWorld(), origin, dir, maxDist, outHit, mask);
    };
    out.OverlapSphereMasked = [](void* engine, MyeVec3 center, float radius, uint32_t mask,
                                 MyeEntityId* outEntities, int maxCount) -> int {
        return OverlapSphereWorld(Sc(engine)->GetWorld(), center, radius, outEntities, maxCount,
                                  mask);
    };
    out.SphereCastMasked = [](void* engine, MyeVec3 origin, MyeVec3 dir, float radius,
                              float maxDist, uint32_t mask, MyeRaycastHit* outHit) -> int {
        return SphereCastWorld(Sc(engine)->GetWorld(), origin, dir, radius, maxDist, outHit, mask);
    };

    // ---- オーディオ (v8、M45g) ----
    // 全て **キューへ積むだけ** で、実際の発音は EngineLoop がハッシュ後に drain する
    // (voice 状態を hashed state へ戻さないための境界)。read API は恒久的に作らない。
    //
    // ★ハンドルを返す 2 本は M45a のスタブと**同じ位置で採番する** — 引数が不正でも
    //   採番を飛ばさないこと。飛ばすと記録と検証で採番列がずれる
    out.PlaySound2 = [](void* engine, const char* soundKey, float volume, float pitch) -> uint64_t {
        ScriptApiContext* c = Ctx(engine);
        const uint64_t handle = ReserveAudioHandle(c);
        if (c->audioQueue == nullptr || soundKey == nullptr) {
            return handle;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::PlayOneShot;
        e.key = HashStr(soundKey);
        e.a = volume;
        e.b = pitch;
        e.handle = handle;
        c->audioQueue->push_back(e);
        return handle;
    };
    out.PlaySoundAt = [](void* engine, const char* soundKey, MyeVec3 worldPos,
                         float volume) -> uint64_t {
        ScriptApiContext* c = Ctx(engine);
        const uint64_t handle = ReserveAudioHandle(c);
        if (c->audioQueue == nullptr || soundKey == nullptr) {
            return handle;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::PlayAtPoint;
        e.key = HashStr(soundKey);
        e.pos = worldPos;
        e.a = volume;
        e.handle = handle;
        c->audioQueue->push_back(e);
        return handle;
    };
    out.StopVoice = [](void* engine, uint64_t handle, float fadeSeconds) {
        PushAudioOp(Ctx(engine), ScriptAudioOp::StopVoice, handle, fadeSeconds);
    };
    out.SetVoiceVolume = [](void* engine, uint64_t handle, float volume) {
        PushAudioOp(Ctx(engine), ScriptAudioOp::SetVoiceVolume, handle, volume);
    };
    out.SetVoicePitch = [](void* engine, uint64_t handle, float pitch) {
        ScriptApiContext* c = Ctx(engine);
        if (c->audioQueue == nullptr) {
            return;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::SetVoicePitch;
        e.handle = handle;
        e.b = pitch; // ピッチは b に載せる規約 (ScriptAudioOp のコメント参照)
        c->audioQueue->push_back(e);
    };
    // ★「AudioSource を持っているか」は**シーンデータ由来 = 決定論**なので sim が読んでよい
    //   (オーディオランタイムの状態ではない)。読んで良いのはこの所持判定までで、
    //   再生中かどうかは絶対に返さない
    out.PlayAudioSource = [](void* engine, MyeEntityId id) -> int {
        ScriptApiContext* c = Ctx(engine);
        if (Sc(engine)->GetWorld().GetComponent<AudioSourceComponent>(ToEngine(id)) == nullptr) {
            return 0;
        }
        if (c->audioQueue == nullptr) {
            return 0;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::PlaySource;
        e.entity = id;
        c->audioQueue->push_back(e);
        return 1;
    };
    out.StopAudioSource = [](void* engine, MyeEntityId id, float fadeSeconds) -> int {
        ScriptApiContext* c = Ctx(engine);
        if (Sc(engine)->GetWorld().GetComponent<AudioSourceComponent>(ToEngine(id)) == nullptr) {
            return 0;
        }
        if (c->audioQueue == nullptr) {
            return 0;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::StopSource;
        e.entity = id;
        e.a = fadeSeconds;
        c->audioQueue->push_back(e);
        return 1;
    };
    out.SetBusVolume = [](void* engine, const char* busName, float volume) {
        ScriptApiContext* c = Ctx(engine);
        if (c->audioQueue == nullptr || busName == nullptr) {
            return;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::SetBusVolume;
        // ★バス名のハッシュ規則は AudioSystem の 1 本だけを使う (大文字小文字無視のため
        //   小文字化してからハッシュする)。ここで素の HashStr を書くと FindBus と食い違う
        e.key = AudioSystem::HashBusName(busName);
        e.a = volume;
        c->audioQueue->push_back(e);
    };
    out.PlayMusic = [](void* engine, const char* soundKey, float fadeSeconds, int loop) {
        ScriptApiContext* c = Ctx(engine);
        if (c->audioQueue == nullptr || soundKey == nullptr) {
            return;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::PlayMusic;
        e.key = HashStr(soundKey);
        e.a = fadeSeconds;
        e.i0 = loop;
        c->audioQueue->push_back(e);
    };
    out.StopMusic = [](void* engine, float fadeSeconds) {
        PushAudioOp(Ctx(engine), ScriptAudioOp::StopMusic, 0, fadeSeconds);
    };
    out.SetListenerEntity = [](void* engine, MyeEntityId id) {
        ScriptApiContext* c = Ctx(engine);
        if (c->audioQueue == nullptr) {
            return;
        }
        ScriptAudioEvent e;
        e.op = ScriptAudioOp::SetListener;
        e.entity = id; // null id = 自動 (AudioListener → primary カメラ)
        c->audioQueue->push_back(e);
    };

    // ---- 部位 (ソケット) クエリ (v9、M48h) ----
    // 実装は Engine/Engine/Parts.{h,cpp} の 1 本きり — エディタ (Inspector のドロップダウン、
    // 構造ロック) とスクリプトが同じ関数を見ることで「エディタで見えた部位」と
    // 「スクリプトが引ける部位」が食い違わないようにしている
    out.FindPart = [](void* engine, MyeEntityId root, const char* utf8Path) -> MyeEntityId {
        const std::string_view path = utf8Path ? std::string_view(utf8Path) : std::string_view();
        return ToShared(Parts::FindPart(Sc(engine)->GetWorld(), ToEngine(root), path));
    };
    out.FindPartsByTag = [](void* engine, MyeEntityId root, uint64_t tag, MyeEntityId* outParts,
                            int32_t cap) -> int32_t {
        std::vector<EntityID> hits;
        Parts::FindPartsByTag(Sc(engine)->GetWorld(), ToEngine(root), tag, hits);
        const int32_t total = static_cast<int32_t>(hits.size());
        // 切り捨てても戻り値は総数のまま (Overlap* と同じ規約)。out=null / cap<=0 は数えるだけ
        const int32_t written = (outParts && cap > 0) ? ((total < cap) ? total : cap) : 0;
        for (int32_t i = 0; i < written; ++i) {
            outParts[i] = ToShared(hits[static_cast<size_t>(i)]);
        }
        return total;
    };

    // ---- 部位ボリューム レイキャスト (v10、M49) ----
    // 実体は Parts::RaycastParts の 1 本きり (エディタのクリック選択・ワイヤ表示と同じ関数 =
    // 「エディタで選べた範囲」と「スクリプトが当てられる範囲」が食い違わない)
    out.RaycastParts = [](void* engine, MyeEntityId root, uint64_t tag, MyeVec3 origin,
                          MyeVec3 dir, float maxDist, MyeRaycastHit* outHit) -> int32_t {
        Parts::PartRayHit hit;
        if (!Parts::RaycastParts(Sc(engine)->GetWorld(), ToEngine(root), tag,
                                 { origin.x, origin.y, origin.z }, { dir.x, dir.y, dir.z },
                                 maxDist, hit)) {
            return 0;
        }
        if (outHit) {
            outHit->entity = ToShared(hit.entity);
            outHit->point = { hit.point.x, hit.point.y, hit.point.z };
            outHit->normal = { hit.normal.x, hit.normal.y, hit.normal.z };
            outHit->distance = hit.distance;
        }
        return 1;
    };

    // ---- 汎用フィールドアクセス (v11、M50d) ----
    // 解決は「compNameHash → TypeId → FieldDesc」の 1 本道 (Inspector / シリアライザと
    // 同じメタデータを読む = スキーマ型も組込み型も同じ道)。ポインタは越境させない —
    // 常に値コピー。★kComponentNoHash (C# スクリプト状態 = 非決定論レーン) は読み書き
    // とも遮断する — そこから 1 bit でも sim へ読むとリプレイが壊れる (EngineAPI.h の契約)
    out.GetComponentField = [](void* engine, MyeEntityId e, uint64_t compNameHash,
                               uint64_t fieldNameHash, void* buf, int32_t bufSize,
                               int32_t* outType) -> int32_t {
        World& w = Sc(engine)->GetWorld();
        if (!w.IsAlive(ToEngine(e))) { return 0; }
        const ComponentTypeId t = ComponentRegistry::Get().FindByNameHash(compNameHash);
        if (t == kInvalidComponentType) { return 0; }
        const ComponentDesc& desc = ComponentRegistry::Get().Desc(t);
        if (desc.flags & kComponentNoHash) { return 0; }
        const void* comp = w.GetComponentRaw(ToEngine(e), t);
        if (!comp) { return 0; }
        for (const FieldDesc& f : desc.fields) {
            if (HashStr(f.name) != fieldNameHash) { continue; }
            const int32_t size = static_cast<int32_t>(FieldTypeSize(f.type));
            if (buf == nullptr || bufSize < size) { return 0; }
            std::memcpy(buf, static_cast<const uint8_t*>(comp) + f.offset,
                        static_cast<size_t>(size));
            if (outType) { *outType = static_cast<int32_t>(f.type); }
            return size;
        }
        return 0;
    };
    out.SetComponentField = [](void* engine, MyeEntityId e, uint64_t compNameHash,
                               uint64_t fieldNameHash, const void* buf, int32_t size) -> int32_t {
        if (buf == nullptr || size <= 0) { return 0; }
        World& w = Sc(engine)->GetWorld();
        if (!w.IsAlive(ToEngine(e))) { return 0; }
        const ComponentTypeId t = ComponentRegistry::Get().FindByNameHash(compNameHash);
        if (t == kInvalidComponentType) { return 0; }
        const ComponentDesc& desc = ComponentRegistry::Get().Desc(t);
        if (desc.flags & kComponentNoHash) { return 0; }
        void* comp = w.GetComponentRaw(ToEngine(e), t);
        if (!comp) { return 0; }
        for (const FieldDesc& f : desc.fields) {
            if (HashStr(f.name) != fieldNameHash) { continue; }
            const int32_t cap = static_cast<int32_t>(FieldTypeSize(f.type));
            uint8_t* dst = static_cast<uint8_t*>(comp) + f.offset;
            if (f.type == FieldType::String64 || f.type == FieldType::String256) {
                // 文字列は size <= cap を許し、残りをゼロ埋め + 終端を保証する。
                // String64 は終端以降もハッシュ対象 (M48i の罠) — 残骸をこの境界で断つ
                if (size > cap) { return 0; }
                std::memcpy(dst, buf, static_cast<size_t>(size));
                std::memset(dst + size, 0, static_cast<size_t>(cap - size));
                dst[cap - 1] = 0;
            } else {
                if (size != cap) { return 0; } // 値型はサイズ厳密一致 (型違いの静かな破壊を防ぐ)
                std::memcpy(dst, buf, static_cast<size_t>(size));
            }
            return 1;
        }
        return 0;
    };
}

} // namespace mye
