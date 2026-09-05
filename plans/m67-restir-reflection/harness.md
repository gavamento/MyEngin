# harness 台帳: m67-restir-reflection

- 依頼原文: "C:\Users\akita\.claude\plans\dynamic-object-adaptive-restir-reflectio-magical-sketch.md"これの計画をエンジンに実装
- 開始: 2026-09-04 / 基点コミット: 30f4993b60d8d948ea2d2522aa86759fbb88798b
- フェーズ: 実装
- 元計画: `C:\Users\akita\.claude\plans\dynamic-object-adaptive-restir-reflectio-magical-sketch.md` (ReSTIR 反射 + ReflectionClass = 反射に映る側の品質制御。写しを `plans/m67-restir-reflection/plan-original.md` に置く)
- 仕様書: `spec.md` 確定 (2026-09-04)。疑った点 §2、受け入れ条件 A1〜A14 (§5)、サブ 7 本 (§6)
- 策定の往復: なし (planner に AskUserQuestion が無かったため全件 planner 裁定 → U1〜U6 を司会が確認。下の「ユーザー判断」)
- ユーザー判断で押し切られた点: U4 temporal の Jacobian。planner は「J=1 近似 (再投影妥当性を通った同一面点なら P_prev ≈ P で誤差 5% 以内、テクスチャ 1 枚 +16 B/px と帯域を節約)」を推したが、ユーザーは**厳密に計算する**を選択。spec §2 S4 / §7 U4 に反対意見ごと記録。以後蒸し返さない。
- 主な裏取り (spec §2): 元計画の受け入れ基準「golden 19 枚無変更 = 後方互換の証明」は RT について空振り (19 本の撮影に `--rt-*` が 1 つも無い) → sub-01 でローカル限定 tol=0 の golden を足す。`--render-demo --deferred --rt-refl` は WARP で 2 回撮って maxDiff=0 (planner 実測)。pad0 を埋める場所は `RtScene::Update` で確定。`DemoContent.cpp` は `src/Engine/Engine/` (元計画の `src/GameLogic/` は誤り)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | OK | 1 | 73a439e | M67a: RT 反射 / GI の golden 2 枚 (ローカル限定 tol=0、`MYE_SHOT_SKIP_RT`) + S0 ベースライン計測。依存: なし。VERDICT round 1 OK (nit 2 → sub-07 申し送り)。coder の [逸脱] (末尾 20/21 枚目) は仕様側の誤りとして planner が sub-01.md を訂正。S0 ベースライン (WARP / Release / frames 20): render-demo refl 4.275 / denoise 22.646 ms、acoustic refl 7.992 / denoise 18.492 ms。`--rt-gi` の run-to-run 決定性リスクは空振り (3 回 maxDiff=0) |
| sub-02 | OK | 1 | 4511cce | M67b: ReflectionClass の配管 (Material → RtInstance.reflectionClass → HLSL) + デバッグ 13 + デモ材質への割り当て。依存: sub-01。VERDICT round 1 OK (should 1 = CookedCache.h のコメント 56→60 が門番 64 と食い違い → sub-07 衛生 8、nit 1 → 申し送り)。planner の見落とし S15: `Material` は cooked blob へ memcpy = M67b が cook 版導入後で初のフィールド追加 → coder の [追加] (kCookVersion 1→2 / static_assert 56→64 / 明示 pad0) を仕様として承認。golden 21 枚 tol=0 緑、replay 全緑、デバッグ 13 の色を 2 デモで画素実測 |
| sub-03 | OK | 2 | ea0860b | M67c: ReSTIR の数学 (rt_restir_common.hlsli ⇄ RtMath.h ミラー + selftest、定数表)。依存: sub-02。round 1 REWORK (must 2 = `RtReservoirMerge` が p̂=0 の候補で M だけ増やす → w=0/非有限は Update を呼ばず false、+ その selftest / nit 1 = Update のコメント)。仕様側の誤り 1 件 (A4「VNDF pdf 半球積分 = 1」は成立しない → 「上半球積分 + 下半球漏れ = 1」に訂正)。裁定: 空 reservoir cls=-1 / スカイ cls=4 / Merge に jMax 引数 / 「M を数える規則」を §4.2 に新設。planner の round 1 応答はセッション上限 (429) で 1 度中断 → 再開して完走。round 2 OK (nit 2 = kWeightMax の昇格余地 / NaN ケース → 申し送り)。coder の [追加] `isfinite` → 定数比較 (fxc X3577 で最適化除去されうる) を承認し spec §4.5 に「HLSL で isfinite/isinf を使わない」を新設 |
| sub-04 | OK | 1 | e10f399 | M67d: reservoir の配管 (初期化 + resolve、M=1 で現行と等価) + デバッグ 12/14 + `--rt-restir`。U4 反映: reservoir 5 枚 (`rpos` 追加、u1-u5 / t11-t15)。依存: sub-02, sub-03。VERDICT round 1 OK (should 1 = spatial の sub-06 用未使用宣言は sub-06 で全部読む側へ / nit 2 → 申し送り)。[追加] 8 件を仕様に取り込み: M=0 の画素は 1spp フォールバック (シルエット際 950 テクセルが永久に黒くなるのを防ぐ) / p̂ は xs から復元した方向で評価 / `rt_restir_cb.hlsli` (CB の唯一の宣言) / b3 は off でも毎フレーム張る / トレースは分岐の外で 1 回 (X4714)。golden 21 枚 tol=0 不変、M=1 で off と maxDiff 1 (120 px、p̂ 往復の 1 ulp)、run-to-run tol=0。restir 1.5 ms (WARP) |
| sub-05 | OK | 1 | (次コミットで記入) | M67e: temporal reuse (クラス別 M 上限、厳密 Jacobian = `RtRestirJacobian(xs', ns', P_prev, P)`、velocity は t16)。依存: sub-04。VERDICT round 1 OK (should 1 = 履歴 UV の解像度を b3 の `gRsOutSize` に寄せる → sub-06 / nit 1 → 申し送り)。**M67d の spatial パスが組 A へ W=0 を書き戻していた欠落を発見・修正** (resolve は wSum を使うので絵に出ず golden も A5 も緑 = sub-04 の受け入れ条件では検出不能。planner も見落とし。CPU ミラーの 2 パス往復 selftest で固定、変異テストで FAIL を確認)。M が Default 16 / Prop 32 で飽和、鏡面パッチのフリッカー 0.226 倍、A14 は 3 条件 tol=0。W クランプは実測で不要 (最大輝度 on 227.5 < off 247.9)。ReSTIR on の golden は **frame 40** で撮ると確定 (sub-07)。restir 2.4 ms (WARP) |
| sub-06 | 実装中 | 0 | | M67f: spatial reuse (クラス駆動) + 可視レイ + `--rt-class-override` + チューニング UI。依存: sub-05 |
| sub-07 | 未着手 | 0 | | M67g: 仕上げ (ADR-016 / engine_spec §6.4 / README / CLAUDE.md / ReSTIR on の golden / replay_verify)。依存: sub-06 |

