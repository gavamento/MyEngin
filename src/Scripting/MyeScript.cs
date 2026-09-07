namespace MyeScripting
{
    // gamepad ボタンマスク (Shared/EngineAPI.h の MyePadButton と同値 = XINPUT_GAMEPAD_*)
    public static class PadButtons
    {
        public const ushort DPadUp = 0x0001;
        public const ushort DPadDown = 0x0002;
        public const ushort DPadLeft = 0x0004;
        public const ushort DPadRight = 0x0008;
        public const ushort Start = 0x0010;
        public const ushort Back = 0x0020;
        public const ushort LThumb = 0x0040;
        public const ushort RThumb = 0x0080;
        public const ushort LB = 0x0100;
        public const ushort RB = 0x0200;
        public const ushort A = 0x1000;
        public const ushort B = 0x2000;
        public const ushort X = 0x4000;
        public const ushort Y = 0x8000;
    }

    // エンティティへの薄いハンドル。Transform 操作の入口。
    public readonly struct MyeEntity
    {
        internal readonly MyeEntityId Id;
        internal MyeEntity(MyeEntityId id) { Id = id; }

        public bool IsAlive => Engine.IsAlive(Id);
        public MyeTransform Transform => new MyeTransform(Id);
        public MyeVec3 Position { get => Engine.GetLocalPosition(Id); set => Engine.SetLocalPosition(Id, value); }

        // ワールド位置 (M70d)。親を持つエンティティで「実際に居る場所」を知る唯一の口。
        // ★親が無ければローカル位置と同値なのでそれを返す (常に厳密)。親があるときは
        //   WorldMatrix を読む — TransformSystem はスクリプト層より後のフェーズなので、
        //   Start と生成直後の tick では原点が返る (dogfooding #3 と同じ罠)
        public MyeVec3 WorldPosition => Engine.GetWorldPosition(Id);

        public void Destroy() => Engine.DestroyGameObject(Id);
        public void SetParent(MyeEntity parent) => Engine.SetParent(Id, parent.Id);

        // ---- 見た目 (v2)。M70d で C# へ開通 ----
        // meshKey は "builtin://cube" などの登録名 (組込み 6 種は起動時に登録済み)。
        // materialKey は登録マテリアル名。実体の解決はエンジン側
        public bool SetMeshRenderer(string meshKey, string materialKey)
            => Engine.SetMeshRenderer(Id, meshKey, materialKey);
        // TextMesh のテキスト差し替え (v5)
        public bool SetText(string text) => Engine.SetTextMeshText(Id, text);

        // ---- キャラクターコントローラ (v5、M29b)。M70d で C# へ開通 ----
        // ★Move は水平速度 (m/s) の**設定**で、値は保持される (毎 tick 呼ぶ想定)。y は無視。
        // ★Jump は「跳躍要求」で接地を見ない — IsGrounded を確かめてから呼ぶこと
        public bool CharacterMove(MyeVec3 move) => Engine.CharacterMove(Id, move);
        public bool CharacterJump(float speed) => Engine.CharacterJump(Id, speed);
        public bool CharacterIsGrounded() => Engine.CharacterIsGrounded(Id);
        public MyeVec3 CharacterVelocity => Engine.CharacterGetVelocity(Id);

        // ---- エフェクト / アニメータ (v6 + v7)。M70d で C# へ開通 ----
        public bool EmitterBurst(int count) => Engine.EmitterBurst(Id, count);
        public bool SetEmitterPlaying(bool playing) => Engine.SetEmitterPlaying(Id, playing);
        public bool RestartEffect() => Engine.RestartEffect(Id);
        // Animator Controller のパラメータ (index 0..3)
        public bool SetAnimatorParam(int index, int value) => Engine.SetAnimatorParam(Id, index, value);
        public int GetAnimatorParam(int index) => Engine.GetAnimatorParam(Id, index);

        // AudioSource コンポーネントの再生 / 停止 (v8、M45g)。非所持なら false。
        // PlayAudio は鳴っている音を鳴らし直す (Unity の AudioSource.Play と同じ)
        public bool PlayAudio() => Engine.PlayAudioSource(Id);
        public bool StopAudio(float fadeSeconds = 0.0f) => Engine.StopAudioSource(Id, fadeSeconds);

        // ---- ゲーム内 UI (UIElement を持つエンティティ、v7 + v12)。**write-only** —
        // UIElement は描画レーン (非ハッシュ) なので毎 tick 書いてよいが、読み返す API は無い。
        // UIElement 非所持なら false
        public bool SetUIText(string text) => Engine.SetUIText(Id, text);
        public bool SetUIFill(float amount) => Engine.SetUIFill(Id, amount);
        public bool SetUIColor(float r, float g, float b, float a = 1.0f)
            => Engine.SetUIColor(Id, new MyeColor(r, g, b, a));
        public bool SetUITexture(string textureKey) => Engine.SetUITexture(Id, textureKey);
        // 解決済みのキャンバス矩形 (v16、M70c)。**UI 幾何の唯一の読み取り口**で、
        // 描画・ヒットテスト・フォーカスナビが通るのと同じ解決を返す
        public bool GetUIRect(out float x, out float y, out float w, out float h)
            => Engine.GetUIRect(Id, out x, out y, out w, out h);

        // ---- ★M70c で C# から閉じた口: SetUIRect / SetUILayout / SetUIFocused ----
        // どれも **UI の当たり判定を動かす** = M70c でワールドハッシュに載った値
        // (hovered / pressed / clicked / focused) を間接的に書き換える。
        // ところが C# レーンは
        //   * タイムトラベル / ロールバックの再シム中は止まる (TickServices::resim)
        //   * ネット対戦中は止まる (TickServices::netLockstep)
        //   * リプレイの被覆外
        // なので、ここから UI 幾何を書くと「録ったときと再生で当たり判定が違う」形の
        // 割れ方をする。**演出 (テキスト / 色 / 塗り率 / テクスチャ) は今までどおり**で、
        // 幾何とフォーカスを動かしたいときは C++ スクリプト側 (ScriptAPI.h の
        // MyeSetUIRect / MyeUISetFocused) を使うこと。

        public static MyeEntity Create(string name) => new MyeEntity(Engine.CreateGameObject(name));
        public static MyeEntity Find(string name) => new MyeEntity(Engine.FindByName(name));

        // ---- コンポーネントの付け外し (v14、M59k) ----
        // ★構造変更 = アーキタイプ移動なので毎 tick は非推奨 (常用する ON/OFF は
        //   フィールドの bool を倒すほうが桁違いに安い)。
        // ★**Add / Remove はどちらも tick 末に適用される** (ADR-005)。Has が答えるのは
        //   常に「この tick の頭の状態」— 付けた直後は false、外した直後は true
        public bool AddComponent(string name) => Engine.AddComponent(Id, name);
        public bool RemoveComponent(string name) => Engine.RemoveComponent(Id, name);
        public bool HasComponent(string name) => Engine.HasComponent(Id, name);

        // 今 tick の接触の詳細 (v14、M59k)。★OnCollision* / LateUpdate 専用
        public bool GetContactInfo(MyeEntity other, out MyeContactInfo info)
            => Engine.GetContactInfo(Id, other.Id, out info);
        // 作用点付きの力 (v14、M59k)
        public bool AddForceAtPosition(MyeVec3 force, MyeVec3 worldPoint)
            => Engine.AddForceAtPosition(Id, force, worldPoint);
        // スリープ (v14、M59k)
        public bool WakeRigidbody() => Engine.WakeRigidbody(Id);
        public bool IsSleeping() => Engine.IsSleeping(Id);

        // ---- 汎用フィールドアクセス (v11、M50d) ----
        // スキーマ codegen (assets/scripts/Generated/Schema.gen.cs) の呼び先。通常は生成側の
        // 型付きアクセサ (HealthSchema.GetCurrent 等) を使う。comp/field は名前の FNV-1a
        // 64bit (生成定数)。T はフィールドと同サイズの unmanaged 型 — サイズ不一致は false。
        // C# スクリプト状態 (非決定論レーン) はエンジン側で遮断され false が返る
        public bool TryGetField<T>(ulong compHash, ulong fieldHash, out T value) where T : unmanaged
            => Engine.TryGetField(Id, compHash, fieldHash, out value);
        public bool SetField<T>(ulong compHash, ulong fieldHash, T value) where T : unmanaged
            => Engine.SetField(Id, compHash, fieldHash, value);
        // 文字列 (String64/256)。Get は終端までを UTF-8 で復元する
        public bool TryGetFieldString(ulong compHash, ulong fieldHash, out string value)
            => Engine.TryGetFieldString(Id, compHash, fieldHash, out value);
        public bool SetFieldString(ulong compHash, ulong fieldHash, string value)
            => Engine.SetFieldString(Id, compHash, fieldHash, value);
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
        // 親の変換を掛けたあとの位置 (M70d)。読み取り専用 — 書けるのはローカルだけ。
        // 規則と Start の注意は MyeEntity.WorldPosition と同じ
        public MyeVec3 WorldPosition => Engine.GetWorldPosition(SelfId);

        // 現在の sim tick 番号 (M70d)。ネイティブは以前から Invoke で渡していたのに
        // 捨てていた — C# にはこれ以外に**決定論的な時間カウンタが 1 つも無い**
        // (実時間は機種依存、dt の積算は誤差が乗る)。「N tick に 1 回だけ」を書く土台
        public ulong Tick { get; internal set; }

        // ---- ヘルパ ----
        protected void Log(string message) => Engine.Log(message);
        // 名前 → FNV-1a 64bit (ネイティブ HashStr と同一定数)。M70d で公開。
        // ★これが無いと汎用フィールドアクセス (TryGetField / SetField) が
        //   スキーマ codegen の生成定数からしか呼べない = 手書きスクリプトから使えない
        protected static ulong NameHash(string name) => Engine.NameHash(name);
        protected bool GetKey(int virtualKey) => Engine.KeyDown((byte)virtualKey);
        protected bool GetMouseButton(int button) => Engine.MouseButton(button);
        protected float Random01() => Engine.RandomFloat01();
        protected float RandomRange(float lo, float hi) => Engine.RandomRange(lo, hi);

        // ---- 入力アクション (v12、M51h)。assets/input/actions.json で定義した名前で引く。
        // 未定義名は常に false / 0。マッピングは ProjectSettings の入力タブで編集できる ----
        protected static bool ActionHeld(string name) => (Engine.GetActionState(name) & 1u) != 0;
        protected static bool ActionPressed(string name) => (Engine.GetActionState(name) & 2u) != 0;
        protected static bool ActionReleased(string name) => (Engine.GetActionState(name) & 4u) != 0;
        protected static float Axis(string name) => Engine.GetAxisValue(name);
        // レーン指定版 (v13、M52i)。player は 0..3 (--local-players / ネットのレーン)。
        // 範囲外や未接続レーンは 0 — レーン 0 へは落ちない (配線ミスを隠さないため)
        protected static bool ActionHeldFor(string name, uint player)
            => (Engine.GetActionForPlayer(name, player) & 1u) != 0;
        protected static bool ActionPressedFor(string name, uint player)
            => (Engine.GetActionForPlayer(name, player) & 2u) != 0;
        protected static bool ActionReleasedFor(string name, uint player)
            => (Engine.GetActionForPlayer(name, player) & 4u) != 0;
        protected static float AxisFor(string name, uint player)
            => Engine.GetAxisForPlayer(name, player);
        // このフレームに累積したホイール生値 (WHEEL_DELTA=120 単位)
        protected static int MouseWheel() => Engine.GetMouseWheel();

        // ---- UI の対話 (v16、M70c)。**読み取りだけ** ----
        // 判定はエンジンが tick 中 (スクリプト層より前) に確定済み。C# は演出レーンなので
        // 「押されたか」を読んで見た目を変える用途に使う — フォーカスを**動かす**口
        // (UISetFocused) は開けていない (ハッシュ対象を C# から書かせないため)。
        // ビットは bit0 hovered / bit1 pressed / bit2 clicked / bit3 focused
        protected static uint UIButtonState(MyeEntity e) => Engine.UIButtonState(e.Id);
        protected static bool UIHovered(MyeEntity e) => (Engine.UIButtonState(e.Id) & 1u) != 0;
        protected static bool UIPressed(MyeEntity e) => (Engine.UIButtonState(e.Id) & 2u) != 0;
        protected static bool UIClicked(MyeEntity e) => (Engine.UIButtonState(e.Id) & 4u) != 0;
        protected static bool UIFocusedNow(MyeEntity e) => (Engine.UIButtonState(e.Id) & 8u) != 0;
        protected static MyeEntity UIFocusedElement() => new MyeEntity(Engine.UIGetFocused());
        // キャンバス座標 (基準 1920x1080、M70b) のマウス位置。UI の座標系はすべてこれ
        protected static MyeVec2 MouseCanvasPos()
        {
            Engine.MouseCanvasPos(out float x, out float y);
            return new MyeVec2(x, y);
        }

        // ---- マウスルック (v15、M64a) ----
        // この tick の生マウスデルタ (Raw Input のカウント)。MousePos の差分ではないので
        // カーソルロック中も回り続ける。**DPI は機種依存**なので感度は必ず調整値で割ること
        protected static MyeVec2 MouseDelta()
        {
            Engine.MouseDelta(out int dx, out int dy);
            return new MyeVec2(dx, dy);
        }

        // カーソルのロック (0 = 通常 / 1 = クライアント矩形へ閉じ込めて非表示)。**出力レーン**。
        // record/verify 中・フォーカス喪失中はエンジンが強制解除する。
        // ★Escape でもエンジンが手放す。再ロックには「0 を出し直してから 1」が要る
        protected static void SetCursorMode(int mode) => Engine.SetCursorMode(mode);

        // ---- ネット対戦の状態 (v13、M52i) ----
        // ★返る値は**機種依存** (自分がどちら側か / 実時間 / 巻き戻し回数)。
        //   sim 状態へ書き戻すとリプレイもネットも壊れる — 表示・カメラ・UI の判断だけに使う。
        // ★C# レーンはネット対戦中は停止している。ここが意味を持つのは非ネット時
        //   (NetConnected() == false / NetPlayerCount() == 1) の分岐だけ
        protected static bool NetConnected() => Engine.NetIsConnected();
        protected static uint NetLocalPlayer() => Engine.NetLocalPlayer();
        protected static uint NetPlayerCount() => Engine.NetPlayerCount();
        protected static float NetPingMs() => Engine.NetPingMs();
        protected static ulong NetRollbackCount() => Engine.NetRollbackCount();

        // ---- gamepad (v3 の回収、M51h)。パッド 0。mask は PadButtons.* ----
        protected static bool PadConnected() => Engine.PadConnected();
        protected static bool PadButton(ushort mask) => Engine.PadButton(mask);
        protected static MyeVec2 PadStickLeft() { Engine.PadSticks(out var l, out _); return l; }
        protected static MyeVec2 PadStickRight() { Engine.PadSticks(out _, out var r); return r; }
        protected static float PadTriggerLeft() { Engine.PadTriggers(out var l, out _); return l; }
        protected static float PadTriggerRight() { Engine.PadTriggers(out _, out var r); return r; }
        // 振動 (v12、出力レーン、0..1)。record/verify 中とフォーカス喪失中はエンジンが 0 に落とす
        protected static void SetPadVibration(float left, float right)
            => Engine.SetPadVibration(left, right);

        // ---- UI ナビ/ヒットテスト (v7 + v12)。どちらも基準解像度 1920x1080 で解決 ----
        protected static MyeEntity UIFocusNav(MyeEntity current, int dir)
            => new MyeEntity(Engine.UIFocusNav(current.Id, dir));
        protected static MyeEntity UIHitTest(float x, float y)
            => new MyeEntity(Engine.UIHitTest(x, y));

        // ---- ゲームフロー (v12、M51g/M51h)。ポーズ/スケールが止めるのはアニメ/物理/衝突/
        // パーティクルで、スクリプト (C#/C++)・UI・入力は動き続ける (だからここから解除できる)。
        // C# レーンは record/verify 中走らない — リプレイで再現したいフロー制御は C++ 側で ----
        protected static void PauseGame(bool paused)
        {
            Engine.GetTimeControl(out _, out int scale);
            Engine.SetTimeControl(paused, scale);
        }
        protected static bool IsGamePaused()
        {
            Engine.GetTimeControl(out bool paused, out _);
            return paused;
        }
        // タイムスケール 0..100 (%)。100 = 等速、50 = 半速。範囲外はクランプ
        protected static void SetTimeScale(int percent)
        {
            Engine.GetTimeControl(out bool paused, out _);
            Engine.SetTimeControl(paused, percent);
        }
        protected static int GetTimeScale()
        {
            Engine.GetTimeControl(out _, out int scale);
            return scale;
        }

        // ---- 永続ストア (v12、M51g)。シーンを跨いで生きる KV (LoadScene で消えない)。
        // 値は unmanaged 型のバイトコピー。sim 状態 (WorldHash 対象) なので書き込みは
        // 決定論レーンの作法で — C# からの書込はリプレイ被覆外 (演出用途向け) ----
        protected static bool PersistSet<T>(string key, T value) where T : unmanaged
            => Engine.PersistSet(key, value);
        protected static bool TryPersistGet<T>(string key, out T value) where T : unmanaged
            => Engine.TryPersistGet(key, out value);
        protected static T PersistGetOr<T>(string key, T fallback) where T : unmanaged
            => Engine.TryPersistGet(key, out T v) ? v : fallback;

        // ---- セーブ/ロード/シーン遷移 (v12 + v3 の回収)。要求は tick 末に消費される。
        // LoadGame は record/verify 中 no-op (「リプレイはセーブ読込を跨がない」) ----
        protected static void SaveGame(int slot = 0) => Engine.SaveGame(slot);
        protected static void LoadGame(int slot = 0) => Engine.LoadGame(slot);
        protected static void LoadScene(string scenePath) => Engine.LoadScene(scenePath);

        // ---- 剛体物理 (v4、M28a)。Rigidbody 非所持なら no-op / Zero ----
        public MyeVec3 Velocity { get => Engine.GetVelocity(SelfId); set => Engine.SetVelocity(SelfId, value); }
        protected bool AddForce(MyeVec3 force) => Engine.AddForce(SelfId, force);
        protected bool AddImpulse(MyeVec3 impulse) => Engine.AddImpulse(SelfId, impulse);
        protected bool AddTorque(MyeVec3 torque) => Engine.AddTorque(SelfId, torque);
        protected static bool Raycast(MyeVec3 origin, MyeVec3 dir, float maxDist, out MyeRaycastHit hit)
            => Engine.Raycast(origin, dir, maxDist, out hit);

        // ---- 空間クエリ (v4 + v7 のマスク版)。M70d で C# へ開通 ----
        // ★**Start() からは使えない** (WorldMatrix がまだ確定していない = 何にも当たらない)。
        //   撒く / 探す処理は Update に置いて 2 tick 目以降で走らせること (dogfooding #3)。
        // ★バッファは呼び出し側が確保する規約 (DLL 境界)。戻り値は**切り捨て前の総数**なので、
        //   戻り値 > 配列長 なら取りこぼしている
        protected static int OverlapSphere(MyeVec3 center, float radius, MyeEntity[] hits)
            => FillEntities(hits, buf => Engine.OverlapSphere(center, radius, buf));
        protected static int OverlapBox(MyeVec3 center, MyeVec3 halfExtents, MyeQuat rotation,
                                        MyeEntity[] hits)
            => FillEntities(hits, buf => Engine.OverlapBox(center, halfExtents, rotation, buf));
        protected static bool SphereCast(MyeVec3 origin, MyeVec3 dir, float radius, float maxDist,
                                         out MyeRaycastHit hit)
            => Engine.SphereCast(origin, dir, radius, maxDist, out hit);
        // レイヤーマスク付き (v7、M36a)。mask のビット = 対象レイヤー
        protected static bool RaycastMasked(MyeVec3 origin, MyeVec3 dir, float maxDist, uint mask,
                                            out MyeRaycastHit hit)
            => Engine.RaycastMasked(origin, dir, maxDist, mask, out hit);
        protected static int OverlapSphereMasked(MyeVec3 center, float radius, uint mask,
                                                 MyeEntity[] hits)
            => FillEntities(hits, buf => Engine.OverlapSphereMasked(center, radius, mask, buf));
        protected static bool SphereCastMasked(MyeVec3 origin, MyeVec3 dir, float radius,
                                               float maxDist, uint mask, out MyeRaycastHit hit)
            => Engine.SphereCastMasked(origin, dir, radius, maxDist, mask, out hit);

        // クエリの共通後処理 (ネイティブは MyeEntityId 配列を書くので MyeEntity へ包み直す)
        private static int FillEntities(MyeEntity[] hits,
                                        System.Func<MyeEntityId[], int> query)
        {
            if (hits == null || hits.Length == 0) return query(System.Array.Empty<MyeEntityId>());
            var buf = new MyeEntityId[hits.Length];
            int total = query(buf);
            int n = total < hits.Length ? total : hits.Length;
            for (int i = 0; i < n; ++i) hits[i] = new MyeEntity(buf[i]);
            return total;
        }

        // ---- 生成 (v7、M37)。M70d で C# へ開通 ----
        // ★生成は **tick 末**。戻り値の fileId をフィールドに覚えておいて、
        //   次の tick 以降に FindByFileId で EntityID へ解決する (即座には引けない)。
        // ★親を渡さない = ルート生成。MyeEntity の既定値 (null id) をそのまま渡してよい
        protected static ulong Instantiate(string prefabKey, MyeVec3 pos)
            => Engine.Instantiate(prefabKey, pos);
        protected static ulong Instantiate(string prefabKey, MyeVec3 pos, MyeEntity parent)
            => Engine.Instantiate(prefabKey, pos, parent.Id);
        protected static MyeEntity FindByFileId(ulong fileId)
            => new MyeEntity(Engine.FindByFileId(fileId));
        // fire-and-forget のエフェクト生成 (ハンドルを返さない版)
        protected static void PlayEffect(string prefabKey, MyeVec3 pos)
            => Engine.PlayEffect(prefabKey, pos);
        protected static void PlayEffect(string prefabKey, MyeVec3 pos, MyeEntity parent)
            => Engine.PlayEffect(prefabKey, pos, parent.Id);

        // ---- デバッグ描画 (v7)。M70d で C# へ開通 ----
        // 描画専用 = 非ハッシュ。今 tick に積んだ線は次の描画フレームに出る
        protected static void DebugDrawLine(MyeVec3 a, MyeVec3 b, float r = 1.0f, float g = 1.0f,
                                            float bl = 1.0f)
            => Engine.DebugDrawLine(a, b, new MyeColor(r, g, bl, 1.0f));

        // ---- キャラクターコントローラ (v5、M29b)。自分に対する省略形 ----
        protected bool CharacterMove(MyeVec3 move) => Engine.CharacterMove(SelfId, move);
        protected bool CharacterJump(float speed) => Engine.CharacterJump(SelfId, speed);
        protected bool CharacterIsGrounded() => Engine.CharacterIsGrounded(SelfId);
        protected MyeVec3 CharacterVelocity => Engine.CharacterGetVelocity(SelfId);

        // ---- 超リアル物理 (v14、M59k) ----
        // 作用点付きの力 (1 tick 分)。端を押せば回る = 並進と回転が同時に入る
        protected bool AddForceAtPosition(MyeVec3 force, MyeVec3 worldPoint)
            => Engine.AddForceAtPosition(SelfId, force, worldPoint);
        // 今 tick の接触の詳細 (代表点 / 法線 / 法線インパルス合計)。
        // ★**OnCollisionEnter / OnCollisionStay / LateUpdate からしか実データが返らない** —
        //   Update は物理より前のフェーズなので必ず false。決定論の要請 (EngineAPI.h の注記)
        protected bool GetContactInfo(MyeEntity other, out MyeContactInfo info)
            => Engine.GetContactInfo(SelfId, other.Id, out info);
        // その点の風速 (m/s)。PhysicsEnvironment 未設置なら false + 無風
        protected static bool SampleWind(MyeVec3 point, out MyeVec3 wind)
            => Engine.SampleWind(point, out wind);
        // ワールド XZ の地形表面 (**当たる地形**。描画の LOD やスカートの影響を受けない)
        protected static bool SampleTerrainHeight(float x, float z, out float height,
                                                  out MyeVec3 normal)
            => Engine.SampleTerrainHeight(x, z, out height, out normal);
        // スリープ (M59h)。力・速度を触る API は自動で起こすので明示呼びは稀
        protected bool WakeUp() => Engine.WakeRigidbody(SelfId);
        protected bool IsSleeping() => Engine.IsSleeping(SelfId);

        // ---- コンポーネントの付け外し (v14、M59k) ----
        // M59 の機能は「付けたら効く」存在ゲートなので、ランタイムの ON/OFF はここが入口。
        // ★構造変更 = アーキタイプ移動なので**毎 tick の付け外しは非推奨** (常用する
        //   ON/OFF は付けたまま bool フィールドを倒すほうが桁違いに安い)。
        // ★**Add / Remove はどちらも tick 末に適用される** (ADR-005)。Has が答えるのは
        //   常に「この tick の頭の状態」— 付けた直後は false、外した直後は true
        // ★AddComponent は v2 からエンジン内部にあったが C# へ露出していなかった —
        //   付けられるのに外せない/確かめられない状態だったので v14 でまとめて開ける
        protected bool AddComponent(string name) => Engine.AddComponent(SelfId, name);
        protected bool RemoveComponent(string name) => Engine.RemoveComponent(SelfId, name);
        protected bool HasComponent(string name) => Engine.HasComponent(SelfId, name);

        // ---- オーディオ (v8、M45g)。**write-only** — 再生位置や再生中判定は取得できない ----
        // soundKey は .sound.json の名前 (無ければ .wav / .ogg のファイル名)。
        // 戻り値は voice ハンドル (0 = 失敗)。StopVoice / SetVoiceVolume / SetVoicePitch で使う
        protected static ulong PlaySound(string soundKey, float volume = 1.0f, float pitch = 1.0f)
            => Engine.PlaySound(soundKey, volume, pitch);
        // 自分の位置で 3D 再生する (足音・衝突音など)。
        // ★M70d で実バグを修正: **ローカル位置をワールド位置として**渡していたので、
        //   親を持つエンティティでは鳴る場所が「親のワールド位置ぶん」ずれていた
        protected ulong PlaySoundHere(string soundKey, float volume = 1.0f)
            => Engine.PlaySoundAt(soundKey, Engine.GetWorldPosition(SelfId), volume);
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
