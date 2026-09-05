# sub-07: 仕上げ — ADR-016 / engine_spec §6.4 / README / CLAUDE.md / ReSTIR on の golden / 全検証

- 依存: sub-06
- 状態: OK (commit abeb851)
- 往復: 1

## やること

元計画 S6。コードの変更は golden 1 枚と文書のみ。**既定値は触らない** (S5 はユーザーが harness の外で回す)。

1. `docs\adr\ADR-016-restir-reflection.md` (ADR-015 の書式): 決定 = reconnection 方式 / クラスは受け側 G-Buffer
   ではなくヒット側 `RtInstance` / v1 は可視レイ既定 off (biased) / **temporal も厳密 Jacobian (受け側位置 `rpos` を
   reservoir に保存。ユーザー判断 U4 — planner の J=1 近似案は却下、理由ごと記録)** / 2 パス (typed UAV load
   回避) / 保存量は W / spatial の候補クラスで半径を制限する理由 / 却下した案 (RT3.a、3 パス、temporal J=1 近似、
   unbiased MIS)。出所は `plans/m67-restir-reflection/spec.md` §2。
2. `engine_spec.md` §6.4: 表に ReSTIR の段 (再利用の入力・出力の次元は不変)、「Acceptance criterion」の段落に
   「ReSTIR off はビット一致 = golden `demo_render_rtrefl` / `demo_render_rtgi`」、ReflectionClass の 5 段と
   Material の出所、既知制限 (skinned は BVH 外 = 主役がスキンなら映らない)。§13 の ADR 一覧に 016。
   ★sub-01 の申し送り: `engine_spec.md:1711` の「captures **fifteen** deterministic screenshots」は 19 の時点で
   既に古い → 22 に。`:403` の「the RT demo is too slow under WARP, so the feature would carry permanently zero
   automated coverage」は前提が半分崩れた (反射 / GI は golden を得た。RT 影は依然ゼロ) → 文言を実態に合わせる。
   ★sub-02 の申し送り (spec §2 S15): `engine_spec.md` §10.2 の Invalidation 行に「`kCookVersion` = 2 since M67b —
   `Material` grew 56 → 64 (reflectionClass + explicit pad); any `Material` layout change bumps it because the blob
   memcpys the struct」を 1 文。Sealed bundle 行に「packages are rebuilt with the new exe (exe と cache は一緒に配る)」。
   ADR-016 には書かない (設計判断ではなく機械的帰結)。
   ★sub-04 の申し送り: engine_spec の RT デバッグモード表を 12 (reservoir M) / 13 (一次ヒットのクラス) / 14 (反射像側の
   クラス) で更新。ADR-016 に載せる reservoir のメモリ = 480×270 で 1 スロット約 14.6 MB (5 枚 × 2 組 × 48 B/px)、
   エディタで SceneView + GameView なら約 29 MB (viewKey 別に遅延確保)。新規 .hlsli は 3 本
   (`rt_restir_common` / `rt_restir_cb` / `rt_reproject`) + CS 1 本 (`rt_refl_restir_spatial`)。
   ReSTIR on の golden は tol=0 (自身が基準。A5 の 1 ulp は off との比較にしか出ない)。
   ★sub-05 の申し送り: `[rt]` restir は 1.5 (M67d) → 2.4 ms (M67e、WARP render-demo frames 20)。ADR の数字は sub-06 の
   spatial 込みで取り直す。
   ★sub-06 の確定事項 (round 2、spec §4.3 / §7 U7): **spatial の既定は off**。golden `demo_render_rtrefl_restir` は
   **既定 = spatial off、frame 40** で撮る (= temporal のみの絵。`--rt-restir` 以外のフラグは足さない)。
   ADR-016 に載せる決定 4 件と実測: (a) **spatial は reservoir を書き戻さない** (書き戻すと Prop が 40 フレームで
   9988 → 40432 px を占拠、平均輝度 +8.4%、一様クラスでもフリッカー 0.15 → 0.26〜0.45)、(b) **半径は受け側 α に比例**
   (`kRtRestirRadiusAlphaRef = 0.36`、1 px 未満はタップ 0 = 鏡面では自然に切れる。鏡面パッチ 0.198 ≤ temporal 0.206)、
   (c) **タップ回転はフレームで回さない** (回すと乗り換えフリッカー 1.21 → 0.61 の 2 倍)、(d) **spatial 既定 off の理由** =
   目標帯 (音響デモの床、粗さ 0.5、反射レーン単体) で temporal 単独 0.181 に対し spatial on 0.255 (MIS 重み無しの biased
   合成では近傍の p̂ 比が重みの分散になる。unbiased 化は spec §3 の外)。**S5 で最初に触るノブは M 上限** (spatial off +
   一様 Prop (32) が既定混在の 1.8 倍良い: 床全体 0.100 vs 0.181)。
   CLI 一覧: `--rt-restir` / `--rt-restir-spatial` / `--rt-restir-no-spatial` / `--rt-restir-visray` (`--rt-restir` と
   `--rt-restir-spatial` を含意) / `--rt-class-override N` (ReSTIR と独立、デバッグ 13 にも効く)。
   GPU 時間 (WARP / Release / frames 20、render-demo): refl 6.520 / denoise 23.327 / **restir 2.320 ms** (既定)、
   spatial on 2.826、visray 6.166。reservoir は 480×270 で 1 スロット約 14.6 MB (5 枚 × 2 組 × 48 B/px、ping-pong)。
   engine_spec §6.4 の表の ReSTIR 段も「temporal reuse (class-capped M), spatial reuse off by default」の文言に。ADR の「既知の制限」に「ReSTIR トグルで SVGF 履歴は落ちない (数フレームで収束)」と
   「W クランプは入れていない (実測で兆候なし。firefly が出たら `kRtRestirWMax`)」を載せる。
