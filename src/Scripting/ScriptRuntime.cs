using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace MyeScripting
{
    // C# スクリプトの実行時。ネイティブ ManagedHost が vtable 経由で駆動する。
    //   Compile      : assets/scripts/*.cs を Roslyn でメモリ上にコンパイル → collectible ALC にロード
    //   CreateInstance: MyeScript 派生型をインスタンス化 (handle を返す)
    //   Invoke       : Start/Update/LateUpdate を呼ぶ
    // フィールドは Inspector 連携 (GetFieldInfo/Get/SetFieldValue) とリロード跨ぎ永続 (Serialize) に対応。
    internal sealed unsafe class ScriptRuntime
    {
        private static readonly ScriptRuntime Inst = new ScriptRuntime();

        private AssemblyLoadContext _alc;
        private readonly List<Type> _types = new List<Type>();               // FullName 昇順
        private readonly Dictionary<int, MyeScript> _instances = new Dictionary<int, MyeScript>();
        private readonly Dictionary<Type, FieldInfo[]> _fieldCache = new Dictionary<Type, FieldInfo[]>();
        // リロード跨ぎのフィールド永続: (typeFullName, entity.index, entity.gen) -> フィールド JSON
        private readonly Dictionary<(string, uint, uint), string> _persist =
            new Dictionary<(string, uint, uint), string>();
        private int _nextHandle = 1;
        private int _reloadCounter = 0;

        // ---- フィールド型マップ (MyeFieldType / Reflection.h の FieldType と同値) ----
        private static int FieldTypeOf(Type t)
        {
            if (t == typeof(float)) return 0;   // FLOAT
            if (t == typeof(int)) return 1;     // INT32
            if (t == typeof(uint)) return 2;    // UINT32
            if (t == typeof(ulong) || t == typeof(long)) return 3; // UINT64
            if (t == typeof(bool)) return 4;    // BOOL
            if (t == typeof(MyeVec2)) return 5; // FLOAT2
            if (t == typeof(MyeVec3)) return 6; // FLOAT3
            if (t == typeof(MyeVec4)) return 7; // FLOAT4
            if (t == typeof(MyeQuat)) return 8; // QUAT
            if (t == typeof(MyeColor)) return 9; // COLOR
            return -1;
        }

        private FieldInfo[] FieldsOf(Type t)
        {
            if (_fieldCache.TryGetValue(t, out var cached)) return cached;
            var arr = t.GetFields(BindingFlags.Public | BindingFlags.Instance)
                       .Where(f => FieldTypeOf(f.FieldType) >= 0)
                       .OrderBy(f => f.Name, StringComparer.Ordinal)
                       .ToArray();
            _fieldCache[t] = arr;
            return arr;
        }

        // ======================= コンパイル =======================

        private int Compile(string scriptsDir)
        {
            // 1) 既存インスタンスのフィールドを退避 (リロード後に復元する)
            SnapshotForReload();

            // 2) *.cs を収集
            var sources = new List<(string path, string text)>();
            try
            {
                if (Directory.Exists(scriptsDir))
                {
                    foreach (var f in Directory.GetFiles(scriptsDir, "*.cs", SearchOption.AllDirectories)
                                               .OrderBy(p => p, StringComparer.Ordinal))
                    {
                        sources.Add((f, File.ReadAllText(f)));
                    }
                }
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] failed to read scripts: " + ex.Message, 3);
                return -1;
            }

            var trees = sources.Select(s => CSharpSyntaxTree.ParseText(s.text, path: s.path)).ToList();

            // 3) 参照アセンブリ (実行中ランタイムの TPA + MyeScripting.dll)
            var refs = GatherReferences();

            var options = new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                optimizationLevel: OptimizationLevel.Release,
                allowUnsafe: true);

            string asmName = "MyeGameScripts_" + (++_reloadCounter);
            var compilation = CSharpCompilation.Create(asmName, trees, refs, options);

            using var peStream = new MemoryStream();
            var result = compilation.Emit(peStream);
            if (!result.Success)
            {
                int shown = 0;
                foreach (var d in result.Diagnostics.Where(d => d.Severity == DiagnosticSeverity.Error))
                {
                    Engine.Log("[csharp] " + d.ToString(), 3);
                    if (++shown >= 25) break;
                }
                Engine.Log("[csharp] compile FAILED — keeping previous scripts", 3);
                return -1;
            }

            // 4) 新しい collectible ALC にロード → 古い ALC を破棄
            peStream.Seek(0, SeekOrigin.Begin);
            var newAlc = new AssemblyLoadContext(asmName, isCollectible: true);
            // 依存の解決: MyeScripting (MyeScript 基底) は実行中アセンブリを返す
            // → コンパイル済みスクリプトの MyeScript が Default と同一 Type になり IsAssignableFrom が成立。
            // hostfxr は MyeScripting を Default 以外の分離コンテキストにロードするため、
            // Default 走査だけでは見つからない (typeof で直接掴む)。
            var hostAssembly = typeof(ScriptRuntime).Assembly;
            var hostCtx = AssemblyLoadContext.GetLoadContext(hostAssembly) ?? AssemblyLoadContext.Default;
            newAlc.Resolving += (ctx, name) =>
            {
                if (name.Name == hostAssembly.GetName().Name) return hostAssembly;
                foreach (var a in hostCtx.Assemblies)
                {
                    if (a.GetName().Name == name.Name) return a;
                }
                foreach (var a in AssemblyLoadContext.Default.Assemblies)
                {
                    if (a.GetName().Name == name.Name) return a;
                }
                return null;
            };
            Assembly asm;
            try
            {
                asm = newAlc.LoadFromStream(peStream);
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] assembly load failed: " + ex.Message, 3);
                newAlc.Unload();
                return -1;
            }

            Type[] allTypes;
            try
            {
                allTypes = asm.GetTypes();
            }
            catch (ReflectionTypeLoadException rtl)
            {
                foreach (var le in rtl.LoaderExceptions)
                {
                    if (le != null) Engine.Log("[csharp] type load: " + le.Message, 3);
                }
                allTypes = Array.FindAll(rtl.Types, t => t != null);
            }

            _instances.Clear();
            _fieldCache.Clear();
            _types.Clear();
            foreach (var t in allTypes)
            {
                if (typeof(MyeScript).IsAssignableFrom(t) && !t.IsAbstract)
                {
                    _types.Add(t);
                }
            }
            _types.Sort((a, b) => string.CompareOrdinal(a.FullName, b.FullName));

            var old = _alc;
            _alc = newAlc;
            old?.Unload();

            Engine.Log("[csharp] compiled " + sources.Count + " file(s), " + _types.Count + " script type(s)");
            return _types.Count;
        }

        private static List<MetadataReference> GatherReferences()
        {
            var byName = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            var tpa = AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string;
            if (!string.IsNullOrEmpty(tpa))
            {
                foreach (var p in tpa.Split(Path.PathSeparator))
                {
                    if (p.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                    {
                        var key = Path.GetFileNameWithoutExtension(p);
                        if (!byName.ContainsKey(key)) byName[key] = p;
                    }
                }
            }
            // MyeScript 基底型を含む MyeScripting.dll を必ず参照に含める
            var self = typeof(MyeScript).Assembly.Location;
            if (!string.IsNullOrEmpty(self)) byName["MyeScripting"] = self;

            var refs = new List<MetadataReference>();
            foreach (var path in byName.Values)
            {
                try { refs.Add(MetadataReference.CreateFromFile(path)); }
                catch { /* ネイティブイメージ等は無視 */ }
            }
            return refs;
        }

        // ======================= インスタンス =======================

        private int CreateInstance(int typeIndex, MyeEntityId self)
        {
            if (typeIndex < 0 || typeIndex >= _types.Count) return 0;
            try
            {
                var inst = (MyeScript)Activator.CreateInstance(_types[typeIndex]);
                inst.SelfId = self;
                // リロード前に退避したフィールドがあれば復元
                var key = (_types[typeIndex].FullName, self.Index, self.Generation);
                if (_persist.TryGetValue(key, out var json))
                {
                    ApplyJson(inst, json);
                    _persist.Remove(key);
                }
                int h = _nextHandle++;
                _instances[h] = inst;
                return h;
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] CreateInstance failed: " + ex.Message, 3);
                return 0;
            }
        }

        private void DestroyInstance(int handle) => _instances.Remove(handle);

        private void Invoke(int handle, int phase, float dt, ulong tick)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return;
            try
            {
                switch (phase)
                {
                    case 0: inst.Start(); break;
                    case 1: inst.Update(dt); break;
                    case 2: inst.LateUpdate(); break;
                }
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] " + inst.GetType().Name + "." + PhaseName(phase) + " threw: " + ex.Message, 3);
            }
        }

        private void InvokeTrigger(int handle, MyeEntityId other, int enter)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return;
            try
            {
                var e = new MyeEntity(other);
                if (enter != 0) inst.OnTriggerEnter(e); else inst.OnTriggerExit(e);
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] " + inst.GetType().Name + ".OnTrigger threw: " + ex.Message, 3);
            }
        }

        private void InvokeCollision(int handle, MyeEntityId other, int kind, MyeVec3 normal)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return;
            try
            {
                var e = new MyeEntity(other);
                switch (kind)
                {
                    case 0: inst.OnCollisionEnter(e, normal); break;
                    case 1: inst.OnCollisionStay(e); break;
                    case 2: inst.OnCollisionExit(e); break;
                }
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] " + inst.GetType().Name + ".OnCollision threw: " + ex.Message, 3);
            }
        }

        private static string PhaseName(int p) => p == 0 ? "Start" : (p == 1 ? "Update" : "LateUpdate");

        private void ResetInstances()
        {
            _instances.Clear();
        }

        // ======================= フィールド (Inspector) =======================

        private int GetFieldCount(int typeIndex)
        {
            if (typeIndex < 0 || typeIndex >= _types.Count) return 0;
            return FieldsOf(_types[typeIndex]).Length;
        }

        private int GetFieldInfo(int typeIndex, int fieldIndex, byte* nameBuf, int bufLen, int* outType)
        {
            if (typeIndex < 0 || typeIndex >= _types.Count) return 0;
            var fields = FieldsOf(_types[typeIndex]);
            if (fieldIndex < 0 || fieldIndex >= fields.Length) return 0;
            var f = fields[fieldIndex];
            if (outType != null) *outType = FieldTypeOf(f.FieldType);
            return WriteUtf8(f.Name, nameBuf, bufLen);
        }

        private int GetFieldValue(int handle, int fieldIndex, byte* buf, int bufLen)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return 0;
            var fields = FieldsOf(inst.GetType());
            if (fieldIndex < 0 || fieldIndex >= fields.Length) return 0;
            var f = fields[fieldIndex];
            var span = new Span<byte>(buf, bufLen);
            return WriteValue(f.GetValue(inst), FieldTypeOf(f.FieldType), span) ? 1 : 0;
        }

        private int SetFieldValue(int handle, int fieldIndex, byte* buf, int bufLen)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return 0;
            var fields = FieldsOf(inst.GetType());
            if (fieldIndex < 0 || fieldIndex >= fields.Length) return 0;
            var f = fields[fieldIndex];
            var span = new ReadOnlySpan<byte>(buf, bufLen);
            var val = ReadValue(FieldTypeOf(f.FieldType), span);
            if (val == null) return 0;
            try { f.SetValue(inst, val); return 1; }
            catch { return 0; }
        }

        private static bool WriteValue(object val, int ftype, Span<byte> b)
        {
            switch (ftype)
            {
                case 0: return BitConverter.TryWriteBytes(b, (float)val);
                case 1: return BitConverter.TryWriteBytes(b, (int)val);
                case 2: return BitConverter.TryWriteBytes(b, (uint)val);
                case 3: return BitConverter.TryWriteBytes(b, Convert.ToUInt64(val));
                case 4: if (b.Length < 1) return false; b[0] = (bool)val ? (byte)1 : (byte)0; return true;
                case 5: { var v = (MyeVec2)val; return b.Length >= 8 && BitConverter.TryWriteBytes(b, v.X) && BitConverter.TryWriteBytes(b.Slice(4), v.Y); }
                case 6: { var v = (MyeVec3)val; return b.Length >= 12 && BitConverter.TryWriteBytes(b, v.X) && BitConverter.TryWriteBytes(b.Slice(4), v.Y) && BitConverter.TryWriteBytes(b.Slice(8), v.Z); }
                case 7: { var v = (MyeVec4)val; return WriteXyzw(b, v.X, v.Y, v.Z, v.W); }
                case 8: { var v = (MyeQuat)val; return WriteXyzw(b, v.X, v.Y, v.Z, v.W); }
                case 9: { var v = (MyeColor)val; return WriteXyzw(b, v.R, v.G, v.B, v.A); }
            }
            return false;
        }

        private static bool WriteXyzw(Span<byte> b, float x, float y, float z, float w)
            => b.Length >= 16 && BitConverter.TryWriteBytes(b, x) && BitConverter.TryWriteBytes(b.Slice(4), y)
               && BitConverter.TryWriteBytes(b.Slice(8), z) && BitConverter.TryWriteBytes(b.Slice(12), w);

        private static object ReadValue(int ftype, ReadOnlySpan<byte> b)
        {
            switch (ftype)
            {
                case 0: return BitConverter.ToSingle(b);
                case 1: return BitConverter.ToInt32(b);
                case 2: return BitConverter.ToUInt32(b);
                case 3: return BitConverter.ToUInt64(b);
                case 4: return b.Length >= 1 && b[0] != 0;
                case 5: return new MyeVec2(BitConverter.ToSingle(b), BitConverter.ToSingle(b.Slice(4)));
                case 6: return new MyeVec3(BitConverter.ToSingle(b), BitConverter.ToSingle(b.Slice(4)), BitConverter.ToSingle(b.Slice(8)));
                case 7: return new MyeVec4(BitConverter.ToSingle(b), BitConverter.ToSingle(b.Slice(4)), BitConverter.ToSingle(b.Slice(8)), BitConverter.ToSingle(b.Slice(12)));
                case 8: return new MyeQuat(BitConverter.ToSingle(b), BitConverter.ToSingle(b.Slice(4)), BitConverter.ToSingle(b.Slice(8)), BitConverter.ToSingle(b.Slice(12)));
                case 9: return new MyeColor(BitConverter.ToSingle(b), BitConverter.ToSingle(b.Slice(4)), BitConverter.ToSingle(b.Slice(8)), BitConverter.ToSingle(b.Slice(12)));
            }
            return null;
        }

        // ======================= シリアライズ (JSON) =======================

        private string SerializeInstance(MyeScript inst)
        {
            var obj = new JsonObject();
            foreach (var f in FieldsOf(inst.GetType()))
            {
                int ft = FieldTypeOf(f.FieldType);
                object v = f.GetValue(inst);
                switch (ft)
                {
                    case 0: obj[f.Name] = (float)v; break;
                    case 1: obj[f.Name] = (int)v; break;
                    case 2: obj[f.Name] = (uint)v; break;
                    case 3: obj[f.Name] = Convert.ToUInt64(v); break;
                    case 4: obj[f.Name] = (bool)v; break;
                    case 5: { var a = (MyeVec2)v; obj[f.Name] = new JsonArray(a.X, a.Y); break; }
                    case 6: { var a = (MyeVec3)v; obj[f.Name] = new JsonArray(a.X, a.Y, a.Z); break; }
                    case 7: { var a = (MyeVec4)v; obj[f.Name] = new JsonArray(a.X, a.Y, a.Z, a.W); break; }
                    case 8: { var a = (MyeQuat)v; obj[f.Name] = new JsonArray(a.X, a.Y, a.Z, a.W); break; }
                    case 9: { var a = (MyeColor)v; obj[f.Name] = new JsonArray(a.R, a.G, a.B, a.A); break; }
                }
            }
            return obj.ToJsonString();
        }

        private void ApplyJson(MyeScript inst, string json)
        {
            JsonObject obj;
            try { obj = JsonNode.Parse(json) as JsonObject; }
            catch { return; }
            if (obj == null) return;
            foreach (var f in FieldsOf(inst.GetType()))
            {
                if (!obj.TryGetPropertyValue(f.Name, out var node) || node == null) continue;
                int ft = FieldTypeOf(f.FieldType);
                try
                {
                    switch (ft)
                    {
                        case 0: f.SetValue(inst, (float)node); break;
                        case 1: f.SetValue(inst, (int)node); break;
                        case 2: f.SetValue(inst, (uint)node); break;
                        case 3: f.SetValue(inst, (ulong)node); break;
                        case 4: f.SetValue(inst, (bool)node); break;
                        case 5: { var a = node.AsArray(); f.SetValue(inst, new MyeVec2((float)a[0], (float)a[1])); break; }
                        case 6: { var a = node.AsArray(); f.SetValue(inst, new MyeVec3((float)a[0], (float)a[1], (float)a[2])); break; }
                        case 7: { var a = node.AsArray(); f.SetValue(inst, new MyeVec4((float)a[0], (float)a[1], (float)a[2], (float)a[3])); break; }
                        case 8: { var a = node.AsArray(); f.SetValue(inst, new MyeQuat((float)a[0], (float)a[1], (float)a[2], (float)a[3])); break; }
                        case 9: { var a = node.AsArray(); f.SetValue(inst, new MyeColor((float)a[0], (float)a[1], (float)a[2], (float)a[3])); break; }
                    }
                }
                catch { /* 型不一致は無視 */ }
            }
        }

        private void SnapshotForReload()
        {
            foreach (var kv in _instances)
            {
                var inst = kv.Value;
                var t = inst.GetType();
                var key = (t.FullName, inst.SelfId.Index, inst.SelfId.Generation);
                _persist[key] = SerializeInstance(inst);
            }
        }

        private int Serialize(int handle, byte* buf, int bufLen)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return 0;
            return WriteUtf8(SerializeInstance(inst), buf, bufLen);
        }

        private void Deserialize(int handle, string json)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return;
            ApplyJson(inst, json);
        }

        // ======================= ユーティリティ =======================

        private static int WriteUtf8(string s, byte* buf, int bufLen)
        {
            var bytes = Encoding.UTF8.GetBytes(s ?? string.Empty);
            if (buf != null && bufLen > 0)
            {
                int n = Math.Min(bytes.Length, bufLen - 1);
                for (int i = 0; i < n; i++) buf[i] = bytes[i];
                buf[n] = 0;
            }
            return bytes.Length;
        }

        // ======================= ネイティブ callable エントリ (UnmanagedCallersOnly) =======================
        // 例外は決してネイティブに伝播させない (プロセスクラッシュを避ける)。

        [UnmanagedCallersOnly]
        public static int NativeCompile(byte* dirUtf8)
        {
            try { return Inst.Compile(Marshal.PtrToStringUTF8((IntPtr)dirUtf8) ?? ""); }
            catch (Exception ex) { Engine.Log("[csharp] Compile error: " + ex.Message, 3); return -1; }
        }

        [UnmanagedCallersOnly]
        public static int NativeGetTypeCount() => Inst._types.Count;

        [UnmanagedCallersOnly]
        public static int NativeGetTypeName(int idx, byte* buf, int bufLen)
        {
            if (idx < 0 || idx >= Inst._types.Count) return 0;
            return WriteUtf8(Inst._types[idx].FullName, buf, bufLen);
        }

        [UnmanagedCallersOnly]
        public static int NativeGetFieldCount(int typeIndex) => Inst.GetFieldCount(typeIndex);

        [UnmanagedCallersOnly]
        public static int NativeGetFieldInfo(int typeIndex, int fieldIndex, byte* nameBuf, int bufLen, int* outType)
            => Inst.GetFieldInfo(typeIndex, fieldIndex, nameBuf, bufLen, outType);

        [UnmanagedCallersOnly]
        public static int NativeCreateInstance(int typeIndex, MyeEntityId self) => Inst.CreateInstance(typeIndex, self);

        [UnmanagedCallersOnly]
        public static void NativeDestroyInstance(int handle) => Inst.DestroyInstance(handle);

        [UnmanagedCallersOnly]
        public static void NativeInvoke(int handle, int phase, float dt, ulong tick) => Inst.Invoke(handle, phase, dt, tick);

        [UnmanagedCallersOnly]
        public static void NativeInvokeTrigger(int handle, MyeEntityId other, int enter) => Inst.InvokeTrigger(handle, other, enter);

        [UnmanagedCallersOnly]
        public static void NativeInvokeCollision(int handle, MyeEntityId other, int kind, MyeVec3 normal) => Inst.InvokeCollision(handle, other, kind, normal);

        [UnmanagedCallersOnly]
        public static int NativeGetFieldValue(int handle, int fieldIndex, byte* buf, int bufLen)
            => Inst.GetFieldValue(handle, fieldIndex, buf, bufLen);

        [UnmanagedCallersOnly]
        public static int NativeSetFieldValue(int handle, int fieldIndex, byte* buf, int bufLen)
            => Inst.SetFieldValue(handle, fieldIndex, buf, bufLen);

        [UnmanagedCallersOnly]
        public static int NativeSerialize(int handle, byte* buf, int bufLen) => Inst.Serialize(handle, buf, bufLen);

        [UnmanagedCallersOnly]
        public static void NativeDeserialize(int handle, byte* json)
            => Inst.Deserialize(handle, Marshal.PtrToStringUTF8((IntPtr)json) ?? "");

        [UnmanagedCallersOnly]
        public static void NativeResetInstances() => Inst.ResetInstances();
    }
}
