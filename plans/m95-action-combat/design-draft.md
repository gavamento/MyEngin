# M95: アクション戦闘の土台 — 事前調査 (司会 → planner)

司会 (メインセッション) が 2026-11-04 に調べた現状と方針案。**planner はこれを仮説として扱い、実コードで裏取りする。**
計画時の番号 (ABI 版・SimSnapshot 版・controller アセット版・ADR 番号) は着手時の末尾から取り直す。

## Context

- ユーザーは MyEngine で大剣主体の 3D アクション (モンハン / ソウル系のボス戦) を作る。モデル・モーションは自作 (Blender → glb)。
- 縦切り (武器 1 種 3 段 + 溜め、ロール、ロックオン、スタミナ、ボス 1 体 = 攻撃 3 種・部位 2 箇所、円形アリーナ) に要る機能を照合した。ゲーム側 (GameLogic) で書ける物を除くと、エンジン側の不足は戦闘の中核に集中している。
- ユーザー回答で決まったこと:
  - 範囲: ヒットストップ / 部位への重なり・掃引 / 遷移の割り込み / CC 同士の接触と接触イベント (dogfooding 17 の解決を含む)
  - 制約解除: **ルートモーションの縦成分** と **イベント時刻の秒・フレーム指定** を解除する。CC カプセルがスケールを拾う挙動は対象外。固定 60 Hz tick と rand() 禁止は解除しない (決定性の根幹)
  - 進め方: /harness で回す
- 既存シーン (三校・HAL Collector) をビット同一に保つため、挙動が変わる物は全て opt-in (既定 off) にする。

## 現状 (調査で確認済み)

