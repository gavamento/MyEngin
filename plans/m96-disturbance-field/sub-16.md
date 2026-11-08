# sub-16: Forward のスカイ無しの背景に地面の霧とフロクセルを掛ける (背景だけの全画面パス、golden の更新)

- 依存: sub-15 (波 W2。spec §6)
- 状態: OK (W2 の統合待ち)
- 往復: 1


## 並列の注意 (波 W2)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): golden PNG の撮り直し (唯一、受け入れ 36 の条件つき)、新しい PS 1 本。RenderSystem の条件。コンポーネント・snapshot は変えない
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §2 #25 (ユーザー指示 2026-11-08: 「Forward でもスカイなしの時に霧を掛けて」、続く回答: フロクセルの背景も Deferred に揃える)、§7 R13 の取り消し。
sub-03 round 2 の実測 (`sub03r2_03_horizon_forward_t100`) では、スカイボックスの無いシーンの Forward で背景が clear 色のまま残り、
床の縁で霧が硬い線になった。Deferred は光パスの背景分岐 (`deferred_light.hlsl` の albedo.a < 0.5) で、地面の霧とフロクセルの両方を掛けている。

1. Forward のパスに「背景だけの全画面パス」を足す:
   - 不透明の後・透明の前に、全画面の三角形を 1 回描く。頂点の深度は far、深度テストは LESS_EQUAL、深度は書き込まない = 不透明が何も描かなかった画素 (clear のままの深度) にだけ乗る。リバース Z を使っているなら、比較の向きと far の値を合わせる (coder が確認する)
   - PS の出力 = clear 色に、**Deferred の背景分岐と同じ処理**を掛けた色:
     - 地面の霧: `GroundFogCompositeBackground` (sub-03。逆ビュー射影から視線を復元し、2000 m 先を面とする)。sub-03 で RenderSystem が張る固定スロット (b6 / b7 / t32 / t33 / s8) を読む
     - フロクセル: Deferred の背景分岐のフロクセル合成の式 (奥行きのスライス・受け渡し) を写す。束縛は Forward の既存のフロクセル SRV と CB (`forward_lit.hlsl` が読んでいるもの) を使う
     - 順序は Deferred と同じ (地面の霧 → フロクセル)
     - 解析フォグ (`FogComponent`) の背景の扱いは、今の Forward のまま変えない (依頼の外。Deferred と違っていても、このサブでは触らない)
   - sub-15 の局所ライトは同じ関数を通るので、自動で揃う
2. 描く条件 (どれか 1 つでも欠けたら、パスそのものを走らせない = 絵はビット不変):
   - Forward のパスで描くビュー
   - そのビューでスカイボックス (skybox / skybox_cubemap / skybox_panoramic) を描いていない
   - 霧のある GroundLayer (濃さチャネルあり、`fogDensity > 0`) が 1 枚以上ある、**または** そのビューでフロクセルが走る (`view.froxelSRV` があり、Forward が合成するフレーム)
3. 速度バッファへは書かない (背景は動かない扱い = 既存の clear と同じ)。MSAA・TAA・ビューポートの扱いは、既存の Forward の全画面の描画 (あれば) に合わせる
4. 変わる絵の洗い出し (受け入れ 36 (c)):
   - `ShowcaseScenes.cpp` の全デモと、`tools\shot_verify.bat` の golden 27 枚について、「Forward かつスカイ無しかつフロクセルが走る」(= 変わってよい) に当たるものを一覧にする。golden の候補は `fog` (`--fog-demo --froxel --particle-backend gpu`、Forward、`shot_verify.bat:257`)。スカイの有無は各デモの組み立てコードで確かめる
   - 当たる各シーンについて:
     - (1) パスを一時的に止めた (条件を偽にした) ビルドで撮った絵が、着手前 (sub-15 のコミットのビルド) とビット一致すること = 変化はこのパスだけが原因
     - (2) 着手前と後の差分の画素が、背景 (深度 far) の画素に限られること (差分画像と、深度が far の画素のマスクを比べる。マスクは一時コードで出してよい)
   - 一時コードは撮影後に戻す (コミットに残さない)
