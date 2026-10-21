# sub-20: 2D ジョイント検証と Effector2D

- 依存: sub-09
- 状態: OK (commit a2b5d90)
- 往復: 2
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

SELF_EVAL: sub-20 (round 1)
実装:
  - Components.h/.cpp: Effector2DComponent (TypeId 90、hash 対象) と effector2dtype:: (Area / Surface / Buoyancy / Point)。type / force / surfaceSpeed / density / drag / magnitude / useMask / mask。SimSnapshot.h を v63、AcousticAudioSelfTest の版固定を 63 へ
  - PhysicsSystem.cpp: 収集直後に Effector2D の領域を集める (plane2D の動的剛体が居るときだけ。Body に surface / surfaceVel、領域は ShapePose + AABB を entity.index 昇順)。外力の帯 (浮力の直後・ジャイロの前) に Area (v += F/m×h) / Point (中心へ magnitude/m×h) / Buoyancy (領域天面を水面に SubmergedFractionWorld × density × 体積 × |g|、没水重心に作用、没水割合で按分する線形抗力)。対象は plane2D の非 kinematic 剛体、領域 AABB → shapes::Overlap の順で絞り、useMask は Body.layer のビット。Surface は ContactConstraint に surf / surfV を持たせ、重心摩擦の目標を「接線の目標相対速度」に差し替える (surf が false の接触は従来式)
  - PhysicsSelfTest.cpp: 新節 M94t。ジョイント 3 組 (Hinge(0,0,1) の振り子 / Slider(1,0,0) の水平レール + リミット / SpringJoint の距離ばね) を plane2D で 1000 tick、Effector 4 種 (Area + マスク / Point 引力と反発 / Surface 順逆と対照 / Buoyancy 浮く沈む) を 300 tick。毎 tick の不変量 (z / vz / wx / wy / qx / qy ビット)、2 ワールド並走の HashWorld 一致、最終ハッシュ焼き込み、TypeId 90
  - InspectorWindow.cpp: Joint (Hinge / Slider) に「2D (Z 軸)」ボタン (undo.Record。Hinge は axis (0,0,1)、Slider は (1,0,0))。Effector2D に警告行 (Collider 無し / Surface にトリガー)
  - SceneViewWindow.cpp/.h: DrawEffector2DGizmos (橙の矢印: Area は力の向き、Surface は面の向き、Buoyancy は上、Point は中心へ 4 本 / 反発は外へ)
  - CreateMenu.cpp: Create → 2D → Effector 2D (トリガー Box + Effector2D)。EditorComponentCatalog.cpp: 2D 分類。LocalizationTable.inl: Create_Effector2D / Insp_Joint2DAxis(Hint) / Insp_Effector2D* (日英)
仕様との差分:
  - [追加] ジョイントの拘束 (sub-01 の A+B+D) は **修正不要だった**: Hinge / Slider / SpringJoint の 3 組は無改造で不変量と並走一致を満たした (ApplyPoseRotation 等は触っていない)。「割れたら直す」の分岐は踏まなかった
  - [追加] Surface の領域は **ソリッドな (isTrigger = false) 静的 / kinematic の Collider が必要**。spec は「同エンティティのトリガー Collider が領域」だが、Surface だけは接触を作る形状が要る (トリガーは接触を作らない) ので、trigger の Surface は何もしない。Inspector に警告を出す。Area / Point / Buoyancy は isTrigger を問わず Collider の形状を領域にする
  - [追加] 領域にできない形状は kMesh / kTerrain / kEdge2D (無視)。領域の姿勢は tick 開始時のもの (サブステップ途中で動く kinematic の領域には追従しない)。Surface の向きも tick 開始時のエンティティ回転 (+X を回した向き) を使う
  - [追加] Surface の目標は面の速度を接触面へ射影したベクトル (平らなベルトは surfaceSpeed そのまま)。対象は plane2D の非 kinematic 剛体だけ (3D 剛体には効かない)
  - [追加] Buoyancy の体積は ShapeVolumeWorld (3D の体積。z 厚みを含む) で、既存の 3D Buoyancy と同じ式。density 既定 1000 は現実の水で、質量 1 kg の小球は圧倒的に浮く (テストでは density 10 を使用)
  - [追加] Effector2D の既定値: surfaceSpeed 2.0 / density 1000 / drag 1.0 / magnitude 10 / mask 全ビット (spec は既定値を指定していない)
  - [追加] Create → 2D → Effector 2D の既定の領域は幅 4 × 高さ 2 × 奥行き 1 (halfExtents 2, 1, 0.5)
