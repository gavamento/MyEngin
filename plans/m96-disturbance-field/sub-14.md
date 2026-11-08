# sub-14: 草 (サーフェスシェーダの予約テクスチャ + デモの草束)

- 依存: sub-03 (波 W2。spec §6)
- 状態: 未着手
- 往復: 0


## 並列の注意 (波 W2)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): PS の固定スロットと**同じ番号を VS にも張る口** (唯一)、`MyEngineSurface.hlsli` の予約テクスチャ。デモは `AddFieldDemoGrass`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §4.3 の草、§7 R6。通った所の草が倒れ、戻る。草の描画は今エンジンに無いので、M79 のサーフェスシェーダで作る。

1. `MyEngineSurface.hlsli` に予約テクスチャ (sub-02 の 2D 配列) と CB、`MyeFieldSample(posW)` (流れと濃さを返す HLSL 関数) を足す。予約の束縛は既存の予約 (`gFroxelVolume` 等、`:127-131`) と同じ自動割り当ての規約に従う。プロジェクトのサーフェスシェーダから使えることを `docs\surface-shaders-deferred-limits.md` 等の既存の作者向け文書に 1 段落
2. `assets\shaders\GrassField.surface.hlsl`: VS で、オブジェクト空間の高さに比例して流れの向きへ倒す (根元は動かない)。場が無い・流れ 0 なら真っ直ぐ。風の揺れ (時間の sin) は描画レーンなので使ってよい
3. デモ: `--field-demo` の末尾に草束 (細い箱のエンティティ数百。数は描画の実測で決める) を霧の広場に並べる
4. インスタンシング経路でサーフェスシェーダが使えるかを確認し (spec §7 R6)、使えなければ個別描画のまま描画時間を `--render-stats-dump` で測って報告

## やらないこと (このサブでは)

- 草の配置ツール・地形の detail 草 (Unity Terrain の detail 相当) — 別のマイルストーン
- 草の当たり判定

## 触る場所 (planner の見立て)

- `assets\shaders\MyEngineSurface.hlsli:120-135`、`MyEngineSurfaceEntries.hlsli`
- `src\Engine\Renderer\Shader\SurfaceDrawBind.h` / `.cpp` (予約の束縛)、`ForwardPath.cpp:321-381` / `DeferredPath.cpp` の surface の CB
- 新規 `assets\shaders\GrassField.surface.hlsl` (+ `.meta` は既存の生成規約に従う)
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 25, 30。

- 25: 球・人が通る前 / 直後 / 戻った後の 3 枚で、通り道の草が倒れて戻る (`--img-diff`)。場の無いシーンに置いた草のマテリアルが真っ直ぐ立つ 1 枚
- `surfaceshader` / `surfacematerial` / `surfacedeferred` の selftest 不変
- 12: replay_verify `field` PASS
- 草束の描画時間 (Release、`--render-stats-dump`)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='surfaceshader,surfacematerial,surfacedeferred,disturbance'; bin\x64\Debug\Editor.exe --selftest`
- スクショ (Runtime.exe、Forward と Deferred)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
