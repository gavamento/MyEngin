using MyeScripting;

// Mover — C# スクリプトのデモ。self を +Y 方向に speed 単位/秒で動かす。
// Assets パネルの [Compile C# Scripts] でコンパイルし、Inspector の Add Component で付与できます。
// public フィールド (speed) は Inspector に出て、Play/リロードを跨いで保持されます。
public class Mover : MyeScript
{
    public float speed = 2.0f;

    public override void Start()
    {
        Log("Mover started (speed=" + speed + ")");
    }

    public override void Update(float dt)
    {
        // 毎 tick (1/60s) 呼ばれる。self の Transform を +Y に動かす。
        var p = Transform.LocalPosition;
        p.Y += speed * dt;
        Transform.LocalPosition = p;
    }
}
