//====================================================================================
//                          MessageProbe.cpp
//  MyEngin/ 秋田蓮音                                                     10/27/2026
//                                          ABI v30 (SendScriptMessage / OnMessage) の恒久 probe
//====================================================================================
// 既定デモ (--なし) の Spinner に付き、7 tick ごとに自分宛てへメッセージを送って同じ tick に受け取り、
// 回数と値を sim 状態 (登録フィールド) へ書き戻す。呼ぶだけでは配達の Debug/Release divergence を
// replay_verify が検知できない (FractureDamageProbe / SchemaHealthDemo と同じ流儀)。
// 自分宛てなのは、既定デモにもう 1 体の受け手を置かずに「積む → 配達点で配る → OnMessage」の経路を
// 丸ごと通すため。宛先の解決 (target の index / generation) は他エンティティ宛てと同じ 1 本
#include "Shared/ScriptAPI.h"

struct MessageProbe : Script<MessageProbe> {
    int32_t sentCount = 0;       // SendScriptMessage が 1 を返した回数
    int32_t receivedCount = 0;   // OnMessage を受けた回数 (配達が同じ tick なので sentCount と一致する)
    int32_t lastValue = 0;       // 直近に受けた value (= 送った tick 番号の下位 31 bit)
    uint32_t lastFromIndex = 0;  // 直近の送り主の index (自分宛てなので self.index と一致する)

    static constexpr uint64_t kPing = MyeNameHash("probe.ping");
    static constexpr uint64_t kEveryTicks = 7;

    void Update(MyeUpdateContext& ctx)
    {
        if (ctx.tickIndex % kEveryTicks != 0) {
            return;
        }
        const int32_t value = static_cast<int32_t>(ctx.tickIndex & 0x7FFFFFFFull);
        if (ctx.api->SendScriptMessage(ctx.api->engine, ctx.self, ctx.self, kPing, value) != 0) {
            ++sentCount;
        }
    }

    void OnMessage(MyeUpdateContext& ctx, MyeEntityId from, uint64_t nameHash, int32_t value)
    {
        (void)ctx;
        if (nameHash != kPing) {
            return;
        }
        ++receivedCount;
        lastValue = value;
        lastFromIndex = from.index;
    }
};
REGISTER_SCRIPT(MessageProbe,
                FIELDS(MYE_F_JP(sentCount, "送信回数"), MYE_F_JP(receivedCount, "受信回数"),
                       MYE_F_JP(lastValue, "直近の値"), MYE_F_JP(lastFromIndex, "直近の送り主")));
