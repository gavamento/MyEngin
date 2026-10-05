# harness 台帳: m85-behavior-tree

- 依頼原文: M85の実装
- 開始: 2026-10-05 / 基点コミット: baf29bf0632d5daccd2b0f8496513c704b7d1237
- フェーズ: 実装

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | a5f7ab1 | BT の核 (.bt/.bb・コンポーネント・実行器・Composite・Wait・BT 節)。snapshot v34、TypeId 78 |
| sub-02 | OK | 1 | 24cc318 | Decorator 5 種と Abort |
| sub-03 | OK | 1 | 6a8bd5b | Task 4 種 (MoveTo に failOnStuck、既定 false) |
| sub-04 | OK | 2 | (このコミット) | round 1 REWORK: SearchArea に failOnStuck。snapshot v36 | AI ノード 4 種 |
| sub-05 | 未着手 | 0 | | 汎用イベントキューと SendEvent |
| sub-06 | 未着手 | 0 | | AnimatorPlay / PlayAnimation / SubTree |
| sub-07 | 未着手 | 0 | | 巡回ルート |
| sub-08 | 未着手 | 0 | | BT 窓 (1) |
| sub-09 | 未着手 | 0 | | BT 窓 (2) Undo・BB 編集・検査 |
| sub-10 | 未着手 | 0 | | BT 窓 (3) ライブ表示 |
| sub-11 | 未着手 | 0 | | ABI v27 |
| sub-12 | 未着手 | 0 | | C# タスク |
| sub-13 | 未着手 | 0 | | --bt-demo・replay_verify・golden |
| sub-14 | 未着手 | 0 | | ADR-025 と全体検証 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- (2026-10-05) BT の実行状態 = BehaviorTreeSystem の表 + SimSnapshot の BT 節 + ハッシュ (planner 裁定どおり)
- (2026-10-05) 根の終了後のやり直し = 次の tick から (裁定どおり)
- (2026-10-05) MoveTo の Stuck = **MoveTo にチェックボックスを持たせ、詰まったら Failure にするかをノードごとに選ばせる** (裁定「Running のまま」から変更。既定値は planner が決める)
- (2026-10-05) BT 無しの巡回 = 作らない (裁定どおり)
- (2026-10-05) ノードの表示位置 = .bt.json に入れる (裁定どおり)
- (2026-10-05) 上記を反映して spec 確定、sub-01 から着手してよい
- (2026-10-05) SearchArea / Patrol の詰まり = MoveTo と同じ名前・同じ意味の failOnStuck (既定 false、true でノード全体 Failure)。spec 2. #18 の planner 裁定どおり

## 申し送り (セッション跨ぎ)
- (sub-01 VERDICT should#1) sub-08 で一時プローブの `--screenshot` を撮る回に、sub-01 の未確認 4 点 (Create メニュー 2 項目 / 型フィルタ・タイルの語 / Inspector の tree ピッカーと注意表示 / Runtime.exe 実起動) も目視して SELF_EVAL に書く。
- (sub-01 VERDICT nit#2) Debug selftest の 1 回目に既知 flake 5 件 (Fracture weight cache 3 / net V1 LoadPersist・LoadGame 2) が再び出たら回数を記録する (原因調査は範囲外)。
- (sub-01 coder → sub-02) Decorator は BtNodeDef に足し、BtLinkAsset が stateSlotCount を数える。評価は各 Visit* の入口と、毎 tick の StepOwner の Visit(root) の前。AbortNode は子孫が先、ノード固有の後始末は Finish(state) の前。ActiveLeafId は active な子を左から探す。
- (sub-01 coder) tools\gen_project_files.ps1 は pwsh で実行する (PS 5.1 は構文エラー)。build\Engine.vcxproj(.filters) はコミット対象。
- (sub-01 coder) 表のキーは (index, generation)。World::Clear が世代を進めるので Play 開始/終了で前の表は自然に落ちる。
- (sub-02 VERDICT should#1) sub-03 で MoveTo の Abort 停止を試すとき、LowerPriority の変化検出 (偽→真) と Abort 後始末の順序 (深い方から) を一時的に壊すと該当テストが FAIL することを 1 回確かめ、SELF_EVAL に書く。
- (sub-02 VERDICT nit#2) MonitorNode の LowerPriority 部が長い。sub-06 で SubTree が監視に加わる前に分けるか coder が判断してよい。
- (sub-02 coder) 欄の並びは [ノード 0..N-1][Decorator (ノード順)]。sub-10 のライブ表示は BtDecoratorDef::slot を引く。本体の Abort 後始末 (MoveTo 停止など) は AbortNode / AbortBody の「ノード固有の後始末」の位置へ。SetAbortTrace が順序検査に使える。監視は BlackboardCondition の abort != None だけで、Task が書いた BB 値は次の tick の監視で反映。
- (sub-03 VERDICT should#1) sub-04 以降は Debug の Server.exe --selftest も回す。
- (sub-03 VERDICT nit#2) YawOf / WrapPi を NavSystem から BT へ複製済み。sub-07 で 3 か所目が要るなら共有の数学ヘッダへ移すか coder が判断。
- (sub-03 VERDICT nit#3) 親付き Agent への RotateTo は未検証 → ADR-025 (sub-14) に既知の限界として書く。
- (sub-03 coder) 追加状態は BtNodeTypeInfo::extraStateBytes + BtNodeDef::extraOffset + BtInstance::extra。構造体はパディングなし 4/8 バイト揃え、足したら snapshot +1 (sub-04 で v36 見込み)。後始末は ReleaseBody の switch に足す。MoveTo 相当を内部で使うノード (SearchArea / Patrol) も「目的地を書いた tick は status を読まない」。ノードのキー名は BtNodeTypeInfo::keyNames、JSON は "keys"。BT 系ファイルは LF。
- 既知 flake の回数: sub-01 1 回目 5 件 / sub-02 0 件 / sub-03 1 回目 4 件 (2 回目 0) / sub-04 0 件。
- (sub-04 VERDICT nit#1) BehaviorTreeSelfTest の診断ログ `[search] stuck ...` は、1 回の実行で 1〜2 行なら残す。tick ごとなら 1 行にまとめるか削除。sub-05 でついでに判断。
- (sub-04 coder) BehaviorTreeSystem::Update(world, tick, nav) — 第 3 引数 NavSystem*。BtParamType::Mask (64 ビット、16 進) と BtNodeCategory::Ai は sub-08 のパラメータ欄・パレットで扱いが要る。SearchArea のパラメータは 4 項目 (usePrediction / radius / pointCount / failOnStuck)。名乗らない音を選んだら Entity キーは空にする (承認済み)。
- (sub-02 VERDICT) snapshot は v34 のまま承認。sub-03 (種類ごとの追加状態) で kSimSnapshotVersion 35 にする。UE 対応の未検証点 (Loop の Failure 抜け、Cooldown の Abort 後計時) は ADR-025 (sub-14) に列挙。ADR-025 は下書き (sub-14 で確定)。
- ロードマップ (`plans\ai-roadmap-m83-m86.md`) には「harness は使わず直接実装」とあるが、2026-10-05 にユーザーが `/harness M85の実装` を明示したのでハーネスで回す。
- (planner 2026-10-05) spec.md 確定 (planner 裁定)・sub-01〜14 を作成。番号の見込み: TypeId 78 BehaviorTree (sub-01) / 79 PatrolRoute (sub-07)、snapshot v34 (sub-01) から各サブで +1、ABI は sub-11 で 1 回だけ v27。調べた事実の要点は spec 2. の表に file:line 付き。
