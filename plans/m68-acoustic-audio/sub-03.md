# sub-03: 仕上げ — ADR-017 / engine_spec §10.6 / README / test_checklists / CLAUDE.md / 進捗表 (M68c)

- 依存: sub-02
- 状態: 未着手
- 往復: 0

## やること

文書だけ。コードは触らない (A1〜A9 は「無風の再確認」として回す)。

1. `engine_spec.md` §10.6 末尾 (`:1655` 付近の `---` の前、`## 11.` の直前) に `**Audible output (M68).**` 段落 (既存の
   「太字リード + 散文」様式、小節は作らない)。内容: 判断 1〜5 の要旨 (3 本目の Dial 写し / 仮想発音位置 / 4 クラス /
   2 アンカー補間 / `PendingWaveShot`)、**sim 状態ゼロ**の主張、振幅 = √エネルギー (spec S2)、`--acoustic-audio-log`。
   `:1701` "all six existing replay pairs and all seventeen golden images" / `:1703` "six scene pairs" / `:1742` "six replay pairs" を
   現行 (seven / twenty-two) に直す。
2. `README.md` `## 主要機能` に M65 + M68 をまとめた 1 bullet (「波面の 4 役目」を 1 文で)。`:91-100` の「被覆は 6 シーン」→ 7
   (音響ショーケースを列挙に足す)、`:119-123` の「リプレイ照合 6 ペア」→ 7。
3. `docs\adr\ADR-017-acoustic-audio.md` (ADR-016 の様式: 冒頭メタ / `## 決定` / `## 理由` (`### なぜ…` に却下案) / `## 帰結`
   (決定論と後方互換 / コスト (実測) / 既知の制限))。M65 に ADR が無いので「波面の 3 役 (整数チャンファ・sim 状態は波表だけ)」の
   決定を 1 節で拾う。却下案は spec §2 から: 共通化 vs 写し (判断 1) / 振幅 vs エネルギー (S2) / 直付け vs 子 (S7) /
   到来方向 1 歩 vs 平均 (S16) / Play 時 1 回 vs 追従 (S17) / override の置き場 (判断 4) / push を tick 側にする理由 (判断 5)。
   実測値は sub-01 / sub-02 の実装メモ (probe ms、box cells、shots) から写す。
4. `docs\test_checklists.md` 末尾に `## M68: 音響 × オーディオ (耳で確認)` 節 (`- [ ] 操作 → 期待`):
   起動 (`Editor.exe --acoustic-demo` → Play、または `Runtime.exe --acoustic-demo`) / 部屋 A で hum がこもって小さい /
   横の廊下を東へ歩くと開いてくる / 縦の廊下で戸口側 (前方) に定位 / 戸口で素通し / 廊下と部屋 B で残響が段差なく変わる /
   Walker の足音が金属で遠くまで・カーペットで数歩で消える / 箱の落下音 / 敵の自発音 / Q 石・E 瓶 / 呼吸は鳴らない /
   ミキサー窓に「音響が上書き中」・combo は資産値のまま / `AcousticAudio.enabled` を切ると全部素通しに戻る。
5. `CLAUDE.md`: 検証表の selftest 行 (45 スイートは M68a で済み — 記述の整合を再確認)、CLI 一覧の `--acoustic-audio-log N` の説明を
   仕上げ、「横断的な変更のチェックリスト」に音響 × オーディオの 1 段落 (`AcousticField` を読むのは出力レーンだけ /
   `ShapeAcousticSpatial` の 1 本 / 波の spatial は rolloff 0)、TypeId 末尾 **50 = AcousticAudio** (Cloth/SoftBody 51/52) の記述。
6. `plan-original.md` の進捗表 (3 行) と申し送り。`harness.md` の申し送りに「メモリ (`myengine-project.md`) の現在地更新は司会」と書く
   (リポジトリ外なので coder は触らない)。

## やらないこと (このサブでは)

- コードの変更。調整値の焼き込み (ユーザーが耳で決めた後の別コミット)。golden の更新 (動いていたらそれは M68a/b のバグ)。

## 触る場所 (planner の見立て)

| ファイル | 場所 |
|---|---|
| `engine_spec.md` | `:1640-1655` (§10.6 末尾)、`:1701`、`:1703`、`:1742` |
| `README.md` | `## 主要機能` (`:91-100`、`:119-123`) |
| `docs\adr\ADR-017-acoustic-audio.md` | 新規 |
| `docs\test_checklists.md` | 末尾 (M46 節の後) |
| `CLAUDE.md` | 検証表 / CLI / チェックリスト / TypeId |
| `plans\m68-acoustic-audio\plan-original.md` | 進捗表 |

## 受け入れ条件 (このサブ)

spec §5 の A1〜A9 (再確認) と A20〜A25。加えて `pwsh -File tools\check_rules.ps1` (文書は対象外だが習慣)。

## 検証コマンド

```
grep -n "six replay\|seventeen golden\|six scene" engine_spec.md      # 空
grep -n "6 シーン\|6 ペア" README.md                                    # 空
ls docs/adr/ADR-017-acoustic-audio.md
grep -n "^## M68" docs/test_checklists.md
cmd /c bin\x64\Debug\Editor.exe --selftest / tools\replay_verify.bat / tools\shot_verify.bat / check_rules (無風の再確認)
```

## 実装メモ (coder が追記)

## フィードバック履歴