3. `README.md`: 機能概要のレイトレ節に ReSTIR 反射 + ReflectionClass を 1 段落、CLI 一覧に
   `--rt-restir` / `--rt-restir-no-spatial` / `--rt-restir-visray` / `--rt-class-override N`。
4. `CLAUDE.md`: CLI 一覧 (`--rt-*` の並び) に同 4 本 **+ 元から未掲載だった `--rt-refl` / `--rt-gi` / `--rt-shadow` /
   `--rt-debug N` / `--rt-no-temporal` / `--rt-no-svgf` / `--rt-freeze-seed` / `--rt-anim-seed`** (sub-01 の申し送り。
   受理フラグは `EditorMain.cpp:299-346` / `RuntimeMain.cpp:297-342`)、検証表の `shot_verify.bat` 行を 22 枚に、
   「横断的な変更のチェックリスト」に「**Material にフィールドを足す** — `ParseMaterialJson` / Inspector の
   load・save・widget / `CreateMaterialAsset` の雛形 / `AssetOpsSelfTest` の 4 者 **+ cooked blob は struct を memcpy
   するので `kCookVersion` bump / `ModelCook.cpp` の `static_assert` / `AssetID` (8 バイト境界) で丸まる分は明示パディング
   (暗黙パディングは cooked ファイルのバイト列を run ごとに変える)**」を 1 項 (M67b で踏んだ手順の固定、spec §2 S15)、
   「環境の罠」に「**`GpuTimer` は kFrames=6 のリングを 7 フレーム目からしか回収しない** — `--frames 6` の撮影 run では
   `[rt]` / `[ssr]` の GPU 時間が全部 0.000 ms。計測は `--frames 20`」を 1 項 (sub-01 で踏んだ罠の固定)。
5. `tools\shot_verify.bat` に `demo_render_rtrefl_restir --render-demo --deferred --rt-refl --rt-restir` を
   `MYE_SHOT_SKIP_RT` の囲いの中に追加 → `--update` で撮り、**中身を見てから**コミット。
   bat の数え書きと CLAUDE.md を 22 に。
   ★**frame 40 で撮る** (`--frames 41 --shot-frame 40`。sub-05 で決定、spec A12): Default の M 上限 16 と Prop の 32 が
   両方飽和した状態を固定する (frame 3 では M ≈ 4 でクラス別上限が写らない)。SHOTBASE は frames 6 / shot-frame 3 なので、
   physics / acoustic の frame 120 枠と同じ方法で `SHOT` を差し替えて撮る (bat 220-240 行付近の書き方を踏襲)。
   撮影条件の注記を bat のコメントに (「凍結シードなので temporal は同一サンプルを積む = M は伸びるが推定値は不変。
   spatial は画素間で違うので写る」)。CLAUDE.md の「frame 120 で撮る 7 枚」の注記に「ReSTIR の 1 枚は frame 40」を足す。
   所要時間の目安: acoustic の frame 120 が 19 s だったので 30 s 以内。
