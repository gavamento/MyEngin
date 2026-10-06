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
| sub-04 | OK | 2 | 3f8ad39 | round 1 REWORK: SearchArea に failOnStuck。snapshot v36 | AI ノード 4 種 |
| sub-05 | OK | 2 | 16fa216 | round 1 REWORK: 配達を tick 頭へ・replay_verify。snapshot v37 | 汎用イベントキューと SendEvent |
| sub-06 | OK | 1 | 7367737 | SubTree は平らな展開、snapshot v37 のまま | AnimatorPlay / PlayAnimation / SubTree |
| sub-07 | OK | 1 | 74b7557 | TypeId 79 PatrolRoute、snapshot v38 | 巡回ルート |
| sub-08 | OK | 1 | 2f86efb | BT 窓 (1) + Entity キーの初期値、snapshot v39 | BT 窓 (1) |
| sub-09 | OK | 1 | 8bef16c | Undo・BB パネル・検査・ReloadHub の同一内容スキップ | BT 窓 (2) Undo・BB 編集・検査 |
| sub-10 | OK | 1 | 20a9f43 | ライブ表示・Abort 矢印・デバッグ線 | BT 窓 (3) ライブ表示 |
| sub-11 | OK | 1 | e16bd96 | ABI v27 = 158、C++ タスク、snapshot v39 のまま | ABI v27 |
| sub-12 | OK | 1 | eec835e | C# タスクと糖衣 | C# タスク |
| sub-12b | OK | 1 | 3d20b33 | C# タスクの fields とクラスのピッカー (ユーザー判断で新設) |
| sub-13 | OK | 1 | bcdf04f | --bt-demo、replay_verify bt、golden bt | --bt-demo・replay_verify・golden |
| sub-14 | OK | 1 | (このコミット) | ADR-025 確定・文書・全体検証 | ADR-025 と全体検証 |

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
- (2026-10-06) Patrol の開始点 = 入るたびに一番近い点から (初回も同じ)。spec 2. #19 の裁定どおり
- (2026-10-06) BB の Entity キーの埋め方 = BehaviorTreeComponent に「Entity キーの初期値」4 組、Inspector で選ぶ (sub-08、snapshot +1)。spec 2. #20 の裁定どおり
- (2026-10-06) C# タスクのノードごとのフィールド欄と BT 窓の C# クラスのピッカー = **M85 で作る** (spec 2. #21 の planner 裁定「後回し」から変更。サブが 1 本増える)
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
- 既知 flake の回数: sub-01 1 回目 5 件 / sub-02 0 件 / sub-03 1 回目 4 件 (2 回目 0) / sub-04 0 件 / sub-05 round 1 の途中 2 件 (net V1)、最終 0 件。
- (sub-05 VERDICT nit#1) sub-06 で BehaviorTreeSelfTest.cpp を触るとき、配達 (DeliverPending) を Update の中へ戻すと新テスト (a) が FAIL することを 1 回確かめ、SELF_EVAL に書く (確かめた後は戻す)。
- (sub-05 coder) イベントの配達は TickRunner のフェーズ 3 直前 (stepSim の中) で DeliverPending、BT の Update は BB への反映だけ。sub-11 の ABI は BehaviorTreeSystem::SendEvent(tick, ...) へ engine の tickIndex を渡す。sub-08 は BtParamType::String (イベント名) を扱う、SendEvent のキー欄は target / vector。
- (sub-05) Debug 中にユーザーが見たアサートのダイアログは別件 `plans\selftest-crt-dialog.md` (M85 の範囲外)。司会が 1c8a69e で対応済み: Editor.exe は先頭で SuppressCrtDialogs() を呼ぶので、Debug の selftest でアサートに当たるとダイアログなしで stderr に出て 0 以外で終わる。
- (sub-05 planner) sub-06: SubTree の入れ子状態を BT 節に足すなら v38。MonitorNode の分割は coder 判断。→ sub-06 は平らな展開で v37 のまま、MonitorNode は分けず。
- 既知 flake: sub-06 Debug 1 回目 5 件 (Fracture 3 + net V1 2)、2 回目 0。
- (sub-06 VERDICT should#1) 部分木アセットを ReloadHub で読み直したとき、取り込んでいる親の木で動くエンティティが「Abort → 根からやり直し」(spec 4.1.9) になることを、sub-07 の SELF_EVAL に 1 行で書く。既存テスト「取り込み元の再登録」が見ていなければ sub-07 で 1 項目足す。
- (sub-06 coder) 展開後の実行木は BehaviorTreeSystem::FindInstance(e)->tree。部分木のノード id は連番で振り直し → sub-10 は (GUID, 元の id) の導出値で表示 (sub-10.md 反映済み)、sub-11 は activeNodeId が展開後の id であることを明記するか対応表を通すか決める。Update(world, tick, nav, controllers, clips)。AnimatorPlay(world, e, stateIndex, duration, controllers) -> bool。
- (sub-07 VERDICT should#1) DrawGizmo / HandlePatrolRoute / OnImGui の「記録を閉じる」処理が 3 か所。sub-10 で 4 か所目を足すなら集約する。
- (sub-07) sub-08 の範囲に spec 2. #20 (Entity キーの初期値 4 組、snapshot v39 見込み) と GUI 実操作の目視 (sub-01 / sub-07 の持ち越し、sub-08.md に列挙) が加わった。spec 2. #19 / #20 の [ユーザーに聞ける] 印は、ユーザー回答 (裁定どおり) を受けて planner が確定に直す。
- (sub-08 VERDICT should#1) 保存直後に ReloadHub が同じ内容を読み直して木が 2 回やり直す → sub-09 で ReloadBehaviorTree / ReloadBlackboard を「内容が同じなら置き換えない」に直す (ReloadHubSelfTest を検証に入れる)。
- (sub-08 VERDICT should#2) sub-09 の最初に BehaviorTreeWindow.cpp の DrawCanvas を入力・描画・メニューに分ける。
- (sub-08 coder) Undo は「ドラッグは押した時点で撮り離して確定、パラメータは編集確定でまとめる」。Delete / Ctrl+Z / Ctrl+Y は BT 窓フォーカス中だけ窓が握る (SetKeyOwner)。dirty は Undo で保存時の内容へ戻ったら消す。キー参照の型違いも検査で拾う。画面確認の道具は scratchpad の ui.ps1 (実マウス・キー入力・キャプチャ)。
- 既知 flake: sub-07 / sub-08 とも Debug 1 回目 5 件、2 回目以降 0。
- (sub-09 VERDICT nit#1) BB パネルの文字欄は textSlot_ 共有。sub-10 で現在値表示を足すとき、編集中の欄と表示が混ざらないことを確かめる。
- (sub-09 coder → sub-10) 座標変換は BehaviorTreeWindow の CanvasView (view_) と HitTest。ライブ表示は DrawNodes に重ねるか後に呼ぶ。CanvasStyle に色を足せる。BB の現在値は DrawBoardKey に足せる。ジェスチャは gestureOwner_ に合わせる。ADR-025 に積む既知の限界の一覧は sub-14.md にある。
- 既知 flake: sub-09 Debug 1 回目 Fracture 3 件 (net V1 0)、2 回目以降 0。
- (sub-10 VERDICT nit#1) SceneView の実行中タスク名の高さ 2.2 m・文字色が名前付き定数でなければ gizmo 名前空間の定数へ。次に SceneViewWindow.cpp を触るサブで。
- (sub-10) sub-07 should#1 (記録を閉じる処理の集約) は条件に当たらず閉じた。
- (sub-10 → sub-11) activeNodeId は展開後の実行木の id。ABI と文書に明記し、DisplayedIdOf は必要になるまで ABI に出さない (sub-11.md 反映済み)。ABI の bump は M85 でこの 1 回だけ (v27)。
- (sub-10 → sub-13) --bt-demo に drawDebug = true の BehaviorTreeComponent を 1 体入れておくと撮りやすい。
- (sub-11 VERDICT should#1) sub-12 で BtTaskCanonicalizeState を一時的に外し、パディングを持つ状態型のテストが FAIL することを確かめる (無ければテストを足す)。
- (sub-11 VERDICT should#2) sub-12 の一時プローブで ABI v27 の 7 スロットを C# から 1 回ずつ呼ぶ。
- (sub-11 VERDICT nit#3) sub-12 で C# タスクの欄を撮るとき、BT 窓の CppTask 欄 (タスク名の選択・フィールド欄) も撮る。
- (sub-11) CsTask は params.class (String) で CppTask の params.task と揃える。Interop.cs には 7 スロットと MyeBbValue / MyeBtEventPayload / MyeBtEvent のミラーだけ (糖衣は sub-12)。ManagedHost::SetBehaviorTree あり。
- (sub-11 → 完了報告) 外部プロジェクト (HAL Collector / 三校) の GameLogic.dll は ABI v27 で再ビルドが要る。ユーザーに伝える。
- (sub-11 → sub-13) GameLogic の BtProbeTask (fields: targetTicks / outcome / ticks) をデモから使える。
- (sub-12) CsTask を含む木を replay_verify の対象シーンに入れない (C# レーンは被覆外)。MyeManagedVTable の BtTask は Engine と MyeScripting.dll の内部契約 (EngineAPI の版は不変)。
- (sub-13 VERDICT should#1) sub-14 で docs\test_checklists.md に --bt-demo の手動確認項目 (5 段階の観察と BT 窓のライブ表示) を足す。
- (sub-14 VERDICT nit#1) test_checklists.md の M84 節の v26 の行を v27 に書き換えている。各節は「その時点の版」を残すほうが揃う (M84 節を v26 に戻し、M85 節に「外部 GameLogic.dll は v27 = 158 で再ビルド」を 1 行)。受け入れには影響しない minor。
- (sub-14 VERDICT nit#2) golden bt は sub-14 では間接判定 (sub-13 で maxDiff 0 を 2 回直接確認済み)。
- (sub-14) Update の性能は Release 37.5 µs (sub-01 は 17 µs、基準 0.5 ms)。
- **別件 (M85 の範囲外、ユーザーへ報告)**:
  - (a) replay_verify の並列 cold cook で起動中に未処理 C++ 例外 (0xE06D7363、tick 0) / 9 分無反応が 1 回ずつ出た (sub-13 の 2 回目、nav の Release 検証と flow の Debug 検証)。crash bundle `bin\x64\Release\crash\20261006_111539` (minidump あり)。RVA → file:line は未特定。
  - (b) golden の既存 FAIL 6 枚 (HEAD 3d20b33 の clean でも同数値): nav 214/21664、parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192。更新するかはユーザー判断。
  - (c) ユーザー要望 (2026-10-06): replay_verify を音なし・最背面で回したい。音は MYE_EXTRA_ARGS=--no-audio で対応可。最背面は CLI オプション (--background: SW_SHOWNOACTIVATE + HWND_BOTTOM) の追加を提案中 (返事待ち)。
- (sub-07 → sub-14) ADR-025: Patrol は入るたびに最近傍点から、親付きルートは前 tick の WorldMatrix。
- (sub-06 → sub-14) ADR-025 の既知の限界: SubTree は平らな展開・上限 1024・部分木の根の LowerPriority は Self 扱い・BB 継承なし・遷移中の AnimatorPlay はブレンド途中のポーズから飛ぶ。
- (sub-04 VERDICT nit#1) BehaviorTreeSelfTest の診断ログ `[search] stuck ...` は、1 回の実行で 1〜2 行なら残す。tick ごとなら 1 行にまとめるか削除。sub-05 でついでに判断。
- (sub-04 coder) BehaviorTreeSystem::Update(world, tick, nav) — 第 3 引数 NavSystem*。BtParamType::Mask (64 ビット、16 進) と BtNodeCategory::Ai は sub-08 のパラメータ欄・パレットで扱いが要る。SearchArea のパラメータは 4 項目 (usePrediction / radius / pointCount / failOnStuck)。名乗らない音を選んだら Entity キーは空にする (承認済み)。
- (sub-02 VERDICT) snapshot は v34 のまま承認。sub-03 (種類ごとの追加状態) で kSimSnapshotVersion 35 にする。UE 対応の未検証点 (Loop の Failure 抜け、Cooldown の Abort 後計時) は ADR-025 (sub-14) に列挙。ADR-025 は下書き (sub-14 で確定)。
- ロードマップ (`plans\ai-roadmap-m83-m86.md`) には「harness は使わず直接実装」とあるが、2026-10-05 にユーザーが `/harness M85の実装` を明示したのでハーネスで回す。
- (planner 2026-10-05) spec.md 確定 (planner 裁定)・sub-01〜14 を作成。番号の見込み: TypeId 78 BehaviorTree (sub-01) / 79 PatrolRoute (sub-07)、snapshot v34 (sub-01) から各サブで +1、ABI は sub-11 で 1 回だけ v27。調べた事実の要点は spec 2. の表に file:line 付き。
