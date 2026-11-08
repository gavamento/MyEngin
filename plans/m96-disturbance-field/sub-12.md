# sub-12: 跡 (地形の雪・砂)

- 依存: sub-11
- 状態: 未着手
- 往復: 0

## やること

spec §4.3 の跡、§7 R5。雪・砂の地形に、通った筋が残り (または時間で消え)、見た目に出る。

1. `terrain_common.hlsli:60` の `SampleTerrainSurface` (または `forward_terrain` / `deferred_terrain` の呼び出し側) で、跡チャネルを持つ 2D 層の `τ` を引き、色を「跡の色」へ寄せ、`τ` の勾配で法線を曲げる (凹みの VS 変位は任意。入れたら影・当たり判定とのずれを SELF_EVAL に書く)
2. 跡の色・強さの欄: 地形側 (TerrainComponent か地形のマテリアル) と場の側のどちらに置くかは、既存の地形のマテリアルの持ち方を見て coder が提案し、SELF_EVAL の「仕様との差分」に書く (planner が判定)。既存の地形の見た目は既定値で不変
3. 束縛: sub-02 の RGBA16F 配列を地形のシェーダの空いた t スロット (t20〜t28 付近が地形で使用中) に。Forward と Deferred の両方
4. 当たり判定は変えない
5. デモ: `--field-demo` の末尾に小さな雪の地形 (跡チャネルの 2D 層、`trailKeep = 1`) を足し、人の経路が通るようにする

## やらないこと (このサブでは)

- 地形の高さ (当たり判定) の変形
- 足跡の形 (spec §7 R5: 0.25 m のマスでは筋になる)

## 触る場所 (planner の見立て)

- `assets\shaders\terrain_common.hlsli:26-60`、`forward_terrain.hlsl`、`deferred_terrain.hlsl`
- 地形の描画パスの束縛 (`grep -rn "gTerrainSplat\|terrain" src\Engine\Renderer\Pipeline`)
- sub-02 の共有ヘッダ
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 24, 30。

- 24: 人が雪を横切る前と後の 2 枚で筋が写る (`--img-diff`)。`trailKeep < 1` にした場合に消えていくことを 3 枚目で。`terrain` の selftest 不変
- 跡チャネルの無いシーン (`--terrain-demo`) の絵が変わらない
- 12: replay_verify `field` PASS

## 検証コマンド

> selftest の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) を**実行しない**。ビルドと check_rules (と、selftest 以外の検証) までを行い、selftest で確かめる条件は「未実行 (sub-07 後 / sub-14 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='terrain,disturbance,render'; bin\x64\Debug\Editor.exe --selftest`
- スクショ (Runtime.exe、Forward と Deferred の両方)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
