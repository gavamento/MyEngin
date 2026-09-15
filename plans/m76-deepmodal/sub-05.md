# sub-05 (M76e): C++ 推論 / バックエンド抽象 / .msfm / ModalSoundLibrary / --modal-bake

- 依存: sub-02 (Voxelizer)、sub-04 (fixture)
- 状態: 未着手
- 往復: 0

## やること
spec §4.1「焼き」、§4.2 `.msfm` / `.dmnet`、§4.4 スレッド (ユーザー計画 Phase 11 / Checkpoint I)。

- `Modal/DmNet.h/.cpp`: `.dmnet` ローダ (フィールド単位読み、magic / version / 予算検査、`weightsHash` 再計算で照合)。
- `Modal/ModalInferenceBackend.h` (ユーザー決定 2):
  ```cpp
  class ModalInferenceBackend {
  public:
      virtual ~ModalInferenceBackend() = default;
      virtual const char* Name() const = 0;                 // "cpu" / "d3d11cs"
      virtual bool RunsOnWorkerThread() const = 0;          // CPU = true / GPU = false (immediate context はメインスレッド専用)
      virtual bool Prepare(const DmNet& net, std::string* err) = 0;   // モデル差し替えごと 1 回
      virtual bool Infer(const VoxelGrid& in, std::vector<float>& out192x4096, std::string* err) = 0;
  };
  ```
- `Modal/CpuModalBackend.h/.cpp`: im2col + 4×4 レジスタブロック GEMM (float、単一スレッド、SIMD なし)。ConvTranspose3d は「stride 散布 + 反転カーネルの Conv」。
- `Modal/ModalFeatureMap.h/.cpp`: `ModalFeatureMap` (spec §4.2)、`BuildFeatureMap(backend, net, grid, out)` (有効 cell の抽出 + fp16 化 + `cellSlot`)、`SerializeModalTable` / `DeserializeModalTable` (公開。`SerializeConvexTable` と同型)。
- `Asset/`: `assetkey::SourcePathForSubAssetKey(const std::string& meshName) -> std::wstring` を新設 (`ConvexCookSourcePath` の中身 = `ParseSubAssetKey` + `assetguid::ResolvePath` を移し、`ConvexCookSourcePath` は委譲。ビット中立)。
- `Modal/ModalSoundLibrary.h/.cpp`: `enum class ModalState { Missing, Baking, Ready, Failed, NoModel }`、
  `Init(RenderResources*)` / `SetBackend(unique_ptr<ModalInferenceBackend>)` / `LoadModel(path)` / `ReloadModel()` / `Request(AssetID) -> ModalState` / `Get(AssetID)` / `Header()` / `Pump()` / `Register(AssetID, ModalFeatureMap)` / `Clear()` / `Shutdown()` / `BakeSync(AssetID)`、
  `namespace modalsound { Install / Library / IsReady(AssetID) / Header() }`。ワーカーは `TextureLibrary::AsyncWorker` (GpuResources.cpp:696-820) と同型 (mutex + cv + deque、Shutdown で join)。
  ジョブは enqueue 時に positions/indices をコピーし `shared_ptr<const DmNet>` を掴む。ボクセル化は常にワーカー、推論は `RunsOnWorkerThread()` で場所が変わる (false なら `Pump()` で 1 フレーム 1 ジョブ)。`.msfm` の読み書きはメインスレッド (`CookedCache::ReadValidated` / `Write`、`.mcvx` と同じ)。
  `Request`: mesh 未登録 / positions 空 → `Missing` (キャッシュしない)。失敗 → `Failed` + WARN 1 回。`modelHash ≠ weightsHash` の entry はミス。
