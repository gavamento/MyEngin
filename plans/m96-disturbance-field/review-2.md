# review-2 — m96-disturbance-field

- 対象: `2b11247..9c42f0a` (sub-18 / M96r。review-1 の修正) と、その周辺の再実行
- 日付: 2026-11-09
- 作業ツリー: 検証後 clean (一時プローブは DisturbanceFieldSelfTest.cpp に入れて実行後に戻し、Release の Editor を HEAD 9c42f0a で再ビルド済み)

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 で突いた外側 3 点 (巨大な速度、復元後の通番、格子の縁と追従の流れ) はどれも実測で直った。今回は巨大な速度を Point / Radial のインパルスと Constant の源の 2 経路 × 2D / Volume × 速度 4 通り (1e11 / 1e30 / −1e30 / 3e38) で突いたが、どれも落ちず値は有限のまま。壊れた blob を復元しようとしても通番は動かない
  機能性: 4 — Debug の disturbance (Fast) は 2.642 / 2.624 / 2.617 秒。受け入れ 37 (c) の「約 2.5 秒」を 5% ほど超えるが、「約」の範囲と見て指摘にしない。disturbancelong (Full) は Debug 48.5 秒・Release 8.2 秒で PASS。焼き込み 0x67C1AB58E919A515 は Debug の selftest と Release の --field-bench で一致。--field-bench は all checks passed で、2D jobs なしの最良 1.204 ms、Volume jobs ありの最良 0.626 ms (受け入れ 15 / 20 の線を維持)
  ビジュアルデザイン: 4 — --field-demo (Deferred) の f60 を撮り直し、round 1 の同じ絵と比べた。速度の切り詰めで sim が変わっても、絵の差は maxDiff 1・2 画素だけで、球の周りの霧の割れ方は変わらない
  コード品質: 4 — 修正はどれも局所的で、理由がコメントに書いてある (ClampCellCoord は「格子外 = 一定値なので切っても値は変わらない」)。スコープの変化 (simparallel / watersurface を Full へ移す、受け入れの線を 2 → 2.5 秒にする) は spec §4.4 / §5 37 / §8 と sub-18 に記録があり、黙った変更ではない
指摘:
  (なし)
検証した手段:
  - 読んだ範囲: `git diff 2b11247..9c42f0a` の全ソース (DisturbanceStep.cpp / DisturbanceState.h / SimSnapshot.cpp / TickRunner.cpp / SelfTestSuites.cpp)、spec.md・ADR-036・harness.md の差分、sub-18.md
  - Debug: `MYE_SELFTEST_ONLY=disturbance --selftest` を 3 回 (2642 / 2624 / 2617 ms、0 failed)。`MYE_SELFTEST_ONLY=disturbancelong --selftest-full` → PASS (48500 ms、焼き込み一致)
  - Release: 一時プローブ入りで `MYE_SELFTEST_ONLY=disturbancelong --selftest-full` → PASS (8158 ms)。その後ソースを戻して再ビルドし、`--field-bench` → all checks passed、2D の final hash 0x67C1AB58E919A515
  - 一時プローブの出力 (scratchpad\r2_probe.log):
    - 速度: 16 通り全部で `queued=1 survived finite=1`。|vx| の最大は 2D インパルス 1.34〜1.48、Volume インパルス 1.15〜1.16、源 4.6〜5.1 で、どれも上限 cellSize×60 = 15 m/s 以下
    - 通番: `before=101 A=102 B=103 B+3=106 A+3=110 afterBrokenRestore=110`
    - 風と追従: `mid=2.000002 west=2.000001 south=2.000001 | follow origin -16->-8 incoming=2.000000 kept=2.000002`
  - スクショ: C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\14d68bbd-02de-486d-b73d-7903e06a5ea3\scratchpad\m96-shots\review2_field_def_f60.png。`Editor.exe --img-diff` で review1_field_def_f60.png と比べて maxDiff=1、diffPixels=2
  - 未実施: selftest-full の全体 / replay_verify / shot_verify (指示どおり sub-18 の結果を採用)、既定層全体の Debug 49.5 秒 (sub-18 の計測を採用)
前回指摘の消込:
  1. 解消 — RestoreSimSnapshot が差し替えの直前に InheritRenderSerial(scene 側) を呼ぶ (SimSnapshot.cpp:820)。中身の違う blob を続けて復元すると通番は 102 → 103 と進み、3 tick 進めた後も 106 / 110 で一致しない。壊れた blob の復元では動かない
  2. 解消 — 注入の目標速度を cellSize×60 で切り (LimitInjectionSpeed。非有限は 0)、逆追跡のマス座標も FloorToInt の前に [−2, n+1] へ切る (ClampCellCoord。2D と 3D)。round 1 で落ちた 1e11 / 1e30 を含む 16 通りが全部生存し、値は有限
  3. 解消 — Fast 層の disturbance は Debug 22.3 秒 → 約 2.6 秒。線は planner が 2.5 秒に緩めた (spec §4.4 / §5 37 (c))。実測はその「約」の範囲内
  4. 解消 — DisturbanceSampleAt が格子外の流れに WindTarget / WindTarget3D を渡す。縁から 0.1 マスの値は 1.200 → 2.000
  5. 解消 — FollowShift が入ってきたマスの流れを目標速度で埋める。入ってきた列 x=31 は 0.4997 → 2.000
  6. 解消 (planner 裁定) — 「許す」と裁定され、spec §2 #28・§4.4 の例外と ADR-036 D3 に記録された。コードは変えていない
  7. 解消 — TickRunner.cpp で RunOneTick の説明が RunOneTick の直上に戻り、BuildClipEventImpulse にも専用の 1 行コメントが付いた
```
