# sub-12: 水面の波紋

- 依存: sub-11
- 状態: 未着手
- 往復: 0

## やること

spec §4.3 の水面、§2 #18。歩く・斬る・物が落ちると水面に波紋が広がる (見た目だけ)。

1. 水面のシェーダ (`water_surface.hlsl`。Gerstner の M79 サーフェス `WaterGerstner.surface.hlsl` も対象にするかは coder が経路を確認して判断し、SELF_EVAL に書く) で、波紋チャネルを持つ 2D 層の `h` を引き、中心差分の勾配を法線に足す。VS に SRV を束縛できるなら変位も足す (できなければ法線だけ。理由を書く)
2. 束縛: sub-02 の RG16F の配列 (R = 波紋) を、`water_surface.hlsl` の空いた t スロット (t0〜t8 が使用中、`:82-90`) に。番号は共有ヘッダへ
3. 浮力 (`WaterWaveComponent` の CPU 側) は**変えない**
4. デモ: `--field-demo` の末尾に浅い池 (水面 + 波紋チャネルの 2D 層、`baseY` を水面の高さに) を足し、デモの人の経路が池を横切るようにする

## やらないこと (このサブでは)

- 浮力・当たり判定への反映 (spec §2 #18)
- 水面の解析波 (Gerstner) の変更

## 触る場所 (planner の見立て)

- `assets\shaders\water_surface.hlsl`、`src\Engine\Renderer\Passes\WaterPass.cpp` (束縛)、必要なら `WaterGerstner.surface.hlsl`
- sub-02 の共有ヘッダ
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 23, 30。

- 23: 人が池を渡る前と渡った後の 2 枚で、水面の領域に差がある (`--img-diff`、静止状態 2 回の差の 5 倍以上)。`watersurface` / `waterwave` の selftest 不変
- 波紋チャネルの無いシーン (既存の水のデモ) の絵が変わらない (`--img-diff` で差 0 か、差の理由)
- 12: replay_verify `field` PASS

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (sub-07 後 / sub-16 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- `$env:MYE_SELFTEST_ONLY='watersurface,waterwave,disturbance,render'; bin\x64\Debug\Editor.exe --selftest`
- スクショ (Runtime.exe)
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
