# sub-11: RenderSystem の線パスを定数の色で描く (カリングの枠・デバッグ線・ナビの線)

- 依存: sub-10
- 状態: OK (コミットは司会)
- 往復: 2

## やること
spec 受け入れ条件 16 (と 9 の見た目)。出所は review-1 #2 / review-2 #1。2026-11-11 にユーザーが「直す」と判断し、対象を「同じ線パスを通る線すべて」に広げた (planner の裁定「カリングの枠だけ」の逆。spec 8.)。

### 対象 (planner が grep で洗い出した。coder は着手時に `linePass_` / `navFillPass_` / `DebugLineCmd` の呼び出し元を grep し直し、増減があれば実装メモに書く)
`RenderSystem::linePass_` (`EditorLinePass`、`RenderSystem.cpp:2788-2845`) に積まれる線すべて:
- ナビメッシュの輪郭線 (`navView->Lines()`、深度テストあり)
- `debugLines` (`DebugLineCmd` の列、深度テストあり)。中身はスクリプトの `DebugDrawLine` (v7) だけではなく、エンジンの次の系も積む (`EngineLoop.cpp:301 / 610 / 1102 / 3295` 経由):
  - 物理のデバッグ (`PhysicsDebugDraw.cpp`、`TickRunner.cpp`)
  - BT (`BehaviorTreeSystem::AppendDebugLines`)
  - 知覚 (`PerceptionSystem::AppendDebugLines`)
  - ナビのエージェント経路 (`NavSystem::AppendDebugLines`)
  - かき乱しフィールド (`DisturbanceAppendDebugLines`)
  - 音響 (`AcousticDebugDraw.cpp`)
- M99 のカリング枠 (理由別の色)、固定した視錐台の白線 (`onTop`)
- M90 の `--hzb-debug-max` の赤い箱 (`onTop`)

**ナビメッシュの塗り (`NavFillPass`、`RenderSystem.cpp:2779-2785`) も対象に入れる。** 線パスではないが同じ `editor_line` シェーダ・同じ 0xRRGGBBAA の色の約束で、輪郭線の直前に描いている。線だけ直すと、輪郭は定数の色・塗りは淡い色、と食い違うため。

**対象外**とその理由 (実装メモで裏を取る):
- エディタ側の `EditorLinePass` / `GhostMeshPass` (`SceneViewWindow` のグリッド・ワイヤ・アウトライン・分岐のゴースト)。これらは RenderSystem の外で Scene View の RT に描く別インスタンス。どの段の目標に描いているか (LDR の後か) を確かめる。もし同じ問題があっても M99 の範囲外として報告だけする (直さない)。

### 1. 先に原因を切り分ける (推測で直さない)
今の線は `view.rtv` (HDR 中間。直描きのときは最終 RT) へ描かれ、その後で `ResolvePost` (トーンマップ・ブルーム・FXAA 等) を通る。色は `EditorLinePass::Unpack` (`EditorLinePass.cpp:41-45`) が 0..1 に割るだけ。疑う要因と確かめ方:
- (a) トーンマップ・露出・ブルーム: ポストプロセスを切った撮影で、同じ線の画素の色が変わるか。
- (b) sRGB / リニアの取り違え: 最終 RT とバックバッファの DXGI 形式 (`_SRGB` か) を確かめる。51/255 が最終 PNG でどの値になるかを計算と照合する。
- (c) FXAA / TAA などの後段フィルタが 1 px の線をぼかしていないか。

線 1 本の中心の画素を決める (距離の箱の 1 辺、`--hzb-debug-max` の赤い箱の 1 辺、`--nav-demo` の輪郭線の 1 本)。条件ごとの RGB を表にして実装メモに書く。

### 2. 直す
推奨は、原因が (a) / (c) のときの次の方式:
- 対象の線と塗りを、ポストプロセスの解決 (`ResolvePost`) の**後**で、最終の LDR 目標へ描く。最終目標が `_SRGB` 形式なら、定数 (sRGB の値として読む) をリニアへ直して渡す。
- 原因が (b) だけなら、変換だけで足りる可能性がある。どちらにしたか、なぜかを実装メモに書く。