6. `plans\m67-restir-reflection\harness.md` の申し送りに S5 の手順 (spec §4.6) と `M67h` で焼く項目を書く
   (司会が転記してもよい)。
7. 全検証 (下記)。

8. 衛生 (コメントの実態合わせ、M66l と同型): `src\Engine\Engine\Asset\CookedCache.h:20` の「56 → 60 バイトになった」は
   `ModelCook.cpp:19` の `static_assert(sizeof(Material) == 64)` と食い違う → 「56 → 64 (60 + `AssetID` 境界の明示
   パディング 4)」に直す (sub-02 round 1 の should)。

## やらないこと (このサブでは)

- 既定パラメータの変更。シェーダ / C++ のロジック変更 (指摘があれば差し戻しで前サブへ。8 のコメント修正だけは例外)。

## 触る場所 (planner の見立て)

- `docs\adr\ADR-016-restir-reflection.md` (新規) / `engine_spec.md` (354-394、2042-2065) / `README.md` (125-135、185-200) /
  `CLAUDE.md` (39、58-63、CLI 一覧 90-130 付近、チェックリスト 133-150 付近)
- `tools\shot_verify.bat` / `tests\golden\demo_render_rtrefl_restir.png` (新規)

## 受け入れ条件 (このサブ)

1. `tools\shot_verify.bat` **22 枚**全緑 (A1 / A12)、`MYE_SHOT_SKIP_RT=1` で 19 枚緑。
2. `tools\replay_verify.bat` 全緑 (A10。sim 非接触の機械確認。ビルド込みで長い — 背景実行)。
3. `Editor.exe --selftest` 緑、`check_rules.ps1` 緑。
4. `demo_render_rtrefl_restir.png` を 2 回撮って `--tol 0` PASS (A14)。
5. ADR-016 の各決定に spec §2 の S 番号が引けること。engine_spec §6.4 の表と本文が矛盾しないこと
   (reviewer が読む)。
6. CLAUDE.md / README の CLI 一覧と `EditorMain.cpp` / `RuntimeMain.cpp` の受理フラグが一致。

## 検証コマンド

