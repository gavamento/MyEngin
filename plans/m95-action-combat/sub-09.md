# sub-09: ヒットストップ中は縦ルートモーションの高さを保つ + 残り minor のテスト 2 件 (review-1 #2 / #4)

- 依存: sub-07 (master 592ffff 以降)。sub-08 と並列可
- 状態: 未着手
- 往復: 0

## 作業場所の注意

- 司会が指示した場所で作業する。worktree なら絶対パスだけを使う。最初と最後に `git status` を取る
- sub-08 が並行して `PhysicsSystem.cpp` / `PhysicsSelfTest.cpp` を触っている。このサブはその 2 ファイルを触らない

## やること

- spec §2 #27 と §4.1 A の「停止中の縦」。停止分岐 (`AnimatorController.cpp` の `if (c->pauseTicks > 0)`、現状 `ApplyRootMotion(world, e, zero, c->rootMotionVelocity, false)`) の最後の引数を次の条件にする: 凍らせた SkinnedMesh の層に `rootMotionY != 0` かつ `weightQ > 0` の層が 1 つでもある (`driven_` のうち `poseLayerCount > 0` の最初のメッシュで判定してよい。層は全メッシュで同じプログラム)。速度は 0 のままなので、CC / 非 kinematic Rigidbody には `useVerticalInput = true, verticalInput = 0` が入り、Rigidbody は `isSleeping = false` になる。LocalTransform は delta 0 で動かない
- 台帳の残り minor のテスト 2 件 (spec 受け入れ 45): `clipEvents` の `fps: -30`、非アクティブ entity の `pauseTicks`

## やらないこと (このサブでは)

- 非アクティブ分岐の変更、欄の追加、snapshot 版・ABI・焼き込み hash の変更
- 停止中に「前 tick に実際に駆動していたか」を厳密に分けること (終端に張り付いた層でも保つ。spec §2 #27)
- 文書とマーカーの整理 (sub-10)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Animation\AnimatorController.cpp`: 停止分岐 (`:1834-1846` 付近)。分岐の上にあるコメント「ルートモーションは 0 を書く (止まった tick に滑らせない)」に、縦は高さを保つ旨を足す
- `src\Engine\Engine\Animation\AnimatorControllerSelfTest.cpp`:
  - 受け入れ 44: `// ---- M95 sub-05 ----` のブロックにある「CC + 物理 (空中)」テスト (`:2236` 付近) の形を流用し、同じブロックの末尾に足す。Animator + SkinnedMesh + CC + PhysicsSystem の実経路で、停止 0 / 6 / 12 tick を比べる。Rigidbody 版と、rootMotionY でないステートの CC が停止中も落ちる対照も入れる
  - 受け入れ 45 (2): `// ---- M95 sub-01 ----` のブロックの末尾に足す
  - 受け入れ 45 (1): `// ---- M95 sub-06 ----` のブロックの末尾に足す
  - マーカー行には触れない

## 受け入れ条件 (このサブ)

spec §5 の 44, 45。

- 44: 停止中の CC は各 tick で `|Δy| <= 1e-5`、物理の後に `velocity.y == 0`。停止の明けた後の y の列は、停止しなかった場合の列と停止 tick 数ぶんずらして `<= 1e-4` で一致し、クリップ終端の y の差も `<= 1e-4`。非 kinematic Rigidbody も停止中に y が変わらない。rootMotionY でないステートの CC は停止中も重力で落ちる
- 45: (1) `fps: -30` は警告して 60 になり、`frame: 3` → tick 3。(2) 非アクティブ 10 tick で `pauseTicks` は 5 のまま、アクティブに戻して 3 tick で 2
- 受け入れ 1〜3 の既存テストが全 PASS

## 検証コマンド

- ビルド: Debug / Release
- 往復中: `$env:MYE_SELFTEST_ONLY='animatorcontroller'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回):
  - `--selftest-full` (Debug / Release)
  - `tools\check_rules.ps1`
  - `tools\replay_verify.bat` (chcp 65001 のまま `cmd /c`)
- 三校は統合時に司会が回す

## 実装メモ (coder が追記)

## フィードバック履歴
