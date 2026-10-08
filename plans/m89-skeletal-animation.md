# M89 骨アニメの深化 — 計画

## 再開手順
1. `git log --oneline --grep "M89"` で完了済みのサブを確認する (進捗の一次情報は git log)。
2. 下の「サブ分割」の表から、依存関係を満たす次のサブを選ぶ。
3. 「運用」の未確認点のうち、そのサブに関係するものを先に確かめる。
4. 1 サブ = 1 コミット = 1 セッション。件名は `M89x: ...`。終わったら /clear。


## Context

アニメは 2 系統に分断している。
- 系統 A: SkinnedMesh + SkinningSystem。骨クリップ 1 本 + クロスフェードのみ。速度なし。
- 系統 B: Animator / AnimatorController (M22) / `.anim.json`。float フィールドのプロパティアニメ。

ステートマシン、BT の `PlayAnimation`、ABI の `AnimatorPlay` は系統 B しか動かせず、骨クリップを切り替えられない。キャラを自然に動かし、戦闘の手触りを作り、Unity/UE 並みの機能をそろえるために、**案 1 (AnimatorController が骨クリップも担当する、Unity 方式)** で統合し、その上にブレンドツリー・アニメイベント・ルートモーション・2 ボーン IK・骨アニメのプレビュー窓を積む。

### ユーザー合意済みの決定
- 案 1 で統合する。5 段階すべてを 1 マイルストーン (M89) で計画する。**harness は使わない**。
- アニメイベントは両方の受け口を作る: エンジンが直接処理 (sound / effect / noise) + スクリプト/BT が受ける。
- 遷移中のイベントは元と先の両方を発火し、イベントごとの `minWeight` で絞る。
- ルートモーション: アニメは Tick の移動量 (速度) を出すだけ。Rigidbody / CC / NavMeshAgent / Transform のうち付いているものが使う。
- エディタ: コントローラ窓 (骨クリップの選択、ブレンドツリー、イベント) + 骨アニメのプレビュー窓 (再生、スクラブ、ボーン表示、イベント位置のタイムライン)。
- params を 4→16 に伸ばすことによる、既存プロパティアニメシーンのハッシュ値の変化は許容する (挙動は同一。.rep は録り直す)。
- テストとデモの素材は、**スクリプトで多クリップの glTF を生成する** (`tools\gen_skinned_beam_fbx.ps1` に倣う)。
- 時刻と再生状態は sim 状態 (整数 tick、スナップショット/ハッシュ対象)。ポーズは時刻と状態からの導出値 (純関数)。描画・部位追従・ラグドールは同じ `SampleSkinnedLocals` を通す原則を維持する。前 Tick のポーズに依存する処理 (慣性補間・ばね) は入れない。

### マイルストーン番号
M86 (Smart Objects) と M87 (MCP サーバ) は予約済みなので **M89** を使う。

## 設計の核

### ポーズプログラム
SkinnedMesh の末尾に、純関数の入力一式を持たせる。フィールド登録はせず (シーンに保存しない、ハッシュしない)、スナップショットには生バイトで入る。

```cpp
static constexpr int kMaxPoseLayers = 8;
struct PoseLayer { int32_t clip; int32_t timeQ; int32_t weightQ; }; // timeQ=1/256 tick, weightQ 合計ちょうど 65536
int32_t poseLayerCount = 0; // 0 = 旧経路 (clip/timeTicks/フェード)。1 命令も変えない
int32_t poseClaim = 0;      // その tick にコントローラが書いた印
PoseLayer poseLayers[kMaxPoseLayers];
// 後のサブで root motion 尾部、IK 尾部、描画補間用の前 tick 時刻を足す
```

- 書くのは AnimatorControllerSystem (と IK の解決段) だけ。
- SkinningSystem: claim==1 なら進めず 0 に戻す。claim==0 なら `poseLayerCount=0` に戻して旧処理を行う。
- `SampleSkinnedLocals(model, sm)` の引数は変えない。3 か所の呼び出し側はそのままで、ブレンド・RM 除去・IK が入る。

