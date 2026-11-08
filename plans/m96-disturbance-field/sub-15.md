# sub-15: 地面の霧を局所ライトで照らす (ON/OFF の切り替え)

- 依存: sub-03 (波 W1。spec §6)
- 状態: 未着手
- 往復: 0


## 並列の注意 (波 W1)

- **司会が渡した git worktree の絶対パスでだけ作業する。** メインの作業ツリー (`C:\HAL\MyEngin`) には 1 バイトも書かない (W1 ではそこで sub-04 が動いている)。作業の最初と最後に、worktree と `C:\HAL\MyEngin` の両方で `git status` を取り、SELF_EVAL に貼る
- このサブが変えてよい共有資源 (spec 付録 A): `pad0` を `fogLocalLights` に改名 (サイズ不変、snapshot 版なし。末尾は触らない)。ライトは**新しい CB** (b8 以降の空きを調べて決め、共有ヘッダに書く)。規則 9 のグループ「地面の霧のライト」。デモは `AddFieldDemoLanterns`
- 付録 A に無い共有資源 (版・スロット・構造体の末尾・規則の番号・golden) を変えたくなったら、実装せずに「不安・質問」で返す
- テストは `DisturbanceFieldSelfTest.cpp` などの既存の関数に混ぜず、自分の名前の static 関数を足して、スイート関数の末尾から 1 行で呼ぶ。デモも自分の関数を足し、`BuildFieldShowcaseScene` の末尾に 1 行だけ足す
- 焼き込み値 (`kBakedFinalHash` など) は変えない (付録 A.2)。`build\Engine.vcxproj` は `tools\gen_project_files.ps1` の出力のまま

## やること

spec §2 #24 (ユーザー回答 2026-11-08: 照らす / 照らさないを ON/OFF で切り替える。太陽光の bool と同じ扱い)、§4.3 の「地面の霧」。
ランタン・松明の周りで地面の霧が光る絵を、GroundLayer の層ごとに選べるようにする。

1. 欄: `DisturbanceFieldComponent` に `fogLocalLights` (bool、場ごと、既定 **false**)
   - 置き場所: sub-01 で入れた明示の詰め物 `pad0` (`fogSunLight` / `autoFroxel` / `drawDebug` の次、2026-11-08 に存在を確認) を `fogLocalLights` に改名する。サイズ不変なので snapshot の版は上げない (spec 付録 A.1)。末尾は触らない (W1 で sub-04 が末尾に欄を足している)
   - コンポーネントの生バイトが変わるので、場を含む焼き込みハッシュ (`DisturbanceFieldSelfTest` の `kBakedFinalHash` など) は**再採取してよい** (sim の結果は変わらない。再採取した値と理由を SELF_EVAL に書く)
   - Volume では無視 (Volume はフロクセルが局所ライトのビームと影を既に持つ)。Inspector に注記
2. ライトの選び方 (CPU、描画レーン): `SceneLightData` の局所ライト (点・スポット、`type != 0`) のうち、影響球 (位置・range) が `fogLocalLights` の層のどれかの板の AABB と交わるものを、**ライト一覧の順**に最大 `kGroundFogMaxLights = 8` 本まで選ぶ。超えた分は落とし、WARN をシーンにつき 1 回 (落とした本数)。選び方は毎フレーム同じ入力から同じ結果になる
3. 10 か所のシェーダへの渡し方: 選んだライトの**縮約した写し** (位置・range・色 × 強度・スポットの向きと cosInner / cosOuter・種類) を、sub-02 / sub-03 の場の CB に載せる (8 本 × 64 バイト程度)。各シェーダが持つ既存のライト一覧の並びには依存しない (粒子・VFX・スカイなど、ライト一覧を持たない合成箇所もあるため)。CB のバイト数と最大本数は C++ と HLSL で一致させ、規則 9 に入れる
4. 計算: `GroundFogComposite` の段ごとの散乱に、`fogLocalLights` の層の σ_s × ライトの放射輝度 × HG(g) × 減衰を足す。減衰とスポットの式は `common.hlsli::LightSample` (フロクセルの注入が使っているもの) と**同じ式**にする (霧と面で光の届き方が食い違わないため)。HG の g は sub-03 と同じ値
   - **影なし** (局所ライトの影 = シャドウアトラスは使わない)。理由: 10 か所のシェーダすべてにアトラスを束縛する必要があり、段数 24 × ライト本数 × 影サンプルのコストになる。影つきのビームが要る場面は Volume (フロクセル、影つき) で作る
   - 速くするため、線分とライトの影響球が交わらないライトは段のループの前に外す (ピクセルごと)
5. Inspector の説明: 照らすライトは最大 8 本 (超えた分は使われない)、影なし、ON にすると GPU のコストが増える (`Tr()` 両言語)
6. デモ: `--field-demo` の末尾に点光源のランタン 2〜3 個を霧の中に置き、層の `fogLocalLights` を true にする

## やらないこと (このサブでは)

- 局所ライトの影、9 本目以降のライト
- Volume (フロクセル) の局所ライトの扱いの変更

## 触る場所 (planner の見立て)

- `src\Engine\Core\Ecs\Components.h` / `.cpp` (欄と説明)、必要なら `SimSnapshot.h`・`AcousticAudioSelfTest.cpp`
- `src\Engine\Engine\Disturbance\DisturbanceRender.*` (ライトの選び方と CB への詰め替え)、`src\Engine\Renderer\Passes\DisturbanceFieldPass.*`
- `assets\shaders\disturbance_common.hlsli` (`GroundFogComposite` と CB)、`common.hlsli` の `LightSample` (読むだけ)
- `tools\check_rules.ps1` 規則 9
- `src\Engine\Engine\Demo\DemoContentField.cpp` の末尾

