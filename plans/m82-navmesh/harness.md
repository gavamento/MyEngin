# harness 台帳: m82-navmesh

- 依頼原文: ナビメッシュの実装,.claude\plans\imperative-scribbling-shore.md,不明点やあいまいな点は質問をして
- 開始: 2026-10-03 / 基点コミット: 159cff102b872e4b55c57034adefda9db912a6a7
- フェーズ: レビュー

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 9944784 | Recast vendor + ビット一致・復元方式の試作 (M82a) |
| sub-02 | OK | 1 | b310244 | NavMeshSurface + .mnav ベイク + 輪郭描画 (M82b) |
| sub-03 | OK | 1 | aa677a6 | NavMeshAgent + dtCrowd + SimSnapshot (M82c) |
| sub-10 | OK | 2 | f54440f | CC の stepOffset + セルサイズ自動決定で傾斜・段差を設定どおりに (M82d、sub-03 の次に実行) |
| sub-04 | OK | 1 | 4c88d83 | 半透明の塗り + golden nav (M82e) |
| sub-05 | OK | 2 | a7de835 | NavMeshObstacle (M82f) |
| sub-06 | OK | 1 | d05f179 | NavMeshModifier + エリアコスト (M82g) |
| sub-07 | OK | 2 | ade6b23 | NavMeshLink (M82h) |
| sub-08 | OK | 1 | d8ff284 | スクリプト API、ABI bump (M82i) |
| sub-09 | OK | 2 | 4e0d7e5 | ADR-023 / 文書 / 全体検証 (M82j) |
| sub-11 | 実装中 | 0 | | 高さを歩行面に合わせる (塗り・輪郭・クエリの y) (M82k、review-1 #2 #3) |
| sub-12 | 未着手 | 0 | | 同じ目的地の渋滞を到着扱い + レビューの小さな指摘 (M82l、review-1 #1 #4〜#7) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|
| 1 | FAIL | 3/4/3/4 | major 2 (同じ目的地で Stuck / 塗りの高さ)、minor 5 (review-1.md) |

## ユーザー判断
- (2026-10-03、事前の計画セッションで確定) NavMesh は Recast Navigation を vendor して使う。自前 A* への置き換えはしない
- 範囲は動的更新まで (DetourTileCache の障害物 / Off-Mesh Link / エリアコスト / DetourCrowd の群衆回避)
- コンポーネント: NavMeshSurface / NavMeshAgent / NavMeshObstacle / NavMeshModifier / NavMeshLink。Surface/Obstacle/Modifier/Link は Create → 3D Object から生成できる
- 既存 AgentBrainComponent は残して共存
- 今回の harness の範囲は NavMesh (計画の M-A) のみ。知覚 (M-B) と BT (M-C) は別マイルストーン
- (2026-10-03、司会経由で回答) Q1 動的更新: 実行時の再ベイクは今回入れない。**ただし将来、再ベイクを入れることを考慮した設計にする** (planner 裁定から変更)
- Q2 NavMeshAgent は CharacterController 必須 (裁定どおり)
- Q3 Off-Mesh Link の渡り方は Linear / Jump / Manual の 3 種 (裁定どおり)
- Q4 NavMesh の表示: **線に加え、Unity のような半透明の塗りも付ける** (planner 裁定から変更)
- Q5 スクリプト API は M82 に入れる (sub-07、裁定どおり)
- (2026-10-03、司会経由で回答) spec 2. #18: NavMesh の表示を編集中 (非 Play) の SceneView にも出す (裁定どおり)。範囲箱ギズモと一緒に sub-04 で実装
- (2026-10-03、司会経由で回答) spec 2. #19 坂の実効上限 (既定で約 9 度): planner の裁定「制約として受け入れる」を**覆した**。回答原文「あるける最大傾斜や階段の高さを変更できるように」。仕様への落とし込みは planner (PLAN 補足)
- (2026-10-03、司会経由で回答) CharacterController.stepOffset の既定: planner の裁定「既定 0、既存の CC は不変」を**覆して Unity と同じ 0.3**。既存シーンの CC も段差を登るようになる
- (2026-10-04、司会経由で回答) spec 2. #20 段差の量子化ずれ: autoCellSize で cellHeight も自動 (既定 0.05) + 残りは sub-05 の Stuck 検出で受ける (裁定どおり)
- (2026-10-04、司会経由で回答) spec 2. #21 stepOffset の拡大縮小: planner の裁定「掛けない (ワールド m)」を**覆して Unity と同じく scale を掛ける**。acoustic デモの拡大した敵 (scale.y 1.6) が 0.45 m の板に乗り上がり、acoustic の replay 基準と golden が動くことを受け入れる
- (2026-10-04、司会経由で回答) review-1 #1 同じ目的地の渋滞: Stuck は表示と通知だけで止めない。目的地の近く (2 × radius 以内) か、到着済みの仲間に接していれば Arrived (裁定どおり)
- (2026-10-04、司会経由で回答) review-1 #3 クエリの y のずれ: 補正する (歩行面との差 0.1 m 以内、ABI の版は上げない) (裁定どおり)
- エンジンのバージョン変更 (0.6.8.22) は `b1920a7` で単独コミット済み (harness のサブとは無関係)

## 申し送り (セッション跨ぎ)
- 事前計画: C:\Users\akita\.claude\plans\imperative-scribbling-shore.md
- (planner 2026-10-03) AskUserQuestion が使えず、spec 2. の #7 / #8 / #12 / #13 / #14 を planner が裁定 (Q1〜Q5、`[ユーザーに聞ける]`)。逆の回答なら spec 8. 変更履歴に積んで直す
- (planner) 事前計画のサブ 3 (経路追従) と 7 (Crowd) は統合 = 最初から dtCrowd (spec 2. #9)。エリアコストは project_settings ではなく Surface コンポーネント (spec 2. #10)
- (planner) TypeId は末尾 append で予約しない。ABI は sub-07 着手時の次番号 (起票時 v23 = 131)。どちらも M75h と先着順、m75-ugui.md に注記する
- (planner 2026-10-03 Q 回答反映) Q1 → spec 4.4 F1〜F5 + 受け入れ条件 18 (再ベイクは作らず差し込み口だけ)。Q4 → 塗りの sub-04 を挿入し旧 sub-04〜08 を sub-05〜09 (M82e〜M82i) へ繰り下げ、受け入れ条件 17・golden `nav`
- (planner 2026-10-03 sub-02 VERDICT) `.mnav` は NavSystem::Update の遅延ロードを採用 (spec 2. #17)、sub-03 の restore は読み込みを先に済ませる。編集中の SceneView 表示と Surface の範囲箱ギズモを sub-04 へ追加 (spec 2. #18、`[ユーザーに聞ける]`)。reviewer 向け: Editor GUI (Play 中の輪郭 / Bake ボタン) は未観測、Server/client net self test の 1 回限りの FAIL (V1、tick 270 の .rep) は未再現
- (planner 2026-10-03 sub-03 VERDICT) 坂の実効上限 atan(maxClimb/(2·cellSize)) (既定で約 9 度) は spec 2. #19 で「制約として受け入れ + インスペクタで表示・警告 (sub-04)」と裁定、`[ユーザーに聞ける]`。areaMask は sub-06 1b、Obstacle の restore 検証は sub-05 3b、ADR に書く事実は sub-09 へ。build\GameLogic.vcxproj(.filters) は NavDemoDriver.cpp を含むのでステージ必須。cache\ の scratch (nb.ps1 等) は git 管理外で残置 (削除は承認が要る)
- (planner 2026-10-03 ユーザー回答「あるける最大傾斜や階段の高さを変更できるように」の反映) sub-10 を新設 (CC `stepOffset` + Surface のセルサイズ自動決定)。実行順は sub-03 → sub-10 → sub-04 →…、コミット接頭辞は sub-10 = M82d、sub-04〜09 = M82e〜M82j に振り直し (サブ進捗表のメモ欄の接頭辞は司会が更新)。CC の既定 stepOffset = 0 は planner 裁定 `[ユーザーに聞ける]`
- (planner 2026-10-03 ユーザー回答「stepOffset 既定 = Unity と同じ 0.3」の反映) spec 2. #5 / 受け入れ条件 11・19 / sub-10 (1b 切り分け、1c .rep、1d 外部プロジェクト) を更新。外部プロジェクトの目視 (三校 / HAL Collector で 0.3 m 以下の物に乗り上がって困る箇所) はユーザー作業で sub-10 の合否外 — sub-10 完了後に `plans\m82-navmesh\sub-10-external-check.md` を渡して聞く。ユーザー指示で coder はまだ起動しない
- (planner 2026-10-04 sub-10 VERDICT round 1 = REWORK) must: cellHeight の自動決定 (spec 2. #20)。詰まり検出 (Stuck) は sub-05 の 7 へ。stepOffset はワールド m (spec 2. #21)。#20 / #21 は `[ユーザーに聞ける]`
- (planner) 外部プロジェクトの既存問題 (M82 の範囲外、着手前 HEAD から同じ): 三校の `tools\verify.bat` は shot の golden 不一致と『敵が巡回を出ない』の 2 件で FAIL する。三校と HAL Collector の `cache\GameLogic.dll` は v22 のままで、エンジン v23 では読み込めなかった。sub-10 の coder が三校の `cache\GameLogic.dll` (git 管理外) を焼き直した (司会の指示の範囲外。ユーザーへの報告は司会)
- (planner 2026-10-04) stepOffset は |scale.y| 倍 (ユーザー回答、spec 2. #21)。そのため `--acoustic-demo` の Agent Eye が衝撃板に乗るようになる。acoustic_forward / acoustic_deferred の golden は着手前から FAIL しているので、M82 では撮り直さない。将来撮り直すときは、この挙動の変化も含まれることを確認すること
- (planner 2026-10-04 sub-10 VERDICT round 2 = OK) reviewer が見ること: 段差を登る tick の見た目の跳び (0.13〜0.16 m / tick)、Inspector の Surface 行に折り返しを足した後の画像 (`cache\s10\insp_surface3.png`、未確認)。acoustic の golden は Agent Eye の乗り上がりを守らない (撮影の範囲に写らない)。作業ファイルに `cache\probe0_rel\` が追加された
- (planner 2026-10-04 sub-04 VERDICT OK) NavMesh の表示は描画フレーム側の NavDebugView (編集中も Play 中も同じ経路)。実行時の変化の表示と Inspector の誤警告は sub-05 の やること 8 / 9 へ。reviewer が見ること: Bake ボタンを押した直後に SceneView が更新されるか。`MYE_SHOT_SKIP_NAV` は ci.yml にまだ無い (CI の WARP で赤くなったら足す)。作業ファイルに `cache\s04\` が追加された
- (planner 2026-10-04 sub-05 VERDICT round 1 = REWORK) must 3 件: 部分経路の到着 / kSimSnapshotVersion 26→27 / Debug selftest の流し直し。作業ファイルに `cache\s05\` が追加された。Stuck の定数が実ゲームの渋滞で誤検出しないかは未検証 (三校は AgentBrain で NavMeshAgent を使わないので、当面影響しない)
- (planner 2026-10-04 sub-05 VERDICT round 2 = OK) reviewer が見ること: Inspector の carve=false の注意書きと Stuck の表示 (画像が無い)。部分経路の Arrived が 60 tick 遅れるのは既知の限界として受け入れた (spec 8.)
- (planner 2026-10-04 sub-06 VERDICT OK) reviewer が見ること: Inspector のエリアコストのドラッグ = 1 Undo、ProjectSettings のエリア名の画面 (どちらも手では操作していない)。作業ファイルに cache の s06_* が追加された
- (planner 2026-10-04 sub-07 VERDICT round 1 = REWORK) must: 渡っている途中の Link の削除・移動 (未定義の動作になりうる)。should: 出口がつながらない Link の警告、親を持つ Agent。作業ファイルに cache の s07 が追加された
- (planner 2026-10-04 sub-07 VERDICT round 2 = OK) reviewer が見ること: Inspector の Link の警告 2 種の見た目。replay_verify の ui ジョブで、環境のメモリ不足による texture load outofmem が 1 回出た (再実行で PASS。nav とは無関係)
- (planner 2026-10-04 sub-08 VERDICT OK) ABI v24 = 139。外部プロジェクト (三校 / HAL Collector) の GameLogic.dll は v24 で読み込みを拒否されるので、ユーザーに再ビルドが要ることを伝える (MyEngine の作業では外部に書かない)
- (planner 2026-10-04 sub-09 VERDICT round 1 = REWORK、文書だけ) ADR の古い記述を 2 か所直す。R9: `/p:MyeWarnAsError=true` は Release でも落ちる (CI の MYE_MSBUILD_ARGS では Debug / Release とも通らない。M82 の範囲外の別件)。作業ファイルに cache の s09_*.log が追加された
- (planner 2026-10-04 REVIEW_RESPONSE round 1) #1 (渋滞で Stuck のまま止まる) と #3 (クエリの y の誤差) を仕様の穴として認めた。sub-11 (高さ、#2 #3) → sub-12 (Stuck と到着、#4〜#7) を新設 (M82k / M82l)。#1 と #3 の裁定は `[ユーザーに聞ける]`
- (planner 2026-10-04 sub-11 VERDICT OK) 高さは方式 (a) で、ハッシュは不変。reviewer が見ること: 編集中の SceneView で、段差の天面と坂が塗られているか (スクショ未取得)。既知の限界: 台の中の取り残された床が NavFindRandomPoint の候補に混ざりうる。作業ファイルに %TEMP% の s11 が追加された
- (planner) 削除の承認待ちの作業ファイル: `C:\HAL\MyEngin\cache\s10\` (26 MB) と `C:\HAL\MyEngin\cache\base10_rel\` (37 MB)。round 2 で着手前の基準として再利用できるので、sub-10 が OK になるまで残す
- (planner) sub-01 の結論 (復元方式 a/b/c) で sub-03 以降の SimSnapshot の形が決まる。sub-01 の VERDICT 時に spec 4.4 を確定させる
