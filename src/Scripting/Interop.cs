using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace MyeScripting
{
    // ---- ネイティブ POD とバイナリ互換の値型 (Shared/MathPod.h と一致) ----

    [StructLayout(LayoutKind.Sequential)]
    public struct MyeEntityId
    {
        public uint Index;
        public uint Generation;
        public bool IsNull => Index == 0xFFFFFFFFu;
        public static MyeEntityId Null => new MyeEntityId { Index = 0xFFFFFFFFu, Generation = 0 };
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MyeVec2
    {
        public float X, Y;
        public MyeVec2(float x, float y) { X = x; Y = y; }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MyeVec3
    {
        public float X, Y, Z;
        public MyeVec3(float x, float y, float z) { X = x; Y = y; Z = z; }
        public static MyeVec3 operator +(MyeVec3 a, MyeVec3 b) => new MyeVec3(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
        public static MyeVec3 operator -(MyeVec3 a, MyeVec3 b) => new MyeVec3(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
        public static MyeVec3 operator *(MyeVec3 a, float s) => new MyeVec3(a.X * s, a.Y * s, a.Z * s);
        public static MyeVec3 operator *(float s, MyeVec3 a) => new MyeVec3(a.X * s, a.Y * s, a.Z * s);
        public static readonly MyeVec3 Zero = new MyeVec3(0, 0, 0);
        public static readonly MyeVec3 One = new MyeVec3(1, 1, 1);
        public static readonly MyeVec3 Up = new MyeVec3(0, 1, 0);
        public static readonly MyeVec3 Right = new MyeVec3(1, 0, 0);
        public static readonly MyeVec3 Forward = new MyeVec3(0, 0, 1);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MyeVec4
    {
        public float X, Y, Z, W;
        public MyeVec4(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MyeQuat
    {
        public float X, Y, Z, W;
        public MyeQuat(float x, float y, float z, float w) { X = x; Y = y; Z = z; W = w; }
        public static readonly MyeQuat Identity = new MyeQuat(0, 0, 0, 1);
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MyeColor
    {
        public float R, G, B, A;
        public MyeColor(float r, float g, float b, float a) { R = r; G = g; B = b; A = a; }
    }

    // Shared/EngineAPI.h の MyeRaycastHit と同一レイアウト
    [StructLayout(LayoutKind.Sequential)]
    public struct MyeRaycastHit
    {
        public MyeEntityId Entity;
        public MyeVec3 Point;
        public MyeVec3 Normal;
        public float Distance;
    }

    // ---- ネイティブ C ABI テーブル (Shared/EngineAPI.h の MyeEngineApi と同一レイアウト) ----
    // フィールド順は EngineAPI.h と厳密に一致させること。
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct MyeEngineApi
    {
        public uint Version;
        public void* Engine;
        public delegate* unmanaged<void*, int, byte*, void> Log;
        public delegate* unmanaged<void*, byte, int> KeyDown;
        public delegate* unmanaged<void*, int, int> MouseButton;
        public delegate* unmanaged<void*, int*, int*, void> MousePos;
        public delegate* unmanaged<void*, byte*, MyeEntityId> CreateGameObject;
        public delegate* unmanaged<void*, MyeEntityId, void> DestroyGameObject;
        public delegate* unmanaged<void*, MyeEntityId, int> IsAlive;
        public delegate* unmanaged<void*, byte*, MyeEntityId> FindByName;
        public delegate* unmanaged<void*, MyeEntityId, MyeEntityId, void> SetParent;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3*, int> GetLocalPosition;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> SetLocalPosition;
        public delegate* unmanaged<void*, MyeEntityId, MyeQuat*, int> GetLocalRotation;
        public delegate* unmanaged<void*, MyeEntityId, MyeQuat, int> SetLocalRotation;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3*, int> GetLocalScale;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> SetLocalScale;
        public delegate* unmanaged<void*, float> RandomFloat01;
        public delegate* unmanaged<void*, float, float, float> RandomRange;
        public delegate* unmanaged<void*, MyeEntityId, byte*, int> AddComponentByName;
        public delegate* unmanaged<void*, MyeEntityId, byte*, byte*, int> SetMeshRenderer;
        // ---- gamepad / 物理 / オーディオ / シーン (v3) ----
        public delegate* unmanaged<void*, int> PadConnected;
        public delegate* unmanaged<void*, ushort, int> PadButton;
        public delegate* unmanaged<void*, MyeVec2*, MyeVec2*, void> PadSticks;
        public delegate* unmanaged<void*, float*, float*, void> PadTriggers;
        public delegate* unmanaged<void*, MyeVec3, MyeVec3, float, MyeRaycastHit*, int> Raycast;
        public delegate* unmanaged<void*, byte*, float, int> PlaySound;
        public delegate* unmanaged<void*, int, void> StopSound;
        public delegate* unmanaged<void*, byte*, void> LoadScene;
        // ---- 剛体操作 + 空間クエリ (v4、M28a) ----
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> AddForce;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> AddImpulse;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> AddTorque;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3*, int> GetVelocity;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> SetVelocity;
        public delegate* unmanaged<void*, MyeVec3, float, MyeEntityId*, int, int> OverlapSphere;
        public delegate* unmanaged<void*, MyeVec3, MyeVec3, MyeQuat, MyeEntityId*, int, int> OverlapBox;
        public delegate* unmanaged<void*, MyeVec3, MyeVec3, float, float, MyeRaycastHit*, int> SphereCast;
        // ---- キャラクターコントローラ + UI テキスト (v5、M29b)。宣言順 = ネイティブと一致 ----
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3, int> CharacterMove;
        public delegate* unmanaged<void*, MyeEntityId, float, int> CharacterJump;
        public delegate* unmanaged<void*, MyeEntityId, int> CharacterIsGrounded;
        public delegate* unmanaged<void*, MyeEntityId, MyeVec3*, int> CharacterGetVelocity;
        public delegate* unmanaged<void*, MyeEntityId, byte*, int> SetTextMeshText;
        // ---- エフェクト制御 (v6、M32f)。宣言順 = ネイティブと一致 ----
        public delegate* unmanaged<void*, MyeEntityId, int, int> EmitterBurst;
        public delegate* unmanaged<void*, MyeEntityId, int, int> SetEmitterPlaying;
        public delegate* unmanaged<void*, MyeEntityId, int> RestartEffect;
        public delegate* unmanaged<void*, byte*, MyeVec3, MyeEntityId, void> PlayEffect;
        // ---- v7 (M37)。宣言順 = ネイティブと一致 ----
        public delegate* unmanaged<void*, byte*, MyeVec3, MyeEntityId, ulong> Instantiate;
        public delegate* unmanaged<void*, ulong, MyeEntityId> FindByFileId;
        public delegate* unmanaged<void*, MyeEntityId, int, int, int> SetAnimatorParam;
        public delegate* unmanaged<void*, MyeEntityId, int, int*, int> GetAnimatorParam;
        public delegate* unmanaged<void*, MyeEntityId, byte*, int> SetUIText;
        public delegate* unmanaged<void*, MyeEntityId, float, int> SetUIFill;
        public delegate* unmanaged<void*, MyeEntityId, MyeColor, int> SetUIColor;
        public delegate* unmanaged<void*, MyeEntityId, int, int> SetUIFocused;
        public delegate* unmanaged<void*, MyeEntityId, int, MyeEntityId> UIFocusNav;
        public delegate* unmanaged<void*, MyeVec3, MyeVec3, MyeColor, void> DebugDrawLine;
        public delegate* unmanaged<void*, MyeVec3, MyeVec3, float, uint, MyeRaycastHit*, int> RaycastMasked;
        public delegate* unmanaged<void*, MyeVec3, float, uint, MyeEntityId*, int, int> OverlapSphereMasked;
        public delegate* unmanaged<void*, MyeVec3, MyeVec3, float, float, uint, MyeRaycastHit*, int> SphereCastMasked;
        // ---- オーディオ (v8、M45)。宣言順 = ネイティブと一致。**write-only** で、
        //      再生位置や再生中判定の読み取り API は意図的に存在しない (EngineAPI.h 参照) ----
        public delegate* unmanaged<void*, byte*, float, float, ulong> PlaySound2;
        public delegate* unmanaged<void*, byte*, MyeVec3, float, ulong> PlaySoundAt;
        public delegate* unmanaged<void*, ulong, float, void> StopVoice;
        public delegate* unmanaged<void*, ulong, float, void> SetVoiceVolume;
        public delegate* unmanaged<void*, ulong, float, void> SetVoicePitch;
        public delegate* unmanaged<void*, MyeEntityId, int> PlayAudioSource;
        public delegate* unmanaged<void*, MyeEntityId, float, int> StopAudioSource;
        public delegate* unmanaged<void*, byte*, float, void> SetBusVolume;
        public delegate* unmanaged<void*, byte*, float, int, void> PlayMusic;
        public delegate* unmanaged<void*, float, void> StopMusic;
        public delegate* unmanaged<void*, MyeEntityId, void> SetListenerEntity;
    }

    // ネイティブ ManagedHost が保持する関数ポインタ表。Bootstrap がここに書き込む。
    // ネイティブ側 MyeManagedVTable (ManagedHost.h) とフィールド順を一致させること。
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct ManagedVTable
    {
        public delegate* unmanaged<byte*, int> Compile;
        public delegate* unmanaged<int> GetTypeCount;
        public delegate* unmanaged<int, byte*, int, int> GetTypeName;
        public delegate* unmanaged<int, int> GetFieldCount;
        public delegate* unmanaged<int, int, byte*, int, int*, int> GetFieldInfo;
        public delegate* unmanaged<int, MyeEntityId, int> CreateInstance;
        public delegate* unmanaged<int, void> DestroyInstance;
        public delegate* unmanaged<int, int, float, ulong, void> Invoke;
        public delegate* unmanaged<int, MyeEntityId, int, void> InvokeTrigger;
        public delegate* unmanaged<int, int, byte*, int, int> GetFieldValue;
        public delegate* unmanaged<int, int, byte*, int, int> SetFieldValue;
        public delegate* unmanaged<int, byte*, int, int> Serialize;
        public delegate* unmanaged<int, byte*, void> Deserialize;
        public delegate* unmanaged<void> ResetInstances;
        // M28c 末尾追加 (ManagedHost.h の MyeManagedVTable と同順)。kind: 0=enter 1=stay 2=exit
        public delegate* unmanaged<int, MyeEntityId, int, MyeVec3, void> InvokeCollision;
    }

    // native → managed の起動引数 (ManagedHost.cpp の MyeBootstrapArgs と一致)
    [StructLayout(LayoutKind.Sequential)]
    internal unsafe struct BootstrapArgs
    {
        public MyeEngineApi* Api;
        public ManagedVTable* OutVtable;
    }

    // MyeEngineApi をラップして C# から呼びやすくする静的窓口。
    // ネイティブが所有する api テーブルへのポインタを保持する (engine コンテキストは毎 tick 更新される)。
    internal static unsafe class Engine
    {
        private static MyeEngineApi* _api;

        public static bool Ready => _api != null;

        public static void Init(MyeEngineApi* api) { _api = api; }

        // UTF-8 (null 終端) にして固定領域から呼ぶための一時バッファヘルパ
        private static byte[] Utf8(string s)
        {
            int n = Encoding.UTF8.GetByteCount(s);
            var b = new byte[n + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            b[n] = 0;
            return b;
        }

        public static void Log(string message, int level = 1)
        {
            if (_api == null) return;
            var b = Utf8(message);
            fixed (byte* p = b) { _api->Log(_api->Engine, level, p); }
        }

        public static bool KeyDown(byte vk) => _api != null && _api->KeyDown(_api->Engine, vk) != 0;
        public static bool MouseButton(int button) => _api != null && _api->MouseButton(_api->Engine, button) != 0;

        public static void MousePos(out int x, out int y)
        {
            x = 0; y = 0;
            if (_api == null) return;
            fixed (int* px = &x) fixed (int* py = &y) { _api->MousePos(_api->Engine, px, py); }
        }

        public static float RandomFloat01() => _api != null ? _api->RandomFloat01(_api->Engine) : 0.0f;
        public static float RandomRange(float lo, float hi) => _api != null ? _api->RandomRange(_api->Engine, lo, hi) : lo;

        public static MyeEntityId CreateGameObject(string name)
        {
            if (_api == null) return MyeEntityId.Null;
            var b = Utf8(name);
            fixed (byte* p = b) { return _api->CreateGameObject(_api->Engine, p); }
        }

        public static void DestroyGameObject(MyeEntityId id) { if (_api != null) _api->DestroyGameObject(_api->Engine, id); }
        public static bool IsAlive(MyeEntityId id) => _api != null && _api->IsAlive(_api->Engine, id) != 0;

        public static MyeEntityId FindByName(string name)
        {
            if (_api == null) return MyeEntityId.Null;
            var b = Utf8(name);
            fixed (byte* p = b) { return _api->FindByName(_api->Engine, p); }
        }

        public static void SetParent(MyeEntityId child, MyeEntityId parent) { if (_api != null) _api->SetParent(_api->Engine, child, parent); }

        public static MyeVec3 GetLocalPosition(MyeEntityId id)
        {
            MyeVec3 v = default;
            if (_api != null) _api->GetLocalPosition(_api->Engine, id, &v);
            return v;
        }
        public static void SetLocalPosition(MyeEntityId id, MyeVec3 v) { if (_api != null) _api->SetLocalPosition(_api->Engine, id, v); }

        public static MyeQuat GetLocalRotation(MyeEntityId id)
        {
            MyeQuat q = MyeQuat.Identity;
            if (_api != null) _api->GetLocalRotation(_api->Engine, id, &q);
            return q;
        }
        public static void SetLocalRotation(MyeEntityId id, MyeQuat q) { if (_api != null) _api->SetLocalRotation(_api->Engine, id, q); }

        public static MyeVec3 GetLocalScale(MyeEntityId id)
        {
            MyeVec3 v = MyeVec3.One;
            if (_api != null) _api->GetLocalScale(_api->Engine, id, &v);
            return v;
        }
        public static void SetLocalScale(MyeEntityId id, MyeVec3 v) { if (_api != null) _api->SetLocalScale(_api->Engine, id, v); }

        public static bool AddComponent(MyeEntityId id, string name)
        {
            if (_api == null) return false;
            var b = Utf8(name);
            fixed (byte* p = b) { return _api->AddComponentByName(_api->Engine, id, p) != 0; }
        }

        public static bool SetMeshRenderer(MyeEntityId id, string meshKey, string materialKey)
        {
            if (_api == null) return false;
            var mb = Utf8(meshKey);
            var cb = Utf8(materialKey);
            fixed (byte* mp = mb) fixed (byte* cp = cb) { return _api->SetMeshRenderer(_api->Engine, id, mp, cp) != 0; }
        }

        // ---- 剛体操作 (v4、M28a) ----
        public static bool AddForce(MyeEntityId id, MyeVec3 force)
            => _api != null && _api->AddForce(_api->Engine, id, force) != 0;
        public static bool AddImpulse(MyeEntityId id, MyeVec3 impulse)
            => _api != null && _api->AddImpulse(_api->Engine, id, impulse) != 0;
        public static bool AddTorque(MyeEntityId id, MyeVec3 torque)
            => _api != null && _api->AddTorque(_api->Engine, id, torque) != 0;

        public static MyeVec3 GetVelocity(MyeEntityId id)
        {
            MyeVec3 v = default;
            if (_api != null) _api->GetVelocity(_api->Engine, id, &v);
            return v;
        }
        public static bool SetVelocity(MyeEntityId id, MyeVec3 v)
            => _api != null && _api->SetVelocity(_api->Engine, id, v) != 0;

        // ---- キャラクターコントローラ (v5、M29b) ----
        public static bool CharacterMove(MyeEntityId id, MyeVec3 move)
            => _api != null && _api->CharacterMove(_api->Engine, id, move) != 0;
        public static bool CharacterJump(MyeEntityId id, float speed)
            => _api != null && _api->CharacterJump(_api->Engine, id, speed) != 0;
        public static bool CharacterIsGrounded(MyeEntityId id)
            => _api != null && _api->CharacterIsGrounded(_api->Engine, id) != 0;
        public static MyeVec3 CharacterGetVelocity(MyeEntityId id)
        {
            MyeVec3 v = default;
            if (_api != null) _api->CharacterGetVelocity(_api->Engine, id, &v);
            return v;
        }

        // ---- UI テキスト (v5 で予約、M29c の TextMesh で実装) ----
        public static bool SetTextMeshText(MyeEntityId id, string text)
        {
            if (_api == null) return false;
            var b = Utf8(text ?? "");
            fixed (byte* p = b) { return _api->SetTextMeshText(_api->Engine, id, p) != 0; }
        }

        // ---- エフェクト制御 (v6、M32f) ----
        public static bool EmitterBurst(MyeEntityId id, int count)
            => _api != null && _api->EmitterBurst(_api->Engine, id, count) != 0;
        public static bool SetEmitterPlaying(MyeEntityId id, bool playing)
            => _api != null && _api->SetEmitterPlaying(_api->Engine, id, playing ? 1 : 0) != 0;
        public static bool RestartEffect(MyeEntityId id)
            => _api != null && _api->RestartEffect(_api->Engine, id) != 0;
        public static void PlayEffect(string prefabKey, MyeVec3 pos, MyeEntityId parent = default)
        {
            if (_api == null) return;
            var b = Utf8(prefabKey ?? "");
            fixed (byte* p = b) { _api->PlayEffect(_api->Engine, p, pos, parent); }
        }

        // ---- 空間クエリ ----
        public static bool Raycast(MyeVec3 origin, MyeVec3 dir, float maxDist, out MyeRaycastHit hit)
        {
            hit = default;
            if (_api == null) return false;
            fixed (MyeRaycastHit* p = &hit) { return _api->Raycast(_api->Engine, origin, dir, maxDist, p) != 0; }
        }

        // ---- v7 (M37) ----
        // Instantiate: 生成は tick 末。戻り値 = 予約されたルート fileId (0 = 失敗)。
        // 次 tick 以降に FindByFileId で EntityID に解決する
        public static ulong Instantiate(string prefabKey, MyeVec3 pos, MyeEntityId parent = default)
        {
            if (_api == null) return 0;
            var b = Utf8(prefabKey ?? "");
            fixed (byte* p = b) { return _api->Instantiate(_api->Engine, p, pos, parent); }
        }
        public static MyeEntityId FindByFileId(ulong fileId)
            => _api != null ? _api->FindByFileId(_api->Engine, fileId) : default;

        public static bool SetAnimatorParam(MyeEntityId id, int index, int value)
            => _api != null && _api->SetAnimatorParam(_api->Engine, id, index, value) != 0;
        public static int GetAnimatorParam(MyeEntityId id, int index)
        {
            int v = 0;
            if (_api != null) _api->GetAnimatorParam(_api->Engine, id, index, &v);
            return v;
        }

        public static bool SetUIText(MyeEntityId id, string text)
        {
            if (_api == null) return false;
            var b = Utf8(text ?? "");
            fixed (byte* p = b) { return _api->SetUIText(_api->Engine, id, p) != 0; }
        }
        public static bool SetUIFill(MyeEntityId id, float amount)
            => _api != null && _api->SetUIFill(_api->Engine, id, amount) != 0;
        public static bool SetUIColor(MyeEntityId id, MyeColor color)
            => _api != null && _api->SetUIColor(_api->Engine, id, color) != 0;
        public static bool SetUIFocused(MyeEntityId id, bool focused)
            => _api != null && _api->SetUIFocused(_api->Engine, id, focused ? 1 : 0) != 0;
        // dir: 0=上 1=下 2=左 3=右。候補が無ければ current をそのまま返す
        public static MyeEntityId UIFocusNav(MyeEntityId current, int dir)
            => _api != null ? _api->UIFocusNav(_api->Engine, current, dir) : current;

        public static void DebugDrawLine(MyeVec3 a, MyeVec3 b, MyeColor color)
        {
            if (_api != null) _api->DebugDrawLine(_api->Engine, a, b, color);
        }

        public static bool RaycastMasked(MyeVec3 origin, MyeVec3 dir, float maxDist, uint mask,
                                         out MyeRaycastHit hit)
        {
            hit = default;
            if (_api == null) return false;
            fixed (MyeRaycastHit* p = &hit)
            {
                return _api->RaycastMasked(_api->Engine, origin, dir, maxDist, mask, p) != 0;
            }
        }
        public static int OverlapSphereMasked(MyeVec3 center, float radius, uint mask,
                                              MyeEntityId[] outEntities)
        {
            if (_api == null) return 0;
            fixed (MyeEntityId* p = outEntities)
            {
                return _api->OverlapSphereMasked(_api->Engine, center, radius, mask, p,
                                                 outEntities?.Length ?? 0);
            }
        }
        public static bool SphereCastMasked(MyeVec3 origin, MyeVec3 dir, float radius,
                                            float maxDist, uint mask, out MyeRaycastHit hit)
        {
            hit = default;
            if (_api == null) return false;
            fixed (MyeRaycastHit* p = &hit)
            {
                return _api->SphereCastMasked(_api->Engine, origin, dir, radius, maxDist, mask, p)
                       != 0;
            }
        }
    }
}