```
"%MSBUILD%" MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
"%MSBUILD%" MyEngine.sln /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
tools\shot_verify.bat --update   (新 1 枚のみ動くこと。他 21 枚が動いたら --update せず報告)
tools\shot_verify.bat
set MYE_SHOT_SKIP_RT=1 && tools\shot_verify.bat
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

### round 1

```
SELF_EVAL: sub-07 (round 1)
実装:
  - docs\adr\ADR-016-restir-reflection.md (新規) — ADR-015 の書式。決定 10 件 / 理由 10 節 /
    帰結 3 節。却下した案は RT3.a (受け側 G-Buffer) / 3 パス / temporal J=1 近似 (U4) /
    unbiased MIS / spatial の書き戻しに対する代案 (A)(B)(D)。既知の制限 6 件。
    実測は spatial 書き戻しの 3 症状表 / 目標帯のフリッカー表 / GPU 時間表 / reservoir 容量。
  - engine_spec.md §6.4 — レーン表に ReSTIR の段 (「1spp の推定を差し替える。次元は不変」+
    「temporal reuse (class-capped M) / spatial は既定 off」)、Acceptance criterion に
    golden 3 枚での機械証明、本文に ReSTIR / ReflectionClass の 6 箇条
    (reservoir の中身 / クラスはヒット側 / 2 パス + 書き戻し無し / M=0 は 1spp を通す /
    デバッグ 12・13・14 / チューニング UI / 既知の v1 制限)。
  - engine_spec.md §10.2 — Invalidation 行に `kCookVersion` = 2 の版記述 (56 → 64、明示パディング、
    `static_assert` が門番)、Sealed bundle 行に「版が動いたらパッケージは新 exe で作り直す」。
  - engine_spec.md §13 — ADR 一覧に **ADR-016** (§6.4)。
  - engine_spec.md §6.5 — 非目標表の RT 影のセル: 「RT は WARP で重すぎるので永久にゼロ被覆」の
    前提が M67a で半分崩れた (重いのは `--rt-demo`。反射 / GI は golden を得た。RT 影は依然ゼロ) を反映。
  - engine_spec.md §11.3 — 枚数の実態合わせ (fifteen → 22、CI 判定 12 / ローカル限定 10、
    SKIP 変数 7 本、frame 3 以外は 8 枚) + RT 3 枚の段落を新設 (存在理由 / 「WARP で重い」の
    前提崩れ / ReSTIR が frame 40 な理由 / tol=0 ローカル限定の理由)。
  - README.md — 主要機能に「ReSTIR 反射 + ReflectionClass」1 段落、CLI 一覧に
    `--render-demo --deferred --rt-refl --rt-restir` と ReSTIR 系 4 本の説明、
    スクショ枚数 15 → 22、CI 環境変数表の `MYE_SHOT_SKIP_*` を 7 本 / 10 枚に。
  - CLAUDE.md — 検証表を 22 枚 + 「ReSTIR の 1 枚だけ frame 40」注記、CI SKIP を 10 枚 /
    20〜22 枚目、CLI 一覧に `--rt-*` 8 本 (元から未掲載) + ReSTIR 5 本、
    チェックリストに「**`Material` にフィールドを足す**」(4 者 + cook 版 / static_assert /
    明示パディングの 3 項)、環境の罠に「**`GpuTimer` は 7 フレーム目からしか回収しない**」。
  - tools\shot_verify.bat — 22 枚目 `demo_render_rtrefl_restir --render-demo --deferred
    --rt-refl --rt-restir` を `MYE_SHOT_SKIP_RT` の囲いの中に追加。`SHOT` を
    `--frames 41 --shot-frame 40` に差し替えて撮り、直後に `%SHOTBASE% --no-fxaa` へ戻す
    (physics / acoustic の frame 120 枠と同じ書き方)。frame 40 の理由・凍結シードの注記・
    「自身が基準なので tol=0」「M67h で撮り直す」をコメントに。数え書き (21 → 22 本)、
    tol=0 の枚数 (9 → 10 枚)、`skip_rt` の説明 (2 枚 → 3 枚)、PASS 行を更新。
  - tests\golden\demo_render_rtrefl_restir.png (新規、960x540)。
  - .github\workflows\ci.yml — `MYE_SHOT_SKIP_RT` の説明コメントを 20〜22 枚目に (コメントのみ)。
  - src\Engine\Engine\Asset\CookedCache.h:20 — 「56 → 60 バイト」を「**56 → 64**
    (60 + `AssetID` 境界の明示パディング 4)」へ。暗黙パディングを禁じる理由も 2 行 (sub-02 should)。
  - assets\shaders\rt_refl_restir_spatial.cs.hlsl — `RtRestirClampM` の直前に
    「**書き戻しが無い今この 1 行は出力に効かない**。CPU ミラーの 2 パス往復テストと形を
    揃えるために置いている」を明記 (sub-06 should)。