検証:
  - Debug / Release `MyEngine.sln /p:MyeWarnAsError=true` → 0 警告 0 エラー (最終コードで両構成。SelfTestUnused の C4100 は今回は出なかった)。ログ cache\m94\s20uild_*_3.log
  - `MYE_SELFTEST_ONLY=m94` Debug / Release → どちらも exit 0、FAIL 0 (PASS 2549 = sub-19 の 2529 + 20)。新節の焼き値: joint2D A82F779A97738D34 / effector2D 85C0AB87B50CE78C、Debug == Release。既存の焼き値は不動 (plane2D 639107476EA401A3 / Shapes2D 2ECF7CFD5CC45314 / CC2D C04B7FFA3E31E5D0 / Tilemap / SpriteShape 丘は PASS 行で確認)
  - joint2D の実測 (Debug): Hinge の留め金のずれ 最大 0.0001 m・最低点 y 5.500 (振り子が真下まで来る)、Slider の y ずれ 0・回転 0・停止位置 x 2.000、SpringJoint の距離 1.177〜2.983 → 終端 2.011、重心 vy 0.5000
  - effector2D の実測: Area の箔 vx -14.67 (領域外の同じ箔は 0)、マスク L5 だけ動く、Point は 97.04〜103.00 を往復 / 反発は 17.9 m/s で領域を出る、ベルト +3.025 / 逆向き -2.995 / 対照 0、浮き球 y 4.024 (水面 4.0) / 沈む球 0.300
  - `check_rules.ps1` → 3 error / 50 warning (基準どおり、増減なし)
  - hash-diff: 基点 26aa0fa の Release Runtime と現行 Release Runtime で `--physics-demo --replay-record --replay-ticks 600 --replay-fast --hash-dump-tick 599` を比較 → 差は UIInteraction.prevSurfX/Y の 2 欄だけ (実マウス座標のミラー = 入力、sub-07 で planner 確認済み)。rep ファイルは別、sim の欄は全部一致。Effector2D の無いシーンは無差分
  - チェックポイント 4 (sub-16〜20):
      1. `Editor.exe --selftest` 全体 Debug / Release → 両方 exit 0 (PASS 8648 同数、FAIL 行なし。"compile FAILED" は C# 失敗系の負のテストのログ)。ログ cache\m94\s20ull_*.log/.err
      2. `toolseplay_verify.bat` (MYE_MSBUILD_ARGS=/p:MyeWarnAsError=true、全文 cache\m94eplay_verify_cp4.log) → 21 ジョブ中 20 PASS、rules だけ FAIL (check_rules の既存 3 error)。個別行: mp / physics / acoustic / demo / fracture / flow / ui / joints / perception / nav / ttrelease / anim / bt / whatifrelease / parts / ttdebug / whatifdebug / compute / computeab / jobsab 全部 PASS
      3. `tools\shot_verify.bat` (MYE_SHOT_TOL=0) → 30 枚 PASS、29 枚 maxDiff=0、demo_terrain_deferred は golden と maxDiff=5 / 2 画素 (基点でも同じ既知分、tol=12 の個別枠) → `cache\m94aseline_terrain.png` と `--img-diff --tol 0` で maxDiff=0 diffPixels=0。割れなし
      4. `toolsuild_managed.bat Release` → 0 警告 0 エラー
      割れたものは無く、原因サブの特定は不要
  - 未実施: Scene View のギズモ (橙の矢印) と Inspector の「2D (Z 軸)」ボタン / Effector 警告行 / Create → 2D → Effector 2D の実画面・実クリック (ユーザー目視 = sub-24 checklists)
自己採点 (1-5):
  仕様適合: 4 — 4 種の挙動・3 ジョイントの検証・hash 対象・snapshot +1・Inspector / Catalog / Create / ギズモまで入れた。Surface がソリッド Collider 必須という spec 解釈の追加が 1 点
  正しさ: 4 — 両構成で毎 tick の不変量と並走一致と焼き値、ベルト / 浮力 / 引力の定量、既存焼き値不動、hash-diff 無差分、チェックポイント全項目 PASS。GUI 部は実操作未確認、スリープ中の剛体は領域に入っても起きない (下記)
  コード品質: 4 — 存在ゲート (plane2D の動的剛体と Effector2D が両方居るときだけ収集・帯が走る)、決定順序は entity.index 昇順、surf が false の接触は従来式。外力の帯が PhysicsSystem の長い Step 内にインラインで増えた
  テスト: 4 — ジョイント 3 + 効果 4 種 + マスク + 対照 (領域外 / 平らな床 / 逆ベルト) + ハッシュ焼き込み。Surface の回転ベルトや親付きは未検証
不安・質問:
  - 眠っている剛体 (isSleeping) は invMass == 0 として収集されるため Effector の力が届かず、後から領域内で起こされない限り流されない (風が止んだあとに領域へ置かれた静止物など)。3D の ConstantForce / 浮力も同じ挙動 (既存の割り切り) なので揃えたが、2D の風で寝た箔が動かないのは気になりうる。spec にも明記なし。限界欄 (sub-24) に書くか、planner が Area / Buoyancy で寝た剛体を起こす規則を求めるか裁定してほしい
  - Surface の移動中ベルトに乗った剛体は、ベルト速度が sleepLinearThreshold (0.05 m/s) 未満だと眠って運ばれなくなる。実用の速度 (>= 0.1) では起きない想定
  - 親付き (frame が identity でない) の Effector 領域 / 回転する kinematic のベルトは未検証
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Core\Ecs\Components.h
  - C:\HAL\MyEngin\src\Engine\Core\Ecs\Components.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Replay\SimSnapshot.h
  - C:\HAL\MyEngin\src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp
  - C:\HAL\MyEngin\src\Engine\Core\Localization\LocalizationTable.inl
  - C:\HAL\MyEngin\src\Editor\Widgets\CreateMenu.cpp
  - C:\HAL\MyEngin\src\Editor\Widgets\EditorComponentCatalog.cpp
  - C:\HAL\MyEngin\src\Editor\Windows\Scene\InspectorWindow.cpp
  - C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewWindow.cpp
  - C:\HAL\MyEngin\src\Editor\Windows\Scene\SceneViewWindow.h
  - C:\HAL\MyEngin\plans\m94-2d\sub-20.md (この実装メモ)
申し送り:
  - sub-21 以降: 新型の追加だけなので既存の焼き値の再採取は不要だった。TypeId は 90 まで使用済み (次は 91)、snapshot は v63。ABI は触っていない (v31 = 188)
  - sub-23 のデモでベルトを置くなら、乗せる物は接地した状態から始めると安定する (薄い箔 0.1 厚を高所から落とすと着地の摩擦衝撃で転がる。実測: 厚 0.1 で高さ 0.45 から落とすとベルト上で転がり続けた)。半厚 0.1 以上・接地開始なら速度 3.0 で安定
  - sub-24 の限界欄: Effector の領域は tick 開始時の姿勢 / Surface は solid 必須 / 寝た剛体に効かない / 親付き・回転ベルトは未検証 / Buoyancy の体積は 3D 体積で density 既定 1000 は実質「軽ければ必ず浮く」/ 領域にできない形状は kMesh・kTerrain・kEdge2D
  - 一時ファイル: cache\m94\s20\ (ビルド / selftest / hash-diff / shot_verify のログ、git 管理外)

SELF_EVAL: sub-20 (round 2)
実装:
  - #1 (must): PhysicsSystem.cpp に起床規則。収集の段 (Effector 領域を集めた直後) で、plane2D の剛体ごとに「有効な領域」への重なりを tick 開始時の状態だけで判定する。有効 = Area は force != 0、Point は magnitude != 0、Buoyancy は density > 0 かつ没水割合 > 0、Surface は surfaceSpeed != 0 かつ面の AABB を kEffectorWakeMargin (0.05) 広げた範囲に入る (useMask も尊重)。有効なら Body.effectorHold を立て、眠っていれば収集時の不動化 (sleeping / isSleeping / invMass / freezeRot) を戻して起こす (慣性は後の pose 確定で組み直される)。入眠判定の静けさ計数は effectorHold のボディの sleepTicks を 0 に保つ (= 島ごと眠らない)。Surface も領域一覧に入るようにし (力の帯は Surface を飛ばす)、剛体の形状取得を EffectorBodyPose に共通化。存在ゲートの内側 (Effector2D が無い / plane2D の動的剛体が居ないシーンは通らない)
  - #1 selftest 3 項目: (a) 眠った箔の Area の force を 0 から非 0 にすると 5 tick 後に起きて動く (b) 平衡で浮く球が眠らない (sleepTicks == 0 を全 tick 維持) (c) force 0 の領域の箔は従来どおり眠り、別の領域の箔も眠ったまま (対照)
  - #2 (nit): #1 で Surface は surfaceSpeed != 0 なら起こすため解消 (ベルト速度が閾値未満でも乗った物は眠らない)
  - #3 / #4 (nit): sub-24 送り、今回作業なし
仕様との差分:
  - [追加] Surface の起床は「面の AABB を 0.05 広げた範囲に剛体 AABB が入る」の AABB 判定のみ (precise な接触判定はしない)。spec 2. #21b の「接触候補の範囲」の解釈
検証:
  - Debug / Release `/p:MyeWarnAsError=true` → 0 警告 0 エラー
  - `MYE_SELFTEST_ONLY=m94` 両構成 exit 0、FAIL 0 (PASS 2553 = 2549 + 新 4 項目)。effector2D の焼き値を再採取: 85C0AB87B50CE78C → 9504211B9041ECC2 (Debug == Release)。joint2D A82F779A97738D34、plane2D 639107476EA401A3 ほか既存の焼き値は不動
  - check_rules → 3 error / 50 warning (増減なし)
  - `--physics-demo` の hash-diff (基点 26aa0fa の Release Runtime 対 現行) → 差は UIInteraction.prevSurfX/Y の 2 欄だけ。sim の欄は全一致
  - 重い検証 (全体 selftest / replay_verify / shot_verify / build_managed) は指示どおり再実行していない
自己採点 (1-5):
  仕様適合: 5 — 起床規則 (#21b) を有効判定付きで実装、selftest 3 項目
  正しさ: 4 — 両構成のテストと既存焼き値不動を確認。GUI 未確認、全体 replay は round 1 から未再実行
  コード品質: 4 — 判定は収集段に集約、存在ゲート内
  テスト: 4 — 起床 / 非起床 / 浮力の不眠を確認
不安・質問: なし
触ったファイル:
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp
  - C:\HAL\MyEngin\src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp
  - C:\HAL\MyEngin\plans\m94-2d\sub-20.md (実装メモ節のみ)
申し送り: Effector2D が影響する tick は入眠しない (島ごと)。sub-24 の限界欄は「寝た剛体に効かない」を削り、「効くが領域内の剛体は眠らない」に置き換える

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-22)。must 1 件: 眠っている plane2D 剛体に Effector2D の力が届かない (coder の不安 1)。spec の「領域に重なる plane2D 剛体へ毎サブステップ力」を満たさず、Unity (Box2D は力で起こす) とも違う。spec 2. #21b / 4.1.5 に規則を追記。それ以外 (ジョイント 3 組・Effector 4 種の定量、焼き値、hash-diff、チェックポイント 4 の全項目) は根拠として確認済みで、round 2 では must 1 の差分だけを見る。
- round 2: VERDICT OK (planner、2026-10-22)。起床規則を収集段の存在ゲート内に実装 (有効判定 4 種 + useMask、`effectorHold` で sleepTicks を 0 に保つ)。selftest (a) force 0 → 非 0 で眠った箔が起きて動く (b) 平衡の浮き球が眠らない (c) force 0 の領域の箔は眠る対照、が両構成 PASS。effector2D の焼き値を 9504211B9041ECC2 に再採取 (Debug == Release)、joint2D / plane2D ほか既存の焼き値不動、`--physics-demo` の hash-diff は入力ミラーだけ。Surface の起床判定を AABB + 0.05 にした解釈は採用 (spec 2. #21b の「接触候補の範囲」)。
