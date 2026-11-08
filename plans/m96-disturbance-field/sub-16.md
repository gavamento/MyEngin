# sub-16: Forward のスカイ無しの背景に地面の霧とフロクセルを掛ける (背景だけの全画面パス、golden の更新)

- 依存: sub-15 (波 W2。spec §6)
- 状態: 未着手
- 往復: 0


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
    - スカイのある Forward の全部 (`--field-demo` を含む)
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

## フィードバック履歴
