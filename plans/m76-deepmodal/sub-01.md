# sub-01 (M76a): モーダル合成器と材質パラメータ

- 依存: なし
- 状態: OK (commit 34d59da)
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

### round 1

SELF_EVAL: sub-01 (round 1)
実装:
  - `src\Engine\Engine\Modal\ModalTypes.h` (新規) — `kModalVoxelN/kModalMapN/kModalBands/kModalChannels/kModalVoxelPad`、`ModalCellFeature`、`MaskCh`/`AmpCh`、`DmNetHeader` POD (spec §4.2 の順序どおり)、`MelBandCenters` (inline、mel(100..10000Hz) を 32 帯域に等分し区間中点の逆変換)
  - `src\Engine\Engine\Audio\ModalSynth.h/.cpp` (新規) — `ModalModeSet`/`ModalPostParams`、`kModalTailAmp=1/32768`、`kModalSampleRate=44100`、`BuildModes` (spec §4.1 手順 1-7 を 1 関数に、mask 閾値→log-amp 逆正規化→|k|加重合成→参照材質 λ→式12 のスケール則→目標材質の c/f→過減衰・ナイキスト・無音の棄却)、`ModalClipSeconds`、`ModalSynthRender` (2 次再帰共振器・double 状態・位相 0・1ms 線形アタック・tanh ソフトクリップ |x|>0.8・mono/44100/int16)
  - `src\Engine\Engine\Audio\ModalSynthSelfTest.h/.cpp` (新規) — 受け入れ条件 1 の (1)-(9) を全て実装 (零交差周波数・RMS 減衰比・振幅線形性・長さ規則両端・Mel の別経路 double 照合・E/ρ/L のスケール則 (相対 1e-4)・mask 閾値・k=0・過減衰/ナイキスト棄却)
  - `src\Engine\Engine\Physics\PhysMatLibrary.h` — `PhysMat` 末尾に `youngsModulus`(既定0) / `poissonRatio`(既定0.3、保持のみ) / `rayleighAlpha`(既定0) / `rayleighBeta`(既定0) を追加
  - `src\Engine\Engine\Physics\PhysMatLibrary.cpp` — `ToJson` は常に4本書く、`FromJson` は contains+既定 (前方互換)、`Sanitize` は E∈[0,1e13] / ν∈[0,0.49] / α∈[0,1e4] / β∈[0,1e-2]
  - `src\Engine\Engine\Physics\PhysMatSelfTest.cpp` — 前方互換 (旧JSON→既定値)・Sanitize (NaN/負値/ν上限0.49)・ToJson/FromJson 往復に4フィールド分のケースを追加
  - `assets\physmats\{metal,steel,glass,tile,wood,rubber,ice,carpet,gravel,glue,water}.physmat.json` — spec §4.2 の初期値 (E/ν/α/β) を追記 (carpet/gravel/glue/water は E/α/β=0, ν=0.3)
  - `src\Engine\Engine\Audio\ImpactSynth.h:23` 付近のコメント更新 (「モーダル経路は ModalSynth.h が別に持つ」を明記)
  - `src\Editor\EditorMain.cpp` — `ModalSynthSelfTest.h` を include、selftest 連鎖の末尾 (ReloadHubSelfTest の次、EngineCliSelfTest の手前) に `RunModalSynthSelfTest()` を追加
  - `pwsh -File tools\gen_project_files.ps1` を実行し `build\Engine.vcxproj` / `.filters` を更新 (新規4ファイルを追加)
仕様との差分:
  - なし。spec §4.1/§4.2 と sub-01.md の指示どおりに実装した。`ModalPostParams`/`BuildModes` の引数から ν を除外する指示も遵守 (型として ν を渡せない形にした)
