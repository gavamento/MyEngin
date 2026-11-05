# M95 review round 2

- 対象: `592ffff..26cb537` (master。修正 559b234 / fd399e7・7c6fec0 / 26155d7・9cf53d6 / 5e2e684、台帳 26cb537)
- 日付: 2026-11-05
- reviewer: harness-reviewer

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 の 2 件はどちらも、目的の縦切りの場面で直っていることを実測で確かめた。壁際の体当たりでは、壁の厚さ 0.2 / 0.5 / 1.0 m、30 m/s、隅への斜めの押し込みのどれでも、低い側が壁を抜けない。跳び込み斬り中のヒットストップでは、CC / Rigidbody とも停止中の |dy| が 0 で、終端の高さも止めなかった場合と同じ。残るのは「ボス → 雑魚 2 体 → 壁」の多段の場合だけ。文書は「A と B が重なり得る」と書いているが、実測ではボスが A をすり抜けて B と重なる (指摘 1、minor)
  機能性: 5 — 受け入れ 42〜46 の自動検証が通る (Debug の m95 群 7 スイート、Release --selftest-full 110 スイート 0 failed)。RV 22 ジョブ PASS (既存シーンはビット同一)、CR 0 error。指摘は無い
  ビジュアルデザイン: N/A — round 2 の修正に見た目の変更は無い (コメントと文書だけ)
  コード品質: 4 — 掃引は固定回数 (区間走査 + 二分探索 12 回、早期 break なし) で、tick 頭の姿勢と障害物しか読まず、順序非依存。テストで生成順を入れ替えてビット一致も確かめている。分岐ゲートで既存シーンはビット同一。予約マーカーは機能名の見出しへ整理済み。気になるのは、CC が密集した場面での掃引のコストを測っていないことだけ (指摘 2、minor)
指摘:
  1. [minor] 宛先: planner — 多段の押し付けの既知の制限の書き方が、実際の挙動と合っていない。
     engine_spec §10.14 D の (1) と ADR-034 の影響欄は「ボスは B で止まらず A を B へ押し込み、A と B は重なり得る」と書いている。実測では、A と B は重ならず (0.6 m 離れたまま)、ボスが A をすり抜けて壁際まで進み、B と 0.4 m の距離で重なる。B が壁を抜けない保証は守られている。縦切り (ボス 1 体 + プレイヤー 1 人) では起きない配置。
     根拠: 一時プローブ (地面と x=1.0〜1.2 の壁、ボス優先度 1 を x=-1.5 から moveInput.x=1.5、A・B 優先度 0 を x=-0.3 / 0.3、300 tick)。出力: `chain boss->A->B->wall(0.2m): boss.x=0.7001 A.x=-0.3002 B.x=0.2998 maxB=0.3011`
     期待: 文書の (1) を「ボスは手前の CC をすり抜けて、壁際の CC と重なり得る」に直す。あるいは、雑魚の群れを押す用途を後続の課題として台帳に残す。
  2. [minor] 宛先: planner — `collideCharacters` の CC が密集した場面の物理コストを記録していない。
     `CharPushAllowance` はサンプル 1 点ごとに全障害物と `shapes::Collide` を 2 回ずつ呼ぶ。動かす前の深さもサンプルのたびに計算し直していて、broadphase も無い。区間は 8〜64 + 二分探索 12 回、重なった組ごと × 4 パスかかる。
     根拠: プローブで、円形に並べた静的な箱と 0.25 m 間隔に密集させた CC を 120 tick 回した (Release、無負荷ではない)。
     - 箱 64 / CC 10: collide off 0.418 → on 1.608 ms/tick
     - 箱 256 / CC 20: 3.036 → 7.940 ms/tick
     round 1 のコードとの比較はしていないので、増分のうち掃引の分は切り分けていない。縦切りの 2 体なら問題にならない規模。
     期待: 雑魚の群れに collideCharacters を付ける用途を想定するなら、計測値を台帳か engine_spec に残す。改善案: depth0 を前計算する、AABB で障害物を絞る。いまは直さなくてよい。