| 項目 | 現状 | 根拠 |
|---|---|---|
| Animator の速度 | `AnimatorControllerComponent` に entity 単位の速度・停止欄が無い。速度はアセットのステート側 (`ControllerState.speed`、整数 tick/進み) だけ | `src\Engine\Core\Ecs\Components.h:782`、`src\Engine\Engine\Animation\AnimatorController.h:58` |
| 遷移評価 | `transitionTo < 0` のときだけ遷移を評価する。`AnimatorPlay(duration>0)` は遷移中でも `transitionTo` を上書きするが、元ステート (`currentState / stateTimeTicks`) はそのままなので、進行中のブレンド結果を捨てて元ステートからやり直す = 姿勢が飛ぶ | `AnimatorController.cpp:1739-1770`、`:1600-1632` |
| ルートモーション | ルートジョイントの差分をワールドへ回し `y` を捨てる。`ApplyRootMotion` の適用先は NavMeshAgent (updatePosition 中は何もしない) → 非 kinematic Rigidbody (x/z) → CC `moveInput` (x/z) → LocalTransform の順 | `AnimatorController.cpp:834-880`、`:1904-1931` |
| アニメイベント | `.controller.json` の `clipEvents` に `tick` 整数で持つ。受け手は `BtEventCount / BtGetEvent` (スクリプト) と BB。Editor の編集窓 `ClipEventEdit` も tick 入力 | `AnimatorController.h:111`、`AnimatorController.cpp:1007-1020`、`src\Editor\Windows\Animation\ClipEventEdit.cpp:24-37` |
| 部位判定 | `Parts::RaycastParts` のみ (レイ)。候補収集 → index 昇順 sort → `MakePartBoundsPose` で ShapePose を作り、厳密 `<` で最近を更新 (RaycastWorld と同じ決定論規約) | `src\Engine\Engine\Animation\Parts.cpp:118-192`、`src\Shared\EngineAPI.h:493` |
| CC の衝突 | `SolveCharacters` は `bodies` (剛体コライダ) に対してだけ固定パス (`kCharPushPasses`) × index 昇順で押し出す。CC 同士は一切見ない。接触イベントは `SolidContact` (剛体同士) からしか生まれない | `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp:1516-1600`、`docs\dogfooding.md` 17 |
| 衝突イベントの配送 | `ScriptHost` が `onCollisionEnter / Stay` (kind 0/1) をスクリプト型ごとに呼ぶ。ここへ流せば CC の接触も同じ書き方で受けられる | `src\Engine\Engine\Script\ScriptHost.cpp:128-145` |
| スクリプト間通信 | `BtSendEvent` → `BtGetEvent` が entity 宛てイベントとして使える (ダメージ通知はこれで足りる見込み。新設不要) | `src\Shared\EngineAPI.h:960-964` |
| 番号の末尾 (2026-11-04) | ABI v31 = 188 スロット、SimSnapshot v65、controller アセット v2、ADR-033、TypeId 93 | `EngineAPI.h:58`、`SimSnapshot.h:175`、`AnimatorController.cpp:964`、`docs\adr\` |

## 方針案 (planner が検証して spec.md へ)

番号の仮置き: **M95、ABI v32、SimSnapshot v66、controller アセット v3、ADR-034**。新しい TypeId は無し。

### M95a ヒットストップ (Animator の一時停止)
- `AnimatorControllerComponent` 末尾に `int32_t pauseTicks = 0` を append (hash 対象)。
- Update の冒頭で `pauseTicks > 0` なら時刻・位相・遷移を進めず、`rootMotionVelocity = 0` / `rootMotionDeltaRotation = 恒等` を書き、イベントも発火せず、ポーズは現在のものを再適用して `--pauseTicks`。
- C ABI: `AnimatorSetPause(engine, entity, int32 ticks)` (上書き、0 で解除) と `AnimatorGetPause`。C# ミラー (Interop.cs) を同時に更新。
- 速度倍率 (スローモーション) は入れない。整数 tick の決定性を崩さず、ジャンル上も停止だけで足りる。

### M95b 部位への重なり・掃引
- `Parts.cpp` に `OverlapSphereParts(root, tag, center, radius, out[], cap)` と `SphereCastParts(root, tag, origin, dir, radius, maxDist, outHit)` を足す。候補収集と index 昇順・厳密 `<` の規約は `RaycastParts` と同じ。形状判定は `MakePartBoundsPose` + 物理の `shapes::Collide` (球/カプセル vs 箱/球) を再利用できるか planner が確認する。
- C ABI 2 本。`MyeRaycastHit.entity` = 部位エンティティ。

### M95c 遷移の割り込み
- `ControllerTransition` に `canInterrupt` (JSON `"canInterrupt": 1`、既定 0) を足し、遷移中でも `canInterrupt` の遷移だけは評価する (Unity の Interruption Source の簡略版)。
- 採用時は進行中の遷移先を確定 (`currentState = transitionTo`、`stateTimeTicks = transitionToTime`、`statePhase = transitionToPhase`、`stateEntered = 0`) してから新しい遷移を張る。`AnimatorPlay(duration>0)` も同じ確定を行い、姿勢の飛びを無くす。
- controller アセット v3 (v2 はそのまま読める)。Editor の遷移インスペクタにチェックを追加。snapshot は変えない。

### M95d CC 同士の接触・押し出しと接触イベント (dogfooding 17 を解決)
- `CharacterControllerComponent` 末尾に `bool collideCharacters = false` と `bool reportContacts = false` を append (hash 対象、既定 off = 既存シーン不変)。
- 押し出し: 両者が `collideCharacters` のときだけ、`SolveCharacters` の既存の固定パスに CC 同士 (カプセル vs カプセル) を足す。index 昇順、深さは半々。ボスの体でプレイヤーを止めたい場面の規則 (質量の代わりに「優先度が高い方は動かない」など) は planner がユーザーと詰める。
- 接触イベント: `reportContacts` の CC について、CC↔CC と CC↔剛体 (kinematic 含む) の接触を `SolidContact` 相当に直して `ScriptHost` の `onCollisionEnter / Stay` へ流す (双方のスクリプトが受ける)。法線は相手→自分。
- ADR-034 に「CC は運動量を交換しない。押し出しと通知だけ」を記録する。

### M95e ルートモーションの縦成分 (制約解除 1)
- `AnimatorControllerComponent` 末尾に `bool applyRootMotionY = false` を append。true のときだけ `rootMotionVelocity.y` に縦速度を書く (false なら従来どおり 0 = hash 不変)。
- 適用先: CC は `velocity.y` を上書きして当該 tick の重力積分を飛ばす (CC 側に「外部が縦速度を書いた」印を 1 tick 持たせる)。非 kinematic Rigidbody は `velocity.y` も書く。NavMeshAgent が握っている間は従来どおり触らない。
- クリップ終端で y が 0 に戻れば重力へ自然に復帰する。

### M95f イベント時刻の秒・フレーム指定 (制約解除 2)
- `clipEvents` の各要素に `"time"` (秒) または `"frame"` を許し、クリップ単位の `"fps"` (既定 60) と合わせてロード時に `tick = llround(time * 60)` / `llround(frame * 60 / fps)` へ丸める (整数化の規則を 1 箇所に固定)。`tick` があればそれを優先。保存は従来どおり `tick` で書き戻す。
- `ClipEventEdit` に秒の併記を足す。内部表現は tick のまま。

### M95g 文書
- `engine_spec.md` (M89 の節への追記または新節)、`docs\adr\ADR-034-*.md`、`docs\dogfooding.md` の 17 を解決へ、`docs\test_checklists.md` に手動確認項目。

推奨順: a → b → c → e → f → d → g (a/b が縦切りで最初に要る。d が最大で他と独立)。最終的な分割は planner が決める。

## 設計の論点 (planner がユーザーと詰める)

1. **M95d の押し出し規則**。半々で押し合うか、優先度 (またはカプセル体積) で片側だけ動かすか。ボスの体当たりをどう表現するか。
2. **M95d のイベント範囲**。CC↔剛体の接触イベントを既定 off のまま opt-in にするか (既存スクリプトの `OnCollisionEnter` が新たに呼ばれる副作用を避ける)。
3. **M95e の CC 側の印**。`CharacterControllerComponent` に欄を足す (snapshot 変更) か、AnimatorController が `velocity.y` を直接書いて PhysicsSystem が同 tick の重力を飛ばす判定を別の経路で持つか。
4. **M95c の割り込み時の確定**。進行中のブレンド結果 (混合ポーズ) から混ぜ直すには姿勢のスナップショットが要るが、hash 対象を増やすので採らない。遷移先の確定で妥協する案の可否。
5. **M95a の停止中の SkinnedMesh**。ポーズプログラムの再適用で止まった姿勢を保てるか、SkinningSystem 側の時刻も止める必要があるか。

## 変更対象ファイル

- `src\Engine\Core\Ecs\Components.h` / `Components.cpp` (末尾 append と反射登録)
- `src\Engine\Engine\Animation\AnimatorController.h` / `.cpp`
- `src\Engine\Engine\Animation\Parts.h` / `.cpp`
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp` (`SolveCharacters`)
- `src\Engine\Engine\Script\ScriptHost.cpp`
- `src\Shared\EngineAPI.h` (v32) と C# ミラー (`src\Scripting`)
- `src\Engine\Engine\Replay\SimSnapshot.h` (v66)
- `src\Editor\Windows\Animation\ClipEventEdit.cpp` と遷移インスペクタ
- `engine_spec.md`、`docs\adr\ADR-034-*.md`、`docs\dogfooding.md`、`docs\test_checklists.md`

## 検証

- 回帰テスト: `AnimatorControllerSelfTest` (停止・割り込み・縦ルートモーション・秒指定の丸め)、`PartSelfTest` (重なり・掃引の決定論順序)、`PhysicsSelfTest` (CC 同士の押し出しと接触イベント)。
- `bin\x64\Debug\Editor.exe --selftest` と Release 版、`tools\check_rules.ps1`、`tools\replay_verify.bat` (opt-in 既定 off で既存シーンがビット一致すること)。
- 外部プロジェクトの一致: `C:\HAL\三校\tools\verify_sanko_replay.ps1` 相当で三校の 7 シーンが Debug / Release 一致のままであること。
- ABI bump の照合: Interop.cs の順序を機械照合 (memory `abi-bump-verification`)。
- 実機: 一時プローブのスクリプトでヒットストップ・部位ヒット・割り込みを動かし、`--screenshot` で確認。
