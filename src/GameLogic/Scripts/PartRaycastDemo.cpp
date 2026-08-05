// 範囲部位レイキャスト (v10、M49) の恒久 probe。
//
// 部位ショーケース (--parts-demo) の Target に付き、リプレイ検証で毎回走る。
// ★結果を sim 状態 (登録フィールド + マーカーの LocalTransform) に**書き戻す**のが本体 —
//   呼ぶだけでは RaycastParts の Debug/Release divergence を replay_verify が検知できない。
//
// レイは tickIndex 由来の三角波で高さを掃引し、WeakPoint (Target の上半分の箱ボリューム)
// をヒット/ミスの両方で通す。読むのはシーンデータだけ、掃引は tick 決定なので決定論。
#include "Shared/ScriptAPI.h"

struct PartRaycastDemo : Script<PartRaycastDemo> {
    int32_t hitCount = 0;    // ヒット総数 (被覆の本体 — ここが構成間でズレたら即 divergence)
    float lastDist = 0.0f;   // 最後のヒット距離
    MyeEntityId marker = {}; // ヒット点の可視化 (LocalTransform が hash に乗る)

    void Start(MyeUpdateContext& ctx)
    {
        if (!MyeEntityIdIsNull(marker)) {
            return; // DLL ホットリロードで Start が再入しても二重に生やさない
        }
        const MyeEngineApi* api = ctx.api;
        marker = api->CreateGameObject(api->engine, "RayMarker");
        api->SetLocalScale(api->engine, marker, { 0.05f, 0.05f, 0.05f });
        api->SetMeshRenderer(api->engine, marker, "builtin://cube", "parts_held");
    }

    void Update(MyeUpdateContext& ctx)
    {
        // 高さ 0.2..1.0 を掃引。WeakPoint の箱はワールド y [0.5, 1.0] なので両方通る
        const float ph = static_cast<float>(ctx.tickIndex % 120u) / 119.0f;
        const MyeVec3 origin = { 2.0f, 0.2f + 0.8f * ph, 0.4f };
        const MyeVec3 dir = { -1.0f, 0.0f, 0.0f }; // MyeRaycastParts は正規化済み前提
        MyeRaycastHit hit = {};
        if (MyeRaycastParts(ctx, MyeEntityId{}, "WeakPoint", origin, dir, 20.0f, hit)) {
            ++hitCount;
            lastDist = hit.distance;
            ctx.api->SetLocalPosition(ctx.api->engine, marker, hit.point);
        }
    }
};
REGISTER_SCRIPT(PartRaycastDemo, FIELDS(hitCount, lastDist, marker));
