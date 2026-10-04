# review-3: m82-navmesh

- 対象: round 1・2 の範囲 + 修正コミット `6e6b1ef` (M82m = sub-13)。差分は `55b2367..HEAD`
- 日付: 2026-10-04
- reviewer: harness-reviewer (round 3、最終)

```
REVIEW: PASS
round: 3
軸 (1-5):
  製品の深度: 4 — 同じ目的地へ 2 / 4 / 8 体 (回避 2) と 8 体 (回避 3) を向かわせると、全員 Arrived になり、途中の Stuck は延べ 0 tick。一列にしか並べない廊下を塞いだ列では、Stuck になるのは先頭の 1 体だけで、最後尾は 0 tick (列は 3 体で、真ん中の値はログに出ない)。渋滞の定数が実ゲームで未検証なのは、planner の線引きどおり既知の限界として扱う
  機能性: 4 — ベイクの確定は、Undo の記録中は持ち越される。記録が閉じたあとに確定し、Undo 1 回目でベイクだけ、2 回目でドラッグが戻る。replay_verify は 15 ジョブ全部 PASS、selftest と shot_verify も既知の FAIL だけ
  ビジュアルデザイン: 4 — round 2 から絵は変わっていない (nav の golden は PASS、maxDiff 0)。段差の縁のギザギザした帯と、GUI の手操作の項目は線引きどおり既知の限界
  コード品質: 4 — 持ち越しは CommitNavBake / ClearNavBake (NavBakeCommit.cpp:37-40, :70) と CommitReadyNavBakes (InspectorWindow.cpp:1812-1815) の両方で守られている。IsBehindJamLeader は「残り距離が厳密に短い相手だけ」を見るので、互いに待つ循環が起きない。キー順の走査で決定的
指摘: なし
前回指摘の消込:
  8. 解消 — Undo の記録中は確定しない (`undo.IsRecording()` で抜け、結果は Ready のまま次のフレームへ)。NavEditorSelfTest の節 3 が round 2 のプローブと同じ手順 (記録を開く → Block を 7 へ → Commit / Clear → 記録を閉じる → Commit → Undo × 2) を再現し、Debug / Release とも 8 項目 PASS。r3self_*.log の「mid-record: ...」行で確認した
  9. 解消 — 渋滞の後続は Stuck にならず、WARN も出ない。NavAgentSelfTest の [jam] は 2 / 4 / 8 体 (回避 2) と 8 体 (回避 3) のどれも「stuck agent-ticks 0」で全員 Arrived。塞がれた列は「max stuck at once 1, tail stuck ticks 0」で、先頭だけが Stuck になる
検証した手段:
  - tools\replay_verify.bat (両構成をビルド) → 15 ジョブ全部 PASS (281.1 s)、rules 0 / 0。ビルドの警告は既知の C4127 だけ
  - Editor.exe --selftest を Release → Debug の順に直列で実行 → FAIL は既知の Source control 2 件だけ。Nav 系は全 PASS。Server.exe --selftest (両構成) → exit 0。ログは `C:\Users\akita\AppData\Local\Temp\m82rev\r3self_*.log / .err`、`r3server_*`
  - tools\shot_verify.bat → FAIL は既知の 5 枚と同じ値 (198/3625、208/137、83/596、82/594、150/192)。nav は PASS (`C:\Users\akita\AppData\Local\Temp\m82rev\r3shot.log`)
  - 差分 `55b2367..HEAD` (NavBakeCommit / InspectorWindow / NavSystem の IsBehindJamLeader / 2 つの SelfTest / 文書) を全部読んだ
  - 線引きどおり既知の限界として扱ったもの: GUI の手操作の項目 (test_checklists.md の M82 節)、段差の縁の帯、渋滞の定数、台の中に取り残される床、FractureBakeService::Pump の割り込み (M80、別件)
```