### 時計 (すべて整数、float は一度だけ量子化)
- 単一クリップのステート: 既存の `stateTimeTicks`。旧経路と同じ `ticks/60.0f`。1 層・満杯・RM/IK なしは `ComputeJointLocals` を直接呼ぶ分岐にしてビット一致を固定する。
- ブレンドツリー: `uint32 statePhase` (2^32 = 1 周)。位相同期。`Δphase = (speed<<48) / Σ(wQ_i·L_i)` を int64 で計算。層の時刻は `timeQ = (phase·L_i·256)>>32`。
- 重み: float で計算し Q16 に 1 回だけ量子化。端数は最大重みの層 (同値なら添字が小さい層) に足す。遷移の重みは `transitionTick*65536/duration`。
- クリップ長 `L = int(dur*60+0.5)` を `SkeletalClipTicks()` に切り出し、全員で共有する。

### tick 内の順序 (フェーズ 3.5)
AnimationSystem → **AnimatorControllerSystem** (遷移 → 時刻を進める → プログラムを書く → イベント → ルートモーション) → SkinningSystem → **FootIkSystem** (新規) → PartFollowSystem → EffectSystem。
引数に `AnimLibraries{Controller, Animation, SkinnedModel}` と `AnimEventSink{BehaviorTreeSystem*, audioQueue*, effectQueue*}` を渡す (TickRunner.cpp:428-444)。

## サブ分割 (1 サブ = 1 コミット = 1 セッション、`M89a: ...`)

