# review-1: m82-navmesh

- 対象コミット範囲: `159cff102b872e4b55c57034adefda9db912a6a7..HEAD` (M82a `9944784` 〜 M82j `4e0d7e5`。`b1920a7` はハーネスの外)
- 日付: 2026-10-04
- reviewer: harness-reviewer (round 1)

```
REVIEW: FAIL
round: 1
軸 (1-5):
  製品の深度: 3 — 決定論・復元・Obstacle/Modifier/Link・ABI は厚く作られていて、実測でも崩れなかった。ただし、同じ目的地へ複数の Agent を向かわせるという一番よくある使い方で、既定設定 (回避あり) のまま Agent が Stuck で恒久停止する (指摘 1)。経路クエリが返す高さも、段差や坂の上では 0.25〜0.4 m ずれる (指摘 3)
  機能性: 4 — 受け入れ条件 1〜19 を検証した手段は全部 PASS した (既知の FAIL は除く)。replay_verify は 15 ジョブ全部 PASS。selftest は Editor (Debug / Release) と Server (Debug / Release) の両方で Nav 4 種が PASS。shot_verify は nav が PASS (maxDiff 0) で、FAIL は既知の 5 枚と同じ値。NavDeterminism のハッシュは Debug と Release で一致した。残りは minor だけ
  ビジュアルデザイン: 3 — 半透明の塗り・輪郭・範囲箱ギズモ・Play 中の表示・carve=false の注意書きは画像で確認できた。ただし塗りが TileCache のポリゴン高さで描かれていて、0.3 m の段差の天面は塗りが床下に埋もれて見えない。坂の塗りは浮いたり沈んだりする (指摘 2)。「Bake 直後に歩ける範囲を目で確かめる」という表示の目的が、段差の上では果たせていない
  コード品質: 4 — 層の分離 (Recast の型は Navigation に閉じる)、キー順の決定的な走査、名前付き定数、理由を書いたコメントは規約どおり。残ったのは古いコメント・文書 (指摘 6, 7) と、復元の原子性の契約の穴 (指摘 4) で、どれも minor
指摘:
  1. [major] 宛先: planner — 同じ目的地へ向かう複数の Agent が、既定の回避 (avoidanceQuality 2) のままだと Stuck になって恒久に止まる。目的地の手前で互いに塞ぎ合う渋滞を、spec 4.1 の Stuck (60 tick の間、残り距離が radius/4 以上縮まない) が「詰まり」と判定してしまう。Stuck になると moveInput は 0 のまま固定され、WARN も出る。目的地を「変える」まで解除されず、同じ値の NavSetDestination では復帰しない (EngineAPI.h:715 に明記)。そのため Arrived を待つスクリプトは先へ進めない。spec 1 の目的 (「敵 / NPC が目的地へ歩く」) のうち、集合地点へ集まる・止まっているプレイヤーを囲む、という基本のケースで破綻する。ADR-023 の既知の限界には「Stuck の定数は実ゲームの渋滞で検証していない」とあり、最小の構成で再現した — 根拠: 一時プローブ (HEAD を `%TEMP%\m82probe` に展開し、NavAgentSelfTest と同じ部品で組んだもの。リポジトリは無変更)。12x12 m の床で、出発点 (-6, z) から目的地 (6, 0, 0) へ向かわせ、900 tick 回した。ログは `C:\Users\akita\AppData\Local\Temp\m82rev\probe.log`。
     - 2 体・回避 2: t=300 で 1 体が status 6 (残り 0.337 m)、t=450 でもう 1 体も status 6 (残り 0.415 m)。900 tick まで両方とも moveInput (0, 0) で、どちらも Arrived にならない
     - 4 体・回避 2: Arrived は 1 体だけ。残り 3 体は status 6 (残り 0.65〜0.76 m) で停止
     - 同じ条件で回避 0: 全員 Arrived (重なって到着)
     判定箇所は NavSystem.cpp:1495-1515 で、spec どおりに実装されている (= 仕様の穴) — 期待: 目的地の近くで他の Agent に塞がれている状態の扱いを仕様で決める。たとえば、目的地から一定半径 (radius の和など) の中で前進が止まったら Arrived (部分的) にする、Stuck を状態の表示だけにして止めない、一定 tick 後に自動で引き直す、のどれか。あわせて「N 体が同じ目的地」の SelfTest を足す
  2. [major] 宛先: coder — NavMesh の塗りと輪郭が、DetourTileCache のポリゴン頂点の高さで描かれている。TileCache のタイルには詳細メッシュが無いので、床とつながった段差の天面のポリゴンは床の高さに近い位置に置かれる。結果として、0.3 m の段差の上では塗りも輪郭も段差の箱の中に埋もれて見えず、坂の塗りは坂面から浮いたり沈んだりする — 根拠:
     - 実測 (上と同じプローブ。NavAgentSelfTest の庭、既定の Surface 設定): 段差 (天面 y=0.30) の上の 15 点で、ポリゴン高さはすべて 0.050。塗りは +0.02 なので y=0.07 になり、段差の箱の中に入る。30 度の坂では、x=4.5 でポリゴン 0.706 / 坂面 0.289 (+0.42 m)、x=7.0 で 1.401 / 1.732 (−0.33 m)
     - 画像: `C:\HAL\MyEngin\tests\golden\nav.png`、`C:\HAL\MyEngin\plans\m82-navmesh\screenshots\sub-07_runtime_link.png`、`C:\Users\akita\AppData\Local\Temp\m82rev\A2_surface_tall.png` (編集中の SceneView)。どれも、Agent が上を歩いている段差の帯 (ベージュ) に塗りが無い。坂と Modifier の塗りは傾いた板として浮いている
     - 描画の経路: NavDebugDraw.cpp:219 の `duDebugDrawNavMesh` → :95-107 で `y + kFillLift` (頂点の高さをそのまま使う)
     spec 2. #18 と 4.3 は「Bake 直後に Play せず結果を確認できる」ことを表示の目的にしている。段差の上を歩けないように見えるのは、目的に反する誤情報になる — 期待: 塗りの高さを歩行面に合わせる。たとえば、ベイク済みの TileCache 層のセルの高さから表示用の高さを引く、表示専用の細分した三角形を層の高さで持ち上げる、など。それが重いなら、少なくとも遮蔽された塗りも見えるようにする (深度テストを外した 2 本目の薄い塗り)。golden `nav` で、段差の天面が塗られていることを確かめる
  3. [minor] 宛先: planner — 指摘 2 と同じ高さの誤差が、スクリプト API の結果にもそのまま出る。段差の上で NavSamplePosition (内部の findNearestPoly) が返す y は 0.050 で、実際の天面 0.30 より 0.25 m 下 (段差の箱の中) になる。坂では ±0.4 m ずれる。sub-08 の C# 実走でも、平らな床で「sample y=0.11」と記録されている (sub-08.md:52)。ADR-023 の「既知の限界」にも、EngineAPI.h の Nav* のコメントにも書かれていない。spec 3. で外したのは Height Mesh だけで、高さの精度は決められていない。後続の BT (M-C) の MoveTo や、スポーン位置の決定はこの点を使う — 根拠: probe.log の `[stepTop 0.30] ... nearestY 0.050`、`[ramp] ...` — 期待: 制約として受け入れるなら ADR と ABI のコメントに誤差の大きさを書く。直すなら指摘 2 と同じ高さ源で補正する、を仕様で決める
  4. [minor] 宛先: coder — RestoreSimSnapshot の「どこで失敗しても現世界に手が付いていない状態で戻れる」(SimSnapshot.cpp:554) という契約を、Nav 節が破っている。NavSystem::ApplySnapshot は World を差し替えた後に呼ばれていて (:651)、失敗すると World は差し替わったまま false を返す (:688)。呼び出し側はたとえば EngineLoop.cpp:1033 で、false を「復元できなかった」として SeekOutcome::Failed にするので、実際の状態と結果の報告が食い違う。起きるのは NavTileStore::LoadState か NavLoadCrowd が失敗したときだけで、まれ — 根拠: コード読み (実行では再現していない) — 期待: Nav 節が当てられるかを World の差し替え前に確かめる (節の entity / guid で .mnav を先に読む)。それが無理なら、関数のコメントと呼び出し側に「false でも World は差し替わっている」場合があることを明記する
  5. [minor] 宛先: coder — ベイクの結果は、Surface の Inspector が描かれているときにしか取り込まれない (TakeResult の呼び出し元は InspectorWindow.cpp:1819-1821 だけ)。ベイク中に別のエンティティを選ぶと、選び直すまで .mnav が書かれず、SceneView も古い表示のままになる。Destructible の焼きも同じ作りなので前例には沿っているが、大きなシーンで秒単位かかるベイクでは気づきにくい — 根拠: grep `navBakeService_\.` の結果 (InspectorWindow.cpp の 1819 / 1821 / 1893 / 1897 / 1902 / 1941 だけ) — 期待: 選択に関係なく、毎フレーム Ready のジョブを確定する (または、未確定の結果があることを表示する)
  6. [minor] 宛先: coder — 実装と食い違う古いコメントが残っている。Components.h:1880「コストの使い道は M82f」(実際は M82g)。Components.cpp:1389「areaCosts は M82f の UI ができるまで Inspector に出さない」(UI は M82g で専用のツリーとして実装済み)。Components.h:1964 と NavSystem.h:57「Box は回転後のワールド AABB で切り抜く」(y 回転だけの箱は NavSystem.cpp:802-827 で DT_OBSTACLE_ORIENTED_BOX として実形のまま切り抜く)。external\recastnavigation\PATCHES.md:5「下の 2 件は」(実際は 4 件) と :61「frand は静的ポインタ経由にする」(M82i では Detour の random 関数を使わない方式になった) — 根拠: 各 file:line — 期待: 現状に合わせて書き直す (AGENTS.md 5 章)
  7. [minor] 宛先: coder — ADR-023:393 の「replay_verify.bat 内の通常ビルド (Debug / Release) は警告 0」は事実と違う。今回の replay_verify のビルドでは、通常ビルドでも ProjectComputeRunnerSelfTest.cpp の C4127 が 1 構成 7 件、計 14 件出ている (M82 の範囲外の既存の警告) — 根拠: `C:\Users\akita\AppData\Local\Temp\m82rev_replay.log` の `warning C4127` 14 行 — 期待: 「M82 のファイルからの警告は 0 (既存の C4127 を除く)」と正確に書く
検証した手段:
  - tools\replay_verify.bat (MYE_REPLAY_JOBS=3、Debug / Release を再ビルドしたうえで) → 15 ジョブ全部 PASS (276.9 s。nav は Debug の snapshot stress、Release、Server.exe を含む)、rules 0 error / 0 warning。ビルドの警告は既知の C4127 (14 件) だけ
  - Editor.exe --selftest は Release → Debug の順に直列で流した (Start-Process -Wait、stdout と stderr を分けて保存)。FAIL は両構成とも既知の Source control 2 件だけ。NavDeterminism / NavSurface / NavEditor / NavAgent は全 PASS。NavDeterminism の固定ハッシュ (bake.layers、crowd.A/B、capture) と庭のハッシュ 1AB952061BC96FF8 は両構成で一致
  - Server.exe --selftest (Release / Debug) → exit 0、ALL PASS
  - tools\shot_verify.bat → nav は PASS (maxDiff 0)。FAIL は parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192 で、既知の値と同じ
  - Editor のスクショ (Release、`C:\Users\akita\AppData\Local\Temp\m82rev\`):
    - A_surface_edit.png / A2_surface_tall.png: 編集中の塗りと範囲箱、Surface の Inspector (セル 0.150 / 0.050 の自動表示、実効の傾斜上限 45.0、ベイクの要約、Bake / Clear)
    - B_obstacle_play.png: Play 中の SceneView の輪郭と塗り、carve=false の注意書き
    - C_link_play.png: Link の Inspector のエリア名
  - 一時プローブ (`C:\Users\akita\AppData\Local\Temp\m82probe\`。HEAD の git archive にプローブを足して Release でビルドしたもので、リポジトリは無変更): 同じ目的地への 2 体 / 4 体 × 回避 0 / 2、段差の天面と坂のポリゴン高さ → `C:\Users\akita\AppData\Local\Temp\m82rev\probe.log`
  - diff の確認: Loop / SimSnapshot / WorldHasher / Rendering / PhysicsSystem / Components / EngineApiTable / EngineAPI.h / ScriptAPI.h / Interop.cs (順序は C++ と一致) / InspectorWindow / NavSystem.cpp (Update〜Snapshot) / NavDebugDraw / NavFillPass / NavBakeService / DemoContent / NavDemoDriver / ツール類。コミットごとに触ったファイルの一覧も見たが、仕様との差分に書かれていない変更は見つからなかった。NavTileCacheSupport.cpp・NavBakeInput.cpp・ベンダーのソースは要所 (PATCHES.md) だけ読んだ
  - 未確認 (手操作が要るもの): Bake ボタンを押した直後の SceneView の更新 (コード上は NavDebugView::Refresh が毎フレーム navAsset の変化を拾う)。エリアコストのドラッグ 1 回 = 1 Undo (コード読みだけ。IsItemActivated で BeginRecord、IsItemDeactivated で EndRecord)。Project Settings のエリア名の画面。Link の警告 2 種と Stuck の Inspector の見た目。段差を登る tick の見た目の跳び (0.13〜0.16 m、既知の限界として受け入れ済み)
前回指摘の消込: (round 1 のため無し)
```
