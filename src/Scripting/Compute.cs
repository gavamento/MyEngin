//====================================================================================
//                          Compute.cs
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          Compute.ForEach の C# 側 API と C# の CPU 版の登録
//====================================================================================
// C++ (Shared/ScriptCompute.h) と同じ名前・引数の順・戻り値にそろえてある。違いは C++ の ctx 引数だけ。
//
//   [ComputeKernels("ParticleUpdate")]
//   class Swarm : MyeScript {
//       Buffer<Particle> buf;
//       public override void Start() { buf = Compute.CreateBuffer<Particle>(count, BufferKind.State); }
//       public override void Update(float dt) {
//           buf.Write(particles);
//           Job job = Compute.ForEach("ParticleUpdate", Execution.Auto, prm, buf);
//           job.Wait(); buf.Read(particles);
//           Compute.ForEach(particles, "ParticleUpdate", Execution.Auto, prm);   // 配列を直接渡す糖衣
//       }
//   }
//
// ★C# が呼ぶ ForEach は常にエンジンが変換した C# の CPU 版で走る (GameLogic.dll の C++ 版は使わない)。
//   C# レーンは Replay の被覆外で、バッファは NoHash のストアに置かれる。
// ★Fx のジョブハンドル / Status / Target / Read の結果 / バッファのハンドルは実行先 (CPU か GPU か) で値が変わりうる。
//   sim の分岐に使ったり FIELD に保存したりしてはいけない。Fx の結果は表示用 (DebugDrawLine など) に限る。
// ★ForEach できるのは [ComputeKernels] で宣言したカーネルだけ。宣言のないカーネル、およびスクリプトのコールバックの外
//   (BT タスクなど) からの呼び出しは、何も実行せずに失敗 (Job.Handle == 0)。
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using MyeScripting.KernelCompat;

namespace MyeScripting
{
    // バッファの種別 (ABI の kind と同値)
    public enum BufferKind : uint { State = 0, Fx = 1 }

    // 実行先の指定 (ABI の execution と同値)。Auto は State なら CPU、Fx なら GPU を優先して決まる
    public enum Execution : uint { Auto = 0, Cpu = 1, Gpu = 2 }

    public enum JobStatus { Done = 0, Pending = 1, Failed = 2, Skipped = 3 }
    public enum JobTarget { None = 0, Cpu = 1, Gpu = 2 }

