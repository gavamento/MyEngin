# sub-13: ベイクの確定が Undo の記録に割り込まない + 渋滞の後続で Stuck を出さない

- 依存: sub-12
- 状態: OK (コミット待ち)
- 往復: 1

## 背景
review-2 で 2 件の指摘を受けた。

- **#8 (major、coder 宛)**: sub-12 で、ベイクの確定を「選択に関係なく毎フレーム」に広げた (`InspectorWindow.cpp:586` の `CommitReadyNavBakes`)。その中の `CommitNavBake` は `undo.BeginRecord` を無条件に呼ぶ (`NavBakeCommit.cpp:53`)。`UndoStack::BeginRecord` (`UndoStack.cpp:31-39`) は、進行中の記録を黙って捨てる。
  - その結果、複数フレームにまたがる記録が途中で割り込まれ、ドラッグの編集が Undo に積まれない。該当する記録はギズモのドラッグ、Inspector のフィールドのドラッグ、エリアコストのドラッグ。プレハブの上書きの記録も抜ける。
  - reviewer のプローブで再現済み: `%TEMP%\m82rev\probe3.log`。
- **#9 (minor、planner 宛)**: 渋滞は最後には全員 Arrived になる。ただしその途中で、一時的な Stuck の WARN が出る (8 体・回避 3 で 5 回)。本当の詰まりと見分けにくい。

## やること
1. **#8**: Undo の記録中 (`undo.IsRecording()`) は、ベイクの確定を次のフレームへ持ち越す。完了したジョブは記録が終わるまで残し、記録が終わったフレームで確定する。確定は 1 Undo のまま変えない。
   - Clear (`ClearNavBake`) など、ほかに `BeginRecord` を呼ぶ確定経路があれば同じ扱いにする。
   - SelfTest: 記録の途中 (BeginRecord 済み・EndRecord 前) にベイクが完了しても確定されないことを確かめる。あわせて、記録を閉じた後に確定されること、Undo 1 回目でベイクが戻り、2 回目でドラッグの編集が戻ることを確かめる。reviewer のプローブと同じ手順 (Block を x 3 → 7 に動かす記録の途中で完了させる) を再現する。
2. **#9**: 渋滞の後続では Stuck にしない。前進が止まった Agent でも、**同じ目的地 (差 ≤ arriveDistance) へ向かう、自分より残り距離が短い Agent に接している** (水平の中心距離 ≤ 半径の和 × 2、sub-12 の `kJamTouchScale`) 間は `Moving` のままにし、WARN も出さない。到着の判定 (sub-12 の (a) / (b)) は変えない。
   - 列の先頭 (自分より目的地に近い仲間がいない Agent) が壁などで塞がれた場合は、これまでどおり先頭だけが Stuck + WARN になる。
   - SelfTest: 8 体・回避 3 で、全員が Arrived になるまでに Stuck の WARN が 0 回であること (WARN の回数か、Stuck になった Agent の数を数える)。経路の途中を壁で塞いだ列では、先頭が Stuck になり、後続は Moving のままであること。
3. ADR-023 の Stuck の節と、engine_spec.md 10.9 を新しい規則に合わせる。

## やらないこと (このサブでは)
- `FractureBakeService::Pump` (M80) にある同じ形の割り込み。M82 の範囲外とし、台帳の申し送りへ回す (別件)。
- round 1・2 で未確認のまま残った GUI の手操作項目。test_checklists.md の M82 節で、ユーザーが手動で確認する (既知の限界)。

## 触る場所 (planner の見立て)
- `src\Editor\Windows\Scene\InspectorWindow.cpp` (`CommitReadyNavBakes`)、`src\Editor\Tools\NavBakeCommit.cpp`、`NavEditorSelfTest.cpp`
- `src\Engine\Engine\Navigation\NavSystem.cpp` (Stuck の判定、`IsJamArrival` の近く)、`NavAgentSelfTest.cpp`
- `docs\adr\ADR-023-navmesh.md`、`engine_spec.md`

## 受け入れ条件 (このサブ)
1. 記録の途中で完了したベイクは確定されず、記録の後に確定される。Undo を 2 回すると、ベイクとドラッグの編集が順に戻る。— NavEditorSelfTest
2. 8 体・回避 3 の渋滞で、Stuck の WARN が 0 回。塞がれた列では先頭だけが Stuck。— NavAgentSelfTest
3. replay_verify の全 15 ジョブが PASS。両構成の Editor / Server の selftest で、Nav 系が ALL PASS。shot_verify は nav が PASS し、既知の 5 枚は値が不変。期待値を焼き直したなら、理由を書く。
4. 警告 0、check_rules 0。

## 検証コマンド
- 両構成のビルド。`Editor.exe --selftest` / `Server.exe --selftest` を両構成で直列に実行する
- `tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-13 (round 1)
- #8: InspectorWindow::CommitReadyNavBakes は undo.IsRecording() の間は TakeResult せず return (結果は Ready のまま持ち越し)。CommitNavBake / ClearNavBake も記録中は何も書かず false。NavEditorSelfTest に reviewer プローブと同じ手順 (Block x 3 -> 7 の記録中に確定拒否、記録後に確定、Undo 1 回目=ベイク、2 回目=ドラッグ) を追加。
- #9: NavSystem::IsBehindJamLeader (新規) を Stuck 判定の直前に挿入。kJamFollowTouchScale = 3。ADR-023 と engine_spec.md を更新。NavAgentSelfTest: 8 体 (回避 2 / 3) で Stuck 延べ 0、塞がれた廊下の 3 体列で同時 Stuck は 1 体・最後尾は 0。
- 差分: [逸脱] 接触距離は spec の kJamTouchScale (2) ではなく後続専用の 3。回避 3 の 8 体で半径の和の 2 倍を超えて開く間隔があり Stuck が 7 agent-tick 出たため (2 では spec の「WARN 0」を満たせない)。
- 差分: [追加] 塞がれた列は幅 1.2 m の廊下で試験。開けた床だと壁際で横並びになり先頭が毎回入れ替わる (spec の「先頭だけ」は同時 1 体として検証)。
- 検証: Debug/Release ビルド警告 0 / Editor・Server --selftest 両構成: Nav 系 ALL PASS (Editor の Source control 2 件 FAIL は既知)。check_rules 0。replay_verify 15 ジョブ全 PASS。shot_verify: nav PASS、既知 5 枚のみ FAIL (fracture_after 150/192 等、値不変)。期待ハッシュの焼き直しなし。
- 自己採点: 仕様適合 4 (接触倍率の逸脱) / 正しさ 4 / コード品質 4 / テスト 4。
- 申し送り: FractureBakeService::Pump (M80) に同形の割り込みあり (未対応)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-04)。#8: 確定経路 2 つ (Commit / Clear) の両方に、記録中は確定しない guard が入った。reviewer のプローブの手順も SelfTest で再現している。#9: 後続の接触距離 × 3 を採用し、spec 4.1 に反映した。既知の限界: 実際のギズモ操作中に確定が持ち越されるかは手で確かめていない (API の経路は SelfTest で確認済み)。倍率は実ゲームで未検証。