検証:
  - `& $MSBUILD MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo /p:MyeWarnAsError=true` → 成功、警告0件 (既存の LNK4204 imgui pdb 警告のみ、無視対象)
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → exit 0、全緑。`ModalSynth self test: ALL PASS` (16件全PASS)、`PhysMat self test: PASS` を個別に確認
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` (`MYE_REPLAY_JOBS=3` に落として実行。既定12並列だとメモリ不足でホストのバックグラウンドタスクが kill された。ジョブ自体はエンジン側の並列度ではなく replay_verify.bat 側の並列実行数の環境変数なので挙動は不変) → Debug/Release 両ビルド成功、13 ジョブ全PASS (`demo/parts/flow/mp/physics/joints/acoustic/ui/ttdebug/ttrelease/rules/whatifrelease/whatifdebug`)、`[parallel] all 13 jobs passed in 302.4s`
自己採点 (1-5):
  仕様適合: 5 — spec §4.1 の手順順序・式・棄却条件、§4.2 の PhysMat 4 フィールドと初期値、sub-01.md の触る場所・やらないことを全て満たし、検証済み
  正しさ: 5 — selftest で零交差周波数・RMS減衰比・振幅線形性・スケール則 (相対1e-4)・mask閾値・過減衰/ナイキスト棄却・Mel のドリフト検知を実測して確認、replay_verify 全ペア (Debug/Release) 緑で sim 非干渉も確認済み
  コード品質: 5 — 純関数のみ (World/PhysMat非依存)、MYE_CHECK不要な範囲 (assert相当の分岐は無く、境界は全てclamp/continueで安全側)、日本語コメントで「なぜ」を記述、宣言時初期化、check_rules緑
  テスト: 5 — 受け入れ条件1の(1)-(9)全部・条件2 (JSON往復/Sanitize/旧JSON既定)・条件3 (replay_verify全ペア) を実装・実行し確認
不安・質問: なし
触ったファイル:
  - src/Engine/Engine/Modal/ModalTypes.h (新規)
  - src/Engine/Engine/Audio/ModalSynth.h (新規)
  - src/Engine/Engine/Audio/ModalSynth.cpp (新規)
  - src/Engine/Engine/Audio/ModalSynthSelfTest.h (新規)
  - src/Engine/Engine/Audio/ModalSynthSelfTest.cpp (新規)
  - src/Engine/Engine/Physics/PhysMatLibrary.h
  - src/Engine/Engine/Physics/PhysMatLibrary.cpp
  - src/Engine/Engine/Physics/PhysMatSelfTest.cpp
  - assets/physmats/metal.physmat.json
  - assets/physmats/steel.physmat.json
  - assets/physmats/glass.physmat.json
  - assets/physmats/tile.physmat.json
  - assets/physmats/wood.physmat.json
  - assets/physmats/rubber.physmat.json
  - assets/physmats/ice.physmat.json
  - assets/physmats/carpet.physmat.json
  - assets/physmats/gravel.physmat.json
  - assets/physmats/glue.physmat.json
  - assets/physmats/water.physmat.json
  - src/Engine/Engine/Audio/ImpactSynth.h
  - src/Editor/EditorMain.cpp
  - build/Engine.vcxproj (gen_project_files.ps1 の生成物。ソース追加に伴う機械更新)
  - build/Engine.vcxproj.filters (同上)
申し送り: sub-02 (ボクセライザ) は独立に着手可能。sub-05 (推論/ModalSoundLibrary) は本サブが定義した `DmNetHeader`/`ModalCellFeature`/`BuildModes`/`ModalSynthRender` をそのまま消費する想定 — フィールド順序・意味は spec §4.1/§4.2 のとおりに実装済みなので変更なしで使えるはず。`replay_verify.bat` を手元でフル並列 (既定12) で回すとホスト側のメモリ不足でプロセスごと kill されることがあるため、次回以降も `MYE_REPLAY_JOBS=3` 程度に落として実行することを推奨する (エンジン自体の挙動やテスト結果には影響しない、ホスト環境固有の制約)。

## フィードバック履歴
- round 1: **VERDICT: OK** (planner、2026-09-16)。planner が独立に再実行して確認: `Editor.exe --selftest` exit 0 (`ModalSynth self test: ALL PASS` / `PhysMat self test: PASS`)、`check_rules.ps1` 0 error、`cache\replay_logs\*.log` (01:54-01:57、再ビルド後) で 8 シーンペア全部 `VERIFY PASS`、tt/whatif/rules も正常終了。ソース照合: `BuildModes` は spec §4.1 手順 1-7 と同値 (2-4 をループ内で融合しているが意味は同じ)、式 12 のスケール係数 `λ *= σ1/(σ2σ3²)`・`a *= σ2^-½σ3^-3/2` 一致、再帰共振器 `y[n] = 2r cosθ·y[n-1] − r²·y[n-2]`、`y[1] = a·r·sinθ` (位相 0) 正しい。PhysMat 4 フィールドは順序・既定・Sanitize 範囲・11 本の JSON 値とも spec §4.2 どおり、`ModalPostParams` に ν が無い。nit (直さなくてよい、sub-08 が回収): `ImpactSynth.h:27` のコメントが `engine_spec §10.7` を参照しているが本文は sub-08 で書く。申し送り: `MYE_REPLAY_JOBS=3` 推奨はホスト環境の話なので台帳の申し送りへ。
