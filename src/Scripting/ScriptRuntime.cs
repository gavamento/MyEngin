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
        // BT の C# タスク (ノード CsTask)。クラスは [BtTask] を付けた MyeBtTask 派生 (FullName で引く)。
        // インスタンスは (owner の index, generation, 木のノードの添字) ごとに 1 つ。ネイティブの BT 表には入らない (決定論の保証外)
        private readonly Dictionary<string, Type> _btTaskTypes = new Dictionary<string, Type>(StringComparer.Ordinal);
        private readonly Dictionary<(uint, uint, int), MyeBtTask> _btTasks = new Dictionary<(uint, uint, int), MyeBtTask>();
        private readonly HashSet<(string, string)> _btFieldWarned = new HashSet<(string, string)>(); // (クラス, フィールド) ごとに 1 回だけ警告
        private string _btCatalog; // BtTaskCatalog の結果 (リロードで作り直す)
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
            // 古い ALC の型を掴んだままだと Unload が終わらないので、タスクのインスタンスも捨てる (動いていた分は次の tick に OnStart からやり直す)
            _btTasks.Clear();
            _btTaskTypes.Clear();
            _btFieldWarned.Clear();
            _btCatalog = null;
            foreach (var t in allTypes)
            {
                if (typeof(MyeScript).IsAssignableFrom(t) && !t.IsAbstract)
                {
                    _types.Add(t);
                }
                else if (typeof(MyeBtTask).IsAssignableFrom(t) && !t.IsAbstract)
                {
                    if (t.IsDefined(typeof(BtTaskAttribute), false))
                    {
                        _btTaskTypes[t.FullName] = t;
                    }
                    else
                    {
                        Engine.Log("[csharp] " + t.FullName + " derives MyeBtTask but has no [BtTask]; ignored", 2);
                    }
                }
            }
            _types.Sort((a, b) => string.CompareOrdinal(a.FullName, b.FullName));

            var old = _alc;
            _alc = newAlc;
            old?.Unload();

            Engine.Log("[csharp] compiled " + sources.Count + " file(s), " + _types.Count + " script type(s), "
                       + _btTaskTypes.Count + " BT task type(s)");
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
                // M70d: ネイティブから来た tick 番号をインスタンスへ渡す。
                // C# レーンにはこれ以外に決定論的な時間カウンタが無い
                inst.Tick = tick;
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

        private void InvokeBreak(int handle, MyeEntityId piece, MyeVec3 point, float impulse)
        {
            if (!_instances.TryGetValue(handle, out var inst)) return;
            try
            {
                inst.OnBreak(new MyeEntity(piece), point, impulse);
            }
            catch (Exception ex)
            {
                Engine.Log("[csharp] " + inst.GetType().Name + ".OnBreak threw: " + ex.Message, 3);
            }
        }

        // BT の C# タスクを 1 手進める。phase: 0 = enter (新しいインスタンスで OnStart)、1 = tick (OnTick。インスタンスが無ければ
        // enter と同じ = リロード後の再開)、2 = abort (OnAbort して捨てる)。戻り値は MyeBtStatus、-1 = そのクラスが無い。
        // Success / Failure と例外 (Failure) でインスタンスを捨てる
        private int RunBtTask(MyeEntityId owner, int nodeIndex, string className, int phase, ulong tick, string fieldsJson)
        {
            if (!_btTaskTypes.TryGetValue(className, out var type)) return -1;
            var key = (owner.Index, owner.Generation, nodeIndex);
            MyeBtTask task = null;
            try
            {
                if (phase == 2)
                {
                    if (_btTasks.Remove(key, out task))
                    {
                        task.Tick = tick;
                        task.OnAbort();
                    }
                    return (int)MyeBtStatus.Failure;
                }
                bool starting = phase == 0 || !_btTasks.TryGetValue(key, out task);
                if (starting)
                {
                    // ノードに入るたびに掃除する: 木ごと消えた (エンティティ破棄) タスクのインスタンスの取りこぼしを落とす
                    if (phase == 0) DropDeadBtTasks();
                    task = (MyeBtTask)Activator.CreateInstance(type);
                    task.SelfId = owner;
                    ApplyBtFields(task, className, fieldsJson);
                    _btTasks[key] = task;
                }
                task.Tick = tick;
                var status = starting ? task.OnStart() : task.OnTick();
                if (status != MyeBtStatus.Running) _btTasks.Remove(key);
                return (int)status;
            }
            catch (Exception ex)
            {
                _btTasks.Remove(key);
                Engine.Log("[csharp] BT task " + className + (phase == 2 ? ".OnAbort" : "") + " threw: " + ex.Message, 3);
                return (int)MyeBtStatus.Failure;
            }
        }

        // ---- BT の C# タスクのフィールド (ノードの "fields" と、BT 窓のフィールド欄の記述子) ----

        // BT 窓に出せるフィールドの型名。それ以外の型は一覧に出さない
        private static string BtFieldTypeName(Type t)
        {
            if (t == typeof(bool)) return "bool";
            if (t == typeof(int)) return "int";
            if (t == typeof(float)) return "float";
            if (t == typeof(string)) return "string";
            if (t == typeof(MyeVec3)) return "vec3";
            return null;
        }

        private static FieldInfo[] BtFieldsOf(Type t)
        {
            return t.GetFields(BindingFlags.Public | BindingFlags.Instance)
                    .Where(f => !f.IsInitOnly && BtFieldTypeName(f.FieldType) != null)
                    .OrderBy(f => f.Name, StringComparer.Ordinal)
                    .ToArray();
        }

        // 値を JSON にする。Json が書けない NaN / 無限大は 0
        private static JsonNode BtFieldToJson(object value, string typeName)
        {
            switch (typeName)
            {
                case "bool": return JsonValue.Create((bool)value);
                case "int": return JsonValue.Create((int)value);
                case "float": { float f = (float)value; return JsonValue.Create(float.IsFinite(f) ? f : 0f); }
                case "string": return JsonValue.Create((string)value ?? string.Empty);
                default:
                {
                    var v = (MyeVec3)value;
                    float Finite(float x) => float.IsFinite(x) ? x : 0f;
                    return new JsonArray(JsonValue.Create(Finite(v.X)), JsonValue.Create(Finite(v.Y)), JsonValue.Create(Finite(v.Z)));
                }
            }
        }

        // [BtTask] クラスの一覧 (JSON)。クラスごとに FullName と、フィールドの (名前, 型, 既定値)。
        // 既定値はインスタンスを 1 つ作って読む。作れないクラスは型の既定値 (0 / false / "")
        private string BtTaskCatalog()
        {
            if (_btCatalog != null) return _btCatalog;
            var classes = new JsonArray();
            foreach (var kv in _btTaskTypes.OrderBy(k => k.Key, StringComparer.Ordinal))
            {
                object sample = null;
                try { sample = Activator.CreateInstance(kv.Value); }
                catch (Exception ex) { Engine.Log("[csharp] BT task " + kv.Key + " could not be instantiated for its field defaults: " + ex.Message, 2); }
                var fields = new JsonArray();
                foreach (var f in BtFieldsOf(kv.Value))
                {
                    string typeName = BtFieldTypeName(f.FieldType);
                    object value = sample != null ? f.GetValue(sample) : null;
                    if (value == null && f.FieldType.IsValueType) value = Activator.CreateInstance(f.FieldType);
                    fields.Add(new JsonObject
                    {
                        ["name"] = f.Name,
                        ["type"] = typeName,
                        ["default"] = BtFieldToJson(value, typeName),
                    });
                }
                classes.Add(new JsonObject { ["class"] = kv.Key, ["fields"] = fields });
            }
            _btCatalog = classes.ToJsonString();
            return _btCatalog;
        }

        private void WarnBtField(string className, string field, string message)
        {
            if (!_btFieldWarned.Add((className, field))) return;
            Engine.Log("[csharp] BT task " + className + " field '" + field + "': " + message + "; the value is ignored", 2);
        }

        // ノードの "fields" (JSON オブジェクト) を、作った直後のインスタンスのフィールドへ書く。
        // 名前が無い・型が違う値は書かない (警告は (クラス, フィールド) ごとに 1 回)
        private void ApplyBtFields(MyeBtTask task, string className, string fieldsJson)
        {
            if (string.IsNullOrEmpty(fieldsJson)) return;
            JsonObject obj;
            try { obj = JsonNode.Parse(fieldsJson) as JsonObject; }
            catch { return; }
            if (obj == null) return;
            var fields = BtFieldsOf(task.GetType());
            foreach (var kv in obj)
            {
                var f = fields.FirstOrDefault(x => x.Name == kv.Key);
                if (f == null) { WarnBtField(className, kv.Key, "no such public field (bool / int / float / string / MyeVec3)"); continue; }
                if (!TryReadBtField(kv.Value, f.FieldType, out var value)) { WarnBtField(className, kv.Key, "the value does not fit the field type"); continue; }
                f.SetValue(task, value);
            }
        }

        private static bool TryReadBtField(JsonNode node, Type type, out object value)
        {
            value = null;
            if (type == typeof(MyeVec3))
            {
                if (node is not JsonArray a || a.Count != 3) return false;
                var c = new float[3];
                for (int i = 0; i < 3; i++)
                {
                    if (!(a[i] is JsonValue jv) || !jv.TryGetValue<double>(out var d)) return false;
                    c[i] = (float)d;
                }
                value = new MyeVec3(c[0], c[1], c[2]);
                return true;
            }
            if (node is not JsonValue v) return false;
            if (type == typeof(bool)) { if (!v.TryGetValue<bool>(out var b)) return false; value = b; return true; }
            if (type == typeof(string)) { if (!v.TryGetValue<string>(out var s)) return false; value = s; return true; }
            if (!v.TryGetValue<double>(out var n)) return false;
            if (type == typeof(float)) { value = (float)n; return true; }
            // int: 3.0 のような整数値の小数も受ける (JSON の数は整数・小数の区別を保証しない)
            if (n != Math.Floor(n) || n < int.MinValue || n > int.MaxValue) return false;
            value = (int)n;
            return true;
        }

        private void DropDeadBtTasks()
        {
            List<(uint, uint, int)> dead = null;
            foreach (var kv in _btTasks)
            {
                if (!Engine.IsAlive(kv.Value.SelfId)) (dead ??= new List<(uint, uint, int)>()).Add(kv.Key);
            }
            if (dead == null) return;
            foreach (var key in dead) _btTasks.Remove(key);
        }

        private static string PhaseName(int p) => p == 0 ? "Start" : (p == 1 ? "Update" : "LateUpdate");

        private void ResetInstances()
        {
            _instances.Clear();
            _btTasks.Clear(); // シーン遷移: 旧シーンの木のタスク (エンティティが別物になる)
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
        public static int NativeBtTask(MyeEntityId owner, int nodeIndex, byte* classUtf8, int phase, ulong tick, byte* fieldsUtf8)
        {
            try
            {
                return Inst.RunBtTask(owner, nodeIndex, Marshal.PtrToStringUTF8((IntPtr)classUtf8) ?? "", phase, tick,
                                      Marshal.PtrToStringUTF8((IntPtr)fieldsUtf8) ?? "");
            }
            catch (Exception ex) { Engine.Log("[csharp] BtTask error: " + ex.Message, 3); return (int)MyeBtStatus.Failure; }
        }

        [UnmanagedCallersOnly]
        public static int NativeBtTaskCatalog(byte* buf, int bufLen)
        {
            try { return WriteUtf8(Inst.BtTaskCatalog(), buf, bufLen); }
            catch (Exception ex) { Engine.Log("[csharp] BtTaskCatalog error: " + ex.Message, 3); return 0; }
        }

        [UnmanagedCallersOnly]
        public static void NativeInvokeBreak(int handle, MyeEntityId piece, MyeVec3 point, float impulse) => Inst.InvokeBreak(handle, piece, point, impulse);

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