5. golden の更新 (受け入れ 36 (d)):
   - 4 の (1)(2) を両方満たした golden だけを `tools\shot_verify.bat --update` で撮り直す。差分画像を目視し、変化が背景の霧だけであることを SELF_EVAL に書いてから、撮り直した png をコミットに含める
   - (1)(2) を満たさない golden が動いたら、それは不具合。`--update` で塗り潰さず、原因を断定して直す (memory `golden-diff-triage`)
   - tol=0 のローカル限定の golden (`shot_verify.bat:65` の一覧) も同じ扱い
   - CI (WARP) と手元で絵が違いうる golden は、既存の tol の規約 (`shot_verify.bat` の注記) に従う。新しく tol を緩めない
6. デモでの確認には、sub-03 round 2 と同じ一時コード (カメラを層の中・地平線向き、スカイ無し) を使い、撮影後に戻す

## やらないこと (このサブでは)

- スカイが無いときに単色のスカイを自動で描く
- Forward の背景への解析フォグの扱いの変更
- Deferred の背景分岐の変更 (sub-03 で済み)

## 触る場所 (planner の見立て)

- `src\Engine\Renderer\Pipeline\ForwardPath.cpp` (不透明と透明の間。スカイを描いたかどうかの判定は、スカイを描く箇所から引く)
- 新しい小さな PS (例 `assets\shaders\forward_background_fog.hlsl`) と、全画面の VS (既存のものがあれば流用)
- `assets\shaders\deferred_light.hlsl` の背景分岐 (式を写すために読む。変えない)、`froxel_common.hlsli`、`ground_fog.hlsli`
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (条件の判定に「霧のある層があるか」「フロクセルが走るか」を渡す)
- `tests\golden\*.png` (4・5 の条件を満たしたものだけ)

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 36。

- 36:
  - (a) sub-03 round 2 と同じ地平線向きの場面で、Forward の背景に地面の霧が掛かる。Forward と Deferred を並べ、背景の領域の平均輝度の差を報告する (目視で同じ霧であること。トーンマップの違いなどで差が出るなら理由を書く)。フロクセルを on にした同じ場面でも、Forward と Deferred を 1 組並べる
  - (b) **差 0 であるべき絵** (着手前 = sub-15 のコミットのビルドと `--img-diff --tol 0`):
    - Deferred の全部
    - スカイのある Forward の全部 (`--field-demo` にはスカイが無いので含まない。round 1 で訂正)
    - フロクセルも地面の霧も無い Forward の全部 (golden の `demo_forward` / `demo_forward_fxaa` / `demo_render_forward` / `acoustic_forward` など)
    - 少なくとも、sub-03 で比べた 4 デモ × Forward / Deferred の 8 組と、`shot_verify.bat` のうち (c) に当たらない golden 全部 (`shot_verify.bat` が PASS = golden と一致)
  - (c) **変わってよい絵**の一覧と、各々の上の 4 (1)(2) の証拠 (前後と差分の画像)
  - (d) golden を撮り直した場合は、その一覧と差分画像、目視の結果。撮り直した後に `tools\shot_verify.bat` が PASS
  - (e) 霧の後ろにある透明物 (粒子か VFX) が、背景の霧の上に正しく重なる (スクショ)
  - (f) 1920×1080 でのパスの GPU 時間 (地面の霧だけ / フロクセルだけ / 両方)
- 12: replay_verify `field` (描画だけの変更なので sim は不変。運用によりまとめて実行)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。`tools\shot_verify.bat` は golden の確認なので、このサブで実行する。

- ビルド: Debug / Release
- スクショ一式 (Runtime.exe、Forward と Deferred)、`Editor.exe --img-diff`
- `tools\shot_verify.bat` (golden を撮り直す前と後)
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-16 (round 1)