仕様との差分:
  - [逸脱] golden の記録に `shot_verify.bat --update` を使っていない。比較 run が撮った
    `tests\actual\demo_render_rtrefl_restir.png` (bat の `:shot` が組んだ撮影条件そのもの) を
    `tests\golden\` へコピーした。理由: `--update` は 22 枚全部を上書きするので
    「他 21 枚が動いていない」の証拠が「上書き後のバイトが同じ」に弱まる。コピーなら 21 枚の
    ファイルに一切触れないので `git status` が直接の証拠になる (実際 `?? tests/golden/
    demo_render_rtrefl_restir.png` の 1 行だけ)。**その後の 22 枚比較 run で新 golden も
    tol=0 PASS** = 別 run の撮影と一致することも確認済み (= A14 の 2 回撮影に相当)。
  - [追加] engine_spec に「RT デバッグモード表」は**存在しなかった** (sub-04 の申し送りは
    表があると想定していた)。§6.4 の新しい箇条書きの 1 項として 12 / 13 / 14 を書いた。
  - [追加] engine_spec §11.3 の枚数記述は "fifteen" 以外にも古かった
    ("Ten of the fifteen gate CI; the other five" / SKIP 変数 4 本 / frame 3 以外は 3 枚)。
    fifteen だけ 22 にすると段落内で矛盾するので、数えを実態 (12 / 10 / 7 本 / 8 枚) に揃えた。
    同じ理由で README の「スクショ 15 枚」と CI 環境変数表の SKIP 4 本も更新。
  - [追加] ci.yml のコメント (`MYE_SHOT_SKIP_RT` = 20/21 枚目) を 20〜22 枚目に。囲い自体は
    M67a で登録済みなので**挙動は変わらない**が、放置すると M63a が踏んだ「囲いを作って
    ここへの登録を忘れる」と同型の食い違いになる。
  - [追加] ADR / engine_spec の reservoir 容量は **56 B/px × 2 組**で書いた。
    spec §4.2 の「48 B/px × 2 組 / 1600×900 で約 35 MB」は算術が合わない —
    実体は pos 16 + rad 8 + nrm 8 + geom 8 + rpos 16 = **56 B/px**
    (`RtPasses::EnsureReservoirs` のフォーマット)。480×270 で 129,600 px × 112 B = **14.5 MB**
    (台帳の「14.6 MB/スロット」と一致)、800×450 で **40.3 MB** (spec の 35 MB は 48 B/px の値)。
  - [逸脱] 「やること 6」の `harness.md` への申し送り追記を**していない**。司会から
    「作業ツリーの台帳更新 (未コミット) には触らないこと」と明示指示があったため。
    sub-07.md 自身が「(司会が転記してもよい)」としているので、S5 の手順と M67h で焼く項目は
    下の「申し送り」に全文を置く。
  - [未実装] なし (既定値・シェーダ / C++ のロジックは 1 行も触っていない。変更はコメント 2 か所のみ)。
検証:
  - `MSBuild MyEngine.sln /p:Configuration=Debug|Release /p:Platform=x64 /m` → 両方 exit 0
  - `cmd /c bin\x64\Debug\Editor.exe --selftest` → exit 0 (全スイート PASS)
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\shot_verify.bat` (新 1 枚を足す**前**の状態で 22 本走らせた) → 既存 21 枚すべて
    `maxDiff=0`、新 1 枚だけ "no golden image" で FAIL = **既存 golden は 1 枚も動いていない**
  - `tools\shot_verify.bat` (golden 記録後) → **22 枚 PASS、全枚 maxDiff=0**。
    新 golden `demo_render_rtrefl_restir` も別 run の撮影と tol=0 一致 (A14 / 受け入れ条件 4)
  - `set MYE_SHOT_SKIP_RT=1 && tools\shot_verify.bat` → **19 枚 PASS**
  - `tools\replay_verify.bat` → **[PASS]** (8 ビルド → 並列 10 ジョブ: demo / parts / flow / mp /
    physics / joints / acoustic の 7 ペア + snapshot 往復 + タイムトラベル ×2 + 規則検査。
    最長ジョブ joints 102.1s)  ← A10
  - `tools\shot_verify.bat` 再走 (replay_verify が焼き直した Release バイナリで) → **22 枚 PASS**
    (CookedCache.h のコメント変更を含むビルドでも golden が動かないことの確認)
  - `tools\shot_verify.bat` 3 走目 (bat の rem 折り返しを直した**最終ツリーの状態**で) →
    **22 枚 PASS** (bat を後から触った分の未検証を残さないため)
  - ReSTIR が golden に実際に写っているかの直接確認: 同条件 (frame 40) を `--rt-restir` 抜きで
    撮って差分 → `maxDiff=93 / diffPixels=9723`。`[rt]` ログも golden 撮影 run が
    `restir 2.414 ms`、off 側が `restir 0.000 ms`
  - `git status` → 新規は `docs/adr/ADR-016-restir-reflection.md` と
    `tests/golden/demo_render_rtrefl_restir.png` の 2 つだけ。**既存 golden 21 枚は未変更**
