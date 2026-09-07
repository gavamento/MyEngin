# harness 台帳: m67h-restir-finalize

- 依頼原文: M67hの実装
- 開始: 2026-09-08 / 基点コミット: 9a893cdeb1095b113c99b03b1af925afe1e36e3c
- フェーズ: 実装 (仕様確定 2026-09-08)
- 前提: M67 (ReSTIR 反射) は 2026-09-05 に完了 (台帳 `plans/m67-restir-reflection/harness.md`)。
  M67h は「S5 (パラメータ調整) の確定値を焼く + review-1 の minor 5 件」として積み残されたもの。
  - U6 (M67 のユーザー判断): S5 は harness の外に置き、**確定値は M67h で焼く**
  - U7 (同): spatial reuse の既定は目標帯の計測で off。**既定を反転するなら 1 行** (M67h)
  - 焼く候補 (sub-07 申し送り): `RtTypes.h` の `kRtReflClassTable` 5 行 / `kRtRestirRadiusAlphaRef` /
    `RtReflRestirParams` の `spatial` / `visRay` / `svgfHistory` / `atrousIterations` /
    (firefly が出るなら) `kRtRestirWMax`
  - minor 5 件は M67 台帳の「申し送り」冒頭 5 項目 (review-1 minor 1〜5)

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 | 未着手 | 0 | | review-1 minor 5 件の回収 (UI の実効判定 / サブメニューの高さ / 未使用 CB 項目 / コメント 2 か所 / .mat.json の整数値受理)。依存: なし |
| sub-02 | 未着手 | 0 | | S5 の確定 (2 軸の計測 → 規則適用 → 既定値と文書、必要なら golden 1 枚)。依存: sub-01 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- (2026-09-08、司会が AskUserQuestion で確認。planner の `[ユーザーに聞ける]` 3 件。**全て裁定どおり**)
- Q1 S5 の確定値を誰が決めるか → **(ii) ヘッドレスの計測で確定する** (裁定どおり)。
  帰無仮説 = 現行の既定表のまま、規則が要求したときだけ 1 行変える。sub-02 の手順 1〜4 を全て回す。
  ユーザーは実機の確定値を持ち込んでいない = 計測が唯一の根拠。
- Q2 `spatial` の既定を on に反転するか → **off 維持** (裁定どおり)。M67 U7 の規則で決着済み
  (目標帯 = 音響デモの床で temporal 単独 0.181 < spatial on 0.255)。M67 台帳の U7 に残っていた
  「完了報告で提示する」はこれで回収済み。**以後蒸し返さない**。
- Q3 `visRay` の既定を on に反転するか → **off 維持** (裁定どおり)。コスト約 2.7 倍
  (WARP / frames 20 で restir 2.320 → visray 6.166 ms)、利得は光漏れ緩和のみで ADR-016 に既知制限。

## 申し送り (セッション跨ぎ)
- (planner 2026-09-08) `spec.md` 確定。サブ 2 本 = sub-01 (review-1 minor 5 件) / sub-02 (S5 の確定)。
  **sub-01 は Q1 の回答に依存しない**ので、回答待ちでも着手できる (これが順序の理由。spec §6)。
- (planner 2026-09-08) **Q1 `[ユーザーに聞ける]`**: S5 の確定値を誰が決めるか。planner の裁定は
  「(ii) ヘッドレスの計測で確定。ただし帰無仮説 = 現行の既定表」。(i) ユーザーが実機の確定値を持っているなら
  sub-02 の手順 3〜4 を飛ばして焼くだけになる。(iii) minor 5 件だけで閉じるなら sub-02 を落とす。
  付随して Q1-a (`spatial` の既定を on にするか。M67 U7 の完了報告に対する回答が台帳に無い) と
  Q1-b (`visRay` の既定を on にするか) も 1 行ずつ聞ける。詳細は spec §7。
- (planner 2026-09-08) 裁定のうち後から効いてくるもの: **sub-06 の「一様 Prop が 1.8 倍良い」だけを根拠に
  mCap を上げてはいけない** (フリッカー指標は静止シーンで長い履歴を無条件に有利にする片側の軸。spec §2 S3)。
  **`radiusPx` / `taps` / `radiusAlphaRef` は焼かない** (spatial 既定 off ではシェーダが 1 タップも撃たない。S4)。
  **`--shot-every` は決定的撮影モードを外す** (`EngineLoop.cpp:704`) ので測定に使えない (S5)。
- (planner 2026-09-08) 副産物: review-1 の「未確認 = J ≠ 1 の temporal 経路」は**ヘッドレスで閉じられる**。
  `rdemo_spin` は roughness 0.45 < `kRtReflMaxRoughness` 0.6 = 自分自身が反射の受け面で、かつ回っている
  = 受け側の `P_prev ≠ P` (spec §2 S9 / sub-02 手順 1)。
- (planner 2026-09-08) コミットは 2 本: `M67h:` (minor) / `M67i:` (S5)。最終レビュー後の司会コミットは
  `M67 追補: レビュー完了 — ...` を推奨 (`M67: レビュー完了` = 8e4272e と衝突するため)。
