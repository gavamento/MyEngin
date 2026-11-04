# sub-05: ルートモーションの縦成分 (ステート rootMotionY、CC / Rigidbody の verticalInput、snapshot v68)

- 依存: sub-02 (CC の末尾 append の順)、sub-04 (controller v3 の宣言)、Wave 1 の統合 (Wave 2。sub-03 と並列)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 最初と最後に両方で `git status` を確認し、SELF_EVAL に貼る
- `Components.h` / `Components.cpp` / `PhysicsSelfTest.cpp` / `AnimatorControllerSelfTest.cpp` / `SkeletonSelfTest.cpp` / `LocalizationTable.inl` への追記は **自分のマーカー (`// ---- M95 sub-05 ...`) の直下**。CC の sub-05 マーカーは sub-02 の 3 欄の下にある
- 同じ波の sub-03 のファイル (`Parts.*`、`EngineAPI.h`、`EngineApiTable.cpp`、`ScriptAPI.h`、`Interop.cs`、`MyeScript.cs`、`PartSelfTest.cpp`) を触らない

## やること

spec §4.1 E。ステート単位の `rootMotionY` で、ルートジョイントの縦の動きを `rootMotionVelocity.y` に出し、ポーズからは同じ量を抜き、
CC と非 kinematic Rigidbody には `useVerticalInput` / `verticalInput` で渡して物理がその tick の重力積分の代わりに使う。LocalTransform 適用先は y も足す。

## やらないこと (このサブでは)

