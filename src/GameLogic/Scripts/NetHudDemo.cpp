// ネット対戦 HUD (M52i、--net-demo)。
//
// ★**このスクリプトが書く先は UIElement だけ**。UIElement は WorldHash 対象外の
//   描画状態 (EngineAPI.h の v12 の注記) なので、ここに機種依存の値を書いても
//   2 台のワールドハッシュは割れない。逆に言うと、ネットの状態
//   (NetLocalPlayer / NetPingMs / NetRollbackCount) を**書いてよいのはここだけ**で、
//   同じ値をコンポーネントのフィールドやスクリプトの登録フィールドへ入れた瞬間に
//   sim 状態になり、次の checkpoint で desync 検出が働いてセッションが止まる。
//
// ABI v13 の Net* 5 本の実走確認も兼ねている (C# レーンはネット中に走らないので、
// このレーンの被覆はここでしか取れない)。
#include "Shared/ScriptAPI.h"

#include <cstdio>

struct NetHudDemo : Script<NetHudDemo> {
    // ★ここに置いてよいのは**設定値だけ**。ネットの状態 (ping / 巻き戻し回数 /
    //   自分のレーン) を「前フレームの値」として持ちたくなるが、登録フィールドは
    //   sim 状態 (WorldHash + スナップショット対象) なので、入れた瞬間に 2 台で
    //   割れる。スクリプトの登録フィールド表は決定論の内と外を分ける境界そのもの
    int32_t style = 0; // 0 = 標準表示 (将来の切替用。ネットの値は絶対に入れないこと)

    void Update(MyeUpdateContext& ctx)
    {
        char buf[256];
        if (MyeNetIsConnected(ctx)) {
            std::snprintf(buf, sizeof(buf),
                          "NET  you are P%u of %u   ping %.0f ms   rollbacks %llu",
                          MyeNetLocalPlayer(ctx) + 1u, MyeNetPlayerCount(ctx),
                          static_cast<double>(MyeNetPingMs(ctx)),
                          static_cast<unsigned long long>(MyeNetRollbackCount(ctx)));
        } else {
            // 非ネット (--local-players 2 でこのシーンを 1 台で回したとき) の表示。
            // ★「接続していない」を空文字にしない — 何も出ないと「HUD が壊れている」と
            //   「繋がっていない」を目視で区別できない
            std::snprintf(buf, sizeof(buf), "LOCAL  (no net session)   tick %llu",
                          static_cast<unsigned long long>(ctx.tickIndex));
        }
        ctx.api->SetUIText(ctx.api->engine, ctx.self, buf);
    }
};

REGISTER_SCRIPT(NetHudDemo, FIELDS(style));