    // スクリプトクラスが使うカーネルの宣言。宣言していないカーネルの ForEach は失敗する
    [AttributeUsage(AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
    public sealed class ComputeKernelsAttribute : Attribute
    {
        public string[] Names { get; }
        public ComputeKernelsAttribute(params string[] names) { Names = names ?? Array.Empty<string>(); }
    }

    // ForEach の結果。Handle == 0 は失敗。同期で終わった CPU 実行は定数、GPU の非同期ジョブだけが連番
    public readonly struct Job
    {
        public readonly ulong Handle;
        internal Job(ulong handle) { Handle = handle; }

        public bool Ok => Handle != 0;
        public JobStatus Status() => (JobStatus)Engine.ComputeJobStatus(Handle);
        public JobTarget Target() => (JobTarget)Engine.ComputeJobTarget(Handle);
        // GPU のジョブの完了まで待つ (明示的なストール)。失敗したジョブは false
        public bool Wait() => Engine.ComputeJobWait(Handle);
    }

    // ForEach に渡せるバッファ。Buffer<T> だけが実装する
    public interface IComputeBuffer
    {
        ulong Handle { get; }
    }

    // 要素 T のバッファ 1 本。値としてコピーしてよい (ハンドルと要素数だけを持つ)。
    // T のレイアウトはカーネルの HLSL struct と同じ (4 バイト詰め、MyeKernels.<Name> の生成 struct か Pack = 4 の自前 struct)
    public unsafe struct Buffer<T> : IComputeBuffer where T : unmanaged
    {
        private ulong _handle;
        private uint _count;

        public static Buffer<T> FromHandle(ulong handle, uint count)
        {
            Buffer<T> b = default;
            b._handle = handle;
            b._count = count;
            return b;
        }

        public bool Valid => _handle != 0;
        public ulong Handle => _handle;
        public uint Count => _count;

        // 先頭の src.Length 要素を書く (バッファの要素数を超えると何も書かずに false)
        public bool Write(ReadOnlySpan<T> src)
        {
            fixed (T* p = src) { return Engine.ComputeWriteBuffer(_handle, p, (uint)src.Length); }
        }
        public bool Write(T[] src) => Write((ReadOnlySpan<T>)src);

        // 先頭の dst.Length 要素を読む。Fx は最新の完了済みの結果 (wait = true で GPU の完了を待つ)
        public bool Read(Span<T> dst, bool wait = false)
        {
            fixed (T* p = dst) { return Engine.ComputeReadBuffer(_handle, p, (uint)dst.Length, wait); }
        }
        public bool Read(T[] dst, bool wait = false) => Read((Span<T>)dst, wait);

        // 解放して無効にする。持ち主のエンティティが破棄されたときも tick 末に自動で解放される
        public void Release()
        {
            if (_handle != 0) Engine.ComputeReleaseBuffer(_handle);
            _handle = 0;
            _count = 0;
        }
    }

    public static unsafe class Compute
    {
        // 要素 count 個のバッファを作る。持ち主は呼び出し元のスクリプトのエンティティ。失敗は Valid == false
        public static Buffer<T> CreateBuffer<T>(uint count, BufferKind kind) where T : unmanaged
        {
            ulong h = Engine.ComputeCreateBuffer(ComputeCaller.Self, count, (uint)sizeof(T), (uint)kind);
            return h != 0 ? Buffer<T>.FromHandle(h, count) : default;
        }

        // kernel を buffers[0] の要素数ぶん実行する。buffers は宣言順 (RW / RO 混在)
        public static Job ForEach<P, B0>(string kernel, Execution execution, in P prm, B0 b0)
            where P : unmanaged where B0 : struct, IComputeBuffer
        {
            ulong* h = stackalloc ulong[1] { b0.Handle };
            return Run(kernel, execution, in prm, h, 1);
        }
        public static Job ForEach<P, B0, B1>(string kernel, Execution execution, in P prm, B0 b0, B1 b1)
            where P : unmanaged where B0 : struct, IComputeBuffer where B1 : struct, IComputeBuffer
        {
            ulong* h = stackalloc ulong[2] { b0.Handle, b1.Handle };
            return Run(kernel, execution, in prm, h, 2);
        }
        public static Job ForEach<P, B0, B1, B2>(string kernel, Execution execution, in P prm, B0 b0, B1 b1, B2 b2)
            where P : unmanaged where B0 : struct, IComputeBuffer where B1 : struct, IComputeBuffer
            where B2 : struct, IComputeBuffer
        {
            ulong* h = stackalloc ulong[3] { b0.Handle, b1.Handle, b2.Handle };
            return Run(kernel, execution, in prm, h, 3);
        }
        public static Job ForEach<P, B0, B1, B2, B3>(string kernel, Execution execution, in P prm, B0 b0, B1 b1, B2 b2, B3 b3)
            where P : unmanaged where B0 : struct, IComputeBuffer where B1 : struct, IComputeBuffer
            where B2 : struct, IComputeBuffer where B3 : struct, IComputeBuffer
        {
            ulong* h = stackalloc ulong[4] { b0.Handle, b1.Handle, b2.Handle, b3.Handle };
            return Run(kernel, execution, in prm, h, 4);
        }

        // 配列を直接渡す糖衣。一時の State バッファへ写して実行し、成功したら結果を書き戻す (同期で完了する)。
        // extra は 2 本目以降のバッファ。失敗したときは data に触らない
        public static Job ForEach<T, P>(T[] data, string kernel, Execution execution, in P prm)
            where T : unmanaged where P : unmanaged
        {
            Buffer<T> temp = Stage(data);
            if (!temp.Valid) return default;
            Job job = ForEach(kernel, execution, in prm, temp);
            return Finish(temp, job, data);
        }
        public static Job ForEach<T, P, B1>(T[] data, string kernel, Execution execution, in P prm, B1 extra1)
            where T : unmanaged where P : unmanaged where B1 : struct, IComputeBuffer
        {
            Buffer<T> temp = Stage(data);
            if (!temp.Valid) return default;
            Job job = ForEach(kernel, execution, in prm, temp, extra1);
            return Finish(temp, job, data);
        }
        public static Job ForEach<T, P, B1, B2>(T[] data, string kernel, Execution execution, in P prm, B1 extra1, B2 extra2)
            where T : unmanaged where P : unmanaged where B1 : struct, IComputeBuffer where B2 : struct, IComputeBuffer
        {
            Buffer<T> temp = Stage(data);
            if (!temp.Valid) return default;
            Job job = ForEach(kernel, execution, in prm, temp, extra1, extra2);
            return Finish(temp, job, data);
        }

        private static Buffer<T> Stage<T>(T[] data) where T : unmanaged
        {
            if (data == null || data.Length == 0) return default;
            Buffer<T> temp = CreateBuffer<T>((uint)data.Length, BufferKind.State);
            if (temp.Valid && !temp.Write(data)) temp.Release();
            return temp;
        }

        private static Job Finish<T>(Buffer<T> temp, Job job, T[] data) where T : unmanaged
        {
            if (job.Ok) temp.Read(data);
            temp.Release();
            return job;
        }

        private static Job Run<P>(string kernel, Execution execution, in P prm, ulong* handles, uint count)
            where P : unmanaged
        {
            fixed (P* p = &prm)
            {
                return new Job(Engine.ComputeForEach(kernel, (uint)execution, p, (uint)sizeof(P), handles, count,
                                                    ComputeCaller.ScriptType));
            }
        }
    }

    // 実行中のスクリプトのコールバック (Start / Update / ...) の間だけ、呼び出し元の型とエンティティを持つ。
    // コールバックの外 (BT タスクなど) では ScriptType が null で、エンジンは ForEach を失敗にする (宣言を持てない呼び出し元)
    internal static class ComputeCaller
    {
        public static string ScriptType;
        public static MyeEntityId Self = MyeEntityId.Null;

        public readonly struct Scope : IDisposable
        {
            private readonly string _prevType;
            private readonly MyeEntityId _prevSelf;
            public Scope(MyeScript script)
            {
                _prevType = ScriptType;
                _prevSelf = Self;
                ScriptType = script.GetType().FullName;
                Self = script.SelfId;
            }
            public void Dispose()
            {
                ScriptType = _prevType;
                Self = _prevSelf;
            }
        }
    }

    // Roslyn でコンパイルしたスクリプト用アセンブリから、C# の CPU 版 (MyeKernels.<Name>.CpuKernel) と
    // [ComputeKernels] の宣言を集めて、エンジンの C# レーンの表へ丸ごと差し替えで登録する (spec D25 / D28-b)。
    // 呼ぶ時点は「コンパイル後、スクリプトをインスタンス化する前」。
    internal static unsafe class ComputeRegistration
    {
        private sealed class KernelInfo
        {
            public string Name;
            public ulong Hash;
            public uint ParamsSize;
            public readonly List<(string name, uint stride, bool writable)> Args = new List<(string, uint, bool)>();
            public IntPtr Range;
        }

        // 戻り値: 登録したカーネル数。登録に失敗したカーネルはログに出して飛ばす
        public static int Register(Type[] allTypes, List<Type> scriptTypes)
        {
            var kernels = new List<KernelInfo>();
            foreach (Type t in allTypes)
            {
                if (t.Name != "CpuKernel" || t.Namespace == null
                    || !t.Namespace.StartsWith("MyeKernels.", StringComparison.Ordinal)) continue;
                try { kernels.Add(Describe(t)); }
                catch (Exception ex) { Engine.Log("[csharp] kernel " + t.Namespace + " skipped: " + ex.Message, 3); }
            }
            kernels.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));

            var decls = new List<(string type, string[] names)>();
            foreach (Type t in scriptTypes)
            {
                var attr = (ComputeKernelsAttribute)Attribute.GetCustomAttribute(t, typeof(ComputeKernelsAttribute), false);
                if (attr != null) decls.Add((t.FullName, attr.Names));
            }

            var mem = new List<IntPtr>();
            try
            {
                var descs = (MyeKernelDesc*)Alloc(mem, (nuint)(sizeof(MyeKernelDesc) * Math.Max(kernels.Count, 1)));
                for (int i = 0; i < kernels.Count; ++i)
                {
                    KernelInfo k = kernels[i];
                    var args = (MyeKernelArg*)Alloc(mem, (nuint)(sizeof(MyeKernelArg) * Math.Max(k.Args.Count, 1)));
                    for (int a = 0; a < k.Args.Count; ++a)
                    {
                        args[a].Name = Utf8(mem, k.Args[a].name);
                        args[a].Stride = k.Args[a].stride;
                        args[a].Writable = k.Args[a].writable ? 1u : 0u;
                    }
                    descs[i].Name = Utf8(mem, k.Name);
                    descs[i].SourceHash = k.Hash;
                    descs[i].ParamsSize = k.ParamsSize;
                    descs[i].ArgCount = (uint)k.Args.Count;
                    descs[i].Args = args;
                    descs[i].Range = (delegate* unmanaged<void*, MyeKernelBuffer*, uint, uint, uint, void>)(void*)k.Range;
                }
                bool okKernels = Engine.ComputeRegisterManagedKernels(descs, (uint)kernels.Count);

                var declArr = (MyeComputeDecl*)Alloc(mem, (nuint)(sizeof(MyeComputeDecl) * Math.Max(decls.Count, 1)));
                for (int i = 0; i < decls.Count; ++i)
                {
                    var names = (byte**)Alloc(mem, (nuint)(sizeof(byte*) * Math.Max(decls[i].names.Length, 1)));
                    for (int n = 0; n < decls[i].names.Length; ++n) names[n] = Utf8(mem, decls[i].names[n] ?? "");
                    declArr[i].ScriptType = Utf8(mem, decls[i].type);
                    declArr[i].KernelNameCount = (uint)decls[i].names.Length;
                    declArr[i].KernelNames = names;
                }
                bool okDecls = Engine.ComputeSetManagedDeclarations(declArr, (uint)decls.Count);
                if (!okKernels || !okDecls)
                {
                    Engine.Log("[csharp] compute registration refused by the engine (kernels=" + okKernels + ", decls=" + okDecls + ")", 2);
                }
            }
            finally
            {
                foreach (IntPtr p in mem) Marshal.FreeHGlobal(p);
            }
            return kernels.Count;
        }

        // CpuKernel クラスから、名前・ハッシュ・params の大きさ・バッファ引数表・レンジ関数を取り出す。
        // 引数表は Body の引数 (id, in params, RWView/ROView ...) の宣言順そのもの = HLSL の宣言順と変数名
        private static KernelInfo Describe(Type t)
        {
            var info = new KernelInfo();
            info.Name = (string)t.GetField("KernelName").GetRawConstantValue();
            info.Hash = (ulong)t.GetField("SourceHash").GetRawConstantValue();

            MethodInfo body = t.GetMethod("Body", BindingFlags.Public | BindingFlags.Static);
            ParameterInfo[] ps = body.GetParameters();
            info.ParamsSize = (uint)Marshal.SizeOf(ps[1].ParameterType.GetElementType());
            for (int i = 2; i < ps.Length; ++i)
            {
                Type pt = ps[i].ParameterType;
                Type def = pt.GetGenericTypeDefinition();
                bool rw = def == typeof(RWView<>);
                if (!rw && def != typeof(ROView<>)) throw new InvalidOperationException("unexpected buffer view " + pt);
                info.Args.Add((ps[i].Name, (uint)Marshal.SizeOf(pt.GetGenericArguments()[0]), rw));
            }

            MethodInfo range = t.GetMethod("Range", BindingFlags.Public | BindingFlags.Static);
            if (range == null || range.GetParameters().Length != 5) throw new InvalidOperationException("Range has an unexpected signature");
            info.Range = range.MethodHandle.GetFunctionPointer();
            return info;
        }

        private static IntPtr Alloc(List<IntPtr> mem, nuint bytes)
        {
            IntPtr p = Marshal.AllocHGlobal((IntPtr)bytes);
            mem.Add(p);
            return p;
        }

        private static byte* Utf8(List<IntPtr> mem, string s)
        {
            int n = Encoding.UTF8.GetByteCount(s);
            byte* p = (byte*)Alloc(mem, (nuint)(n + 1));
            fixed (char* c = s) { Encoding.UTF8.GetBytes(c, s.Length, p, n); }
            p[n] = 0;
            return p;
        }
    }
}
