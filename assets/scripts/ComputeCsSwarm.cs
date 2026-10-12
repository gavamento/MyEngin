using MyeKernels.DemoSwarmStep;
using MyeScripting;
using MyeScripting.KernelCompat;

// ComputeCsSwarm — Compute.ForEach のデモ (--compute-demo) の C# 版。C++ の群れ (ComputeDemoSwarm) と同じ DemoSwarmStep を、
// C# に変換した CPU 版で走らせる。見える体 (DemoCsBoid_00..) の位置へ結果を写す。
// C# レーンは record / verify 中は止まるのでリプレイの照合には入らない (体は初期位置のまま)。バッファは NoHash のストアに置かれる。
[ComputeKernels("DemoSwarmStep")]
public class ComputeCsSwarm : MyeScript
{
    private const uint Count = 48; // シーン側の DemoCsBoid の数と同じ
    private const uint Columns = 8;
    private static readonly MyeVec3 Center = new MyeVec3(9.0f, 3.0f, 0.0f);
    private const float Radius = 2.0f;

    private Buffer<DemoBoid> _bufferA;
    private Buffer<DemoBoid> _bufferB;
    private bool _flip;
    private readonly DemoBoid[] _boids = new DemoBoid[Count];
    private readonly MyeEntity[] _visible = new MyeEntity[Count];

    public override void Start()
    {
        _bufferA = Compute.CreateBuffer<DemoBoid>(Count, BufferKind.State);
        _bufferB = Compute.CreateBuffer<DemoBoid>(Count, BufferKind.State);
        for (uint i = 0; i < Count; ++i)
        {
            float col = (float)(i % Columns) - 0.5f * (float)(Columns - 1);
            float row = (float)(i / Columns);
            _boids[i].pos = new float3(Center.X + col * 0.25f, Center.Y - 1.0f + row * 0.15f, Center.Z + row * 0.1f);
            _boids[i].vel = new float3(0.0f, 0.0f, 0.0f);
            _visible[i] = Find("DemoCsBoid_" + i.ToString("D2"));
        }
        _bufferA.Write(_boids);
    }

    public override void Update(float dt)
    {
        Buffer<DemoBoid> prev = _flip ? _bufferB : _bufferA;
        Buffer<DemoBoid> next = _flip ? _bufferA : _bufferB;
        var prm = new DemoSwarmStepParams
        {
            dt = dt,
            time = (float)Tick * dt,
            center = new float3(Center.X, Center.Y, Center.Z),
            radius = Radius,
            count = Count,
            tick = (uint)Tick,
        };
        if (!Compute.ForEach("DemoSwarmStep", Execution.Auto, in prm, next, prev).Ok) return;
        _flip = !_flip;
        if (!next.Read(_boids)) return;
        for (uint i = 0; i < Count; ++i)
        {
            if (_visible[i].IsAlive) _visible[i].Position = new MyeVec3(_boids[i].pos.x, _boids[i].pos.y, _boids[i].pos.z);
        }
    }

    private static MyeEntity Find(string name) => MyeEntity.Find(name);
}
