# sub-20: 2D ジョイント検証と Effector2D

- 依存: sub-09
- 状態: 実装中
- 往復: 0
- **coder は着手前に `C:\HAL\MyEngin\AGENTS.md` を全文読む**。spec: 2. #20, #21、4.1.5

## やること
1. ジョイント検証: `PhysicsSelfTest` の `Plane2D` 節に Hinge(axis (0,0,1)、振り子) / Slider(axis (1,0,0)、水平レール) / SpringJoint (距離ばね) の 3 組を plane2D で追加し、1000 tick の不変量 (spec 4.1.5) と 2 ワールド並走一致、Debug / Release のハッシュ焼き込み。割れたら sub-01 の拘束で足りていない箇所 (関節の `ApplyPoseRotation` :4287-4457 など) を直す。Inspector の JointComponent に「2D (Z 軸)」ボタン (axis を (0,0,1) に、Slider なら (1,0,0))。
2. `Effector2DComponent` (**hash 対象**、TypeId 末尾): `type int32 (0 Area / 1 Surface / 2 Buoyancy / 3 Point)` / `force Float2` (Area: 一定力 N) / `surfaceSpeed float` (Surface: 接触面の速度 m/s、+X 方向をエンティティの Z 回転で回す) / `density float` (Buoyancy: 流体密度、水面 = 領域 AABB の天面) / `drag float` (Buoyancy: 水中抗力) / `magnitude float` (Point: 中心への力、負で反発) / `useMask bool` + `mask uint32` (対象レイヤー)。同エンティティの `ColliderComponent (isTrigger = true)` が領域。Catalog、Create → 2D → Effector。`kSimSnapshotVersion` +1。
3. PhysicsSystem: 外力の段 (:2662-3548 の並び、浮力の隣) に `Effector2D` を足す。対象は plane2D の非 kinematic 剛体で、領域 (トリガー形状のワールド AABB → 形状で精密化は `shapes::Overlap`) に重なるもの。Area: `v += F / m × h`。Point: 中心へ `magnitude / m × h`。Buoyancy: 浸水率 (形状 AABB と水面 y) × density × 体積 × g を上向きに + drag。Surface: 領域に**接触している**剛体 (接触生成の結果が要るので、Surface だけは接触解決の段で、接触点の接線速度目標を `surfaceSpeed` にする = ベルトコンベア。接触ペアに「目標接線速度」を持たせる)。Effector2D が 0 個なら 1 演算も増やさない。
4. selftest (spec 5. 34): 4 種それぞれの挙動 + 2 ワールド並走一致 + Debug / Release ハッシュ焼き込み。
5. Scene View ギズモ: 領域 (トリガー Collider のギズモは既存) に type の矢印 (橙)。

## やらないこと (このサブでは)
- Wheel / Relative / Target / Friction ジョイントの新設、Effector の Unity 互換パラメータ (Force Variation / Angle 等)

## 触る場所 (planner の見立て)
- `PhysicsSelfTest.cpp`、`InspectorWindow.cpp` (Joint 節のボタン)
- `Components.h` / `Components.cpp`、`EditorComponentCatalog.cpp`
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.{h,cpp}` (:2662-3548 外力、:3842-4096 接触生成、:4099-4266 速度ソルバの接線目標)
- `SceneViewWindow.cpp`、`SimSnapshot.h`、`AcousticAudioSelfTest.cpp:129`、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)
1. (spec 34) ジョイント 3 組の不変量 + Effector 4 種の selftest が Debug / Release で PASS、焼いたハッシュ同値。
2. Effector2D の無いシーンの hash-diff 無差分 (`--physics-demo` 1 本)。
3. (spec 1-3) 0 警告、check_rules 0、`MYE_SELFTEST_ONLY=m94` の Physics suite が両構成 PASS、snapshot +1 のみ。
4. **チェックポイント (sub-16〜20 の分、spec 4-5)**: `Editor.exe --selftest` 全体 (Debug / Release) に新 FAIL 無し、`tools\replay_verify.bat` 全ジョブ PASS、`tools\shot_verify.bat` + 既存 golden 30 枚 `--tol 0` で maxDiff=0。割れたら sub-16〜20 のどれが原因かを特定して SELF_EVAL に書く。

## 検証コマンド
毎サブ必須: Debug / Release ビルド、`pwsh tools\check_rules.ps1`、`MYE_SELFTEST_ONLY=m94` の selftest (両構成)
- `Runtime.exe --physics-demo --replay-record` → `--hash-diff`
**チェックポイント (このサブで回す)**: `Editor.exe --selftest` 全体 (Debug / Release)、`tools\replay_verify.bat`、`tools\shot_verify.bat` + 既存 30 枚 `--tol 0`

## 実装メモ (coder が追記)

## フィードバック履歴
