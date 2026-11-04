# harness 台帳: m95-action-combat

- 依頼原文: M95 アクション戦闘の土台 (事前調査は plans\m95-action-combat\design-draft.md)
- 開始: 2026-11-04 / 基点コミット: 69c2207c2b8c5c79af7d37ababad0b03d397d228
- フェーズ: 実装
- 仕様確定: 2026-11-04 (planner PLAN_RESULT round 2)。番号: ABI v32 = 193 スロット、SimSnapshot v66 / v67 / v68、controller アセット v3、ADR-034、TypeId 追加なし

## サブ進捗
| サブ | 波 | 依存 | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|---|---|
| sub-00 selftest の登録表・フィルタ・所要時間ログ・full 入口・CI・予約マーカー | 0 | なし | OK (条件付き) | 1 | 8405f20 | master で単独。VERDICT 指摘 1: Release の --selftest-full と replay_verify は、ユーザーの対話用 Editor.exe (PID 39252) が Release exe を掴んでいるため未実施。解放後に司会が回して結果をここに書く。FAIL なら差し戻し |
| sub-00b 計測に基づく層分けと縮小 (既定 60 秒以内) | 1 | sub-00 | 実装中 | 0 | | 計測は単独で回す |
| sub-01 ヒットストップ (ABI v32、snapshot v66) | 1 | sub-00 | 実装中 | 0 | | |
| sub-02 CC 同士の押し出しと接触イベント (snapshot v67、ADR-034) | 1 | sub-00 | 実装中 | 0 | | |
| sub-04 遷移の割り込み (controller v3) | 1 | sub-00 | 差し戻し中 (round 2) | 1 | | round 1 REWORK: 最終検証 (Release / full / CR / RV / 三校) 未実施。コード変更なし |
| sub-06 イベント時刻の秒・フレーム指定 | 1 | sub-00 | 実装中 | 0 | | |
| sub-03 部位の重なり・掃引 (ABI 193 スロット) | 2 | sub-01、Wave 1 統合 | 未着手 | 0 | | |
| sub-05 ルートモーションの縦成分 (snapshot v68) | 2 | sub-02, sub-04、Wave 1 統合 | 未着手 | 0 | | |
| sub-07 文書 + 最終統合確認 | 3 | 全部 | 未着手 | 0 | | |

## 波の統合
| 波 | 統合コミット | 目視 (付録 B) | 検証 |
|---|---|---|---|

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- 制約解除の範囲: ルートモーションの縦成分 と イベント時刻の秒・フレーム指定 を解除する。CC カプセルのスケール挙動は対象外。固定 60 Hz tick と rand() 禁止は解除しない (2026-11-04、司会の AskUserQuestion)
- 進め方: /harness で回す (同上)
- 2026-11-04 planner の PLAN_RESULT (round 1) の「ユーザーに聞ける」論点を司会が AskUserQuestion で確認した結果:
  - 全体: spec §5 の受け入れ条件 1〜30 と §6 の 7 サブ分割はこのまま進める
  - #9 CC 同士の押し出し規則: 全量押し出し + pushPriority (planner 裁定どおり)
  - #15 縦ルートモーションの opt-in 単位: ステート単位 `"skel":{"rootMotionY":1}` (planner 裁定どおり)
  - #16 非 kinematic Rigidbody にも縦を適用: **適用する** (planner 裁定と逆。sub-05 に Rigidbody の「重力を飛ばす印」が増える)
  - #5 OverlapCapsuleParts: **要る** (planner 裁定と逆。sub-03 に ABI +1 スロットと受け入れ条件 +1)
  - #7 AnimatorPlay(duration>0) を遷移中に呼んだときも「近い側へ確定」へ変える (planner 裁定どおり)
  - selftest の所要時間対策 (スイート登録表 + `MYE_SELFTEST_ONLY` の汎用フィルタ + スイートごとの所要時間ログ) を **sub-00 として先頭に足す**。以降の coder は往復中は関連スイートだけ回し、完走はサブの最後 1 回
  - selftest の最小化は「テストを減らす」ではなく「1 回の実行で回す量を最小にする」3 層構成 (関連スイートだけ / `--selftest` 既定 = 軽い層 60 秒以内 / `--selftest-full` = 全部、VERDICT OK 前・レビュー・push 前・CI)。層分けは所要時間の計測結果に基づく。planner へ補足 2 として送信済み
  - coder は **波単位で並列** (1 coder = 1 worktree、同時ビルド 2〜3 本)。見立て: Wave 1 = {sub-01, sub-02, sub-04, sub-06}、Wave 2 = {sub-03, sub-05}、最後に sub-07。共有資源 (ABI スロット順・Components.h の追記順・snapshot 版・controller 版) の予約表を spec に置く
  - Wave 3 (sub-07、文書だけ) の最終検証: Wave 2 統合時の full 検証 (selftest-full Debug / Release、check_rules、replay_verify、三校) を受け入れ条件 30 の証拠にする。sub-07 では文書の参照先の実在と check_rules だけ確認する (2026-11-04、司会の AskUserQuestion。planner へ補足として送信)

