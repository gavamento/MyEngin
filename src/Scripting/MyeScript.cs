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

        // ---- ライフサイクル (すべて任意オーバーライド) ----
        public virtual void Start() { }
        public virtual void Update(float dt) { }
        public virtual void LateUpdate() { }
        public virtual void OnTriggerEnter(MyeEntity other) { }
        public virtual void OnTriggerExit(MyeEntity other) { }
    }
}
