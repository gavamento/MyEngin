# sub-00b: 計測に基づく selftest の層分けと縮小 (既定層 60 秒以内)

- 依存: sub-00 (Wave 1。sub-01 / sub-02 / sub-04 / sub-06 と並列)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 最初と最後に両方で `git status` を確認し、SELF_EVAL に貼る
- 触ってよいのは `SelfTestSuites.cpp` の `tier` 列、縮小すると決めたスイートの中身、`AGENTS.md` §7 の 1 行。他サブのファイル (`Components.*`、`AnimatorController.*`、`PhysicsSystem.cpp`、`EngineAPI.h` など) は触らない
- 縮小対象が M95 のテストを足すスイート (physics / animatorcontroller / part / skeleton) に当たる場合は、縮める前に「不安・質問」で planner へ (Wave 1 の他サブがそのスイートへ追記中)

## やること

spec §4.0 の「sub-00b」節。sub-00 の ms ログで全スイートの所要時間を Debug / Release 各 2 回計測して表に残し、
「Debug の所要時間の降順で上位 N 本を `Full` にしたとき既定層が 60 秒以内に収まる最小の N」で層を分ける。
`Full` 候補ごとに「縮小して `Fast` に残す」案を検討し、採否と理由を書く。

## やらないこと (このサブでは)

- 検査を落とすこと (全スイートは `--selftest-full` で従来どおり回る)
- Debug / Release のビット一致・焼き込みハッシュ・golden を検査するスイートの値の変更 (例: `twoddemo` 3000 tick、`navdeterminism`、`simparallel`、`worldhasher`、`simsnapshot`)
- 製品コードの変更

## 触る場所 (planner の見立て)

- `src\Editor\SelfTest\SelfTestSuites.cpp` の表の `tier` 列
- 縮小を選んだスイートの中身 (例: ベンチ系の反復回数・解像度。M80 の破砕ベンチで res を落とした前例を探す: `grep -n "res\|bench" src/Engine/Engine/Physics/Fracture/FractureSelfTest.cpp`)。縮めた場合、元の値で回す入口 (環境変数または `--selftest-full` のときだけ元の値) を残すか、縮めた値で検査が同値であることを実装メモに書く
- `AGENTS.md` §7: 「`--selftest` は fast 層 (Debug で 60 秒以内が目安)、提出前と CI は `--selftest-full`」の 1 行
- `sub-00b.md` の実装メモ: 計測表 (スイート / Debug ms ×2 / Release ms ×2 / tier / 縮小の採否と理由)

## 受け入れ条件 (このサブ)

spec §5 の 38, 39, 40, 41。
- 38: 計測表と、N の決め方が表から追えること
- 39: `bin\x64\Debug\Editor.exe --selftest` の total が 60 秒以内 (2 回とも)
- 40: `--selftest-full` (Debug / Release) の集合・順が sub-00 着手前の全走と同じ (名前列の diff が空)、全 PASS。縮小したスイートは縮小前の値でも 1 回 PASS させた記録
- 41: `--selftest-list` の fast + full = 登録表の本数

## 検証コマンド