自己採点 (1-5):
  仕様適合: 4 — sub-07 の 8 項目すべてに手を入れ、受け入れ条件 1〜4・6 を実測で満たした。
    5 にしないのは (a) golden の記録方法が指示の `--update` と違う ([逸脱] 1 件)、
    (b) `harness.md` への追記を司会指示との衝突で見送った ([逸脱] 1 件)、
    (c) 受け入れ条件 5 (ADR と engine_spec の内容の整合) は reviewer が読んで判定する項目で
    自分では緑を主張できない、の 3 点。
  正しさ: 5 — 文書変更なので「正しさ」= 記述と実装の一致。ADR / engine_spec に書いた数値は
    コードから引き直して確認した (`RtTypes.h` の定数表・既定値、`EnsureReservoirs` の
    フォーマット、両 main の受理フラグ、`ModelCook.cpp` の `static_assert`)。
    spec の 48 B/px が合わないことも実物で見つけて訂正した。唯一のコード変更 (コメント 2 か所) は
    22 枚 golden + replay_verify + selftest で無影響を確認済み。
  コード品質: 4 — bat の追加は既存の frame 120 枠の書き方を踏襲し、コメントに
    「なぜ frame 40 か」「凍結シードで何が写り何が写らないか」を残した。日本語コメント規約に準拠。
    5 にしないのは engine_spec §11.3 の古い記述を全部は直しきっていない
    ("all fourteen other shots would stay green" のような歴史的記述は残した)。
  テスト: 4 — 新規テストコードは足していない (このサブの成果物は文書と golden 1 枚で、
    CLAUDE.md がテスト追加を要求する領域 = ECS / シリアライズ / リプレイ / アセット /
    ローカライズ / レンダラ定数 / ホットリロード のいずれにも触れていない)。
    代わりに golden 1 枚が**機械的な回帰**として増えた (ReSTIR の絵はこれ以前どこにも
    固定されていなかった) ことと、22 枚 + 19 枚 + replay の 3 本を全部回したことで 4。
不安・質問:
  1. `spec.md` §4.2 の reservoir 容量「48 B/px × 2 組」「1600×900 で約 35 MB/viewKey」は
     実装 (56 B/px、40.3 MB) と食い違う。ADR / engine_spec は実装側の数字で書いた。
     spec 本文の訂正は planner の領分なので手を入れていない。
  2. 「やること 6」(`harness.md` への申し送り) を司会指示との衝突で見送った。
     下の「申し送り」を台帳へ転記してよいか、planner / 司会の確認がほしい。
  3. `MYE_SHOT_SKIP_RT` を CI で立てている以上、新しい 22 枚目は**ランナーでは一度も撮られない**。
     ReSTIR の絵の回帰は開発機でしか検出できない (SSR / FXAA と同じ既知の取引)。
     spec §5 A1 の「全枚」は「ローカルで全枚」の意味だと解釈した。
触ったファイル:
  - docs\adr\ADR-016-restir-reflection.md (新規)
  - engine_spec.md
  - README.md
  - CLAUDE.md
  - tools\shot_verify.bat
  - tests\golden\demo_render_rtrefl_restir.png (新規、版管理対象)
  - .github\workflows\ci.yml
  - src\Engine\Engine\Asset\CookedCache.h
  - assets\shaders\rt_refl_restir_spatial.cs.hlsl
  - plans\m67-restir-reflection\sub-07.md (実装メモ節のみ)