## 受け入れ条件 (このサブ)

spec §5 の 12, 30, 35。

- 35:
  - (a) `fogLocalLights = false` の絵が切り替え前 (sub-14 のコミット) と `--img-diff` で差 0
  - (b) true でランタンの周りの霧が明るくなる (ランタンの近くと遠くの霧の平均輝度を報告)。ランタンが霧の層から離れていれば (影響球が板と交わらない) 絵が変わらない
  - (c) 9 本置くと 8 本だけ使われ、WARN が 1 回 (ログ)
  - (d) 1920×1080 で層 1 枚・ライト 0 / 1 / 8 本の GPU 時間を報告 (合否の線は置かず、planner が最大本数の既定を判断)
- 12: replay_verify `field` (運用によりまとめて実行)

## 検証コマンド

> selftest と replay_verify の運用 (spec §5 冒頭、ユーザー指示 2026-11-08): このサブでは selftest (`--selftest` / `--selftest-full` / `MYE_SELFTEST_ONLY`) と `tools\replay_verify.bat` を**実行しない**。ビルドと check_rules (と、スクショなど selftest / replay_verify 以外の検証) までを行い、selftest / replay_verify で確かめる条件は「未実行 (W2 の統合後 / sub-17 でまとめて実行)」と報告する。selftest のコードは書く。

- ビルド: Debug / Release
- スクショ一式 (Runtime.exe、Forward と Deferred)、`Editor.exe --img-diff`
- `tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL sub-15 (round 1) は司会への返信に記載 (実装・差分・検証・採点)。中断メモは下の通り。

## フィードバック履歴

## 中断メモ (2026-11-08)

worktree: C:\HAL\MyEngin\.claude\worktrees\agent-ad788ccedd6a6eede / ブランチ: worktree-agent-ad788ccedd6a6eede (起動時 aa3288e だったため、クリーンな状態で `git reset --hard b7cb1fa` して master 先頭に合わせた。変更は全て未コミット)

終わったこと:
- Components.h/.cpp: pad0 -> fogLocalLights (bool、サイズ 76 不変)、反射登録、tooltip (fogSunLight の「局所ライト未対応」を削除)
- DisturbanceFieldPass.h/.cpp: kGroundFogLightCbSlot = 8、kGroundFogMaxLights = 8、GroundFogLightCB (64 B) / GroundFogLightsCB (528 B)、SelectGroundFogLights (純粋関数)、DisturbanceFieldGpu.localLightMask、BindGroundFog は b6-b8 の 3 本を張る / Unbind、超過 WARN (超過が続く間 1 回)
- DisturbanceRender.cpp (fogLocalLights を upload へ)、RenderSystem.cpp (gf.lights = &f.lights)
- ground_fog.hlsli: b8 cbuffer、GfogLightMask、GfogLocalLightRadiance (LightSample と同式)、GfogStepRadiance に局所ライト項 (firstbitlow ループ)
- tools\check_rules.ps1 規則 9 に 2 エントリ (スロットと最大本数。1 グループに 2 整数は入らないため 2 つ)
- DisturbanceFieldSelfTest.cpp: RunDisturbanceLocalLightTests (未実行)
- DemoContentField.cpp: AddFieldDemoLanterns + 末尾に呼び出し 1 行
- Release ビルド OK。スクショ (OFF / ON) 撮影済み: scratchpad\m96-shots\sub15_01_off_t100.png、sub15_02_on_i30_t100.png、sub15_03_on_i120_t100.png (ON でランタン周りが明るくなることを確認)

途中のこと:
- DemoContentField.cpp に一時の環境変数改造 (`// S15TEMP`、MYE_S15_N / _FLAG / _INT と #include <cstdlib>) が入ったまま。正規版は scratchpad\sub15\DemoContentField.final.cpp に退避済み (これで上書きして戻し、ランタン強度は 30 -> 80 前後に上げる予定)
- (a) の比較: main の旧 Release Runtime のコピー (scratchpad\sub15\Runtime_baseline.exe) は DLL 不足で起動せず (0xC0000135)。新 exe のランタン無し・flag off の絵 (sub15_04_*) は撮れている。比較対象の旧絵は別の手段が要る (旧 exe を worktree の bin\x64\Release へ一時的に置く、など)
- 未実施: (c) 9 本で WARN 1 回の確認、(d) ライト 0 / 1 / 8 本の GPU 時間 (--render-stats-dump、実 GPU、1080p。デモには既存の Lamp が 1 本あるので本数は Lamp + N)、Forward / Deferred 両方、Debug 再ビルド (最後のビルドは firstbitlow 書き換え・デモ改造より前)、check_rules、SELF_EVAL
- selftest / replay_verify は運用により未実行

次にやること: 上の途中を片付け -> 一時改造を戻す -> Release / Debug 再ビルド -> check_rules -> SELF_EVAL

触ったファイル (worktree 内、絶対パスの基点は上): src\Engine\Core\Ecs\Components.h / Components.cpp、src\Engine\Renderer\Passes\DisturbanceFieldPass.h / .cpp、src\Engine\Engine\Disturbance\DisturbanceRender.cpp / DisturbanceFieldSelfTest.cpp、src\Engine\Engine\Rendering\RenderSystem.cpp、src\Engine\Engine\Demo\DemoContentField.cpp、assets\shaders\ground_fog.hlsli、tools\check_rules.ps1、plans\m96-disturbance-field\sub-15.md
