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

## フィードバック履歴
