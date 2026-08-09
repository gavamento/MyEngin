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
        // ---- 部位 (ソケット) クエリ (v9、M48h)。宣言順 = ネイティブと一致 ----
        public delegate* unmanaged<void*, MyeEntityId, byte*, MyeEntityId> FindPart;
        public delegate* unmanaged<void*, MyeEntityId, ulong, MyeEntityId*, int, int> FindPartsByTag;
        // ---- 部位ボリューム レイキャスト (v10、M49)。宣言順 = ネイティブと一致 ----
        public delegate* unmanaged<void*, MyeEntityId, ulong, MyeVec3, MyeVec3, float,
            MyeRaycastHit*, int> RaycastParts;
        // ---- 汎用フィールドアクセス (v11、M50d)。宣言順 = ネイティブと一致 ----
        public delegate* unmanaged<void*, MyeEntityId, ulong, ulong, void*, int, int*, int>
            GetComponentField;
        public delegate* unmanaged<void*, MyeEntityId, ulong, ulong, void*, int, int>
            SetComponentField;
        // ---- v12 (M51h): 入力アクション / UI 拡張 / ゲームフロー / パッド振動。
        //      宣言順 = ネイティブと一致 (tools\check_rules.ps1 規則 11 が機械照合) ----
        public delegate* unmanaged<void*, int> GetMouseWheel;
        public delegate* unmanaged<void*, MyeEntityId, float, float, float, float, int> SetUIRect;
        public delegate* unmanaged<void*, MyeEntityId, int, int, int, int, int, int> SetUILayout;
        public delegate* unmanaged<void*, MyeEntityId, byte*, int> SetUITexture;
        public delegate* unmanaged<void*, float, float, MyeEntityId> UIHitTest;
        public delegate* unmanaged<void*, ulong, uint> GetActionState;
        public delegate* unmanaged<void*, ulong, float> GetAxisValue;
        public delegate* unmanaged<void*, int, int, void> SetTimeControl;
        public delegate* unmanaged<void*, int*, int*, void> GetTimeControl;
        public delegate* unmanaged<void*, ulong, void*, int, int> PersistSet;
        public delegate* unmanaged<void*, ulong, void*, int, int> PersistGet;
        public delegate* unmanaged<void*, int, void> SaveGame;
        public delegate* unmanaged<void*, int, void> LoadGame;
        public delegate* unmanaged<void*, float, float, void> SetPadVibration;
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

        // ---- オーディオ (v8、M45g) ----
        // ★**write-only**。再生位置・再生中判定・バス音量の取得 API は存在せず、今後も追加しない
        //   (実時間で動くオーディオを sim が読むとリプレイが壊れるため。EngineAPI.h 参照)。
        //
        // soundKey は .sound.json の名前 (= ファイル名から ".sound" を除いた stem) が第一候補で、
        // 見つからなければ素の .wav / .ogg のファイル名 stem で引かれる。
        // 戻り値のハンドルは呼出時に予約される単調増加値で、リプレイでも同じ値になる (0 = 失敗)

        public static ulong PlaySound(string soundKey, float volume = 1.0f, float pitch = 1.0f)
        {
            if (_api == null) return 0;
            var b = Utf8(soundKey ?? "");
            fixed (byte* p = b) { return _api->PlaySound2(_api->Engine, p, volume, pitch); }
        }
        // ワールド座標で鳴らす。2D 設定の音でも 3D に載る (座標を渡した以上は定位させる)
        public static ulong PlaySoundAt(string soundKey, MyeVec3 worldPos, float volume = 1.0f)
        {
            if (_api == null) return 0;
            var b = Utf8(soundKey ?? "");
            fixed (byte* p = b) { return _api->PlaySoundAt(_api->Engine, p, worldPos, volume); }
        }
        // fadeSeconds > 0 で音量を落としてから止める。鳴り終わったハンドルへの指定は無視される
        public static void StopVoice(ulong handle, float fadeSeconds = 0.0f)
        {
            if (_api != null) _api->StopVoice(_api->Engine, handle, fadeSeconds);
        }
        public static void SetVoiceVolume(ulong handle, float volume)
        {
            if (_api != null) _api->SetVoiceVolume(_api->Engine, handle, volume);
        }
        public static void SetVoicePitch(ulong handle, float pitch)
        {
            if (_api != null) _api->SetVoicePitch(_api->Engine, handle, pitch);
        }
        // AudioSource コンポーネントを持つエンティティの再生 / 停止。非所持なら false
        public static bool PlayAudioSource(MyeEntityId id)
            => _api != null && _api->PlayAudioSource(_api->Engine, id) != 0;
        public static bool StopAudioSource(MyeEntityId id, float fadeSeconds = 0.0f)
            => _api != null && _api->StopAudioSource(_api->Engine, id, fadeSeconds) != 0;
        // busName は "Master" / "BGM" / "SE" / "UI" 等 (.mixer.json のバス名。大文字小文字無視)
        public static void SetBusVolume(string busName, float volume)
        {
            if (_api == null) return;
            var b = Utf8(busName ?? "");
            fixed (byte* p = b) { _api->SetBusVolume(_api->Engine, p, volume); }
        }
        // BGM。同じ曲を再指定しても頭出しし直さない (シーンを跨いで鳴り続ける)
        public static void PlayMusic(string soundKey, float fadeSeconds = 1.0f, bool loop = true)
        {
            if (_api == null) return;
            var b = Utf8(soundKey ?? "");
            fixed (byte* p = b) { _api->PlayMusic(_api->Engine, p, fadeSeconds, loop ? 1 : 0); }
        }
        public static void StopMusic(float fadeSeconds = 1.0f)
        {
            if (_api != null) _api->StopMusic(_api->Engine, fadeSeconds);
        }
        // 3D リスナーを固定する。default (null id) で自動 (AudioListener → primary カメラ) に戻る
        public static void SetListenerEntity(MyeEntityId id)
        {
            if (_api != null) _api->SetListenerEntity(_api->Engine, id);
        }

        // ---- 部位 (ソケット) (v9、M48h) ----
        // 「アセットが公開した取り付け位置」を名前パス / タグで引く。取り付けは SetParent。
        // ★C# レーンはリプレイ/ワールドハッシュの対象外 (別レーン) — 引く行為自体は
        //   決定論だが、C# から sim を動かす経路は record/verify で回らないので注意

        // タグ名 → タグ ID。FNV-1a 64bit で、ネイティブ Engine/Core/Hash.h の HashStr と
        // 同一の定数 (バイト列は UTF-8)。ここがズレると同じ名前で別 ID を引くことになる
        public static ulong PartTag(string name)
        {
            if (string.IsNullOrEmpty(name)) return 0ul;
            ulong h = 14695981039346656037ul;
            foreach (byte b in Encoding.UTF8.GetBytes(name))
            {
                h ^= b;
                h *= 1099511628211ul;
            }
            return h;
        }

        // root から '/' 区切りの名前パスで降下する ("Hips/LegL")。空パスは root 自身。
        // 見つからなければ MyeEntityId.Null
        public static MyeEntityId FindPart(MyeEntityId root, string path)
        {
            if (_api == null) return MyeEntityId.Null;
            var b = Utf8(path ?? "");
            fixed (byte* p = b) { return _api->FindPart(_api->Engine, root, p); }
        }

        // タグ一致の部位を DFS 順に outParts へ書く。戻り値は **切り捨て前の総ヒット数**
        // (戻り値 > outParts.Length ならバッファが足りていない)
        public static int FindPartsByTag(MyeEntityId root, string tagName, MyeEntityId[] outParts)
        {
            if (_api == null) return 0;
            fixed (MyeEntityId* p = outParts)
            {
                return _api->FindPartsByTag(_api->Engine, root, PartTag(tagName), p,
                                            outParts?.Length ?? 0);
            }
        }

        // 部位ボリューム (Part + PartBounds) へのレイキャスト (v10、M49)。
        // root = Null でシーン全体、tagName null/空で全部位。dir は正規化済みであること
        public static bool RaycastParts(MyeEntityId root, string tagName, MyeVec3 origin,
                                        MyeVec3 dir, float maxDist, out MyeRaycastHit hit)
        {
            hit = default;
            if (_api == null) return false;
            fixed (MyeRaycastHit* p = &hit)
            {
                return _api->RaycastParts(_api->Engine, root, PartTag(tagName), origin, dir,
                                          maxDist, p) != 0;
            }
        }

        // ---- 汎用フィールドアクセス (v11、M50d) ----
        // comp/field は名前の FNV-1a 64bit (スキーマ codegen の生成定数を使う)。値コピーのみ。
        // C# スクリプト状態 (NoHash = 非決定論レーン) はエンジン側で遮断され失敗する。
        // Shared/ScriptTypes.h の MyeFieldType と同値 (文字列判定に使う分だけ)
        private const int FieldTypeString64 = 12;
        private const int FieldTypeString256 = 14;

        public static bool TryGetField<T>(MyeEntityId id, ulong compHash, ulong fieldHash,
                                          out T value) where T : unmanaged
        {
            value = default;
            if (_api == null) return false;
            T v = default;
            int type;
            int got = _api->GetComponentField(_api->Engine, id, compHash, fieldHash, &v,
                                              sizeof(T), &type);
            if (got != sizeof(T)) return false; // サイズ不一致 = 型違い (エンジン側でも拒否)
            value = v;
            return true;
        }

        public static bool SetField<T>(MyeEntityId id, ulong compHash, ulong fieldHash, T value)
            where T : unmanaged
        {
            if (_api == null) return false;
            return _api->SetComponentField(_api->Engine, id, compHash, fieldHash, &value,
                                           sizeof(T)) != 0;
        }

        // 文字列 (String64/256)。Get は終端までを UTF-8 で復元、Set はエンジン側が
        // 尾部ゼロ埋め + 終端を保証する (String64 ハッシュ罠対策)
        public static bool TryGetFieldString(MyeEntityId id, ulong compHash, ulong fieldHash,
                                             out string value)
        {
            value = null;
            if (_api == null) return false;
            var buf = new byte[256]; // String64/256 の両方を受ける (実サイズは戻り値)
            int type;
            int got;
            fixed (byte* p = buf)
            {
                got = _api->GetComponentField(_api->Engine, id, compHash, fieldHash, p,
                                              buf.Length, &type);
            }
            if (got <= 0 || (type != FieldTypeString64 && type != FieldTypeString256))
                return false;
            int n = 0;
            while (n < got && buf[n] != 0) n++;
            value = Encoding.UTF8.GetString(buf, 0, n);
            return true;
        }

        public static bool SetFieldString(MyeEntityId id, ulong compHash, ulong fieldHash,
                                          string value)
        {
            if (_api == null) return false;
            var b = Utf8(value ?? "");
            fixed (byte* p = b)
            {
                return _api->SetComponentField(_api->Engine, id, compHash, fieldHash, p,
                                               b.Length) != 0;
            }
        }

        // ---- v3 の回収 (M51h): LoadScene / gamepad ----
        // これまでスロットはあったが C# へ公開していなかった分。
        // LoadScene は tick 末に遅延ロードされる (パスは assets 相対)
        public static void LoadScene(string scenePath)
        {
            if (_api == null) return;
            var b = Utf8(scenePath ?? "");
            fixed (byte* p = b) { _api->LoadScene(_api->Engine, p); }
        }
        public static bool PadConnected() => _api != null && _api->PadConnected(_api->Engine) != 0;
        public static bool PadButton(ushort buttonMask)
            => _api != null && _api->PadButton(_api->Engine, buttonMask) != 0;
        public static void PadSticks(out MyeVec2 left, out MyeVec2 right)
        {
            left = default; right = default;
            if (_api == null) return;
            fixed (MyeVec2* pl = &left) fixed (MyeVec2* pr = &right)
            {
                _api->PadSticks(_api->Engine, pl, pr);
            }
        }
        public static void PadTriggers(out float left, out float right)
        {
            left = 0; right = 0;
            if (_api == null) return;
            fixed (float* pl = &left) fixed (float* pr = &right)
            {
                _api->PadTriggers(_api->Engine, pl, pr);
            }
        }

        // ---- v4 の回収 (M51h): 無印 Overlap / SphereCast (マスク無し全レイヤー版) ----
        public static int OverlapSphere(MyeVec3 center, float radius, MyeEntityId[] outEntities)
        {
            if (_api == null) return 0;
            fixed (MyeEntityId* p = outEntities)
            {
                return _api->OverlapSphere(_api->Engine, center, radius, p,
                                           outEntities?.Length ?? 0);
            }
        }
        public static int OverlapBox(MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                                     MyeEntityId[] outEntities)
        {
            if (_api == null) return 0;
            fixed (MyeEntityId* p = outEntities)
            {
                return _api->OverlapBox(_api->Engine, center, halfExtents, rotation, p,
                                        outEntities?.Length ?? 0);
            }
        }
        public static bool SphereCast(MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                                      out MyeRaycastHit hit)
        {
            hit = default;
            if (_api == null) return false;
            fixed (MyeRaycastHit* p = &hit)
            {
                return _api->SphereCast(_api->Engine, origin, dir, radius, maxDist, p) != 0;
            }
        }

        // ---- v12 (M51h): 入力アクション / UI 拡張 / ゲームフロー / パッド振動 ----
        // 名前 → FNV-1a 64bit (ネイティブ HashStr と同一定数。PartTag と同じ実装)
        public static ulong NameHash(string name) => PartTag(name);

        public static int GetMouseWheel()
            => _api != null ? _api->GetMouseWheel(_api->Engine) : 0;

        // アクションマップ (assets/input/actions.json)。bit0=held 1=pressed 2=released
        public static uint GetActionState(string name)
            => _api != null ? _api->GetActionState(_api->Engine, NameHash(name)) : 0u;
        public static float GetAxisValue(string name)
            => _api != null ? _api->GetAxisValue(_api->Engine, NameHash(name)) : 0.0f;

        // UI 書込 (write-only — UIElement は描画レーンなので読み取り API は無い)。
        // w/h・anchor 以降の負値は「現値維持」
        public static bool SetUIRect(MyeEntityId id, float x, float y, float w = -1.0f,
                                     float h = -1.0f)
            => _api != null && _api->SetUIRect(_api->Engine, id, x, y, w, h) != 0;
        public static bool SetUILayout(MyeEntityId id, int anchor, int space = -1,
                                       int clipChildren = -1, int align = -1, int wrap = -1)
            => _api != null
               && _api->SetUILayout(_api->Engine, id, anchor, space, clipChildren, align, wrap)
                  != 0;
        public static bool SetUITexture(MyeEntityId id, string textureKey)
        {
            if (_api == null) return false;
            var b = Utf8(textureKey ?? "");
            fixed (byte* p = b) { return _api->SetUITexture(_api->Engine, id, p) != 0; }
        }
        // 基準解像度 (1920x1080) でのヒットテスト。無ヒットは MyeEntityId.Null
        public static MyeEntityId UIHitTest(float x, float y)
            => _api != null ? _api->UIHitTest(_api->Engine, x, y) : MyeEntityId.Null;

        // ゲームフロー (M51g)。ポーズ中もスクリプト (C#/C++) は走り続ける
        public static void SetTimeControl(bool paused, int scalePercent)
        {
            if (_api != null) _api->SetTimeControl(_api->Engine, paused ? 1 : 0, scalePercent);
        }
        public static void GetTimeControl(out bool paused, out int scalePercent)
        {
            int p = 0, s = 100;
            if (_api != null) _api->GetTimeControl(_api->Engine, &p, &s);
            paused = p != 0;
            scalePercent = s;
        }

        // PersistStore (シーン跨ぎ永続)。値は unmanaged 型のバイトコピー
        public static bool PersistSet<T>(string key, T value) where T : unmanaged
            => _api != null
               && _api->PersistSet(_api->Engine, NameHash(key), &value, sizeof(T)) != 0;
        public static bool TryPersistGet<T>(string key, out T value) where T : unmanaged
        {
            value = default;
            if (_api == null) return false;
            T v = default;
            // 戻り値は実バイト数 (-1 = 不在)。サイズ厳密一致で読めたときだけ採用 (型違い防止)
            if (_api->PersistGet(_api->Engine, NameHash(key), &v, sizeof(T)) != sizeof(T))
                return false;
            value = v;
            return true;
        }

        public static void SaveGame(int slot) { if (_api != null) _api->SaveGame(_api->Engine, slot); }
        public static void LoadGame(int slot) { if (_api != null) _api->LoadGame(_api->Engine, slot); }

        // パッド振動 (出力レーン、0..1)。record/verify 中とフォーカス喪失中はエンジンが 0 に落とす
        public static void SetPadVibration(float left, float right)
        {
            if (_api != null) _api->SetPadVibration(_api->Engine, left, right);
        }
    }
}