| サブ | 内容 | Snapshot | ABI | TypeId |
|---|---|---|---|---|
| **a** | ポーズプログラムと多層サンプラ `ComputeJointLocalsLayered`。3 か所の入口 (RenderSystem.cpp:1256 の `IsSkinFading` を `UsesLocalsPath` に、PartFollow の PoseCache キーを `SamePoseInputs` に) を揃える。claim 手順。この時点ではまだ誰も書かない | v40 | – | – |
| **b** | 多クリップ glTF の生成スクリプト `tools\gen_anim_test_gltf.ps1` (待機/歩き/走り/攻撃、ルートが前進するクリップを含む)。コントローラ `.controller.json` v2 (`"skel":{"clip":"Walk"}`、v1 も読める)。クリップは**名前ハッシュ**で参照し、`SkinnedModel::FindClipByHash` を使う。部分木の全 SkinnedMesh を駆動し、主 SkinnedMesh は entity index 最小。無効化時は claim を立てて凍らせる。`ControllerStateLengthTicks` を BT と共有する。ルートジョイントの空間と上向きを下調べ (CesiumMan / skinned_beam / 生成素材)。デモ `--anim-demo` と replay_verify の `anim` ジョブ | – | – | – |
| **c** | 型付きパラメータ: `params[16]` (float はビット列)、アセットで `type` int/float/bool/trigger を宣言。trigger は採用した遷移が消費する。ABI v28: `AnimatorSetFloat/SetInt/SetBool/SetTrigger/GetParam/GetState` (名前ハッシュ)。C# の `Interop.cs` / `MyeScript.cs`。旧 `SetAnimatorParam` は残す。コントローラ窓のパラメータ欄を型別にする | v41 | v28 | – |
| **d** | 1D ブレンドツリー (`"blend1d"`、区分線形、2 本まで) と位相同期、`statePhase` / `transitionToPhase` | v42 | – | – |
| **e** | 2D ブレンドツリー (Freeform Cartesian、gradient band、上位 4 本に絞る)。純関数 `ComputeBlendWeights` をプレビューと共有する | – | – | – |
| **f** | 骨アニメの描画補間: プログラムに層ごとの前 tick 時刻を持たせ、`interpAlpha` (RenderSystem.h:137) でサンプル時刻を補間する。決定的撮影 (alpha=1) では今と一致すること。部位追従と sim は補間しない (済: 層に `prevTimeQ` / `stepQ`、ADR-027 決定 8) | v43 | – | – |
| **g** | コントローラ窓: ステートの種類 (Property/Skeletal/Blend1D/Blend2D)、骨クリップのピッカー、1D/2D の可視化、Play 中の層表示 | – | – | – |
| **h** | アニメイベントの定義 (コントローラの `clipEvents`、クリップ名がキー) と発火規則 (下表)。`kind:"script"` は `BehaviorTreeSystem::SendEvent` に積み、既存の `BtEventCount/BtGetEvent` と BB の `eventName` で受ける (ABI 追加なし) (済: 入った tick を覚える `stateEntered` が要ったので v44、ADR-027 決定 9。出口は `AnimatorControllerSystem::FiredEvents()` を TickRunner が BT へ配る) | v44 | – | – |
| **i** | エンジンが直接処理するイベント: `sound` → `ScriptAudioEvent PlayAtPoint` (出力レーン、`ReserveAudioHandle` は使わない)、`effect` → `EffectSpawnRequest`、`noise` → `PerceptionReportNoise` (済: 位置は任意の `joint` で `JointGlobalFromLocals × 主 SkinnedMesh の WorldMatrix`、無ければエンティティの位置。script の BT イベントの vec にも位置を入れた。ADR-027 決定 10。handle 0 は FindByTag が弾くので問題なし。テスト未実施 = 全サブ後にまとめて) | – | – | – |
| **j** | ルートモーション (水平移動): 層ごとの ΔT を重み付きで合算 (折り返し対応)。上向きは LocalTransform の連鎖から算出 (Z-up 対策、逆行列は使わない)。ポーズからは水平分を除去する。適用先: NavAgent (`updatePosition=false` のとき `CC.moveInput`) / 非 kinematic Rigidbody の水平速度 / CC.moveInput / Transform。`applyRootMotion`、`rootMotionVelocity` (済: 測るのは親の無い最初のジョイント、ポーズはクリップの先頭からの水平分を抜く、kinematic の Rigidbody は Transform へ。ADR-027 決定 11。テスト未実施 = 全サブ後にまとめて) | v45 (f が v43、h が v44 を使用) | – | – |
| **k** | ルートモーションのヨー回転 (swing-twist、sqrt と四則のみ)。NavAgent の `updateRotation` と衝突するときは適用しない (済: 読み出し用の `rootMotionDeltaRotation` とメッシュの `poseRootYaw` を足したので v46。回すときは移動を回したぶん戻して表す。ADR-027 決定 12。テスト未実施 = 全サブ後にまとめて) | v46 | – | – |
| **l** | 2 ボーン IK: `TwoBoneIKComponent` (4 チェーン、endJoint 名、mode、target、poleHint、weight)。純関数ソルバを `SampleSkinnedLocals` の最後で呼ぶ (acos/atan2 を使わず半角公式)。ラグドール作動中は無効。スクリプトからは汎用の SetComponentField で指定する (済: 解決段 `TwoBoneIkSystem` が目標をメッシュの空間へ直して SkinnedMesh の `poseIk` に書く。目標はエンティティかワールドの点、pole はキャラのローカルの点。ADR-027 決定 13。テスト未実施 = 全サブ後にまとめて) | v47 (k が v46 を使用) | – | **80** |
| **m** | 足の接地: `FootIkSystem` が前 tick の WorldMatrix と `RaycastWorld` で目標を決め、骨盤を下げる (`pelvisMaxDrop`)。自分への当たりは除外 (済: TwoBoneIK の mode 3 として持ち、設定は末尾に足したので v48。足の位置はこの tick の連鎖で求める。RaycastWorld に除外の関数。ADR-027 決定 14。テスト未実施 = 全サブ後にまとめて) | v48 | – | – |
| **n** | 骨アニメのプレビュー窓 `AnimationPreviewWindow` (AssetPreviewCache.cpp:151- の一時 Scene + RenderSystem + RenderTexture の型を流用)。クリップ/ステートモード、再生・スクラブ・±1 tick、ブレンド param スライダ、ボーンの線描画、イベント位置の印。`ReleaseGpu()` で M88 のデバイス復旧の対象に登録。`--anim-preview <名前> --anim-preview-tick N` で撮影 | – | – | – |
| **o** | タイムライン上でイベントを編集 (追加・ドラッグ・削除・種類別の欄)、ルートモーション軌跡の表示 | – | – | – |
| **p** | (小) glTF ローダが STEP/CUBICSPLINE を読んでいない件 (ModelLoader.cpp:298-331): 少なくとも警告を出し、可能なら対応する | – | – | – |