深度テストのある線 (ナビ・`debugLines`・ナビの塗り) について:
- LDR 目標と深度バッファの寸法・射影が一致することを確かめる (解像度スケールや TAA のジッタで食い違わないか)。
- 一致しない構成があれば、その構成での扱い (例: その構成だけ今の経路のまま) を決め、実装メモと engine_spec に書く。
- 壁の裏に入った線が隠れることは変えない。

経路と表示先:
- 直描き (HDR 中間を使わない) の経路でも同じ色になること。
- Editor の Scene View / Game View と Runtime のどれでも同じ扱いにする。

文書:
- ツールチップの凡例 (カリング枠) と見た目の色名が合っていることを確かめる。
- engine_spec に「RenderSystem の線と塗りはポストプロセスの後に定数の色で描く」(採った方式と、深度テストの扱い) を書く。場所は §6.14 のデバッグの項、DebugDrawLine の節 (M37)、ナビの描画の節。

### 3. 動く golden の確かめ方 (更新は確かめてから)
`nav.png` は動く前提。他にも線を含む golden (`perception.png`、`bt.png`、`physics.png`、`joints.png`、`acoustic_*.png`、`fog*` など) が動きうる。

動いた golden ごとに、**差分が線と塗りの画素に限られる**ことを次の手順で示してから `--update` する。
- **一時プローブ** (コミットしない。memory の「一時プローブ + --screenshot」) で、`linePass_` と `navFillPass_` の描画だけを止めた新ビルドの撮影 N を撮る。
- 次の 3 つの差分の画素集合を求める。
  - D = 旧 golden と新ビルドの通常撮影の差分
  - L_old = 旧 golden と N の差分 (旧の線の画素)
  - L_new = 新ビルドの通常撮影と N の差分 (新の線の画素)
- **D ⊆ L_old ∪ L_new** であることを確かめる。つまり、線と塗りの画素以外は tol 0 で一致している。
- 画素数 (|D|、|L_old ∪ L_new|、D のうち外れた画素の数 = 0) を golden ごとに表にして実装メモに書く。
- 外れた画素が 1 つでもあれば、golden は更新しない。原因を報告する。

