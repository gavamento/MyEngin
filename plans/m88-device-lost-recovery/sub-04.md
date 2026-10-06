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
7. (sub-03 から移管) surface マテリアル (`*.surface`) を使うシーンで復旧後に描画が一致し、`perMaterialGpuCB` が作り直されている。SelfTest でもスクショ比較でもよい。
8. (sub-03 から移管) メッシュの作り直しに失敗した (vb/ib が null) ときに、Forward / Deferred / 影 / ピッキング以外のエンジン描画経路が落ちないことをコードで確認する。必要なら null を読み飛ばす処理を足す。

## 検証コマンド
- MSBuild Debug|x64 / Release|x64
- `Runtime.exe <scene> --simulate-device-lost 30 [--warp] --screenshot ...` と消失なしの同条件、`Editor.exe` の img-diff で比較
- replay の検証再生 + `--simulate-device-lost`
- `Editor.exe --selftest` (Debug/Release)、`tools\check_rules.ps1`、`tools\replay_verify.bat`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要点)
- 新規の所有者: `ComputeAbiRunner::ReleaseGpu / RecreateGpu` (ハンドル・世代・count/stride/flags を残し、生きているスロットを同じハンドル・ゼロ内容で作り直す。ABI 変更なし)。`ProbeBaker::ReleaseGpu` (専用 RenderSystem・IBL・深度)。EngineLoop の RecoverDevice が computeAbi / probeArray / probeBaker を扱う (プローブは `--probe-bake-all` で焼いていたときだけ復旧後に BakeAll し直す。ディスクへは書かない)。FroxelPass / RtPasses の `Shutdown` で GpuTimer を Release。ProjectComputeRunner / ProjectEffectRunner は sub-02 の `RenderSystem::ReleaseGpu` で既定構築へ差し替え済みで、fxstack は `loadedFxStackId_` クリアにより次フレームで読み直される (確認済み)。
- 実シーンで見つかった不具合 2 件を直した: UIRenderer / VfxRenderer の `Shutdown` が `vbCapacity_` を 0 に戻さず、復旧後に vb_ 無しで Map して d3d11 内で AV (UI を持つシーン = ui_probe / flow_title / flow_game で再現、Release / Debug とも)。VfxRenderer::Shutdown が CPU 側の `trails_` 履歴まで消していたので、復旧をまたいで残すようにした (fog デモのトレイルがスクショ不一致になる原因だった)。
- 受け入れ 8: `MeshLibrary::GetDrawable` (vb / ib が無ければ nullptr) を新設し、Renderer/Passes と Pipeline の描画側 (Forward / Deferred / 影 (Atlas 含む) / ピッキング / 地形) の `meshes.Get` をこれへ置換。Water / Ghost は元から vb/ib を見ていた。RtScene は CPU データ参照で GPU メッシュを使わない。Editor 側の描画 (SceneView 等) は sub-05。
- R3: `Shared/EngineAPI.h` に compute バッファの読み戻し API は無い (Create / Release / Set* / Dispatch のみ) ので、sim へ戻る経路は無い。
- 検証 (Release、Runtime、--warp、960x540、--no-fxaa、frames 130 / shot 120、`--simulate-device-lost 30` と無し、img-diff tol=0):
  - 一致 (maxDiff=0): forward / deferred / render-demo deferred / terrain deferred / ssr / physics / parts / flow_title / flow_game / ui_probe / joints / particle_cpu / acoustic deferred / ui_widgets / fracture / nav / perception / bt。`--froxel-no-temporal` 付き froxel、`--rt-no-temporal` 付き rt-refl / rt-gi も一致。
  - 履歴依存で不一致 (想定どおり): taa (maxDiff 1, 11505 px = TAA 履歴)、froxel / rt-refl / rt-gi の時間フィルタ有り (maxDiff 1〜、24〜1508 px)、particle_gpu / fog (GPU 粒子の現在状態が消える。spec 通り)。
  - 未解決: `--render-demo --deferred --probe-bake-all` で、復旧後に BakeAll し直すと不一致 (maxDiff 17〜21、123〜198 px。forward は一致)。ベイクを復旧後 (frame 40) にすると一致するので、「同じ実行で BakeAll を 2 回走らせると deferred の絵が変わる」ベイカー側の既存の性質の可能性が高い (原因は未特定)。CLI の one-shot 診断でしか通らない経路。
  - 受け入れ 1: demo / 三校 main.scene.json を HW / `--warp` で `--simulate-device-lost 30,60` → 2 回とも復旧、World ハッシュ unchanged=1、exit 0。`30,60,90` で 3 回目は Fatal (exit 6)。
  - 受け入れ 4: `tools\replay_verify.bat` 17 job 全 PASS。`MYE_EXTRA_ARGS=--simulate-device-lost 30` 付きは 13 job PASS、4 job (time-travel / what-if の Editor.exe) は旧デバイス参照 23 でゲート不合格 = Editor 側の復旧 (sub-05) 待ちで想定どおり。`--replay-verify` の実走は 600 tick を最初の数フレームで消化するので、frame 30 では疑似消失が発火しない → `--simulate-device-lost 1` で Runtime.exe の golden*.rep 10 本 (parts / flow は cache の古い rep でフラグ無しでも tick 0 不一致のため除外) が復旧しつつ VERIFY PASS。
  - 受け入れ 5: ComputeAbiSelfTest に復旧ブロックを追加 (WARP: ReleaseGpu → RecycleDevice (ゲート合格) → RecreateAll / RecreateGpu → 同じハンドルで SetComputeBuffer / DispatchCompute 成功)。ABI 定義の差分なし。
  - 受け入れ 7: DeviceRecoverySelfTest に `CheckSurfaceMaterialRecovery` を追加 (WARP): `*.surface` マテリアルの perMaterialGpuCB が ReleaseGpu で消え、復旧後に作り直され、読み戻した CB の中身と CPU 側のパック済みバイト列が復旧前と一致。同テストに「vb が無いメッシュは GetDrawable が nullptr」も追加。実シーンのスクショ (`*.surface` を使うシーン) はリポジトリに無く未実施。
  - `Editor.exe --selftest` Debug / Release → exit 0。`tools\check_rules.ps1` → 0 error。

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ 6 は「履歴無効の条件で tol=0」で成立。受け入れ 7 は SelfTest で代替。受け入れ 10 の Editor 分 4 job は sub-05 へ移した。probe-bake-all の deferred の不一致は既知の差 K1 (範囲外) とした。