- ビルド Debug / Release (worktree)
- 計測: `bin\x64\Debug\Editor.exe --selftest-full` ×2、`bin\x64\Release\Editor.exe --selftest-full` ×2 (他のビルド・テストと同時に回さない。同時ビルド中の計測は所要時間が狂う)
- 往復中: 縮小したスイートだけ `$env:MYE_SELFTEST_ONLY='<名>'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層。total を貼る)
- VERDICT OK の直前 (最後の round): `--selftest-full` Debug / Release、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (coder、worktree の再計測)

計測: 3 スロット全取得で単独。`--selftest-full` を Debug 2 回 / Release 2 回 (全 110 スイート 0 failed)。単位 ms。上位 14 本だけ載せる (全表は 110 行、残りは各 5 秒未満)。

| 順 | スイート | Debug 1 | Debug 2 | Release 1 | Release 2 | tier |
|---|---|---|---|---|---|---|
| 1 | fracture | 207049 | 191166 | 16583 | 16507 | Full |
| 2 | servernet | 100248 | 91528 | 9468 | 9485 | Full |
| 3 | physics | 55005 | 51625 | 5501 | 5468 | Full |
| 4 | navagent | 39699 | 39776 | 2506 | 2620 | Full |
| 5 | fractureeditor | 27582 | 24627 | 1978 | 2201 | Full |
| 6 | surfacedeferred | 16997 | 16682 | 15460 | 15500 | Full |
| 7 | twoddemo | 13066 | 12390 | 1526 | 1472 | Full |
| 8 | surfacematerial | 10143 | 10089 | 9378 | 9530 | Full |
| 9 | behaviortree | 9495 | 9430 | 415 | 426 | Full |
| 10 | occlusion | 5246 | 5206 | 5106 | 5148 | Full (round 2) |
| 11 | online | 5046 | 5031 | 5034 | 5047 | Fast |
| 12 | meshlod | 4754 | 4772 | 951 | 958 | Fast |
| 13 | simparallel | 4810 | 4715 | 209 | 220 | Fast |
| 14 | watersurface | 4799 | 4691 | 4394 | 4345 | Fast |

- N の決め方: 全スイートの Debug 平均合計 = 518.2 秒。上位 N 本を Full にした残り = N=8 で 64.3 秒 (> 60)、**N=9 で 54.9 秒 (<= 60)**。最小の N = 9 (上の表の Full)。Debug 2 回の差は fracture (207 / 191 秒) と servernet (100 / 92 秒) が大きいだけで、順位は 2 回とも同じ
- Release の `--selftest-full` total は 84.7 / 85.2 秒 (Release は既定層と full で分けない。tier は共通)
- 縮小の検討 (採否):
  - fracture / fractureeditor: 不採用。検査の本数が多く 1 つの res で縮む形に見えない (全文は読んでいない。ベンチ系は既に `--fracture-bench` へ移済み。FractureSelfTest.cpp:1410)。縮めると検査が落ちるので Full
  - servernet / navagent / behaviortree / physics: 不採用。中身を精査していないが、決定性・並走一致を見る系で、パラメータを縮めると被覆が変わる恐れがあり、spec の「検査を落とさない」を優先して Full へ移すだけにした (縮小の再検討は申し送り)
  - twoddemo: 焼き込みハッシュ (3000 tick) なので値を変えない (spec) → Full
  - surfacedeferred / surfacematerial: GPU 描画の実走が必要で、Debug / Release 両方で約 10〜17 秒かかる (Release でも同じ) → Full
  - 縮小したスイートは 0 本。よって「縮小前の値で 1 回 PASS」は該当なし
- 検証欄の計測 (既定層、Debug、同一 PC): `[selftest] total 101 suites, 0 failed, 59701 ms (mode: fast)` と `54602 ms`。1 回目は 60 秒まで 0.3 秒しかない (ばらつきの余白が薄い。申し送り参照)
- `--selftest-list`: fast 101 + full 9 = 110 = 登録表の本数
- 名前列の diff: HEAD の登録表の名前 (110) と Debug / Release の full の実走順の名前 (110) は完全一致 (差分 0)
- 追加: `VerifySelfTestSelectionRules()` (SelfTestSuites.cpp) を `engineindexdump` スイート (EngineIndexDumpSelfTest.cpp) から呼ぶ。sub-00 VERDICT nit (2) の対応 (fast / full / only の並びと重複除去、別名 1 個の並び、servernet-* の前方一致、未知名)

### round 2 (FIX_REQUEST #1: 余白の規則)

- 余白規則 (実測 total の大きい方が 55 秒超なら N+1) により N=9 (実測 59.7 / 54.6 秒) から N=10 へ。occlusion を Full に移した
- 上の round 1 の説明 (N=9) は round 1 時点のもの。round 2 の確定は N=10、Full は 10 本 (表の tier 列のとおり)

- 既定層 Debug (単独 3 スロット): `[selftest] total 100 suites, 0 failed, 50315 ms (mode: fast)` / `49620 ms` (大きい方 50.3 秒 <= 55)
- `--selftest-list`: fast 100 + full 10 = 110
- check_rules: 0 error(s), 50 warning(s)。full の再実行は不要 (tier は集合を変えない)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-04)。must 1 件: 既定層の余白。実測 59.7 / 54.6 秒は受け入れ 39 を満たすが余白 0.3 秒で、Wave 1 / 2 が既定層のスイート (animatorcontroller / part / skeleton) にテストを足すので統合後に割る見込み。spec §4.0 の規則に「その N で `--selftest` (Debug) を 2 回回した total の大きい方が 55 秒を超えるなら N+1」を追加した → N=10 (occlusion 5.2 秒を Full へ) にして既定層を 2 回再計測し、`--selftest-list` (fast 100 + full 10 = 110) を貼る。`--selftest-full` Debug / Release の再実行は不要 (tier は集合を変えない。round 1 の結果を流用してよい)。CR は再実行。RV は司会の再実行 (sub-00b worktree) が PASS ならそれを証拠にする (demo の `call :chain` ラベルエラーは環境要因と判定、spec §2 #25)。質問 3 (physics が Full): 許容 (spec §4.0 に明記。physics を触るサブは往復中 ST(physics) を名指し、OK 前の full で全走)。VerifySelfTestSelectionRules を engineindexdump に置いた判断は妥当 (M95 が触らないスイート)。縮小 0 本・採否の理由は受け入れ 38 の要件を満たす
- round 2: VERDICT OK (planner、2026-11-05)。occlusion を Full にして N=10、既定層 Debug 50.3 / 49.6 秒 (大きい方 55 秒以内)、list fast 100 + full 10 = 110、CR 0 error。full Debug / Release は round 1 の 110 スイート 0 failed ×2 ×2 を流用 (tier は集合を変えない)。RV は司会の再実行 (sub-00b worktree、22 ジョブ PASS 255 s、demo 114.5 s) を証拠にし、round 1 の demo FAIL は環境要因で確定。ユーザー判断: N=10 (司会の AskUserQuestion)。申し送り: 別名 m95 の並びは VerifySelfTestSelectionRules が固定 / fracture・servernet の縮小は後続 / sub-06 nit (1) の fps 負値テストは sub-07 へ
