# review-2: m82-navmesh

- 対象: round 1 の範囲 + 修正コミット `e145a66` (M82k = sub-11) と `55b2367` (M82l = sub-12)。差分は `4e0d7e5..HEAD`
- 日付: 2026-10-04
- reviewer: harness-reviewer (round 2)

```
REVIEW: FAIL
round: 2
軸 (1-5):
  製品の深度: 4 — 同じ目的地へ 2 / 4 / 8 体を向かわせると、回避 2 / 3 のどちらでも 900 tick までに全員が Arrived になった。経路の途中を壁で塞がれた Agent は Stuck と表示され、止まらずに押し続ける。クエリの y は歩行面との差が 0.025 m 以内に収まった。残るのは渋滞の途中で一時的に出る Stuck の WARN (指摘 9) だけ
  機能性: 3 — 修正した部分は全部実測で通った。ただし、今回ベイクの確定を「選択に関係なく毎フレーム」に広げたことで、Undo の記録が別の記録の途中で割り込まれる不具合を作った。ギズモのドラッグやフィールドのドラッグの途中でベイクが確定すると、そのドラッグの編集が Undo に積まれない (指摘 8)
  ビジュアルデザイン: 4 — 段差の天面と坂に、編集中の SceneView でも golden でも塗りが付いた。段差の縁には、ADR 決定 13 に書かれている幅 3 セルほどのギザギザした帯が出る (受け入れ済みの既知の限界)。round 1 で未確認だった GUI 項目 (エリアコストのドラッグ = 1 Undo、Project Settings のエリア名、Link の警告 2 種、Stuck の Inspector 表示、登る tick の跳び) は、今回も自動では操作できず未確認
  コード品質: 4 — SampleSurfaceHeight は層のバイト列だけを読む純関数で、sim のハッシュは 1 つも動いていない。古いコメント・文書の訂正も確認した。残りは指摘 8 の割り込み
指摘:
  8. [major] 宛先: coder — CommitReadyNavBakes (InspectorWindow.cpp:586。OnImGui の先頭で毎フレーム呼ばれる) の中の CommitNavBake は、undo.BeginRecord を無条件に呼ぶ (NavBakeCommit.cpp:53)。UndoStack::BeginRecord (UndoStack.cpp:31-39) は、進行中の記録 pending_ を黙って捨てて上書きする。複数フレームにまたがる記録は次の 3 つで、どれもドラッグの開始から終了まで記録を開いたままにしている。
     - ギズモ (SceneViewWindow.cpp:1203-1225)
     - Inspector のフィールドのドラッグ (InspectorWindow.cpp:204-219 の HandleEditUndoMulti)
     - エリアコストのドラッグ
     この途中でベイクが確定すると、ドラッグの before が捨てられる。ドラッグの終わりの CaptureAfter / EndRecord は、記録中でないので何もしない。結果としてドラッグの編集は Undo に積まれず、CaptureAfter で撮るはずのプレハブの上書きの記録 (UndoStack.cpp の M48e のコメントにある「静かなデータ損失」の経路) も抜ける。round 1 ではベイクは Surface を選んでいるときにしか確定しなかったので、この重なりは同じ Surface のエリアコストのドラッグに限られていた。sub-12 の変更で、どのエンティティを編集していても起きるようになった。ベイクに秒単位かかるシーンで、待っている間に編集を続けると当たる — 根拠: 一時プローブ (`%TEMP%\m82probe2`。HEAD に NavEditorSelfTest の手順を 1 か所足したもの。リポジトリは無変更)。Gizmo の記録を開いて Block を x 3 → 7 へ動かし、その途中で CommitNavBake を呼び、そのあとでドラッグを終えた。ログは `C:\Users\akita\AppData\Local\Temp\m82rev\probe3.log`:
     - 「recording after commit: 0 / top label now: 'Bake NavMesh'」
     - 1 回 Undo すると「block x = 7.00 (drag start 3.00), navAsset null = 1」
     - 2 回目の Undo でも x = 7.00 のまま (ドラッグの編集を戻せない)
     — 期待: undo.IsRecording() の間は確定を次のフレームへ持ち越す (ReadyIds を残しておく)。「記録中にベイクが完了した」場合の SelfTest を足す。補足: FractureBakeService::Pump (InspectorWindow.cpp:592、M80) にも同じ形があるが、M82 の範囲外
  9. [minor] 宛先: planner — 渋滞は最後に全員 Arrived へ収まるが、その途中で一時的な Stuck の WARN が出る。8 体・回避 3 の構成では、Arrived へ移る前に「is stuck」が 5 回出た。集合のたびにログへ詰まりの警告が並ぶので、本当の詰まり (塞がれた経路) と見分けにくい — 根拠: `C:\Users\akita\AppData\Local\Temp\m82rev\probe2.err` の WARN 5 行 (残り 0.63〜1.65 m)、probe2.log `[same n=8 q=3] t=450 moving 1 arrived 7` → t=900 arrived 8 — 期待: 同じ目的地で到着済みの Agent が近くにいる間は WARN を出さない、などの扱いを決める。今のままで受け入れるなら ADR の既知の限界に書く
前回指摘の消込:
  1. 解消 — 同じ目的地へ 2 / 4 / 8 体 × 回避 2 / 3 の構成で、900 tick 後に全員 Arrived (probe2.log の [same ...])。Stuck は止まらずに押し続けることも確かめた。完成後に、x=0 に床全体を横切る壁を置いて経路を塞いだ構成では、Stuck (残り 8.44 m) と表示されたまま moveInput を出し続けた (probe2.log の [latewall])。NavSystem.cpp の IsJamArrival と、Stuck が止めなくなったことはコードでも確認した
  2. 解消 — 編集中の SceneView の段差の天面と坂に塗りが付いた (`C:\Users\akita\AppData\Local\Temp\m82rev\R2_edit_surface.png`)。golden も同じ (`C:\HAL\MyEngin\tests\golden\nav.png`。shot_verify で PASS)。表示の作り直しは 1,014 三角形で 2.38 ms (R2a.log)
  3. 解消 — NavSamplePosition の y を、段差 (天面 0.30) の 3 点で 0.325、30 度の坂の 4 点で歩行面との差 0.002〜0.018 m と測った (probe2.log の [sample ...])。EngineAPI.h と ADR 決定 13 に精度が書かれている
  4. 解消 (案 ii) — SimSnapshot.h:120-122 と SimSnapshot.cpp:554、EngineLoop.cpp:1031 に、「Nav 節の失敗だけは World を差し替えた後に false を返す」と明記された
  5. 解消。ただし新しい不具合を伴う (指摘 8) — ベイクの確定が OnImGui の先頭、`if (!open) return;` と ImGui::Begin より前に移ったので、選択・ウィンドウの開閉・折りたたみに関係なく確定する (InspectorWindow.cpp:584-589)。消えた Surface の結果は WARN を出して捨てる
  6. 解消 — Components.h / .cpp、NavSystem.h、PATCHES.md の各箇所が現状に合わせて書き直された
  7. 解消 — ADR-023:393 が、既存の C4127 14 件を除いた書き方に直った
  新規指摘 8 を round 1 で見落とした理由: round 1 では「確定するタイミング」(選択が条件) だけを見ていて、確定が Undo の記録と重なる場合を調べていなかった
検証した手段:
  - tools\replay_verify.bat (両構成をビルド) → 15 ジョブ全部 PASS (451.3 s)、rules 0 / 0
  - Editor.exe --selftest を Release → Debug の順に直列で実行 → FAIL は既知の Source control 2 件だけ (Nav 系は全 PASS)。Server.exe --selftest (両構成) → exit 0
  - tools\shot_verify.bat → nav PASS。FAIL は既知の 5 枚と同じ値 (198/3625、208/137、83/596、82/594、150/192)
  - Editor の編集中のスクショ (R2_edit_surface.png)、golden nav.png の目視
  - 一時プローブ (`C:\Users\akita\AppData\Local\Temp\m82probe2\`。HEAD の展開で、リポジトリは無変更):
    - probe2.log / .err: 渋滞、塞がれた経路、クエリの高さ
    - probe3.log: Undo の割り込み
  - 差分 `4e0d7e5..HEAD` の確認: NavSystem / NavTileCacheSupport / NavDebugDraw / InspectorWindow / NavBakeService / SimSnapshot / EngineAPI.h / 文書
  - 未確認 (手操作が要るもの): エリアコストのドラッグ = 1 Undo、Project Settings のエリア名、Link の警告 2 種と Stuck の Inspector 表示、登る tick の跳び。Inspector を閉じた構成での確定はコード読みだけ (OnImGui は EditorApp.cpp:767 で毎フレーム呼ばれ、確定は open の判定より前にある)
```
