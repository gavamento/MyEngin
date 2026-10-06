# sub-04: エンジン層の残りの GPU 所有者と Runtime の復旧完成

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること

注 (2026-10-07、sub-02 の前倒しを反映): UI/VFX/粒子の GPU 側、RenderSystem の遅延パス群・RT・フロクセル・IBL・ユーザーポスト、組込みメッシュ/White は sub-02 で復旧対象に入った。このサブで新しく入れるのは、compute runner (ComputeAbiRunner。今は `computeAbi.Shutdown` を呼んでいない)、ProjectComputeRunner / ProjectEffectRunner、ProbeBaker / probeArray / EnvMapBaker、FroxelPass / RtPasses の GpuTimer の取りこぼし。前倒しした分は、下の一覧のうち「実シーンで動くことの確認」(受け入れ 1〜5) だけを行う。Deferred を使うシーンで描画を確認することも含める。
Engine 層で GPU を持つ残りの所有者を復旧手順に加え、Runtime.exe (エディタ UI 無し) で代表シーンが復旧するところまで閉じる。

対象 (planner の棚卸し。`ComPtr<ID3D11` を持つヘッダから。coder は漏れを確認する — 漏れはゲートが教える):
- `src\Engine\Engine\UI\UIRenderer.*`、`src\Engine\Renderer\Text\FontAtlas.*`
- `src\Engine\Engine\Vfx\VfxRenderer.*`
- `src\Engine\Engine\Particles\GpuParticleBackend.*` / `CpuParticleBackend.*` (GPU 粒子の現在状態は失われてよい。エミッタは初期状態から再発生。sim 側の粒子 RNG ストリームには触れない — memory: demo-entity-order-is-rng-stream)
- `src\Engine\Engine\RayTracing\RtScene.*`、`src\Engine\Renderer\RayTracing\RtPasses.*` (BLAS/TLAS 相当は再構築)
- `src\Engine\Engine\Rendering\ProbeBaker.*`、`src\Engine\Renderer\Passes\EnvMapBaker.*` (シーン読み込み時と同じ扱いで再ベイク / ディスクから再読込。ベイク結果をディスクへ書き戻す経路を復旧で走らせないこと)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` が持つキャッシュ (MeshInstancing 等)
- `src\Engine\Renderer\Passes\TaaPass.*` (履歴は破棄 = 初回フレーム扱い)
- `src\Engine\Renderer\Compute\ComputeAbiRunner.*`、`ProjectComputeRunner.*`、`src\Engine\Renderer\PostFx\ProjectEffectRunner.*`: compute ハンドルは**同じ値・同じ desc**で作り直し、中身はゼロ。ABI は変えない (v27 = 158)。
- 復旧中にホットリロード (`ReloadHub`) が走らないこと (セーフポイントの順序で保証)

R3 の確認: GameLogic / C# が compute の結果を sim へ読み戻していないか (`ReadComputeBuffer` 相当の API があれば呼び出し側を確認)。読み戻しがあれば既存の決定性問題として planner へ報告 (直さない)。

## やらないこと (このサブでは)
- エディタの RT・プレビュー・ImTextureID・メニュー (sub-05)。

## 触る場所 (planner の見立て)
上の一覧 + `C:\HAL\MyEngin\src\Engine\Engine\Loop\EngineLoop.cpp` の復旧手順 (L332 `particleSystem.Init`、L468-469 `SetComputeAbi`、L2837-2888 の probeBaker 周辺)。

## 受け入れ条件 (このサブ)
1. (spec 5 Runtime) 代表シーン (demo シーン + 三校の代表シーン 1 つ) で `Runtime.exe --simulate-device-lost 30` が復旧、ゲート合格、0 終了。HW と `--warp` の両方。
2. (spec 6) 消失あり/なしのスクショ一致 (TAA・GPU 粒子など履歴依存を無効にした条件。許容差と理由を記録)。
3. (spec 7) 代表シーンで 2 回復旧、3 回目で Fatal。
4. (spec 10) 既存の rep を検証再生する実行に `--simulate-device-lost` を足して一致。`tools\replay_verify.bat` 一致。
5. (spec 11) compute を使うシーン/テストで復旧後の Dispatch がエラーなく動く。ABI 定義の差分なし。
6. (spec 14) selftest (Debug/Release)、check_rules、replay_verify。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Runtime.exe <scene> --simulate-device-lost 30 [--warp] --screenshot ...` と消失なしの同条件、`Editor.exe` の img-diff で比較
- replay の検証再生 + `--simulate-device-lost`
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

## フィードバック履歴