実装:
- assets\shaders\forward_background_fog.hlsl (新規): 全画面三角形 (z=1) と PS。PS は deferred_light.hlsl の背景分岐と同じ式 (GroundFogCompositeBackground -> フロクセル SampleLevel(FroxelSampleWFar))。CB b3 に clear 色 / フロクセル有効・スライス数 / 画面実寸。t7 / s2 はスカイと同じくホストが張ったものを読む。解析フォグは掛けない
- src\Engine\Renderer\Passes\SkyboxPass.cpp/.h: NeedsBackgroundFog (static、純粋関数: skyMode < 0 かつ (FroxelIsBound または 層に霧あり = look.w >= 0.5 && look.x > 0。ground_fog.hlsli の積分条件と同じ)) と RenderBackgroundFog (深度 LESS_EQUAL・書き込み無し・不透明ブレンド、SkyboxPass の状態を共有)。VS・深度状態・ブレンドを新設しないため SkyboxPass に置いた
- src\Engine\Renderer\Pipeline\ForwardPath.cpp: skybox_.Render の直後 (水面・透明の前、!wire の中) に skybox_.RenderBackgroundFog を 1 行。リバース Z は使っていない (スカイと同じ z=1 / LESS_EQUAL / clear 深度 1.0)
- src\Engine\Engine\Disturbance\DisturbanceFieldSelfTest.cpp: 自分の static 関数 RunBackgroundFogConditionTests (条件の 7 検査。スカイあり / 波紋だけの層 / 無効な層 / 層 0 枚 / 範囲外の層数) を RunDisturbanceFieldSelfTest の末尾から 1 行で呼ぶ
- tools\check_rules.ps1: 規則 9 の froxel t7 の対応表に新 PS を 1 行足した (新しい規則ではない。番号は変えていない)
- tests\golden\fog.png: 撮り直し (下)
- assets\shaders\forward_background_fog.hlsl.meta: エンジンが自動生成した guid (他のシェーダと同じく追跡対象)

仕様との差分:
- [逸脱] sub の「(b) 差 0 であるべき絵: スカイのある Forward の全部 (--field-demo を含む)」は事実と違う。--field-demo にスカイは無い (SkyboxComponent を置くデモは --render-demo だけ)。--field-demo の Forward は「スカイ無し + 霧のある層」なので (c) の変わってよい絵に入る。(b) からは外し (c) へ入れた。Deferred の --field-demo は差 0 を確認
- [追加] 新 PS の置き場として SkyboxPass にメソッドを足した (sub の見立ては ForwardPath.cpp + 新 PS。ForwardPath に深度状態などを増やさないため)。SkyboxPass.h の公開面が 2 つ増える
- [追加] check_rules.ps1 規則 9 の対応表に 1 行 (上記)
- [追加] .hlsl.meta (自動生成)
- [未実装] 速度バッファへ書かない・MSAA・TAA: Forward は TAA を持たず (TAA は Deferred のみ) RTV 1 枚 (view.rtv) に描くスカイと同じ経路なので追加の処理は無い
- 解析フォグ (FogComponent) の背景の扱いは変えていない (依頼の外)