- 配線: `EngineLoop.cpp:260-274` の install ブロックに `modalSounds.Init(&resources); SetBackend(cpu or CLI); modalsound::Install(&modalSounds); LoadModel(ResolveDeepModalPath())`、`Pump()` は `audioSources.Update` (EngineLoop.cpp:2257) の**直前**、終了は `modalsound::Install(nullptr)` → `Shutdown()` を `convexcol::Install(nullptr)` (2530) の隣。
- `Platform/PathUtil`: `FindEngineDeepModalDir()` (単ルート、`FindEngineShaderDir` と同型)。呼び手で「プロジェクト assets\deepmodal → エンジン assets\deepmodal」。
- `HotReload/ReloadHub`: `ReloadKind::ModalNet`、`{ L".dmnet", ReloadKind::ModalNet, 6 }` (rank)。処理は `ReloadModel()` → `Clear()`。
- `EngineCli.cpp` の表に `--modal-backend <cpu|d3d11cs>` (`--particle-backend` と同型。未実装名は WARN + cpu、綴り違いは false = exit 1)。`EngineCliSelfTest` にケース。
- `src\Editor\ModalTools.cpp` に `RunModalBakeCli(projectDir)`: `.dmnet` を読み AssetDatabase のモデルをヘッドレス登録 → 各 prim を `BakeSync` → `.msfm`。1 行/メッシュ (`name state ms validCells`) + 合計 (`bakes= bakeMsAvg=`)。exit 0 / 1 / 2 (モデル無し)。`EditorMain.cpp` では連鎖の外で拾う (sub-02 と同じ場所)。
- `ModalSelfTest` に積み増し。selftest 連鎖に `RunModalSelfTest()` が無ければ足す。

## やらないこと (このサブでは)
`D3d11ModalBackend` の実装 (差し込み口だけ)、ECS / 接触 / 再生 (sub-06)、Inspector。

## 触る場所 (planner の見立て)
- 新規 `src\Engine\Engine\Modal\DmNet.h/.cpp`、`ModalInferenceBackend.h`、`CpuModalBackend.h/.cpp`、`ModalFeatureMap.h/.cpp`、`ModalSoundLibrary.h/.cpp`
- `src\Engine\Engine\Asset\` (AssetKey / CookedCache のどちらか。`kCookVersion` は触らない)、`Physics\ConvexColliderLibrary.cpp:31-41` (委譲)
- `src\Engine\Engine\EngineLoop.h/.cpp` (260-274 / 2257 / 2530)、`Platform\PathUtil.h/.cpp`、`EngineCli.cpp` / `EngineLoop.h` (config フィールド)、`EngineCliSelfTest.cpp`
- `HotReload\ReloadHub.h:55-70` / `.cpp:47-69`、`ReloadHubSelfTest.cpp`
- `src\Editor\ModalTools.cpp`、`EditorMain.cpp`
- 参考: `GpuResources.cpp:720` AsyncWorker、`RunFroxelVolumeProbe` (将来の GPU 読み戻しの雛形。今回は触らない)
- ソース追加後 `pwsh -File tools\gen_project_files.ps1`

## 受け入れ条件 (このサブ)
spec §5 の 11, 12, 13, 14。
1. ModalSelfTest: (1) `fixture.dmnet` + `fixture_in.mvox` → 64 cell × 192 で `max|Δ| < 1e-3` (**インストール済みバックエンドに対して回す**) / (2) Conv3d / ConvTranspose3d の小ケース (奇数 k、stride 2、pad、outPad) を素朴 6 重ループと 1e-6 で一致 / (3) `.msfm` 表の往復 memcmp / (4) Library: `Register` → `Get`、モデル無しで `NoModel`、`BakeSync` (fixture + 立方体) → `Ready` かつ validCount = voxel から独立に数えた有効 cell 数 / (5) `SourcePathForSubAssetKey("builtin://cube")` が空、guid キーで `ConvexCookSourcePath` と同結果
2. `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` (fixture モデルを `assets\deepmodal\deepmodal.dmnet` に**仮置き**、コミットしない) → 行数 = メッシュ数、2 回目は全部 cached。`CookedCacheSelfTest` 不変
3. **時間**: sub-04 の `--random-full` `.dmnet` を仮置きして `--modal-bake` → `bakeMsAvg ≤ 1500` (Release)。超過なら「不安・質問」に上げる (sub-04 の構造を縮める)
4. `--modal-backend d3d11cs` → WARN + `Name() == "cpu"`、`--modal-backend foo` → exit 1 (EngineCliSelfTest)
5. ReloadHubSelfTest: `.dmnet` → ModalNet / rank 6

## 検証コマンド
- Debug/Release ビルド → `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → 上の `--modal-bake` 2 種
- `pwsh -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
