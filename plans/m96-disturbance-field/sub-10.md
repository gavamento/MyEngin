# sub-10: 地形の起伏に沿う 2D 層 (列ごとの基準高さ)

- 依存: sub-09 (波 W4。spec §6)
- 状態: 未着手
- 往復: 0


## 並列の注意 (波 W4)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): snapshot **v75**。`DisturbanceFieldComponent` の末尾 (`followTerrain`)、DFLD のレイアウト (columnBaseY)。デモは `AddFieldDemoHill`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §2 #12 (ユーザー回答 2026-11-08: 今回入れる)、§4.1 A / C / G の列ごとの基準高さ。丘の上でも霧が地面を這う。

1. `DisturbanceFieldComponent` の末尾に `followTerrain` (bool、既定 false。GroundLayer だけで有効、Volume では無視して Inspector に注記) を足す。snapshot は spec 付録 A.1 の予約の版。`followTerrain` は「変えたら作り直す」欄に入れる (spec §4.1 A)
2. 状態: `followTerrain` の場のスロットに列ごとの基準高さ `columnBaseY` (float × cellsX × cellsZ、絶対のワールド y) を持つ。ハッシュと DFLD 節に入れる
   - **導出値だが状態に入れる** 理由: 地形のコライダーが実行時に動く・増える場合も「焼いた時点の値」で決定的になり、スナップショットの復元の後に焼き直す順序の誤り (M65a の音響の距離場と同じ型) が起きない。64 KiB / 128² の場
3. 焼き込み: 作り直しの step で全列、追従のずらし (sub-09) では**入ってきた列だけ**、列の中心で `SampleTerrainHeightWorld(world, x, z, &h, nullptr)` (`PhysicsSystem.h:218-225`。当たる地形を引く、重なりは最も高い面・同値は entity index 小 = 決定的) を呼ぶ。外れ (地形が無い) の列は場のエンティティの y。以後は焼き直さない (地形を実行時に動かしても追わない = spec §7 R8)
4. 使う所 (`baseY` の代わりに列の基準高さ):
   - 注入の板の距離 (spec §4.1 C の `dy`): そのセルの列の値
   - `DisturbanceSampleAt` の `p(y)` と「含む」範囲 (spec §4.1 G): 列の値を双線形で
   - 描画: sub-02 の `RG16F` の G = `columnBaseY − baseY` (half。±1000 m で切る)。地面の霧の描画 (sub-03 の `GroundFogComposite`) の `p(y)` がこれを使う。`followTerrain = false` の場は G = 0 のまま
   - 移流・投影・波紋・跡は変えない (格子は平らなまま。斜面に沿う流れはしない)
5. デモ: `--field-demo` の末尾に、高さ数 m の丘の地形と、その上を覆う `followTerrain = true` の 2D 層を足す。デモの人の経路が丘を登る形

## やらないこと (このサブでは)

- 斜面に沿った流れ (重力で霧が谷へ流れ落ちる等)
- 地形を実行時に変えたときの焼き直し
- Volume の起伏対応

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Disturbance\` (状態・作り直し・注入・読み取り・ずらし)
- `src\Engine\Core\Ecs\Components.h` / `.cpp` の末尾欄
- `src\Engine\Engine\Replay\WorldHasher.cpp` / `SimSnapshot.cpp` の DFLD (列の配列)、版
- sub-02 の詰め替え (G チャネル) と `froxel_inject.cs.hlsl` の `p(y)`
- 地形を selftest で組む方法は `TerrainSelfTest` / `PhysicsSelfTest` の地形コライダーの組み方に合わせる
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 33。

- 33:
  - (a) 斜面の地形の上の `followTerrain = true` の層で、地形の高さ + 0.5 × thickness で読んだ濃さが、丘の上と平地で一致 (静止状態)。`followTerrain = false` なら丘の上では層の外 (`FieldSample` が 0)
  - (b) `followTerrain = false` の場の sim の配列が、この変更の前と 600 tick 毎 tick ビット一致 (列の配列を確保しない)
  - (c) 追従のずらしで、残った列の `columnBaseY` がビット不変で写り、入ってきた列だけが焼かれる
  - (d) 2 ワールド 600 tick 一致、snapshot 往復 100 tick 一致
  - (e) 焼き込み (128² 列) の ms を Release で報告。5 ms を超えたら数値を報告 (planner が判断)
  - (f) 丘を覆う霧のスクショ 1 枚 (霧が地面に潜っていない / 浮いていない)
- 12: replay_verify `field` PASS

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='disturbance,terrain,simsnapshot,acousticaudio'; bin\x64\Debug\Editor.exe --selftest` と Release
- スクショ (Runtime.exe)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