依存関係: a→b→c→d→e→f→g、h は d の後、i は h の後、j は d の後、k は j の後、l は b の後、m は l と j の後、n は b の後 (ブレンド表示は e の後)、o は h と n の後。
**最初に潰す不確実性**: a (プログラム方式と旧経路のビット一致の両立) と、b の下調べ (ルートの空間・上向きの求め方)。

### イベントの発火規則 (h、全行を selftest の表にする)
| 状況 | 規則 |
|---|---|
| 順再生 | `(old, new]` |
| ステートに入った tick | `[0, new]` |
| ループの折り返し | `(old, L) ∪ [0, new]`、L ちょうどは 1 回 |
| 1 tick で複数周 | 各イベントは 1 tick に最大 1 回 |
| 逆再生 | `[new, old)` |
| 非ループの終端 | 終端の tick を 1 回だけ含める |
| ブレンドツリー | 位相で判定し、最大重みの層だけが発火 (二重の足音を防ぐ) |
| 遷移中 | 元と先の両方が発火し、`minWeight` で絞る |

発火順はエンティティの走査順 → 層 → tick 順。ジョイントのワールド座標は `JointGlobalFromLocals × 前 tick の WorldMatrix` (1 tick 遅れは許容し、文書に書く)。

### b の下調べの結果 (ルートジョイントの空間と上向き、2026-10-08)
j (ルートモーション) の前提。確認は `SkeletonSelfTest` の M89b 節と、glTF の JSON の直読み。
- **CesiumMan.glb**: ノード `Z_UP` (X 軸 -90 度の行列) → `Armature` (Z 軸 90 度) → ルートジョイント `Skeleton_torso_joint_1`。
  `Z_UP` と `Armature` は非ジョイントなのでエンティティ側 (WorldMatrix) に載る。ルートの局所移動は親空間の **+Z が上** (高さ 0.64〜0.71)。
  クリップは 1 本で名前が無い (`""`) ので名前では引けない。ルートは水平にはほぼ動かない (その場歩き)。
- **skinned_beam.fbx**: FBX ローダは祖先閉包込みでジョイントを持ち、エンティティ側は恒等 (M48a の規約)。ルートの水平移動なし。
- **anim_test.glb / anim_test_zup.glb**: ルートジョイント `Root` (index 0、parent -1) の局所移動は、Y-up 版で (0, 0, -1.4) / 周、
  Z-up 版で (0, -1.4, 0) / 周 (前進が親空間の -Y 軸に出る。親空間の上は glTF の +Z = ローダの Z 反転後の **-Z**)。
  どちらも `JointGlobal × entityWorld` ではワールド (0, 0, -1.4) で一致する (偏差 3.6e-7)。
- 結論: ルートの親空間の上向きはモデルごとに違い、**エンティティ側の変換 (非ジョイント祖先の LocalTransform の連鎖) からしか分からない**。
  j では「ワールドの上 (0,1,0) をルートの親空間へ戻した軸」を、逆行列を使わずに LocalTransform の連鎖の回転 (共役) で求める (計画どおり)。
- 既知の制約 (ADR-027 決定 5): ステートの長さが骨クリップ (モデルの読み込み) に依存するので、モデルを読まない構成ではステートの時刻が進まない。
  クック読み込み (`ModelCook.cpp`) も `SkinnedModelLibrary::Register` を通る (未確認点の解消)。`FindClipByHash` は表を持たず毎回ハッシュする。

