# sub-07: 仕上げ — ADR-016 / engine_spec §6.4 / README / CLAUDE.md / ReSTIR on の golden / 全検証

- 依存: sub-06
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