検証:
- Release / Debug ビルド (MSBuild、MyEngine.sln) -> error 0。Debug の警告は既存 (AnimatorControllerSelfTest C4456 / libtess2 C4267) のみで、触ったファイルから新規の警告なし
- pwsh tools\check_rules.ps1 -> 0 error / 59 warning (rule 7 / 17 の既存)。初回は規則 9 の ERROR (自分の追記の \f 文字化け) -> 直して再実行で 0
- 受け入れ 36(b)/(c) の測り方: 着手前 (bc957ea) を別 worktree でビルドし、スクリプト (.ps1 / numpy) で撮影・比較。一時コード (環境変数でパス OFF / マスク色出力 / 地平線カメラ / GPU タイマ) は撮影後にすべて戻した (git diff に TMP-SUB16 なし。ビルドし直して shot_verify)
- (1) パスを止めた (MYE_TMP_BGFOG=off) ビルド vs 着手前: fog / field / fieldh (地平線) / fieldfx (--froxel) / fieldhfx の 5 場面すべて diffPixels=0 maxDiff=0 (= 変化はこのパスだけが原因)
- (2) 着手前 vs 新: 変わった画素 = fog 231715 (maxDiff 11) / field 7991 (202) / fieldh 50880 (223) / fieldfx 67564 (145) / fieldhfx 296437 (210)。クリア色を magenta にして出したマスク (パスが塗る画素) の外で変わった画素は 5 場面とも 0
- (b) 着手前 vs 新 (frame 60、Forward / Deferred): --fog-demo / --render-demo / --particle-demo / --terrain-demo の 8 組すべて maxDiff=0 diffPixels=0 (numpy で全画素比較。--render-demo の Forward はスカイあり = パスが走らない)。--field-demo の Deferred も 0。--field-demo の Forward だけ 7991 画素が変わる (上の (c))
- (a) 地平線向き (カメラ y=0.6、仰角 4 度、スカイ無し) の背景領域 (297556 画素) の平均輝度 (Rec.709): Forward 着手前 19.43 -> 新 44.70、Deferred 44.70 (着手前も同じ)。Forward 新と Deferred の背景の差は平均 0.0002・最大 1 (3 超え 0 画素)。フロクセル on (--froxel) の同じ場面: Forward 着手前 18.49 -> 新 50.82、Deferred 50.82 で差は 0 (全画素一致)。トーンマップ等の差は出なかった。目視でも床の縁の硬い線が消え、層の上端で柔らかく消える帯が背景に続く
- (c) 変わってよい絵の一覧: golden では fog (--fog-demo --froxel --particle-backend gpu、Forward) の 1 枚だけ。デモでは --field-demo の Forward (フロクセルなしは霧のある領域だけ 7991 画素、--froxel 付きは背景ほぼ全面)。ほかの Forward (demo_forward / demo_forward_fxaa / demo_render_forward / acoustic_forward / physics / joints / particle_cpu / particle_gpu など) は霧もフロクセルも無いか、スカイがあり、shot_verify が golden と一致
- (d) golden の撮り直し: tools\shot_verify.bat --update は全 golden を書き直すので、fog.png 以外 (demo_terrain_deferred.png / flow_title.png が動いた) は git checkout で戻した。撮り直した golden は fog.png の 1 枚だけ。差分画像の目視: 変わるのは背景 (地平線より上) の全画素で、縦に帯 (背景のフロクセル最遠スライスの高さ依存) が付き、手前の柱・床・粒子・FOG の文字は 0。差は最大 11。撮り直し後 shot_verify.bat -> 31 枚 PASS、1 枚 FAIL (flow_title、下記・着手前と同一)
- flow_title の FAIL は着手前 (bc957ea の別 worktree でビルドした exe) の shot_verify でも同じ値で FAIL (maxDiff=62 diffPixels=944、画面下の「D-Pad / Arrows ...」の文字の明るさだけ。背景ではない)。このサブとは無関係な既存の FAIL。golden は触っていない
- (e) 透明物の重なり: fog golden の粒子 (GPU) / Sprite・Trail / TextMesh の「FOG」を背景の霧の上へ重ねた画像で確認 (前後と差分 x20)。粒子の下の背景だけが霧色に変わり、粒子の輪郭・色は壊れない。文字と柱は差 0
- (f) 1920x1080 の背景パスの GPU 時間 (Release、RTX 3060、TIMESTAMP クエリ、40 フレーム後から 500 フレーム平均、パス本体の Draw のみ): 地面の霧だけ (--field-demo、既定カメラ) 0.050 ms / 地面の霧だけ・地平線カメラ (背景が画面の約 57%) 0.155 ms / フロクセルだけ (--fog-demo --froxel) 0.027 ms / 両方 (--field-demo --froxel) 0.050 ms、地平線カメラで 0.159 ms
- 画像: C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\14d68bbd-02de-486d-b73d-7903e06a5ea3\scratchpad\m96-shots\ の sub16_*.png (fog / field / fieldh / fieldfx / fieldhfx それぞれ _base (着手前) / _off (パス停止) / _on (新) / _mask / _diff、fieldh(fx)_def_base / _def_on、b_*_base / b_*_new、sub16_golden_fog_old / _new / _diffx20、sub16_e_particles_over_bg_before_after_diffx20)
- selftest / replay_verify -> 未実行 (運用。selftest のコードは書いた。W2 統合後にまとめて実行)

