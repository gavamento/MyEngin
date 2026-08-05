// 部位 (ソケット) API のスモークデモ (M48h)。
//
// 「アセット側が決めた取り付け位置に、ランタイムが物を付ける」= 仕様書の `SetEffect(leg)`
// 相当の最小形。v9 でエンジンが増やしたのは **引く手段** (FindPart / FindPartsByTag) だけで、
// 取り付け自体は既存の SetParent — という設計をそのまま写している。
//
// 部位ショーケース (--parts-demo) のモデルルートに付いており、リプレイ検証で毎回走る。
// 引いているのはシーンデータ (階層 / 名前 / PartComponent) だけなので決定論。
#include "Shared/ScriptAPI.h"

struct PartAttachDemo : Script<PartAttachDemo> {
    int32_t done = 0;              // 取り付け済み (登録フィールド = DLL リロードを跨いで保持)
    MyeEntityId attached = {};     // 取り付けたエンティティ (Inspector から辿れるように持つ)

    void Start(MyeUpdateContext& ctx)
    {
        if (done) {
            return; // DLL ホットリロードで Start が再入しても二重に生やさない
        }
        // 自分のサブツリーから "HandR" タグの部位を引く。タグ検索は入れ子プレハブの
        // 境界で止まらないので、外側 (キャラのルート) から 1 回で引ける
        const MyeEntityId hand = MyeFindPartByTag(ctx, ctx.self, "HandR");
        if (MyeEntityIdIsNull(hand)) {
            MyeLogf(ctx, "[parts] no 'HandR' part under self - nothing to attach");
            return;
        }

        const MyeEngineApi* api = ctx.api;
        const MyeEntityId charm = api->CreateGameObject(api->engine, "PartCharm");
        api->SetLocalPosition(api->engine, charm, { 0.0f, 0.18f, 0.0f }); // 部位ローカル
        api->SetLocalScale(api->engine, charm, { 0.06f, 0.06f, 0.06f });
        api->SetMeshRenderer(api->engine, charm, "builtin://cube", "parts_drop");
        api->SetParent(api->engine, charm, hand); // ← 取り付けは v3 からある既存スロット
        attached = charm;
        done = 1;
        MyeLogf(ctx, "[parts] attached 'PartCharm' to the HandR socket");
    }
};
REGISTER_SCRIPT(PartAttachDemo, FIELDS(done, attached));
