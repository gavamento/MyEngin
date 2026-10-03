# harness 台帳: m82-navmesh

- 依頼原文: ナビメッシュの実装,.claude\plans\imperative-scribbling-shore.md,不明点やあいまいな点は質問をして
- 開始: 2026-10-03 / 基点コミット: 159cff102b872e4b55c57034adefda9db912a6a7
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 9944784 | Recast vendor + ビット一致・復元方式の試作 (M82a) |
| sub-02 | OK | 1 | b310244 | NavMeshSurface + .mnav ベイク + 輪郭描画 (M82b) |
| sub-03 | 実装中 | 0 | | NavMeshAgent + dtCrowd + SimSnapshot (M82c) |
| sub-04 | 未着手 | 0 | | 半透明の塗り + golden nav (M82d) |
| sub-05 | 未着手 | 0 | | NavMeshObstacle (M82e) |
| sub-06 | 未着手 | 0 | | NavMeshModifier + エリアコスト (M82f) |
| sub-07 | 未着手 | 0 | | NavMeshLink (M82g) |
| sub-08 | 未着手 | 0 | | スクリプト API、ABI bump (M82h) |
| sub-09 | 未着手 | 0 | | ADR-023 / 文書 / 全体検証 (M82i) |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

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
- エンジンのバージョン変更 (0.6.8.22) は `b1920a7` で単独コミット済み (harness のサブとは無関係)

## 申し送り (セッション跨ぎ)
- 事前計画: C:\Users\akita\.claude\plans\imperative-scribbling-shore.md
- (planner 2026-10-03) AskUserQuestion が使えず、spec 2. の #7 / #8 / #12 / #13 / #14 を planner が裁定 (Q1〜Q5、`[ユーザーに聞ける]`)。逆の回答なら spec 8. 変更履歴に積んで直す
- (planner) 事前計画のサブ 3 (経路追従) と 7 (Crowd) は統合 = 最初から dtCrowd (spec 2. #9)。エリアコストは project_settings ではなく Surface コンポーネント (spec 2. #10)
- (planner) TypeId は末尾 append で予約しない。ABI は sub-07 着手時の次番号 (起票時 v23 = 131)。どちらも M75h と先着順、m75-ugui.md に注記する
- (planner 2026-10-03 Q 回答反映) Q1 → spec 4.4 F1〜F5 + 受け入れ条件 18 (再ベイクは作らず差し込み口だけ)。Q4 → 塗りの sub-04 を挿入し旧 sub-04〜08 を sub-05〜09 (M82e〜M82i) へ繰り下げ、受け入れ条件 17・golden `nav`
- (planner 2026-10-03 sub-02 VERDICT) `.mnav` は NavSystem::Update の遅延ロードを採用 (spec 2. #17)、sub-03 の restore は読み込みを先に済ませる。編集中の SceneView 表示と Surface の範囲箱ギズモを sub-04 へ追加 (spec 2. #18、`[ユーザーに聞ける]`)。reviewer 向け: Editor GUI (Play 中の輪郭 / Bake ボタン) は未観測、Server/client net self test の 1 回限りの FAIL (V1、tick 270 の .rep) は未再現
- (planner 2026-10-03 sub-03 VERDICT) 坂の実効上限 atan(maxClimb/(2·cellSize)) (既定で約 9 度) は spec 2. #19 で「制約として受け入れ + インスペクタで表示・警告 (sub-04)」と裁定、`[ユーザーに聞ける]`。areaMask は sub-06 1b、Obstacle の restore 検証は sub-05 3b、ADR に書く事実は sub-09 へ。build\GameLogic.vcxproj(.filters) は NavDemoDriver.cpp を含むのでステージ必須。cache\ の scratch (nb.ps1 等) は git 管理外で残置 (削除は承認が要る)
- (planner) sub-01 の結論 (復元方式 a/b/c) で sub-03 以降の SimSnapshot の形が決まる。sub-01 の VERDICT 時に spec 4.4 を確定させる
