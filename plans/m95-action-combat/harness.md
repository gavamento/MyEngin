# harness 台帳: m95-action-combat

- 依頼原文: M95 アクション戦闘の土台 (事前調査は plans\m95-action-combat\design-draft.md)
- 開始: 2026-11-04 / 基点コミット: 69c2207c2b8c5c79af7d37ababad0b03d397d228
- フェーズ: 実装 (Wave 1 統合済み e1bba10。2026-11-05 02:00 から Wave 2 = sub-03 / sub-05 を worktree 並列で実装中)
- 仕様確定: 2026-11-04 (planner PLAN_RESULT round 2)。番号: ABI v32 = 193 スロット、SimSnapshot v66 / v67 / v68、controller アセット v3、ADR-034、TypeId 追加なし

## サブ進捗
| サブ | 波 | 依存 | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|---|---|
| sub-00 selftest の登録表・フィルタ・所要時間ログ・full 入口・CI・予約マーカー | 0 | なし | OK | 1 | 8405f20 | master で単独。VERDICT 指摘 1 (Release 検証が Editor.exe の占有で未実施) は解消: 19:38 司会が master (a6bc7aa、シミュレーションは 8405f20 と同一) で `tools\replay_verify.bat` → **PASS** (22 ジョブ 433 s、15 シーン Debug/Release 一致 + snapshot 往復 + jobs A/B + compute A/B + タイムトラベル、check_rules 0 error)。20:08 Release `--selftest-full` (cmd 経由) → **PASS** (110 スイート 0 failed、100 s) |
| sub-00b 計測に基く層分けと縮小 (既定 60 秒以内) | 1 | sub-00 | OK | 2 | 030af20 (m95/sub-00b) | round 1 は余白 0.3 s で REWORK (ユーザー判断 N=10)。Full 10 本、既定層 Debug 50.3 / 49.6 s、full 110 スイート 0 failed ×2 ×2、CR 0 error、RV は司会の再実行 22 jobs PASS (demo FAIL は環境要因) |
| sub-01 ヒットストップ (ABI v32、snapshot v66) | 1 | sub-00 | OK | 1 | 0078d43 (m95/sub-01) | full 検証済み (Release full 0 failed、Debug full は hash 更新前 109 PASS + 更新後 twoddemo 単独、RV 22 jobs PASS、CR 0 error)。check_rules.ps1 の `32 = 190` は planner が spec に正式化。TwoDDemo の焼き込み hash を更新 (統合後に再採取) |
| sub-02 CC 同士の押し出しと接触イベント (snapshot v67、ADR-034) | 1 | sub-00 | OK | 1 | 2992ad3 (m95/sub-02) | full 検証済み (Debug 972 s / Release 107 s、RV PASS)。焼き込み hash 5 件 + AcousticAudio 版チェックは統合後に再採取 |
| sub-04 遷移の割り込み (controller v3) | 1 | sub-00 | OK | 2 | 3dca0f6 (m95/sub-04) | round 1 は最終検証未実施で REWORK。full 検証済み (Debug 814 s / Release 108 s、RV PASS)。三校は環境要因 |
| sub-06 イベント時刻の秒・フレーム指定 | 1 | sub-00 | OK | 1 | cda5413 (m95/sub-06) | full 検証済み (Debug 619 s / Release 96 s、RV 22 jobs PASS)。三校 FAIL は別件 |
| sub-03 部位の重なり・掃引 (ABI 193 スロット) | 2 | sub-01、Wave 1 統合 | OK | 1 | 6cc89b5 (m95/sub-03) | worktree `C:/HAL/MyEngin-wt/sub-03` (基点 e1bba10)。Release full 全 PASS、RV 22 ジョブ一致、ABI 照合 193/193。Debug full の fractureeditor FAIL は負荷要因 (下記申し送り) |
| sub-05 ルートモーションの縦成分 (snapshot v68) | 2 | sub-02, sub-04、Wave 1 統合 | 実装中 (round 1) | 0 | | worktree `C:/HAL/MyEngin-wt/sub-05` (旧 sub-02 を move、ブランチ m95/sub-05、基点 e1bba10) |
| sub-07 文書 + 最終統合確認 | 3 | 全部 | 未着手 | 0 | | |

