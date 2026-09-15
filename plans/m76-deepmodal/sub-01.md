# sub-01 (M76a): モーダル合成器と材質パラメータ

- 依存: なし
- 状態: 未着手
- 往復: 0

## やること
spec §4.1「後処理 BuildModes」「合成 ModalSynthRender」「Mel」と §4.2 の PhysMat 3 フィールド。
ネットも ECS も無しで「既知の (f, a, c) → PCM」と「特徴ベクトル + 材質 → (f, a, c)」の 2 段を純関数として完成させる
(ユーザー計画 Phase 1 / Checkpoint A、Phase 13 のデータ側)。

- `ModalTypes.h` (Modal/): 定数 `kModalVoxelN=32` / `kModalMapN=16` / `kModalBands=32` / `kModalChannels=192` / `kModalVoxelPad=1`、
  `DmNetHeader` POD (spec §4.2 のフィールド)、`ModalCellFeature { float v[192]; }`、`MaskCh(j,i)` / `AmpCh(j,i)`、`MelBandCenters(float out[32])`。
  ★定数は `constexpr int kModalVoxelN = 32;` の形で 1 行 1 定数 (check_rules の正規表現が拾える形。sub-03 が Python と照合する)。
- `Audio/ModalSynth.h/.cpp`: `ModalModeSet { int32_t count; float freqHz[32]; float amp[32]; float decay[32]; }`、
  `ModalPostParams { young, density, sizeL, alpha, beta, maskThreshold, gain }`、
  `BuildModes(const ModalCellFeature&, const float k[3], const DmNetHeader&, const ModalPostParams&, ModalModeSet&)` (spec §4.1 の 7 手順を**この順で 1 関数に**)、
  `ModalClipSeconds(const ModalModeSet&)`、`ModalSynthRender(const ModalModeSet&, AudioClip&)` (2 次再帰共振器、double 状態、1 ms 線形アタック、tanh ソフトクリップ、mono/44100/int16)。
  World も PhysMat も渡さない (値だけ) = selftest がデバイスもワールドも無しで全経路を叩ける。
- `PhysMat` 末尾にこの順で `youngsModulus` / `poissonRatio` (既定 0.3f、**保持のみ** — ランタイムは読まない。ユーザー判断 spec §2 #4。struct のコメントに「FEM / 参照材質メタデータ用、現行の BuildModes は使わない」と書く) / `rayleighAlpha` / `rayleighBeta` (ToJson 常時 / FromJson contains + 既定 / Sanitize 範囲 E [0,1e13] / ν [0,0.49] / α [0,1e4] / β [0,1e-2])。`assets\physmats\*.physmat.json` 11 本に spec §4.2 の初期値 (E / ν / α / β)。
  ★`ModalPostParams` と `BuildModes` の引数に ν を**入れない** (読んでいないことを型で示す)。
- `ImpactSynth.h:23` のコメント更新 (「モーダル経路は ModalSynth が別に持つ」)。
- `EditorMain.cpp` の selftest 連鎖末尾に `RunModalSynthSelfTest()`、PhysMatSelfTest に往復ケース追加。

## やらないこと (このサブでは)
ボクセル化、推論、ECS コンポーネント、AudioSourceSystem への接続、Inspector。`poissonRatio` をランタイムのスケーリングで読むこと (保持だけ。spec §2 #4)。

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Modal\ModalTypes.h`
- 新規 `src\Engine\Engine\Audio\ModalSynth.h/.cpp`、`ModalSynthSelfTest.h/.cpp`
- `src\Engine\Engine\Physics\PhysMatLibrary.h:33-67` (struct 末尾) / `.cpp:118` ToJson / `:140` FromJson / `:171` Sanitize / `PhysMatSelfTest.cpp`
- `assets\physmats\{carpet,glass,glue,gravel,ice,metal,rubber,steel,tile,water,wood}.physmat.json`
- `src\Engine\Engine\Audio\ImpactSynth.h:23`、`src\Editor\EditorMain.cpp` (selftest 連鎖)
- 参考: `ImpactSynth.cpp:77` `AddDampedSine` (アタック規約)、`ImpactSynthSelfTest.cpp:37` `RmsIn`
- ソース追加後 `pwsh -File tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
spec §5 の 1, 2, 3。
1. ModalSynthSelfTest: (1) 1 モード f=440, c=5, a=0.5 → 零交差周波数 ±1%、RmsIn [0.1,0.2]/[0.6,0.7] = e^{−2.5} ±5% / (2) a×2 → ピーク×2 / (3) 同入力 2 回 memcmp / (4) 長さ規則の両端 (0.05 / 2.0) / (5) `MelBandCenters` が double 期待値と 1e-2 Hz / (6) BuildModes のスケール則 (E×4 → f×2、ρ×4 → f/2 & a/2、L×2 → f/2 & a/2^1.5、相対 1e-4) / (7) mask 閾値で帯域が落ちる / (8) k=0 → count 0 / (9) 過減衰・ナイキスト超えが落ちる
2. PhysMatSelfTest: 4 フィールド (E / ν / α / β) の JSON 往復 + Sanitize (ν の上限 0.49 を含む) + キー無し旧 JSON → E/α/β = 0、ν = 0.3
3. replay_verify 全ペア不変 (PhysMat の読みは sim に入らないことの証明)

## 検証コマンド
- Debug ビルド (`/p:MyeWarnAsError=true`) → `cmd /c "bin\x64\Debug\Editor.exe --selftest"`
- `pwsh -File tools\check_rules.ps1`
- `tools\replay_verify.bat` (Release ビルドを含む)

## 実装メモ (coder が追記)

## フィードバック履歴