検証した手段:
  - 読んだ範囲: spec の差分 (§2 #26〜#28、§4.1 A / D / E、受け入れ 42〜46、§6、§8)。`git diff 592ffff..26cb537` の製品コード (PhysicsSystem.cpp の CharPushAllowance / MakeCharOneWayContext / SolveCharacters / startVelY、AnimatorController.cpp の停止分岐)。テストの差分 (PhysicsSelfTest の CC wall push、AnimatorControllerSelfTest の hit stop / fps 負値 / 非アクティブ)。マーカーの書き換え、文書の差分 (engine_spec §10.14 A / D / E と §12.2、ADR-034、dogfooding の見出し、test_checklists)
  - ビルド: master 26cb537 で Release / Debug を増分ビルド、どちらも exit 0
  - `cmd /c "bin\x64\Release\Editor.exe --selftest-full > log 2>&1"` → `[selftest] total 110 suites, 0 failed, 87225 ms (mode: full)`
  - `MYE_SELFTEST_ONLY=m95` で Debug の `--selftest` → `total 7 suites, 0 failed, 51832 ms (mode: only)`。CC wall push 15 件 (壁 3 通り、30 m/s、隙間 0.1、壁なし、平行、同優先度、生成順入れ替えでビット一致、半径 0) と hit stop 6 件が PASS
  - `tools\check_rules.ps1` → `0 error(s), 50 warning(s)`
  - `tools\replay_verify.bat` (chcp 65001) → `all 22 jobs passed in 232.4s` / `[PASS] replay consistency (...)`
  - 一時プローブ (scratch の probe2、master の Release Engine.lib にリンク。master には書いていない)。結果は次のとおり。
    - 壁際の押し付け: 壁 1.0 / 0.5 / 0.2 m で `mob.x=0.7001 boss.x=0.1002`。30 m/s で `mob.x=0.7000`、ボスは壁ごと突き抜ける (`boss.x=119`、§4.1 D の既知の制限どおり)
    - 隅への斜めの押し込み: `mob=(0.7001,0.7001)`、距離 0.5999
    - 多段: 指摘 1
    - ヒットストップ: CC で停止 0 / 6 / 12 tick、Rigidbody で停止 0 / 6 tick。どれも `max |dy| while paused 0.000000`、終端の y は 10.0333 で一致 (round 1 は +0.143 / +0.187 m ずれていた)
    - 性能: 指摘 2
  - Debug の `--selftest-full` と三校 verify は回していない。台帳「波の統合」表の review-1 修正の行 (Debug / Release full 110 suites 0 failed、三校は既存 2 件のみ) を証拠として採用した。sub-10 はコメントと文書だけ
  - 別件候補の切り分け (`[script] [csharp] compile FAILED — keeping previous scripts`): 出所は computescript スイート (ComputeManagedSelfTest.cpp) の意図的な負のテスト「RW を int で添字する C# はコンパイルエラーになる (前の登録は残る)」。BadIndexProbe.cs(9,11) の CS1503 を出し、そのテスト自体は PASS する。M91g (8afe3d3、2026-10-12) から入っている = M95 以前からある正常なログ。別件として起票する必要はない
前回指摘の消込:
  1. [major] 壁際の押し付けでのすり抜け / 重なり — 解消。ユーザー判断 (掃引で切る) どおりに実装されている。round 1 と同じ配置で、壁 0.2 / 0.5 / 1.0 m のどれも低い側が壁面 − r (0.70) を超えない。ボスは相手の手前 (0.1002) で止まり、moveInput は 1.5 のまま。30 m/s で押す側が抜けるのは、ユーザー判断で既知の制限 (文書化済み)
  2. [major] ヒットストップ中に縦へ流れる — 解消。CC / Rigidbody とも停止中の |dy| が 0 で、終端の y は止めなかった場合と一致する。rootMotionY でないステートの CC が停止中に落ちること (§2 #3) もテストで守られている
  3. [minor] 予約マーカーの残り — 解消。`grep -rn "// ---- M95 sub-" src` は 0 件、中身の無いマーカーも 0 件。ただし受け入れ 46 の字面「直下が空行のマーカーが 0 件」には 3 件当たる (AnimatorControllerSelfTest.cpp:2090 / :2347、PhysicsSelfTest.cpp:9225)。どれも見出しの次が空行で、その下に中身のブロックがある。条件の趣旨 (中身の無いマーカーを消す) は満たしているので、指摘にはしない
  4. [minor] 台帳の残り minor — 解消。fps 負値と非アクティブ entity の pauseTicks のテストが入り、PASS している (受け入れ 45)。dogfooding の見出しは「M95 時点」、§12.2 の期間は「2026-07-19 → 2026-11-05」に直っている。fracture / servernet の縮小と別名 m95 の固定は、台帳どおり後続の扱い
新規指摘を前回書かなかった理由: 1 は round 1 で押し付けの検査を 2 体に限っていて、多段の配置を試していなかった。2 は round 2 で入った掃引のコードを受けての新しい観点
```