## 波の統合
| 波 | 統合コミット | 目視 (付録 B) | 検証 |
|---|---|---|---|
| 1 (sub-00b, 01, 02, 04, 06) | e1bba10 (2026-11-05 01:55) | merge 順 00b→01→02→04→06、競合は sub-02 の 3 ファイルのみ (SimSnapshot.h = 最大値 67 + 版コメント両方、AcousticAudio 版チェック 67、TwoDDemo hash)。目視: Components.h/.cpp の各サブ欄がマーカー直下、EngineAPI.h / Interop.cs の関数ポインタ順 190/190 diff 0、Update に pauseTicks / canInterrupt、SelfTest 各マーカー、LocalizationTable M95、Full 10 本。新規ファイル無し (gen_project_files 不要) | Debug / Release ビルド OK。焼き込み hash で動いたのは TwoDDemo のみ (0x1FD6DDE81A0FCA6C → 実測 `[twod] world hash after 3000 ticks: 0xAAC7A2732CA0AA85`)、Physics / NavAgent / Tilemap / GridNav2D は不変。`--selftest-full` Debug 110 スイート 0 failed (549.0 s) / Release 110 スイート 0 failed (94.4 s、同じ hash で PASS)。check_rules 0 error / 50 warning。replay_verify 22 ジョブ PASS (271.5 s)。三校: master で DLL を v32 で作り直し (`[dll] GameLogic loaded ... (3 scripts)`)、verify.bat は既存の 2 件 (shot main diffPixels=28402 / patrol) のみ FAIL = 増えていない (合格条件どおり) |

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
  - sub-00b の既定層の余白 (planner 裁定 N=10、occlusion も Full 層へ、「2 回の大きい方が 55 秒超なら N+1」の規則): **N=10** (planner 裁定どおり。2026-11-05、司会の AskUserQuestion)
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
- sub-06 VERDICT の nit: (1) `ClipEventTickFromJson` の fps 負値のテストが無い (sub-00b か sub-07 で 1 件足してよい)。(2) 秒の併記 (受け入れ 28) の目視は sub-07 の test_checklists へ
- 別件 (M95 とは無関係、基点以前から): 三校 `toolserify.bat` が 2 件 FAIL (shot main の img-diff diffPixels=34541、patrol の listener)。原因は三校側の `cache\GameLogic.dll` が API v30 のままで、M94 の ABI v31 以降ロードされていない (`replay_record.log`: `[dll] API version mismatch (dll=30, engine=31)`)。対処は三校の `toolsuild_scripts.bat` で DLL を作り直してから verify する。ABI を上げる M95 では Wave 1 統合後 (v32) にも作り直しが要る
- 三校 verify の扱い (sub-04 round 2 の VERDICT で spec §5 冒頭に追記): 三校の cache/GameLogic.vcxproj は master 固定参照なので worktree では構造上通せない。司会が波の統合時に master で三校 DLL を作り直し (tools/build_scripts.bat) てから回し、結果を台帳に残す。worktree での DLL 不一致 FAIL は採点外、coder は回さなくてよい。Wave 1 統合後は ABI v32 で作り直す
- sub-02 VERDICT の nit: 同優先度の CC が隙間から歩み寄ると最大 1 歩ぶん重なる tick があり得る (順序非依存の設計から出る性質、許容。ADR-034 影響欄に記載) → sub-07 の engine_spec に 1 行。spec §2 #21 の「既存シーンの hash 不変」は訂正 (WorldHash は欄の生バイトを混ぜるので値が変わる。シミュレーション結果は不変)
- Wave 1 / Wave 2 の統合で焼き込み hash の再採取が要る (付録 B 4.): PhysicsSelfTest (CC2D) / NavAgentSelfTest (庭) / TilemapSelfTest / GridNav2DSelfTest / TwoDDemoSelfTest の定数 5 件と AcousticAudioSelfTest の kSimSnapshotVersion チェック。Debug の --selftest-full のログから取り、Release で同値 PASS を確認してから統合コミット
- 三校 cache/GameLogic.dll は sub-02 の coder が worktree (v31) のヘッダで上書きした (gitignore 配下)。統合時に master で作り直す
- 別件の切り分け結果 (2026-11-04、司会): master 8405f20 (シミュレーションは基点 69c2207 と同一) で三校 DLL を v31 で作り直し (`[dll] GameLogic loaded ... (3 scripts)`) ても三校 verify は同じ 2 件 FAIL (shot main diffPixels=28402 / maxDiff=122、replay は VERIFY PASS だが「agent never left patrol」)。= M95 以前からの既存失敗。golden は 2026-10-12 撮影で、その後の M90 / M93 / M94 のどれかで変わった可能性。M95 の範囲外としてユーザーへ報告、統合時の三校 verify は「この 2 件以外に FAIL が増えないこと」を合格条件にする
- 一時停止時の再開手順 (2026-11-04 14:05): (1) sub-00b と sub-01 は新しい coder を worktree (`C:/HAL/MyEngin-wt/sub-00b`, `sub-01`) で起動し、「未コミットの差分が残っている。`git diff` を読んで引き継ぎ、sub-NN.md の実装メモと spec を基準にやり直し部分を判断せよ」と伝える (前の coder の文脈は失われている)。(2) planner もセッションを跨ぐと再起動が要る (spec / sub / 台帳から文脈を渡す)。(3) OK 済み 3 本はブランチにある: m95/sub-02 = 2992ad3、m95/sub-04 = 3dca0f6、m95/sub-06 = cda5413。(4) Wave 1 の 5 本が揃ったら付録 B で統合 (焼き込み hash の再採取、三校は master で DLL を v32 で作り直し、既存 2 件以外の FAIL が無いこと)。(5) 残骸 `C:/HAL/MyEngin-wt/sub-02-base` (45 MB) の削除はユーザー許可待ち。(6) sub-00 の Release 検証 (--selftest-full と replay_verify) は Release exe の解放後に未実施のまま
- 再開時の planner 引き継ぎ (2026-11-04、planner 再起動): sub-00b の未コミット差分は tier を Full にした 9 本 (physics / surfacematerial / surfacedeferred / fracture / fractureeditor / servernet / navagent / behaviortree / twoddemo) + `VerifySelfTestSelectionRules` (SelfTestSuites.h/.cpp、sub-00 nit (2) の小テスト) を `PartSelfTest.cpp` 冒頭から呼ぶ形。縮小は未実施、計測表 (受け入れ 38) も未記入。sub-01 の差分は ABI v32 2 本 / snapshot v66 / AcousticAudio 版チェック 66 / PartSelfTest の ABI 断言まで入っている。**sub-00b と sub-01 の両方が `PartSelfTest.cpp` を触る** (冒頭 vs 末尾マーカー直下、テキストは重ならない) ので Wave 1 統合の目視 (付録 B 3.) に `PartSelfTest.cpp` を足す
- 一時停止 2 回目 (2026-11-04 19:55): 再開手順は 1 回目と同じ (新しい coder を worktree で起動して `git diff` で引き継ぐ、planner も再起動)。追加の注意: (a) sub-01 の差分に `tools/check_rules.ps1` が含まれる — coder の役の範囲 (sub-01.md の触る場所) に無いので、次の coder に「変更理由を仕様との差分に書かせる」か、planner に妥当性を判定させる。(b) Release の `--selftest-full` を PowerShell の `&` で直接起動すると GUI exe を待たずに戻ってログが空になる。CI と同じ `cmd /c bin\x64\Release\Editor.exe --selftest-full > log 2>&1` で回す (sub-00 の Release 検証はこれで未完)。(c) 司会のスロット名は `moderator`。停止時に全スロットを解放済み、MSBuild 4 本は自然終了待ち
- sub-01 VERDICT の nit: (1) 非アクティブ entity で `pauseTicks` が減らないことの断言がテストに無い (コード上は正しい) → sub-07 か reviewer が `// ---- M95 sub-01 ----` 直下に 1 件足してよい。(2) Components.h の sub-01 マーカー文言が変わっている (実害なし、統合の目視は sub 名で grep)。仕様側: ScriptAPI.h の Animator 糖衣は仕様の前提違いで削除、`tools/check_rules.ps1` の `$apiVersionSlots` は sub-01 が `32 = 190`、sub-03 が 193 へ直す (sub-03.md に追記済み)
- sub-00b round 1 の RV demo ジョブ FAIL (`cannot find the batch label - chain`、0.4 s) は環境要因と確定: bat は master / sub-01 / sub-00b で md5 同一、司会が sub-00b の worktree で回し直して 22 ジョブ PASS (demo 114.5 s、2026-11-05 00:05)。spec §2 #25。cmd の `call :label` はバイト位置で読み直すのでコンソールのコードページ残留 (chcp 65001) で外れ得る → RV を回す前に `chcp` を確認
- sub-03 VERDICT の nit: (1) `RaycastParts` / `SphereCastParts` は呼び手が dir を正規化する (`SphereCastWorld` は正規化する) → sub-07 の engine_spec に 1 行。(2) C# 糖衣 (`Interop.cs` / `MyeScript.cs`) は `dotnet build` のみで実走なし (C# レーンは replay 被覆外) → reviewer が Phase 3 で temp プローブを回すかは裁量
- 別件候補 (M95 とは無関係): fractureeditor の cancel / shutdown は時間依存テスト。他 coder のビルドと並走する負荷下では sub-03 worktree (118 s) でも sub-03 無しの master e1bba10 (219 s、同期 bake 134 s) でも同じ 2 項目 (「cancelled bake が Ready に」「cancelled を報告」) が落ちる。無負荷の Wave 1 統合時は 25 s で PASS。Wave 2 統合の無負荷 Debug full で再確認し、落ちたら別件として起票。重い Debug full の判定は無負荷で行う
- sub-00b VERDICT の nit: (1) 別名 `m95` の並びを `VerifySelfTestSelectionRules` (engineindexdump) が固定している → 別名表を変えるサブはこの検査も直す。(2) fracture (約 200 s) / servernet (約 95 s) の縮小は M95 の範囲外として後続。(3) sub-06 nit (1) の `ClipEventTickFromJson` fps 負値テストは sub-07 で足す。Full 層 10 本 = physics / surfacematerial / surfacedeferred / fracture / fractureeditor / servernet / navagent / behaviortree / occlusion / twoddemo。Full スイートを触るサブは往復中 ST(名) で名指し (spec §5 冒頭)