- kinematic Rigidbody への縦の適用 (従来どおり LocalTransform へ)
- entity 単位の bool (spec §2 #15)
- スクリプト向けの新 ABI (`SetComponentField` で `useVerticalInput` / `verticalInput` を書ける)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Animation\AnimatorController.h:58-80` `ControllerState` に `int32_t rootMotionY = 0;`
- `src\Engine\Engine\Animation\AnimatorController.cpp`
  - `:404-410` `SkeletalSource` に `int32_t rootMotionY = 0`
  - `ToJson` のステート `skel` (`:988-1004`) に `rootMotionY` (1 のときだけ書く / ブレンドツリーは `skel` 直下)、`FromJson` のステート読み (`:1109-1110` の `skel` ブロック) で `skel.value("rootMotionY", 0)`。`:964` の版コメントに `rootMotionY` を追記 (値 3 は sub-04 が書いた)
  - `BuildSkeletalSources` でステートから `rootMotionY` を写す
  - `RootMotionDelta` (`:745-787`): 既存の 3D 累積は変えず、`rootMotionY` の層だけで `upDelta = Σ w · (m.delta · up)` を別の出力に積む。呼び出し元 (8. `:1904-1931`) は従来どおり回して `deltaWorld[1] = 0.0f` (`:1922`) を保ち、`rootMotionY` の層があるときだけ `up · upDelta` を `f.s` / `f.q` でワールドへ回した y を `deltaWorld[1]` に入れる (既存シーンは分岐に入らない = ビット同一)。`rootMotionVelocity.y = deltaWorld[1] * kTicksPerSecond`
  - ★ `RootMotionDelta` の中で層ごとに up 成分を引く案は不可: up が軸に揃っていない連鎖では x/z の丸めが変わり既存シーンが割れる
  - `ApplyRootMotion` (`:834-880`): 「この tick に `rootMotionY` の層が駆動した」旗を引数で受ける。CC は `moveInput.xz` に加え旗が立っていれば `useVerticalInput = true; verticalInput = velocity.y`。非 kinematic Rigidbody は `velocity.xz` に加え旗が立っていれば `useVerticalInput = true; verticalInput = velocity.y; isSleeping = false`。LocalTransform は `local[1]` も足す (今も足している — `deltaWorld[1]` が 0 だっただけ)。NavMeshAgent は従来どおり
  - 6. (`:1873-1893`) で `PoseLayer` へ `rootMotionY` を書く (`WriteSkeletalProgram`)。**冒頭の停止分岐 (sub-01) と 1. (sub-04) は触らない**
- `src\Engine\Core\Ecs\Components.h`
  - `:339-350` `SkinnedMeshComponent::PoseLayer` のマーカー直下に `int32_t rootMotionY = 0;` (NoHash だが生バイトは snapshot に載る)
  - `CharacterControllerComponent` の `// ---- M95 sub-05 (末尾 append) ----` 直下に `bool useVerticalInput = false; float verticalInput = 0.0f;`
  - `RigidbodyComponent` (`:463` 付近) のマーカー直下に同じ 2 欄
  - `Components.cpp` の CC / Rigidbody の反射登録のマーカー直下に 2 欄ずつ (読み取り専用)
- `src\Engine\Engine\Animation\SkinningSystem.cpp:110-170` `StripRootMotion`: 縦の寄与を `rootMotionY` の層だけで別に積み、層があるときだけ up に射影して抜く (水平は従来どおり全層)。`:193-196` のプログラム比較に `rootMotionY` も含める
- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp`
  - `:1526` CC の `vy` の初期化を `useVerticalInput ? verticalInput : velocity.y + g·dt` に、書き戻し (`:1672` 付近) で `useVerticalInput = false`。sub-02 が足した CC 同士の走査・記録のコードは触らない
  - `:1882` 剛体の収集 `b.vy = rb->velocity.y` を印付きに (`Body` に `bool skipGravityY` 等)、サブステップの重力 (`:3117-3122`) で y だけ飛ばす (印が無ければ 1 命令も変えない)、書き戻し (`:5765-5767`) で `useVerticalInput = false`
- `src\Engine\Engine\Replay\SimSnapshot.h:175` v68 + 版コメント (付録 A.3)
- selftest: `AnimatorControllerSelfTest.cpp` の `// ---- M95 sub-05 ----` 直下に 21 / 22 / 24 / 32 (Animator 側)、`PhysicsSelfTest.cpp` の `// ---- M95 sub-05 ----` 直下に 23 / 32 (物理側)、22 を `SkeletonSelfTest.cpp` に置くなら `// ---- M95 sub-05 ----` 直下
- `src\Editor\Windows\Animation\ControllerStateEdit.cpp` (ステートの編集 UI) に「縦のルートモーション」チェックがあると親切だが、**必須ではない** (JSON 手書きで足りる。足すなら `LocalizationTable.inl` の `// ---- M95 ----` 直下に `Anim_RootMotionY`)

## 受け入れ条件 (このサブ)

spec §5 の 21, 22, 23, 24, 25, 32。
- 21: ルートが y に動くクリップで `rootMotionY = 1` のとき `rootMotionVelocity.y == (y(t) − y(t−1)) × 60`、`rootMotionY = 0` のとき 0。既存の水平 / ヨーのテストが不変
- 22: 縦あり層 (ratio r) と縦なし層を混ぜたときに `StripRootMotion` が抜く縦の量が `r × (now.y − start.y)` に等しい
- 23: CC: `useVerticalInput` の tick は `velocity.y == verticalInput` (重力なし)、次 tick は `useVerticalInput == false` で重力に戻る。負の `verticalInput` で床に着くと `velocity.y == 0`、`isGrounded`。`jumpSpeed` と同時なら接地中は `jumpSpeed` が勝つ
- 24: applyRootMotion + CC で縦クリップを再生 → CC の y が上がって戻り、終端後に重力で落ちる。LocalTransform 適用先は y が足される。NavMeshAgent (`updatePosition`) 中は何も書かない
- 32: 非 kinematic Rigidbody (linearDamping 0): `useVerticalInput` の tick は物理後に `velocity.y == verticalInput` で x/z は従来どおり、消費され、次 tick は重力が戻る。kinematic には書かれない。applyRootMotion + Rigidbody で縦クリップを再生すると `velocity.y` にルートの縦速度、`isSleeping == false`
- 25: RV 全 PASS、三校 verify.bat PASS (最後の round)

## 検証コマンド

- ビルド Debug / Release (worktree)
- 往復中: `$env:MYE_SELFTEST_ONLY='animatorcontroller,physics,skeleton,simsnapshot'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、`cmd /c C:\HAL\三校\tools\verify.bat`

## 実装メモ (coder が追記)

### round 1
- 実装: ControllerState.rootMotionY / SkeletalSource.rootMotionY / PoseLayer.rootMotionY (JSON は skel 直下、1 のときだけ書く、版コメント追記)。RootMotionDelta は 3D 累積を変えず rootMotionY の層だけ upDelta を別に積む。8. で upDelta があるときだけ deltaWorld[1] に入れ rootMotionVelocity.y = deltaWorld[1]*60、ApplyRootMotion に verticalDriven を追加 (CC / 非 kinematic Rigidbody に useVerticalInput / verticalInput、Rigidbody は isSleeping=false)。StripRootMotion は verticalMask の層だけ up 方向も抜く。SamePoseInputs に rootMotionY。PhysicsSystem は CC の vy 初期化と Rigidbody の重力 y を切り替え、収集 (Rigidbody) / 書き戻し (CC) で useVerticalInput を消費。snapshot v68。
- 検証: Debug/Release ビルド OK。Release --selftest-full 110 本 0 failed。Debug --selftest-full は hash 定数更新前に 4 本 FAIL (下記)。check_rules 0 error。

### round 2
- #1: RootMotionDelta で rootMotionY の層のサンプル区間が空でない (startQ != endQ または wraps != 0) ときだけ outHasUp を立てる (outUpDelta の式は不変)。テストに「終端に張り付いた Rise は AnimatorPlay なしで旗が立たず重力が戻る」を追加。#2: SimSnapshot.h の版コメントを M95e へ。#3: 未対応のまま (spec §7 の記録待ち)。#4: Debug / Release --selftest-full 110 本 0 failed、check_rules 0 error、replay_verify 22 jobs PASS。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-05)。must 1 件: 受け入れ 24 の「クリップ終端で重力へ戻る」。spec §4.1 E に「駆動した」の定義を追加 = rootMotionY の層のうち、この tick のサンプル区間が空でない (RootLayerDelta の startQ != endQ または wraps != 0) 層が 1 つ以上あるときだけ verticalDriven。非ループの終端に張り付いた層は駆動しない → useVerticalInput を立てず重力へ (ステートを切り替えなくても落ちる)。upDelta の積算は変えない (旗だけ)。テスト: Rise (loop=0) を終端まで回し、AnimatorPlay せずに数 tick で cc->useVerticalInput == false かつ velocity.y < 0 (落下) を断言する。既存の「切り替えてから落ちる」断言は残してよい。should: 無し。nit: (1) SimSnapshot.h の版コメント「v68 (M95c)」は M95e (sub-05 のコミット接頭辞) に直す、(2) 一方通行床の 1 tick 遅れは既存の jumpSpeed と同じ遅れなので直さない (spec §7 に制限として記録、sub-07 の文書へ)。仕様側の訂正 (coder が正しかった): Rigidbody の消費は収集時、rootMotionVelocity.y は applyRootMotion を問わず書く。round 2 は OK を求める round なので ST-full Debug / Release (hash 定数更新後の最終コードで通し) + CR + RV を回すこと。computemanaged / fractureeditor の Debug FAIL は司会が無負荷で master と比較中 (sub-05 に起因しなければ別件)
- round 2: VERDICT OK (planner、2026-11-05)。must #1 解消: RootMotionDelta が層ごとに sampled (単一クリップ prevTime != time || wraps、ブレンドの子 startQ != endQ || wraps = RootLayerDelta に渡す区間そのもの) を求め、rootMotionY 層が sampled のときだけ outHasUp。upDelta の式は不変。終端の断言 (AnimatorPlay なしで旗が下り velocity.y == vEnd − g·dt) が PASS = 非ループの時計が終端で止まることも実測で確認。nit #2 (v68 M95e) 解消。検証: hash 更新後の最終コードで Debug full 110 / 0 failed、Release full 110 / 0 failed、CR 0 error、RV 22 ジョブ PASS。computemanaged / fractureeditor も Debug full で PASS (round 1 の FAIL は負荷下の時間依存)。三校は規則どおり統合時に司会
