# sub-08: CC 同士の押し出しを障害物への掃引で切る (review-1 #1、ユーザー判断 = ボスが押し続ける)

- 依存: sub-07 (master 592ffff 以降)。sub-09 と並列可
- 状態: 未着手 (2026-11-05 にユーザー判断で仕様を改訂。初版の「壁の法線 + 同優先度」案は却下)
- 往復: 0

## 作業場所の注意

- 司会が指示した場所 (master 直、または worktree) で作業する。worktree なら絶対パスだけを使い、`C:\HAL\MyEngin` へは書かない。最初と最後に `git status` を取る
- sub-09 が並行して `AnimatorController.cpp` / `AnimatorControllerSelfTest.cpp` を触っている。このサブはその 2 ファイルを触らない

## やること

spec §2 #26 と §4.1 D の「障害物への押し付け」(i) / (ii)。

- 掃引関数 `CharPushAllowance(pose, dir, L, 除外の文脈) -> t` を作る
  - `pose` を `dir` 方向へ `t ∈ [0, L]` 動かしても、solid の障害物それぞれへの `shapes::Collide` の深さが「動かす前の深さ + `kCharSweepTol` (1e-4)」を超えない最大の `t` を返す
  - 障害物の除外は押し出しループと同じ (自分の collider / `CanCollide` / plane2D の一方通行)
  - 求め方は固定回数にする
    - 区間数 `n = max(kCharSweepScanSteps (8), ceil(L / radius))`、上限 64
    - 区間の端点を `t` の小さい順に調べる
    - 最初に破れた端点とその手前の端点の間を、`kStepBisectIters` (12) 回の二分探索で詰める
    - 破れない側を返す。全点で破れなければ `L` を**そのまま**返す
    - 早期 break はしない (回数を消化する)
  - 方式を変えたい場合は「不安・質問」で理由を書く。固定回数・順序非依存・開けた場所で `L` をそのまま返す、の 3 点は外さないこと
- (i) 受ける側: CC 同士の押し出し (優先度 `>=` の相手) を `applyPush(n, depth)` から `applyPush(n, CharPushAllowance(今の姿勢, n, depth))` に変える。plane2D の CC は z 成分 0 の向きで掃引する。接地 / 天井の判定は従来どおり `n` で行う
- (ii) 押す側: 優先度の低い相手も `Collide(pose, 相手の tick 頭)` を取る。重なっていれば `room = CharPushAllowance(相手の tick 頭の姿勢, −n, depth)` を求め (相手の層・マスク・一方通行の文脈で)、`depth − room > 0` のときだけ `applyPush(n, depth − room)` する
- 接触の記録 (`reportContacts`) は従来どおり「重なったら記録」。押し出しの量とは無関係
- パスの順 (`bodies` の走査 → CC の走査) は変えない

## やらないこと (このサブでは)

- コンポーネントの欄・snapshot 版・ABI・焼き込み hash の変更 (要るなら止めて planner へ)
- 掃引の相手に CC を含めること (押し付けの伝播は 1 段だけ = spec の既知の制限)
- 掃引で切った残りを壁に沿って滑らせること
- 文書 (sub-10)・マーカーの整理 (sub-10)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp`
  - `SolveCharacters` の CC 走査 (`:1671-1697`)
  - 優先度で `continue` している箇所 (`other.cc->pushPriority < c.cc->pushPriority`)
  - 掃引関数は `TryCharStepUp` (`:1415`) の近く。二分探索の流儀は `:1464` を参考にする
  - 定数 `kCharSweepTol` / `kCharSweepScanSteps` / 区間数の上限は既存の定数群 (`:1367-1380`) の近く
  - 相手の tick 頭の一方通行の文脈 (plane2D の相手のとき) は、相手の `CharBody` の tick 頭の値から作る (`oneWay.startFeetY = py − halfSeg − radius` 等。今の `c` 用の作り方 `:1598-1604` を関数にして共有してよい)
- `src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp`: `// ---- M95 sub-02 ----` のブロックの末尾 (sub-02 のテストの後、`// ---- M95 sub-05 ----` の前) に受け入れ 42 / 43。マーカー行には触れない

## 受け入れ条件 (このサブ)

spec §5 の 42, 43。

- 42: 壁の厚さ 0.2 / 0.5 / 1.0 m の 3 通りで、全 tick の末に次が成り立つ
  - 低い側の中心 x ≤ `壁面 x − r + 1e-3`
  - 高い側の中心 x ≤ `低い側の中心 x − 2r + 1e-3`
  - 最後の 60 tick で高い側の x の進みが 1e-3 未満、かつ `moveInput.x == 1.5` のまま
- 42 の追加配置
  - (d) `moveInput.x = 30` で 0.2 m の壁でもすり抜けない
  - (e) 壁から 0.1 m の隙間がある場合、低い側は壁に接するまで動き、高い側はその分だけ進む
- 42 の対照
  - (a) 壁が無ければ従来どおり (高い側は低い側を無視して進み、低い側が押される)
  - (b) 壁と平行に押す配置では低い側が壁沿いに動く
  - (c) 同優先度の 2 体を壁際で押し合わせても壁を抜けない
- 受け入れ 7〜11 の既存テストが**数値を変えずに**全 PASS する。落ちたら書き換えずに「不安・質問」へ出す
- 43: 42 の全配置で CC の生成順を入れ替えても、最終位置が entity どうしでビット一致する

## 検証コマンド

- ビルド: Debug / Release
- 往復中: `$env:MYE_SELFTEST_ONLY='physics'; bin\x64\Debug\Editor.exe --selftest` (physics は Full 層なので名指しする。終わったら環境変数を消す)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層) と ST(physics)
- VERDICT OK の直前 (最後の round で 1 回)
  - `--selftest-full` (Debug / Release)
  - `tools\check_rules.ps1`
  - `tools\replay_verify.bat` (chcp 65001 のまま `cmd /c`。spec §2 #25)
- 三校は統合時に司会が回す (欄の変更が無いので DLL の作り直しは不要)

## 実装メモ (coder が追記)

## フィードバック履歴

- 2026-11-05 仕様改訂 (planner): ユーザー判断「ボスが押し続ける」により、初版 (壁の法線の前計算 + 押し付けの組を同優先度 + CC の押し出しを障害物の前へ) を掃引ベースへ書き換えた。まだ着手前
