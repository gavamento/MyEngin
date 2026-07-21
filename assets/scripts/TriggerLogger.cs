using MyeScripting;

// トリガーイベントの検証用スクリプト。OnTriggerEnter/Exit をログに出す。
public class TriggerLogger : MyeScript
{
    public int hitCount = 0;

    public override void OnTriggerEnter(MyeEntity other)
    {
        hitCount++;
        Log("TriggerLogger.OnTriggerEnter (hitCount=" + hitCount + ")");
    }

    public override void OnTriggerExit(MyeEntity other)
    {
        Log("TriggerLogger.OnTriggerExit");
    }
}
