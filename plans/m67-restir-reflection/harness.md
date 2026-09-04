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
| sub-03 | OK | 2 | (次コミットで記入) | M67c: ReSTIR の数学 (rt_restir_common.hlsli ⇄ RtMath.h ミラー + selftest、定数表)。依存: sub-02。round 1 REWORK (must 2 = `RtReservoirMerge` が p̂=0 の候補で M だけ増やす → w=0/非有限は Update を呼ばず false、+ その selftest / nit 1 = Update のコメント)。仕様側の誤り 1 件 (A4「VNDF pdf 半球積分 = 1」は成立しない → 「上半球積分 + 下半球漏れ = 1」に訂正)。裁定: 空 reservoir cls=-1 / スカイ cls=4 / Merge に jMax 引数 / 「M を数える規則」を §4.2 に新設。planner の round 1 応答はセッション上限 (429) で 1 度中断 → 再開して完走。round 2 OK (nit 2 = kWeightMax の昇格余地 / NaN ケース → 申し送り)。coder の [追加] `isfinite` → 定数比較 (fxc X3577 で最適化除去されうる) を承認し spec §4.5 に「HLSL で isfinite/isinf を使わない」を新設 |
| sub-04 | 実装中 | 0 | | M67d: reservoir の配管 (初期化 + resolve、M=1 で現行と等価) + デバッグ 12/14 + `--rt-restir`。U4 反映: reservoir 5 枚 (`rpos` 追加、u1-u5 / t11-t15)。依存: sub-02, sub-03 |
| sub-05 | 未着手 | 0 | | M67e: temporal reuse (クラス別 M 上限、厳密 Jacobian = `RtRestirJacobian(xs', ns', P_prev, P)`、velocity は t16)。依存: sub-04 |
| sub-06 | 未着手 | 0 | | M67f: spatial reuse (クラス駆動) + 可視レイ + `--rt-class-override` + チューニング UI。依存: sub-05 |
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
