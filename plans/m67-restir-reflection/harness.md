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
| sub-01 | OK | 1 | (次コミットで記入) | M67a: RT 反射 / GI の golden 2 枚 (ローカル限定 tol=0、`MYE_SHOT_SKIP_RT`) + S0 ベースライン計測。依存: なし。VERDICT round 1 OK (nit 2 → sub-07 申し送り)。coder の [逸脱] (末尾 20/21 枚目) は仕様側の誤りとして planner が sub-01.md を訂正。S0 ベースライン (WARP / Release / frames 20): render-demo refl 4.275 / denoise 22.646 ms、acoustic refl 7.992 / denoise 18.492 ms。`--rt-gi` の run-to-run 決定性リスクは空振り (3 回 maxDiff=0) |
| sub-02 | 未着手 | 0 | | M67b: ReflectionClass の配管 (Material → RtInstance.reflectionClass → HLSL) + デバッグ 13 + デモ材質への割り当て。依存: sub-01 |
| sub-03 | 未着手 | 0 | | M67c: ReSTIR の数学 (rt_restir_common.hlsli ⇄ RtMath.h ミラー + selftest、定数表)。依存: sub-02 |
| sub-04 | 未着手 | 0 | | M67d: reservoir の配管 (初期化 + resolve、M=1 で現行と等価) + デバッグ 12/14 + `--rt-restir`。U4 反映: reservoir 5 枚 (`rpos` 追加、u1-u5 / t11-t15)。依存: sub-02, sub-03 |
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

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## 申し送り (セッション跨ぎ)
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