申し送り:
  - (sub-07 → 司会 / 台帳) **S5 (パラメータ調整) の手順** (spec §4.6 の「ユーザー」行の実務):
    1. `cmd /c bin\x64\Release\Editor.exe --render-demo --deferred --rt-refl --rt-restir`
       (と `--acoustic-demo` の同構成)。**エディタは乱数を凍結しない**ので、
       `--rt-anim-seed` 相当の「動くノイズ」がそのまま見える。
    2. 表示 → RT Debug → ReSTIR サブメニューで実行中に触る: クラス表 5 行 ×
       (半径 1〜32 px / タップ 0〜8 / M 上限 1〜32) / α 基準 (0.01〜1.0) /
       `Spatial reuse` / `Visibility ray` / クラス上書き / SVGF 履歴 1〜32 /
       A-Trous 0〜4 / `Reset to defaults`。**非永続** (プロジェクトには保存されない)。
    3. 見るもの: `--rt-debug 12` (M のヒートマップ = 履歴がどこまで伸びたか) /
       `13` (一次ヒットのクラス色) / `14` (反射像側のクラス色)。
    4. **最初に触るノブは M 上限** (sub-06 の実測: 出荷構成で一様 Prop (32) が
       既定の混在より 1.8 倍良い)。半径・タップは spatial が既定 off の間は効かない。
  - (sub-07 → 後続 `M67h`) **焼く項目**: `RtTypes.h` の `kRtReflClassTable` 5 行
    (半径 / タップ / M 上限) / `kRtRestirRadiusAlphaRef` / `RtReflRestirParams` の
    `spatial` (ユーザーが on を選ぶなら 0 → 1) / `visRay` / `svgfHistory` /
    `atrousIterations`。firefly が出ていたら `kRtRestirWMax` を 1 本足す (ADR-016 の既知制限)。
    **焼いたら `tools\shot_verify.bat --update` で `demo_render_rtrefl_restir` を撮り直し、
    他 21 枚が動いていないことを `git status` で確認してからコミットする。**
  - (sub-07 → planner) spec §4.2 の reservoir 容量の数字 (48 B/px / 35 MB) が実装と合わない。
    実測は 56 B/px / 40.3 MB (上の「仕様との差分」参照)。
  - (sub-07 → reviewer) 読む順の推奨: `docs\adr\ADR-016-restir-reflection.md` →
    `engine_spec.md` §6.4 の「ReSTIR reflections and `ReflectionClass`」→ §11.3 の RT 3 枚の段落。
    ADR の各決定に対応する spec §2 の S 番号は S1 (golden) / S2 (`pad0` → `reflectionClass`) /
    S3 (2 パス) / S4 (厳密 Jacobian = U4) / S5 (reservoir の `geom`) / S6 (デバッグ 13/14) /
    S7 (チューニング UI) / S8 (`--rt-class-override`) / S12 (保存量 W) / S15 (cook 版)。
    golden の実物は `tests\golden\demo_render_rtrefl_restir.png` (frame 40)、
    ReSTIR off の同フレームは `tests\actual\m67g_f40_off.png` (gitignore、再撮影可)。
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-09-05)。受け入れ条件 1〜4・6 を SELF_EVAL の検証欄で確認 (22 枚 PASS ×3 run、
  SKIP で 19 枚、replay_verify PASS、selftest / check_rules 緑、新 golden は別 run と tol=0、ReSTIR off の同フレームと
  maxDiff=93 / 9723 px = ReSTIR が写っている)。条件 5 (ADR と engine_spec の整合) は reviewer の読み判定。
  planner 側の裏取り: `git status` (変更 7 + 新規 2、既存 golden 21 枚は未変更)、`tests/golden` が 22 枚、ADR-016 の決定 10 件
  と理由 10 節の見出し (書き戻し無し / α 比例 / 回転固定 / 既定 off / M 上限の所見 / p̂ = 0 の M / VNDF 積分 / isfinite)、
  `CookedCache.h:19-24` の 56 → 64 訂正、`rt_refl_restir_spatial.cs.hlsl:169-174` のコメント、
  `EnsureReservoirs` の 5 フォーマット (= 56 B/px、coder の指摘どおり spec の 48 は planner の誤算 → spec §4.2 を訂正)。
  [逸脱] golden を `--update` でなく比較 run の実物のコピーで記録 → 承認 (他 21 枚に触れない証拠が強い)。
  [逸脱] harness.md の見送り → 司会指示どおりで正しい。[追加] engine_spec §11.3 / README / ci.yml の数え直し → 承認。
  不安 3 (A1 = ローカルで全枚) → 承認、spec A1 に明文化。`MyeWarnAsError` 未使用 → コメント 2 行以外にコードを触って
  いないので判定に影響しない (nit として reviewer のビルドで拾う)。
