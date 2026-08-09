using MyeScripting;

// FlowMenu — flow 統合デモ (M51j) のタイトル演出。ヒント文の点滅だけを担当する。
// C# レーンは record/verify 中は走らない (リプレイ被覆外の演出専用) — リプレイで
// 再現したいフロー制御はすべて FlowTitleDriver / FlowGameDriver (C++) 側にある。
// このスクリプトはヒント文のエンティティ (TitleHint) 自身に付く。
public class FlowMenu : MyeScript
{
    private float _t;

    public override void Update(float dt)
    {
        _t += dt;
        float a = 0.55f + 0.45f * (float)System.Math.Sin(_t * 4.0);
        Self.SetUIColor(1.0f, 1.0f, 1.0f, a);
    }
}