自己採点 (1-5):
  仕様適合: 4 — 条件・式・順序・golden の扱いは仕様どおりで (1)(2) を実測。(b) の「--field-demo はスカイあり」の前提が誤りだったので (c) に移した。(f) は 1 台の GPU のみ
  正しさ: 4 — 全画素比較で着手前と一致 / 背景以外は 0。Forward と Deferred が背景でほぼ同一。selftest の条件テストは未実行、Debug の絵は未撮影 (Release のみ)
  コード品質: 4 — 既存のスカイの流儀に合わせた最小の追加。SkyboxPass の責務が「背景の描画」へ少し広がった
  テスト: 3 — 純粋関数の条件テストを書いた (未実行)。GPU の絵は shot_verify (fog golden) と実測の比較で固定

不安・質問:
- (質問 1) flow_title golden の既存 FAIL (着手前から同一) の扱い: このサブの範囲外として触っていない。司会 / planner が別サブか台帳の申し送りへ
- (質問 2) 解析フォグ (FogComponent) が有効な Forward でスカイ無しのとき、背景は解析フォグを受けず (既存)、今回の地面の霧 / フロクセルだけ掛かる。Deferred と同じ構造なので揃っているが、「解析フォグ有り・スカイ無し・フロクセル無し・層無し」は引き続き素の clear 色。仕様どおり (依頼の外)
- (質問 3) SkyboxPass にメソッドを足した設計でよいか (ForwardPath 内に持つ案もある)。差し戻しなら移す

触ったファイル:
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\assets\shaders\forward_background_fog.hlsl (新規)
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\assets\shaders\forward_background_fog.hlsl.meta (新規。自動生成)
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\src\Engine\Renderer\Passes\SkyboxPass.h
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\src\Engine\Renderer\Passes\SkyboxPass.cpp
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\src\Engine\Renderer\Pipeline\ForwardPath.cpp
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\src\Engine\Engine\Disturbance\DisturbanceFieldSelfTest.cpp
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\tools\check_rules.ps1
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\tests\golden\fog.png
- C:\HAL\MyEngin\.claude\worktrees\w2-sub16\plans\m96-disturbance-field\sub-16.md (実装メモ)

申し送り:
- sub-17 以降へ: Forward のスカイ無し背景は、この新 PS が Deferred の背景分岐と同じ式で描く。式を変えるときは ground_fog.hlsli の GroundFogCompositeBackground を直せば両方揃う。フロクセルの背景の式 (最遠スライス) を変えるなら deferred_light.hlsl と forward_background_fog.hlsl の 2 か所
- 局所ライト (sub-15) は GroundFogCompositeBackground を通るので背景にも自動で乗る (撮影はしていない)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-09)。パスを止めたビルドが着手前と 5 場面ビット一致、変わった画素は 5 場面ともマスク (背景) の内側だけ、(b) の 8 組 + field の Deferred が差 0、Forward と Deferred の背景が平均差 0.0002 / froxel 付きは 0、golden は fog.png 1 枚だけ撮り直し。SkyboxPass に置いた設計と、--field-demo を (c) へ移した訂正を受理。flow_title の FAIL は着手前から同じ既存の別件
