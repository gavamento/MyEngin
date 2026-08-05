namespace MyeScripting
{
    // エンティティへの薄いハンドル。Transform 操作の入口。
    public readonly struct MyeEntity
    {
        internal readonly MyeEntityId Id;
        internal MyeEntity(MyeEntityId id) { Id = id; }

        public bool IsAlive => Engine.IsAlive(Id);
        public MyeTransform Transform => new MyeTransform(Id);
        public MyeVec3 Position { get => Engine.GetLocalPosition(Id); set => Engine.SetLocalPosition(Id, value); }

        public void Destroy() => Engine.DestroyGameObject(Id);
        public void SetParent(MyeEntity parent) => Engine.SetParent(Id, parent.Id);

        // AudioSource コンポーネントの再生 / 停止 (v8、M45g)。非所持なら false。
        // PlayAudio は鳴っている音を鳴らし直す (Unity の AudioSource.Play と同じ)
        public bool PlayAudio() => Engine.PlayAudioSource(Id);
        public bool StopAudio(float fadeSeconds = 0.0f) => Engine.StopAudioSource(Id, fadeSeconds);

        public static MyeEntity Create(string name) => new MyeEntity(Engine.CreateGameObject(name));
        public static MyeEntity Find(string name) => new MyeEntity(Engine.FindByName(name));
    }

    // Transform ハンドル。get/set は毎回エンジンの ECS を読み書きする。
    // class にしているのは Transform.LocalPosition = v の代入を成立させるため
    // (struct を返すプロパティへの代入は CS1612 になる)。
    public sealed class MyeTransform
    {
        private readonly MyeEntityId _id;
        internal MyeTransform(MyeEntityId id) { _id = id; }

        public MyeVec3 LocalPosition
        {
            get => Engine.GetLocalPosition(_id);
            set => Engine.SetLocalPosition(_id, value);
        }
        public MyeQuat LocalRotation
        {
            get => Engine.GetLocalRotation(_id);
            set => Engine.SetLocalRotation(_id, value);
        }
        public MyeVec3 LocalScale
        {
            get => Engine.GetLocalScale(_id);
            set => Engine.SetLocalScale(_id, value);
        }
    }

    // すべての C# スクリプトの基底クラス。assets/scripts/*.cs にこれを継承したクラスを書く。
    // エンジンは MyeScript 派生型を 1 つずつ ECS コンポーネントとして登録し、
    // アタッチされたエンティティごとに Start / Update / LateUpdate を呼ぶ。
    //
    // 決定論の注意: C# レーンはリプレイ/ワールドハッシュの対象外 (別レーン)。
    // それでも乱数は Random01 / RandomRange を使うこと (エンジン管理のストリーム)。
    public abstract class MyeScript
    {
        internal MyeEntityId SelfId;
        private MyeTransform _transform;

        // このスクリプトが付いているエンティティ
        public MyeEntity Self => new MyeEntity(SelfId);
        public MyeTransform Transform => _transform ??= new MyeTransform(SelfId);

        // Transform の省略アクセス (Position += ... のように書ける)
        public MyeVec3 Position { get => Engine.GetLocalPosition(SelfId); set => Engine.SetLocalPosition(SelfId, value); }
        public MyeQuat Rotation { get => Engine.GetLocalRotation(SelfId); set => Engine.SetLocalRotation(SelfId, value); }
        public MyeVec3 Scale { get => Engine.GetLocalScale(SelfId); set => Engine.SetLocalScale(SelfId, value); }

        // ---- ヘルパ ----
        protected void Log(string message) => Engine.Log(message);
        protected bool GetKey(int virtualKey) => Engine.KeyDown((byte)virtualKey);
        protected bool GetMouseButton(int button) => Engine.MouseButton(button);
        protected float Random01() => Engine.RandomFloat01();
        protected float RandomRange(float lo, float hi) => Engine.RandomRange(lo, hi);

        // ---- 剛体物理 (v4、M28a)。Rigidbody 非所持なら no-op / Zero ----
        public MyeVec3 Velocity { get => Engine.GetVelocity(SelfId); set => Engine.SetVelocity(SelfId, value); }
        protected bool AddForce(MyeVec3 force) => Engine.AddForce(SelfId, force);
        protected bool AddImpulse(MyeVec3 impulse) => Engine.AddImpulse(SelfId, impulse);
        protected bool AddTorque(MyeVec3 torque) => Engine.AddTorque(SelfId, torque);
        protected static bool Raycast(MyeVec3 origin, MyeVec3 dir, float maxDist, out MyeRaycastHit hit)
            => Engine.Raycast(origin, dir, maxDist, out hit);

        // ---- オーディオ (v8、M45g)。**write-only** — 再生位置や再生中判定は取得できない ----
        // soundKey は .sound.json の名前 (無ければ .wav / .ogg のファイル名)。
        // 戻り値は voice ハンドル (0 = 失敗)。StopVoice / SetVoiceVolume / SetVoicePitch で使う
        protected static ulong PlaySound(string soundKey, float volume = 1.0f, float pitch = 1.0f)
            => Engine.PlaySound(soundKey, volume, pitch);
        // 自分の位置で 3D 再生する (足音・衝突音など)
        protected ulong PlaySoundHere(string soundKey, float volume = 1.0f)
            => Engine.PlaySoundAt(soundKey, Engine.GetLocalPosition(SelfId), volume);
        protected static ulong PlaySoundAt(string soundKey, MyeVec3 worldPos, float volume = 1.0f)
            => Engine.PlaySoundAt(soundKey, worldPos, volume);
        protected static void StopVoice(ulong handle, float fadeSeconds = 0.0f)
            => Engine.StopVoice(handle, fadeSeconds);
        protected static void SetVoiceVolume(ulong handle, float volume)
            => Engine.SetVoiceVolume(handle, volume);
        protected static void SetVoicePitch(ulong handle, float pitch)
            => Engine.SetVoicePitch(handle, pitch);
        protected static void SetBusVolume(string busName, float volume)
            => Engine.SetBusVolume(busName, volume);
        protected static void PlayMusic(string soundKey, float fadeSeconds = 1.0f, bool loop = true)
            => Engine.PlayMusic(soundKey, fadeSeconds, loop);
        protected static void StopMusic(float fadeSeconds = 1.0f) => Engine.StopMusic(fadeSeconds);
        // 3D リスナーを自分に固定する (三人称カメラで「プレイヤーの耳」で聴かせたいとき)
        protected void MakeListener() => Engine.SetListenerEntity(SelfId);

        // ---- 部位 (ソケット) (v9、M48h) ----
        // アセットが公開した取り付け位置を名前パス / タグ名で引く。
        // 取り付けは AttachToPart (中身は SetParent)。部位が無ければ何もせず false

        // 自分のサブツリーから引く ("Hips/HandR")
        protected MyeEntity FindPart(string path) => new MyeEntity(Engine.FindPart(SelfId, path));
        // 任意のルートから引く
        protected static MyeEntity FindPart(MyeEntity root, string path)
            => new MyeEntity(Engine.FindPart(root.Id, path));

        // タグ名一致の部位を DFS 順に集める (max 件で打ち切り)
        protected static MyeEntity[] FindPartsByTag(MyeEntity root, string tagName, int max = 32)
        {
            if (max <= 0) return System.Array.Empty<MyeEntity>();
            var buf = new MyeEntityId[max];
            int total = Engine.FindPartsByTag(root.Id, tagName, buf);
            int n = total < max ? total : max; // 戻り値は切り捨て前の総数
            var result = new MyeEntity[n];
            for (int i = 0; i < n; ++i) result[i] = new MyeEntity(buf[i]);
            return result;
        }

        // 自分を root の部位へ取り付ける。部位が見つからなければ false
        protected bool AttachToPart(MyeEntity root, string path)
        {
            var part = Engine.FindPart(root.Id, path);
            if (part.IsNull) return false;
            Engine.SetParent(SelfId, part);
            return true;
        }

        // 部位ボリューム (Part + PartBounds) へのレイキャスト (v10、M49)。部位ダメージ判定用。
        // tagName null/空 = 全部位。dir は正規化済みであること。シーン全体を対象にする
        protected static bool RaycastParts(MyeVec3 origin, MyeVec3 dir, float maxDist,
                                           out MyeRaycastHit hit, string tagName = null)
            => Engine.RaycastParts(MyeEntityId.Null, tagName, origin, dir, maxDist, out hit);
        // root のサブツリー限定版
        protected static bool RaycastParts(MyeEntity root, MyeVec3 origin, MyeVec3 dir,
                                           float maxDist, out MyeRaycastHit hit,
                                           string tagName = null)
            => Engine.RaycastParts(root.Id, tagName, origin, dir, maxDist, out hit);

        // ---- ライフサイクル (すべて任意オーバーライド) ----
        public virtual void Start() { }
        public virtual void Update(float dt) { }
        public virtual void LateUpdate() { }
        public virtual void OnTriggerEnter(MyeEntity other) { }
        public virtual void OnTriggerExit(MyeEntity other) { }
        // ソリッド衝突イベント (M28c)。normal は相手→自分方向 (ワールド)
        public virtual void OnCollisionEnter(MyeEntity other, MyeVec3 normal) { }
        public virtual void OnCollisionStay(MyeEntity other) { }
        public virtual void OnCollisionExit(MyeEntity other) { }
    }
}