## やらないこと (このサブでは)
- 線の太さ・形、カリングの枠の上限、カリングの判定の変更
- 各系 (物理・BT・知覚・ナビ・音響・かき乱し) が積む線の色・本数の変更
- エディタ側の `EditorLinePass` / `GhostMeshPass` の変更 (問題があれば報告だけ)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp`
  - 線の積み込み :2788-2845
  - ナビの塗り :2779-2785
  - `ResolvePost` :2848-
  - 色の定数 :669-673
- `src\Engine\Renderer\Passes\EditorLinePass.{h,cpp}`、`src\Engine\Renderer\Passes\NavFillPass.*`
- 最終目標の形式: `PostProcess` / `GraphicsDevice` のバックバッファとオフスクリーン RT の作成箇所
- `src\Engine\Core\Localization\LocalizationTable.inl` (凡例)、`engine_spec.md`
- `tests\golden\*.png` (確かめた後の更新のみ)

## 受け入れ条件 (このサブ)
1. 原因の切り分けの表 (条件ごとの線の画素の RGB) が実装メモにあり、採った直し方がその結果から説明できる。対象の呼び出し元の一覧 (grep の結果) と、対象外にした物の理由もある。
2. 色の一致を数字で判定する (round 1 で式を変更。spec 8.)。
   - 判定の式: 線で変わった画素 (線ありと線なし N の差分) のうち、「N の画素と定数色の α 混合 (α は画素ごとに最小二乗で求める) との各チャネルの残差がすべて 3 以下」で説明できる画素の割合が **95% 以上**。修正前のビルド (または HEAD 版の exe) では同じ割合が明らかに低いことも示す (少なくとも青とナビの輪郭)。
   - 撮影の共通条件: Runtime.exe、`--warp --no-audio --font-embedded --no-fxaa`、960x540。
   - 色ごとの撮影 (round 1 の組み合わせの誤りを訂正):
     - 距離の青: `--culling-distance-demo --cull-debug --cull-max-distance 40`
     - 画面サイズの緑: `--culling-distance-demo --cull-debug --no-distance-culling --cull-min-screen-size 0.02` (距離が先に当たらないように距離を切る。出なければ `--render-bench-demo --cull-debug --cull-min-screen-size 0.02`)
     - 視錐台の黄と固定した視錐台の白: `--render-bench-demo --cull-debug --cull-freeze-frame 10 --render-bench-cut-frame 20`
     - オクルージョンの赤: `--render-bench-demo --hzb-debug 1 --hzb-debug-max` (`--cull-debug` 経由の赤は同じ箱の経路なので代替でよい)
     - ナビの輪郭 (`kBoundaryColor` 0x40E8FFFF) と、`debugLines` の色を少なくとも 1 つ (知覚か BT の線): `--nav-demo`、`--perception-demo` または `--bt-demo`
   - FXAA を通す既定の撮影 (`--no-fxaa` なし) でも青で同じ判定が通ること。
   - 色ごとに「変わった画素数 / 説明できた画素数 / 割合 / 修正前の割合」を表にする。
   - ナビの塗りは、round 1 の床の矩形の中央値 (61.5 → 0.5) で満たしたとみなす。
3. 壁の裏の線は今どおり隠れる。深度テストのある線で、遮られた部分の画素が修正前と同じく描かれていないことを示す (例: `--nav-demo` で壁の裏に入るナビの線の領域について、L_new に入る画素が無い。またはそれに準じる確かめ方)。
4. golden の扱い (上の「3. 動く golden の確かめ方」):
   - `tools\shot_verify.bat` を回し、動いた golden ごとに D ⊆ L_old ∪ L_new の表を作る。外れた画素が 0 の物だけ `--update` する。
   - 最終的に shot_verify が既知の `flow_title` 以外 PASS する。
   - 線を含まない golden は動かない。
5. `tools\cull_verify.bat` が PASS する。既定ベンチ、`--culling-demo`、`--culling-distance-demo` の `--render-stats-dump` の `counts` が sub-10 後と一致する (cull_counts.ps1 で機械比較)。`--cull-debug` ありでも `counts` は枠表示なしと一致する。
6. `tools\replay_verify.bat` が PASS する (描画だけの変更で sim に触れていないことの確認。golden.rep は画像を含まない)。
7. 凡例の色名と見た目が合う。ローカライズ規則と `tools\check_rules.ps1` が 0 error。
8. 撮影した画像のパス (修正前後の比較を含む) と golden の差分の表を SELF_EVAL に書く。最終の目視はユーザー。

## 検証コマンド
- Debug / Release ビルド
- 上記の Runtime.exe 撮影と、画素数を数える一時スクリプト。線を止める一時プローブはコミットしない
- `tools\shot_verify.bat` (`--update` は表で確かめた golden だけ)、`tools\cull_verify.bat`、`tools\replay_verify.bat`、`tools\check_rules.ps1` (pwsh)
- `MYE_SELFTEST_ONLY=render,renderstats,occlusion,nav,perception,behaviortree,physics` で `Editor.exe --selftest` (スイート名は `--selftest-list` で確かめる)

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し。撮影の置き場 S = `C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\f2580926-5c7d-48c2-9842-9b1a8a51de55\scratchpad\s11\`)

**原因の切り分け (受け入れ条件 1)**: 距離の青 (51,153,255) の枠、`--culling-distance-demo --cull-debug --cull-max-distance 40 --cull-min-screen-size 0.02`、修正前ビルド、Release / `--warp --no-fxaa` / 960x540。
| 条件 | 定数色との各チャネル差 8 以下の画素数 | 最も近い画素 |
|---|---|---|
| 既定 (トーンマップあり) `S\old_dist.png` | 0 | (78,148,182) / (113,187,213) |
| `--no-bloom` `S\old_dist_nobloom.png` | 0 (既定と同じ) | 同上 |
| `--no-postfx` (直描き、トーンマップなし) `S\old_dist_nopost.png` | 10 | (53,154,253) 等 = ほぼ定数 |
- (a) トーンマップが原因。ブルームは無関係。(c) FXAA は `--no-fxaa` で撮っているので除外 (FXAA ありの 1 枚は修正後に撮った)。(b) 最終 RT は `R8G8B8A8_UNORM` (`SwapChain.cpp:36` / `RenderTexture.cpp:113` / `PostProcess.cpp:247`)、sRGB カーブはトーンマップのシェーダが掛けるので、最終 RT へ直接描けば変換は要らない。
- 採った直し方: 推奨どおり、線と塗りを `ResolvePost` の後に最終 LDR 目標へ描く (`RenderSystem::DrawDebugOverlay` を新設して呼ぶ)。変換は入れていない。
- 深度: 解決は `target.dsv` を触らず、HDR 中間も `target` と同じ幅・高さで確保される (`BeginView`: `Acquire(target.width, target.height)`) ので、全構成で寸法が一致。射影は TAA ジッタなしの `view.projNoJitter` で描く (深度はジッタ付きで書かれているが、線は 0.2% 手前へ寄せるので許容。`--deferred --taa` で撮影して確認)。深度 SRV が束縛されたままでも競合しないよう、`view.dsvReadOnly` があればそれを使う。
- 呼び出し元の grep (`linePass_` / `navFillPass_` / `DebugLineCmd` / `debugLines`): `RenderSystem.cpp` の 1 か所の線パスに、ナビの輪郭 (`navView->Lines()`)、`debugLines`、カリング枠 (`cullDebugBoxes_`)、オクルージョン箱 (`f.occlusionBoxes` = `--hzb-debug-max` と `--cull-debug` の赤)、固定した視錐台 (白) が積まれる。`debugLines` の積み手は planner の一覧どおり (増減なし)。ナビの塗りは `navFillPass_` の 1 か所。
- 対象外の裏取り: `EditorLinePass` のエディタ側インスタンス (`SceneViewWindow`) は元から解決後に描いている (`EditorLinePass.cpp` 冒頭のコメントと `RenderSystem::BeginView` のコメント: 「解決後にエディタが重ねるギズモ/線」) ので問題の対象外。変更していない。

**撮影した画像 (受け入れ条件 2、8)**: 無印 = 線あり、`_N` = 一時プローブで線と塗りを止めた撮影、`_F` = 塗りだけ。共通条件は Runtime.exe (Release)、`--warp --no-audio --font-embedded --no-fxaa`、960x540 (FXAA 行だけ `--no-fxaa` なし)。スクリプト `scratchpad\shots2.ps1` / `shots3.ps1` (撮影)、`match.py` (判定の式)、`multi.py` / `blend.py` (補助)、`subset.py` / `occl.py` (golden)、`fill.py` (塗り)。
判定の式 (定数色との各チャネル差 8 以下の画素数、線あり − 線なし) の実測:
| 撮影 | 色 | 線あり | 線なし | 差 | 判定 |
|---|---|---|---|---|---|
| `new_dist` (距離デモ) | 青 0x3399FF | 5 | 0 | 5 | **未達 (<20)** |
| `new_dist` | 緑 0x33CC33 | 0 | 0 | 0 | 枠が出ない (下記) |
| `new_distfxaa` (FXAA あり) | 青 | 5 | 0 | 5 | **未達** (`new_dist` と同じ。FXAA は線の前に終わる) |
| `new_bench` (視錐台固定) | 黄 0xFFD700 | 2247 | 0 | 2247 | OK |
| `new_bench` | 赤 0xFF3333 | 0 | 0 | 0 | 赤の枠が出ていない |
| `new_bench` | 固定した視錐台の白 | 0 | 0 | 0 | **未達** (細い斜め線で、暗い空の上でも alpha < 1) |
| `new_benchsize` (追加: ベンチ + 画面サイズ 0.02) | 緑 0x33CC33 | 2296 | 0 | 2296 | OK |
| `new_hzb` | 赤 0xFF3333 | 23241 | 0 | 23241 | OK |
| `new_nav` | 輪郭 kBoundaryColor 0x40E8FF | 0 | 0 | 0 | **未達** (最も近い画素 (62,222,246)。修正前は (65,209,219)) |
- 未達の理由は式の側にある: 線は `AntialiasedLineEnable` の 1 px 線 (α 被覆で混ぜる。形は変えない約束) なので、明るい背景の上では α ≥ 0.95 の画素がほとんど無い (α=0.9 でも背景 220 の上だと 17 ずれて 8 を超える)。そこで補助の判定として「線で変わった画素が、背景 (線なし) と定数色の α 混合で説明できるか」を画素ごとに残差で調べた (`multi.py`、残差 3 以下):
| 撮影 | 変わった画素 | 定数色の α 混合で説明できた画素 (うち α≥0.5) |
|---|---|---|
| `new_dist` 青 | 896 | 896 (473) = 100% |
| `new_distfxaa` 青 | 896 | 896 (473) |
| `new3_nopost` (直描き経路) 青 | 896 | 896 (478) |
| `new3_taa` (`--deferred --taa`) 青 | 896 | 896 (477) |
| `new3_defer` (`--deferred`) 青 | 896 | 896 (477) |
| `new_hzb` 赤 | 51700 | 51700 (41284) |
| `new_bench` 黄 / 白 / 赤 | 9370 | 黄 6319 (5378) + 白 3049 (776) + 赤 1 = 全部 |
| `old_dist` (修正前) 青 | 898 | 239 (**α≥0.5 は 0**) = 659 画素が説明できない |
| `new_nav` (背景 = `_F` = 塗りだけ) | 22075 | 輪郭 16198 + 経路 2955 + 障害物 648 + 修飾 1242 + Link 162 ほか。説明できない 859 (4%) |
- 緑・赤が出ない撮影: 距離デモは `--cull-min-screen-size 0.02` でも画面サイズで落ちる物が無く緑は出ない (これは M99 の挙動で、直した物ではない。受け入れ条件 2 の組み合わせが緑を撮れない)。緑は `--render-bench-demo --cull-debug --cull-min-screen-size 0.02` で出る (上の表)。ベンチ (視錐台固定) の赤は WARP のこの撮影フレーム (30) ではオクルージョンの枠が 0 個。赤は `--hzb-debug-max` で撮れる。
- `new_benchsize` は線あり / 線なしで背景も違う画素が多い (変わった 56395、説明できない 49928) = 画面サイズカリングの判定が撮影ごとに揺れる (線の有無と無関係な、実行間の差。この撮影が決定的でないことの発見。M99 の既存挙動で、今回は直していない)。緑の判定には影響しない (線なし側が 0)。

**塗り (受け入れ条件 2 の最後)**: `nav` の床 (area 0 = (0,192,255)、α 0.38) の矩形 (560,445)-(660,470) で、期待する混合色 `0.62*背景 + 0.38*定数` との最大チャネル差の中央値は修正前 61.5 → 修正後 0.5 (別の矩形 (200,380)-(300,460) でも 62.2 → 0.5)。
画像: 修正前 `tests\golden\nav.png` (git の HEAD)、修正後 `tests\golden\nav.png` (更新後)、`S\new_nav.png` / `S\new_nav_F.png` / `S\new_nav_N.png`。修正前は淡く明るい水色、修正後は暗くて彩度のある水色 (見た目は大きく変わる)。

**壁の裏の線 (受け入れ条件 3)**: `occl.py` で線の画素集合 L_old (旧 golden と N の差) と L_new (新と N の差) を比べた。**新しく出た画素 (L_new − L_old) = 0**、旧にだけあった画素は 5 (nav) / 1 (perception) / 4 (bt) = α が極小の画素の丸め。nav は 141005 画素、bt は 149205 画素の集合で、遮蔽面の上に線が増えていない。

**golden (受け入れ条件 4)**: `tools\shot_verify.bat` を回し、動いたのは `flow_title` (既知、触らない) と `nav` / `perception` / `bt` の 3 枚だけ。差分の表 (`subset.py`、D = 旧 golden と新の通常撮影、L_old = 旧と N、L_new = 新と N、N は線と塗りを止めた新ビルドの撮影):
| golden | \|D\| | \|L_old\| | \|L_new\| | \|L_old ∪ L_new\| | D のうち外れた画素 |
|---|---|---|---|---|---|
| nav | 141005 | 141005 | 141000 | 141005 | **0** |
| perception | 1013 | 1013 | 1012 | 1013 | **0** |
| bt | 149205 | 149205 | 149201 | 149205 | **0** |
→ この 3 枚だけ golden を更新した (`tests\actual` のコピー。`--update` は全部撮り直すので使わず、`flow_title` を守った)。それ以外の 24 枚は PASS のまま。`demo_terrain_deferred` は PASS だが 2 画素が maxDiff 5 で違う (tol 12 以内): 新の通常撮影と N が同一 (線なし)・旧 golden との 2 画素差は今回の変更と無関係 (線も塗りも出ない)。
physics / joints / acoustic_* / fog / demo_* は動かなかった (線の既定が出ない)。

**検証 (受け入れ条件 5〜7)**: `tools\replay_verify.bat` PASS (Debug / Release ビルド込み。規則チェック含む)、`tools\cull_verify.bat` PASS (23 checks、`counts` は構成間で equal)、`tools\check_rules.ps1` 0 error、`MYE_SELFTEST_ONLY=render,renderstats,occlusion,navdeterminism,navsurface,navagent,perception,behaviortree,physics` の Debug `--selftest` は 9 suites 0 failed。凡例 (`Menu_CullDebugHint`) は黄 / 青 / 緑 / 赤で、定数 (0xFFD700 / 0x3399FF / 0x33CC33 / 0xFF3333) と合っているので文言は変えていない。engine_spec に §6.14 *Overlay lines and fill (M99k)* と DebugDrawLine の節 (物理デバッグ線の段落)・ナビの Display の 3 か所を書いた。

**一時プローブ**: `RenderSystem::DrawDebugOverlay` に環境変数で線と塗りを止める行を足して撮影し、撮影後に削除した (`grep PROBE-TEMP` で残りなし、`git status` に痕跡なし)。

### round 2 (FIX_REQUEST #1 #2。S = `...\scratchpad\s11\`)
判定式は新式 (線で変わった画素のうち、N の画素と定数色の α 混合の残差が各チャネル 3 以下の割合 ≥ 95%)。修正前は HEAD の exe を作業ツリーで再現して撮った (`git apply -R` で src を戻して Release ビルド → 撮影 → 再適用。`old2_nav.png` は HEAD の `tests\golden\nav.png` とハッシュ一致 = 修正前ビルドの確認)。撮影は全部 `--warp --no-audio --font-embedded 960x540`、`--no-fxaa` (FXAA 行だけ無し)。N は一時プローブ (撮影後に削除済み、`grep PROBE-TEMP` 0 件) で線と塗りを止めた撮影。ナビは背景に塗りが乗るので `_F` (塗りだけ、新旧それぞれ) を背景にした。表は `scratchpad\table.py` / `table2.py`。
| 色 (撮影) | 変わった画素 (新 / 旧) | 説明できた (新) | 割合 (新) | 説明できた (旧) | 割合 (旧) |
|---|---|---|---|---|---|
| 距離の青 (dist) | 896 / 898 | 896 | 100.0% | 239 | 26.6% |
| 距離の青 FXAA あり (distfxaa) | 896 / 963 | 896 | 100.0% | 285 | 29.6% |
| 画面サイズの緑 (ベンチ + `--cull-debug --no-occlusion --cull-min-screen-size 0.02`、benchgreen) | 18951 / 18960 | 18951 | 100.0% | 164 | 0.9% |
| 視錐台の黄 + 固定した視錐台の白 (bench。黄と白のどちらかで説明) | 9370 / 9384 | 9370 | 100.0% | 2608 | 27.8% |
| オクルージョンの赤 (hzb) | 51700 / 51684 | 51700 | 100.0% | 8213 | 15.9% |
| ナビの全線 (nav、背景 = 塗りだけ。輪郭 0x40E8FFFF + 経路 + 障害物 + 修飾 + Link) | 22075 / 22148 | 21216 | 96.1% | 867 | 3.9% |
| ナビの輪郭の色だけ (同上、候補 = kBoundaryColor のみ) | 22075 / 22148 | 16384 | 74.2% (残りは他の線色の画素) | 482 | 2.2% |
| 知覚の `debugLines` (perception、kSeen / kLastPos / kPredict) | 1012 / 1013 | 1012 | 100.0% | 23 | 2.3% |
- 黄だけ・白だけの内訳 (bench): 黄 6367 / 白 3080 (合計 9447 ≥ 9370 = 一部の画素は両方で説明できる)。旧は黄 53 / 白 2578。
- ナビの輪郭の色だけの 74.2% は、変わった画素に経路線などの別色が含まれるため。全線の候補を合わせると 96.1%。残り 3.9% は異色の線が重なった画素と見ている (未確認)。
- 距離デモで緑は 8 画素しか出ない (`--no-distance-culling --cull-min-screen-size 0.02`)。planner の指示どおりベンチへ代替 (`--no-occlusion` を足して、赤が混ざって単色モデルから外れるのを避けた)。`--cull-debug --cull-min-screen-size 0.02` だけのベンチでは赤 (`--cull-debug` 経由のオクルージョン) と緑が重なる画素が多く、緑 + 赤 + 黄で 90.1% (旧 0.8%) = 2 色の重なりは単色モデルの対象外。赤だけでは 79.5% / 旧 0.8%。
- 壁の裏の線 (#3 は round 1 のまま確認済み)。

**shot_verify の再実行 (指摘 #2)**: golden 3 枚更新後に `tools\shot_verify.bat` を再実行 → `flow_title` (既知、maxDiff 62 / 944 px) だけ FAIL、他は PASS (img-diff PASS 31 件)。`[FAIL] screenshot regression: 1 shot(s) differ`。

**round 1 の訂正**: 「`--render-bench-demo --cull-debug --cull-min-screen-size 0.02` の撮影が実行ごとに揺れる」と書いたのは誤り。同条件の N を 2 回撮ると画素一致 (0 差)。56395 画素の差は線ありと線なしの差で、赤 (オクルージョン) の枠が単色モデルの候補に入っていなかっただけ。sub-12 の出発点: 揺れの観測は無い (--warp)。実 GPU では撮っていない。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-11)。原因の切り分け (トーンマップ。最終 RT は UNORM で sRGB 取り違えは無い) と直し方 (`DrawDebugOverlay` を ResolvePost の後へ) は妥当。golden 3 枚の D ⊆ L_old ∪ L_new (外れ 0)、壁の裏の画素 (L_new − L_old = 0)、cull_verify / replay_verify / check_rules / selftest は確認。受け入れ条件 2 の式は planner の誤りとして変更済み (上の 2.)。must: (1) 新しい式で全色の表 (青・緑・黄・白・赤・ナビの輪郭・debugLines 1 色) を出す。現状は青と hzb の赤しか測っていない。(2) golden 更新後の `tools\shot_verify.bat` を再実行して flow_title 以外 PASS を観測する (受け入れ 4 の最後の段は未観測)。不安 3 (画面サイズ + 枠表示の撮影が実行ごとに違う) は sub-12 で切り分ける (sub-11 の範囲外)。
- round 2: VERDICT OK (planner、2026-11-11)。must #1: 新式の表で全色 ≥ 95% (青 100 / 青 FXAA 100 / 緑 100 / 黄 + 白 100 / hzb の赤 100 / ナビの全線 96.1 / 知覚 100)、修正前は 0.9〜29.6% で明確に低い。修正前の撮影は HEAD の src を一時的に戻したビルドで、`old2_nav.png` が HEAD の golden とハッシュ一致することで裏付け済み。作業ツリーは戻っている (git status は予定のファイルだけ、PROBE-TEMP 0 件)。must #2: golden 更新後の shot_verify を再実行し、flow_title 以外 PASS を観測。緑をベンチ + `--no-occlusion` で撮った代替は採用 (2 色の重なりは単色モデルの対象外という説明に矛盾なし)。ナビの輪郭の色だけで 74.2% なのは、経路などの別の色の線が混ざるためで、全線の候補で 96.1% なので条件を満たす。round 1 の不安 3 は coder の測り違いと訂正された (同条件の N 2 回で差 0)。sub-12 は縮小して残す (spec 17)。nit: PixelPerfect の低解像度経路と Editor の画面はユーザーの目視。