## ユーザー判断
- (2026-09-04、司会が AskUserQuestion で確認。planner 裁定 = U1〜U6 を spec §7 に記載)
- U1 GI の golden (`demo_render_rtgi`) も足す → **足す** (裁定どおり)
- U2 実行中スライダのチューニング UI → **入れる** (裁定どおり、A13 有効)
- U3 デモ Material へのクラス割り当て → **割り当てる** (裁定どおり: rdemo_spin=Hero / rdemo_pillar_*=Prop / adem_player=Hero / adem_agent_*=Character)
- U4 temporal の Jacobian → **厳密に計算する** (**裁定と逆**。reservoir に P_prev (R32G32B32A32、+16 B/px) が増える。planner へ補足として送り spec / sub-05 (と reservoir レイアウトを持つサブ) を直させてから実装へ)
- U5 パス構成 → **2 パス** (裁定どおり。typed UAV load を避ける)
- U6 S5 (パラメータ調整) → **harness の外に置き、確定値は M67h で焼く** (裁定どおり)
- (2026-09-05 ユーザー指示、セッション運用) 全サブ完了 → レビュー → 完了報告 → Notion の活動記録 → **PC をシャットダウン** (`shutdown /s /t 120`、取り消しは `shutdown /a`) の順で司会が無人で進める

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
- (sub-05 → sub-06、should) 履歴 UV の解像度に `gRfOutSize` (b2) を使い、CB には `gRsOutSize` (b3) もある = 出所が 2 つ。spatial のタップ座標を書くときに **rt_refl 側も `gRsOutSize` に寄せて b2 依存を 1 本減らす** (同じ値を C++ が詰めるので絵は動かない。golden で確認)
- (sub-05 → sub-06 / reviewer) `hasLast` を落とす場所は `RtPasses.cpp` の 3 か所: (1) `restirOn == false` のフレーム (`RenderReflection` 冒頭)、(2) 内部解像度が変わった (`EnsureReservoirs`)、(3) spatial を走らせられなかった (`restirRan == false` かつ `restirOn`)。`rsHistValid` は `slot.lastSerial + 1 == view.rtViewSerial` と `view.prevViewProjValid != 0` も要求。**reviewer の実機確認**: ReSTIR を off → on → off と切り替えてデバッグ 12 が「赤に戻ってから伸び直す」こと (受け入れ条件 6)
- (sub-05 → sub-06) 候補の M 上限に使うクラスは **候補側 (映っている物体) の `cls`**、書き戻し前の `RtRestirClampM` だけが採用サンプルの `cls`。タップでも同じ使い分け。temporal で積まれた M が spatial の入力になるので、タップを足すと M の伸び方が変わる — デバッグ 12 の矩形 (330,255)-(470,400) の「平均 G」と「連続 2 フレームの平均絶対差」で効きを読む
- (sub-05 → sub-07) ReSTIR on の golden は **frame 40** (`--frames 41 --shot-frame 40`、Default 16 / Prop 32 の M 上限が両方飽和した状態)。ADR の数字は sub-06 の spatial 込みで取り直す (M67e 時点 restir 2.4 ms、reservoir 14.6 MB/スロット)。既知の制限 2 件 (ReSTIR トグルで SVGF 履歴 `reflHist_` が落ちず数フレーム混ざる = 実害なし / W クランプ無し = カメラが動く条件で firefly が出たら M67h で `kRtRestirWMax` を 1 行) を ADR に
- (sub-05 → 後続、nit) 変異テストの手順 (`s.W = 0` で growOk が落ちる) を selftest のコメントに 1 行 (任意)
- (sub-05 → reviewer) 画像は `testsctual\` の `m12_f3` / `m12_f40` / `m12_f80` / `flicker_{on,off}_{40,41}` / `d11_restir_{on,off}` / `m67e_a14_f40_{1,2,3}` (gitignore、再撮影可)
- (sub-04 → sub-06、should) `rt_refl_restir_spatial.cs.hlsl` の sub-06 用未使用宣言 (t9 / t14 / t15、`gRsSpatialOn` / `gRsVisRay` / `gRsClassOverride` / `RtRestirClassParams(cls)`) は「まだ誰も読んでいない」。sub-06 で**全部読む側に回す** (読まずに残ったら reviewer が死コードとして拾う)。タップ挿入点は spatial の「M67f: ここに近傍タップ〜」のコメント位置
- (sub-04 → sub-05) temporal の入口は全部配線済み (t11-t15 / `gRsHistValid` / `gRsUseVelocity` / `gRsPrevViewProj` / `gRsPrevCameraPos` / `gRsDepthThreshold` / `gRsNormalThreshold` / `gRsJacobianMax`)。velocity SRV (t16) だけ未配線。**どれも一度も実行していない** = sub-05 が最初の実走者。受け側の再投影は `rt_reproject.hlsli` の `RtHistoryUv` / `RtReprojectValid`。組 A の `geom` は RtHistory.geom と同レイアウト
- (sub-04 → sub-05 / 06) **p̂ の評価方向は必ず `RtRestirSampleDir(r, P)`** (撃った L で評価すると再利用ゼロでも絵が 10% 級に動く)。`rt_restir_cb.hlsli` に 1 本だけ
- (sub-04 → sub-06) チューニング UI は `RenderSystem::rtReflRestirParams` を直接触れば効く。`svgfHistory` / `atrousIterations` は RtPasses 側で [1,32] / [0,4] にクランプ済み。クラス表は 1 画素も検証していない (タップ 0)。`--rt-class-override` の CLI と `RtScene::Update` の引数はまだ無い
- (sub-04 → sub-07) CLAUDE.md の CLI 一覧に `--rt-restir`。engine_spec の RT デバッグモード表を 12 / 13 / 14 で更新。reservoir は 480×270 で 1 スロット約 14.6 MB (5 枚 × 2 組 × 48 B/px)、SceneView + GameView で約 29 MB (ADR の数字)。ReSTIR on の golden は自身が基準なので tol=0 で撮る (A5 の tol 1 は off との比較にだけ効く)
- (sub-04 → 後続、nit) HLSL 側 `float4 gRsClass[...]` の直前に「C++ の `offsetof` 144 と一致 — 前に float を足すときは両方」の 1 行 (任意)。`rt_blit.hlsl` の点サンプラ s1 は mode 4 専用 — 12 の M 表示を厳密に読むなら mode 1/2 も点サンプラへ寄せる余地 (今は触らない)
- (sub-04 → reviewer) off = 現行の証拠は golden 21 枚 + `--rt-debug 10` の off/on tol=0。M=1 等価の証拠は A5 (maxDiff=1 / 超過 0 px)。画像は `testsctual\` の m1_off / m1_on / m1_on2 / probe_rtdebug12 / probe_rtdebug14 / d10_* / d11_* (gitignore、再撮影可)
- (sub-03 → sub-04 以降) **HLSL で `isfinite()` / `isinf()` を使わない** — fxc は `/Gis` 抜きだと警告 X3577 を出したうえで最適化除去しうる (実測)。`ShaderManager` は `D3DCOMPILE_IEEE_STRICTNESS` を渡していない。非有限の防波堤は `!(w < kWeightMax)` のような普通の比較で書く (spec §4.5)
- (sub-03 → sub-04 / 05 / 06) 候補を「外す」ときは **`RtReservoirUpdate` を呼ばずに M を加算しないまま return**。幾何不一致 (深度・法線・クラス半径) の棄却も Update より前に置く。`RtLuminance` は `MYE_RT_LUMINANCE_DEFINED` ガード付き (`rt_reproject.hlsli` へ括り出すときも同じガード)。`RtReservoirUnpack` の cls は `round`、初期サンプルは `w = lum(Ls)` / `wSum = w` / `M = 1` / `W = RtRestirWeight(wSum, M, p̂)` の 4 行、`RtRestirResolve` は scale を先に求める順序 (spec §4.2 / sub-04.md)
- (sub-03 → sub-06) spatial の `[loop]` 上限に `MYE_RT_RESTIR_MAX_TAPS` を使う (規則 9 の登録を形骸化させない)
- (sub-03 → 後続、nit) `kWeightMax = 1e30f` は両言語の関数ローカル定数。同じ上限を別の場所に書く必要が出たら `RtTypes.h` / `rt_restir_common.hlsli` の定数群へ昇格させて規則 9 に載せる。`RtSelfTest.cpp` の非有限ケースは `INFINITY` のみ — `quiet_NaN()` を 1 行足すと C++ 側でも「両方の比較に落ちる」が機械化される (任意)
- (sub-03 → reviewer) 数学の根拠は selftest のログ 5 行 (integral / leak / pdf peak / RIS ratio)。ガードを外すと selftest が狙った 3 件だけ FAIL する (変異テスト実施済み)
- (sub-02 → sub-07) `CookedCache.h:20` のコメント「56 → 60 バイト」は門番 `ModelCook.cpp:19` の `static_assert(sizeof(Material) == 64)` と食い違う → 「56 → 64 (60 + 明示パディング 4)」に直す (VERDICT should 1、sub-07 衛生 8)。`Material::pad0` の「値は読まない」コメントに「cooked blob のバイト列を run ごとに変えないため 0 で初期化する」を添えると消されにくい (nit、任意)
- (sub-02 → sub-04) デバッグ 13 は `rt_debug.cs.hlsl` の CS 経路で **`--rt-refl` 不要**。`RtPasses::RenderDebug` の 4〜11 は Blit で早期 return するので、12 / 14 を足すときは 13 より前に if を置く (13 は「どの早期 return にも当たらない」ことで CS へ落ちている)。反射像側 (14) は同じ `RtReflClassColor` を使えば 13 と色が揃う (スカイ = cls 範囲外は黒)
- (sub-02 → sub-03 以降) `RtInstance` の残りの空きは `pad1` 1 本だけ (sizeof==80 の枠が満杯)。クラス以外の per-instance 値が要るなら 80 → 96 = golden 再撮影が要る
- (sub-02 → reviewer) A3 の Inspector 往復 (`LoadMaterialEdit` / `MaterialEditToJson` は private でヘッドレス不可) は **reviewer の実機操作**で担保する (spec §5 A3 を実態に合わせ済み)。probe 画像: `testsctual\probe_rtdebug13.png` / `probe_rtdebug13_acoustic.png` (gitignore 配下、再撮影可)
- (sub-01 → sub-07) `tools\shot_verify.bat:335` の「RT レーン (M46) の唯一のピクセル被覆」は正確には「RT 反射 / GI の」(RT 影 M46g は依然ゼロ被覆)。engine_spec `:403` / `:1711` (fifteen → 22) を直すときに bat 側の文言も揃える (VERDICT nit 1)
- (sub-01 → sub-04 以降) `--frames 6` では GpuTimer (kFrames=6 のリング、スロット再利用時にしか回収しない) が全項 0.000 ms を返す。GPU 時間の計測 run は **`--frames 20`** で別に回す (golden の撮影条件 frames 6 は不変、両者のスクショはビット一致)。ログは標準出力 (`.log` は作られない)。GpuTimer は M67 で触らない (VERDICT nit 2 / spec §5 A11)
- (sub-01 → sub-07) CLAUDE.md の CLI 一覧に `--rt-refl` / `--rt-gi` 等の `--rt-*` 8 本が元から未掲載。`--rt-restir` 系を書くときに一緒に載せる
- (sub-01 → 以降全サブ) golden `demo_render_rtrefl` / `demo_render_rtgi` が「ReSTIR off = 現行とビット一致」の唯一の証拠。動いたら **`--update` で塗り潰さず報告が先** (golden-diff-triage の 4 点計測)
- (planner 2026-09-04) 元計画 S5 (パラメータ調整 = ユーザー判定ループ) は **harness の外**。sub-06 が実行中スライダ +
  `--rt-class-override` を用意し、sub-07 は既定値を触らずに閉じる。ユーザーの確定値は後続 `M67h` で
  `RtTypes.h` の定数表へ焼き、golden `demo_render_rtrefl_restir` を `--update` する (spec §4.6)。
- (planner) `AskUserQuestion` が無い環境で策定し、U1〜U6 は司会がユーザーに確認済み (2026-09-04)。
  **U4 だけ裁定と逆 (temporal の Jacobian を厳密に計算)** → reservoir に `rpos` (R32G32B32A32) が増え、
  sub-04 (配管、u1-u5 / t11-t15) と sub-05 (J の式、velocity は t16) と sub-07 (ADR) に反映済み (spec §8)。