## 主な変更ファイル
- `C:\HAL\MyEngin\src\Engine\Core\Ecs\Components.h` / `Components.cpp` (SkinnedMesh の尾部、AnimatorController の拡張、TwoBoneIK)
- `C:\HAL\MyEngin\src\Engine\Renderer\Mesh\Skeleton.h/.cpp` (`ComputeJointLocalsLayered`、`FindClipByHash`、IK ソルバ。**`ComputeJointLocals` と `ComputeJointLocalsBlended` には触らない**)
- `C:\HAL\MyEngin\src\Engine\Engine\Animation\SkinningSystem.*`、`AnimatorController.*`、`PartFollowSystem.cpp`、新規 `FootIkSystem.*`
- `C:\HAL\MyEngin\src\Engine\Engine\Loop\TickRunner.cpp`、`Rendering\RenderSystem.cpp`
- `C:\HAL\MyEngin\src\Engine\Engine\AI\BehaviorTreeSystem.cpp` (長さの取得を共有化)
- `C:\HAL\MyEngin\src\Engine\Engine\Replay\SimSnapshot.h` (版上げのたびに `Audio\Spatial\AcousticAudioSelfTest.cpp:129` の直書きも直す)
- `C:\HAL\MyEngin\src\Shared\EngineAPI.h`、`Script\EngineApiTable.cpp`、`src\Scripting\Interop.cs` / `MyeScript.cs`
- `C:\HAL\MyEngin\src\Editor\Windows\Animation\AnimatorControllerWindow.*`、新規 `AnimationPreviewWindow.*`、`LocalizationTable.inl`
- `C:\HAL\MyEngin\tools\gen_anim_test_gltf.ps1` (新規)、`tools\replay_verify.bat`
- 文書: 新規 ADR-027 (骨アニメはポーズプログラムに一本化し純関数で評価する)、`engine_spec.md` のアニメ章と ABI 節、`docs\test_checklists.md`

## 後方互換
- コントローラが無いシーン、および `poseLayerCount=0` の旧経路 (`SkinnedMesh.clip` の直書き) は 1 命令も変えない (a のテストで固定)。SkinnedMesh は NoHash なので、ハッシュ値も変わらない (SimSnapshot の版だけ上がる)。
- コントローラが駆動している間は `SkinnedMesh.clip` への直書きを無視する。これを Components.h の注記と仕様書に書く。
- プロパティだけのコントローラは挙動を変えない。ハッシュ値は c で 1 回変わる (合意済み)。
- `.controller.json` v1 はそのまま読める。

## 検証
- 各サブ: Debug/Release の `bin\x64\{Debug,Release}\Editor.exe --selftest`、`tools\check_rules.ps1` (ABI の並びは規則 11)、`tools\replay_verify.bat` (b 以降は `anim` ジョブを含む。`--snapshot-stress` で往復の非対称も検出される)。
- 回帰テストは `SkeletonSelfTest.cpp` / `AnimatorControllerSelfTest.cpp` / `BehaviorTreeSelfTest.cpp` / 新規 IK のテストに置く。主な項目:
  - 旧経路の pose checksum が不変
  - 1 層の Layered と `ComputeJointLocals` がビット一致
  - 重みの和がちょうど 65536
  - 遷移中にスナップショットを往復
  - イベントの境界表の全行
  - ルートモーションの 600 tick の移動量が ΣΔ と一致、Z-up モデルで正しい
  - IK の届く/届かない/pole/weight、部位が IK 後の関節に付いてくる
- 窓 (g, n, o): データ操作は selftest、見た目は `--screenshot` と `--anim-preview` で撮って目視する。
- 描画補間 (f): 決定的撮影で既存の golden が不変であることを確認し、144Hz での滑らかさはユーザーの手動確認とする。

## 運用
- 承認後、この計画を `C:\HAL\MyEngin\plans\m89-skeletal-animation.md` に置き、冒頭に再開手順を書く (進捗の一次情報は git log)。
- 1 サブ = 1 コミット = 1 セッション。件名は `M89a: ...`。ABI 変更 (c) は件名に明記する。
- 各サブの開始時に、そのサブの未確認点を先に確かめる:
  - `handle=0` の PlayAtPoint が問題ないか (i)
  - クック読み込みも `SkinnedModelLibrary::Register` を通るか (b)
  - kinematic Rigidbody の扱い (j) → 物理は kinematic を積分しない (invMass 0) ので Transform へ書く。CC も Rigidbody が居ると無効なので同じく Transform
  - `ts.behaviorTree` が null になる構成があるか (h) → World 単体の selftest 経路だけ。null ならイベントは配らない (発火の判定は走る)
  - 描画補間で版上げが要るか (f) → 要った (層が生バイトで載るので v43。以降の j / l は 1 つずつずれる)
