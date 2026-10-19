# sub-01: 平面拘束 `Rigidbody.plane2D` と一方通行 `Collider.oneWay`、2D 不変量の selftest

- 依存: なし
- 状態: 未着手
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む** (第 4 章 必須制約、第 5 章 規約、第 6・7 章 進め方と検証)。spec は `C:\HAL\MyEngin\plans\m94-2d\spec.md` (2. #3, #4, #9、4.1.5、4.4)

## やること
M94 で最も荷重のかかる未知 = 「3D ソルバへの平面拘束で z / X・Y 回転が**ビット単位で**動かないか」を潰す。

1. `RigidbodyComponent` 末尾に `bool plane2D = false` (hash 対象、`Components.cpp` の登録表に和名付きで追加)。`ColliderComponent` 末尾に `bool oneWay = false` (hash 対象)。`kSimSnapshotVersion` を +1 し `SimSnapshot.h` の履歴と `AcousticAudioSelfTest.cpp:129` の固定値を更新。
2. `PhysicsSystem::Update` に拘束を入れる (spec 2. #3 の A / B / D)。
   - A: ボディ収集 (`PhysicsSystem.cpp:1571-1683`) で `b.plane2D` を持ち、`invI` 確定 (:2635-2654) で plane2D なら Z 軸成分 (ワールド zz) だけ残して他を 0 にする。
   - B: 速度ソルバ (:4099-4266) の直後・位置補正パス (:4287) の前で `vz = wx = wy = 0.0f`。
   - D: 位置・姿勢積分 (:4826-4868) の直後で `pz = z0`、`qx = qy = 0.0f` (正規化は既存経路に任せる。正規化で x/y が非 0 になるなら正規化後にもう一度 0 を書く)、`vz = wx = wy = 0.0f`。CCD の差し替え (:4840) もこの後に来ることを確認。
   - z0 は tick 開始時に `LocalTransform.position.z` から取る (サブステップをまたいで同じ値)。
   - **OFF のボディでは 1 演算も増やさない** (分岐ゲート。`+ 0.0f` は -0.0f を化けさせる — M59f1 のコメント参照)。
   - スリープ (:4929-5018)、CCD、複合コライダー (M60e)、関節 (Hinge) と組み合わせて不変量が成立すること。
3. `oneWay` の接触フィルタ: 接触生成 (:3842-4096) で、静的側 (または kinematic 側) の形状が `oneWay` なら「法線 (床→物体) の y が 0 以下」または「物体の tick 開始時の最下点 (形状のワールド AABB の y 最小) < 床の天面 (oneWay 形状のワールド AABB の y 最大) − slop」なら接触を捨てる。両方 oneWay なら従来どおり (捨てない)。`isTrigger` は従来どおり接触を作らない。
4. ABI の `AddForce` / `AddImpulse` / `AddTorque` / `AddForceAtPosition` は plane2D ボディで z 成分 / X・Y トルクを捨てる (`ApplyTorqueWorld` の freezeRotation の前例 :5318)。
5. `PhysicsSelfTest.cpp` に `Plane2D` 節 (spec 4.1.5): 箔 10 + 球 5 + Hinge(axis=(0,0,1)) 1 組を plane2D で床に落として 3000 tick。(a) 全ボディの不変量 (b) 2 ワールド並走の `HashWorld` 一致 (c) 最終 `HashWorld` を定数に焼き Debug / Release で同値 (`NavDeterminismSelfTest` の kExpected の流儀) (d) oneWay 床を球が下から抜け、上から乗り、横から当たっても止まらない (e) plane2D OFF のボディだけのシーンのハッシュが着手前と同一 (既存ハッシュを着手前に 1 回採って焼く)。
6. Inspector: Rigidbody / Collider の新フィールドは登録表から自動で出る。`plane2D` の和名の説明に「異なる z の物同士は当たらない」を書く (`MYE_JP` の和名か Components.h のコメント)。

## やらないこと (このサブでは)
- CharacterController の 2D (sub-09)、2D 形状・クエリ (sub-08)、ABI の新スロット (sub-14)、デモ (sub-23)

## 触る場所 (planner の見立て)
- `src\Engine\Core\Ecs\Components.h` (Rigidbody / Collider の末尾)、`Components.cpp` (登録表)
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (:1571-1683 収集、:2635-2654 invI、:4266 付近、:4826-4868 積分、:3842-4096 接触生成、:5318 / :5357 ABI の力)、`PhysicsSystem.h` (Body 構造体)
- `src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp`
- `src\Engine\Engine\Replay\SimSnapshot.h` (版)、`src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp:129`
- 参考: `src\Engine\Engine\Navigation\NavDeterminismSelfTest.cpp` (焼いたハッシュの流儀)、`Components.h:381-421` の Rigidbody コメント (M59f1 / M59h の規約)

## 受け入れ条件 (このサブ)
1. (spec 6) `Plane2D` 節の (a)(b)(c) が Debug / Release で PASS。焼いたハッシュの値を SELF_EVAL に書く。
2. (spec 7) (d) が PASS。
3. (spec 8) `--physics-demo` と `--joint-demo` を着手前後で `--replay-record` → `--hash-diff` し、差分が `plane2D` / `oneWay` の append だけ (ログを SELF_EVAL に)。
4. (spec 1, 2, 3, 4) 既存 golden 30 枚 maxDiff=0、replay_verify 全ジョブ PASS、0 警告、check_rules 0、版 bump は snapshot +1 だけ。
5. 不変量が割れた場合は spec 7. R1 の順で拘束点を増やし、どこまで要ったかを SELF_EVAL と ADR 下書き (sub-24 が仕上げる) に書く。

## 検証コマンド
- `msbuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` と Release
- `bin\x64\Debug\Editor.exe --selftest`、`bin\x64\Release\Editor.exe --selftest` (Plane2D 節のハッシュ行を両方から抜く)
- `pwsh tools\check_rules.ps1`
- `tools\shot_verify.bat` (Release)、既存 30 枚に `Editor.exe --img-diff tests\golden\X.png tests\actual\X.png --tol 0`
- `tools\replay_verify.bat`
- `bin\x64\Release\Runtime.exe --physics-demo --replay-record cache\before.rep ...` → 着手後 → `--hash-diff` (exe は PowerShell ツールから)

## 実装メモ (coder が追記)

## フィードバック履歴
