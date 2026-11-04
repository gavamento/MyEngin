# sub-08: 壁へ押し付けられた CC の押し返し (review-1 #1)

- 依存: sub-07 (master 592ffff 以降)。sub-09 と並列可
- 状態: 未着手
- 往復: 0

## 作業場所の注意

- 司会が指示した場所 (master 直、または worktree) で作業する。worktree なら絶対パスだけを使い、`C:\HAL\MyEngin` へは書かない。最初と最後に `git status` を取る
- sub-09 が並行して `AnimatorController.cpp` / `AnimatorControllerSelfTest.cpp` を触っている。このサブはその 2 ファイルを触らない

## やること

spec §2 #26 と §4.1 D の「障害物への押し付け」(i)〜(iii)。

- (i) 壁の法線を前計算する。`collideCharacters` の CC が 1 体でも居る tick に限り、`SolveCharacters` の解決ループの前に計算する。対象は各 CC の tick 頭のカプセル (半径 + `skinWidth`) と solid の障害物。押し出しループと同じものを除外する (自分の collider / `CanCollide` / plane2D の一方通行)。法線が `|n.y| < cosSlope` のものだけを、障害物 index 昇順に最大 `kCharMaxWallNormals = 4` 本、`CharBody` へ積む。`CharBody` は一時データ (コンポーネントの欄も snapshot も変えない)
- (ii) 押し付けを判定する。`c` の CC 走査では、従来どおり優先度の低い `other` を飛ばす。ただし `other` の壁の法線 `w` のどれかで `(other.px − c.px)·w.x + (other.pz − c.pz)·w.z < 0` (tick 頭の位置で見て、`c` が `other` を壁へ押し込む向き) なら、飛ばさずに同優先度として押し出しを受ける。plane2D の CC は z 項を 0 とする。正規化はせず、符号だけを見る
- (iii) 各パスの中で、CC 同士の押し出しを障害物の押し出しより**先**に行う
- 接触の記録 (`reportContacts`) の規則は変えない (記録する相手・key・法線の向きは従来どおり。走査順が変わるので記録の順は変わってよいが、合流で key 昇順にソートするので出力は同じであること)

## やらないこと (このサブでは)

- コンポーネントの欄・snapshot 版・ABI・焼き込み hash の変更 (要るなら実装を止めて「不安・質問」で planner へ)
- 押し付けの多段伝播、1 tick で半径を超える歩幅への対策 (spec §4.1 D の既知の制限)
- 文書 (engine_spec / ADR-034 / test_checklists) — sub-10 が書く。実装上の知見は SELF_EVAL の申し送りへ
- 予約マーカーの整理 (sub-10)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Physics\Rigid\PhysicsSystem.cpp`
  - `CharBody` (`:1355`): 壁の法線の配列 (`float` × 3 × 4 + 本数) を足す
  - `SolveCharacters`: 解決ループの前に前計算を置く。ゲートは「`collideCharacters` の CC が 1 体でも居るか」。パスのループ (`:1630` 付近) で、CC の走査 (`:1671-1697`) を `bodies` の走査の前へ移す。CC の走査の優先度判定 (`other.cc->pushPriority < c.cc->pushPriority` で `continue`) に (ii) の例外を足す
  - 定数 `kCharMaxWallNormals = 4` は既存の 2D 定数群 (`:1378-1380`) の近くに置く
- `src\Engine\Engine\Physics\Rigid\PhysicsSelfTest.cpp`: `// ---- M95 sub-02 ----` のブロックの末尾 (sub-02 のテストの後、`// ---- M95 sub-05 ----` の前) に受け入れ 42 / 43 のテストを足す。マーカーの書き換えは sub-10 がやるので、このサブはマーカー行に触れない

## 受け入れ条件 (このサブ)

spec §5 の 42, 43。

- 42: 壁の厚さ 0.2 / 0.5 / 1.0 m の 3 通りで次を全 tick 確認する
  - 低い側の中心 x が `壁面 x − r + 1e-3` 以下に留まる
  - 高い側の中心 x が `低い側の中心 x − 2r + 1.5/60 + 1e-3` 以下に留まる (重なりは最大 1 歩)
  - 最後の 60 tick で高い側の x が 1e-3 以上進まない
  - 対照 (a) 壁が無いと低い側は押されて動く、(b) 壁と平行に押す配置では低い側が壁沿いに動く (押し付け扱いにならない)、(c) 同優先度は互いに止まる
  - 受け入れ 7〜11 の既存テストが全 PASS
- 43: CC の生成順を入れ替えたシーンで、42 の 3 通りの最終位置が entity どうしでビット一致する

## 検証コマンド

- ビルド: Debug / Release
- 往復中: `$env:MYE_SELFTEST_ONLY='physics'; bin\x64\Debug\Editor.exe --selftest` (physics は Full 層なので名指しする。終わったら環境変数を消す)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層) と ST(physics)
- VERDICT OK の直前 (最後の round で 1 回):
  - `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`
  - `tools\check_rules.ps1`
  - `tools\replay_verify.bat` (chcp 65001 のまま `cmd /c`。spec §2 #25)
- 三校は統合時に司会が回す (欄の変更が無いので DLL の作り直しは不要)

## 実装メモ (coder が追記)

## フィードバック履歴