## 申し送り (セッション跨ぎ)
- 事前調査は design-draft.md。planner は仮説として扱い実コードで裏取りする
- 既存シーン (三校・HAL Collector) をビット同一に保つため、挙動が変わる物は opt-in (既定 off)
- sub-00 (8405f20) から: `MYE_SELFTEST_ONLY` の値は `--selftest-list` の名前で指定する (例 `animatorcontroller,part`)。予約マーカーの文言は spec 付録 A.2 / A.5 / A.6 と同一で、各サブは grep で自分のマーカーを探して直下にだけ書く。登録表の tier を書き換えるのは sub-00b だけ。`twoddemo` / `navdeterminism` / `simparallel` は値を変えない
- sub-00 VERDICT の nit: (2) `ResolveSelfTestSelection` の専用テストが無い。sub-00b が tier を触るときに既存スイート (part か enginecli) へ「fast / full / only の 3 モード、別名 1 個の並び、servernet-* の前方一致、未知名で false」の小テストを足してよい (新スイートは足さない)。(3) EditorMain.cpp の `MYE_SELFTEST_ONLY` の wide → narrow 変換は 0x80 以上を `?` にする (名前は ASCII なので実害なし)
- Wave 1 の作業場所: `C:/HAL/MyEngin-wt/sub-<NN>` (ブランチ `m95/sub-<NN>`、基点 8405f20)。同時ビルド・テストは `C:/HAL/MyEngin-wt/locks/acquire-slots.ps1` / `release-slots.ps1` で 3 本までに制限 (sub-00b の計測は Count 3 で単独)
- Debug 全走の所要時間 (sub-00 の計測、全体 567 s): fracture 210 s / servernet 106 s / physics 64 s / navagent 45 s / fractureeditor 28 s / surfacedeferred 18 s / twoddemo 17 s。physics を縮める前に planner へ確認 (sub-00b.md の規則)
- Wave 1 共通規則 (sub-04 round 1 の VERDICT で planner が spec §5 冒頭に追加): 「最後の round」の判定は coder が行い、OK を求める round では SELF_EVAL を返す前に --selftest-full Debug / Release + check_rules + replay_verify + 三校 verify を回す。planner は full 検証の結果が無い SELF_EVAL に OK を出さない。司会は Wave 1 の 5 本全部にこの規則を通知済み (2026-11-04)
- sub-04 VERDICT の nit (申し送り): 割り込みで確定した tick は stateEntered = 0 になる (直前の AnimatorPlay(0) の stateEntered = 1 を潰す) → sub-07 の文書に 1 行。canInterrupt == 0 の遷移は走査内の先頭 continue で可 (anyInterruptible フラグ不要、spec §4.1 C を緩和)
