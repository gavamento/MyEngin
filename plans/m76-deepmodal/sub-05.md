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

SELF_EVAL: sub-05 (round 1)
実装:
  - `src\Engine\Engine\Modal\DmNet.h/.cpp` (新規) — `.dmnet` ローダ。フィールド単位で
    256B ヘッダ + op 表 (48B/エントリ) を読み、(1) magic/version (2) paramCount ≤
    2,000,000 の予算 (3) 全 op の weightOffset/biasOffset がファイル範囲内 (4) 重み+
    バイアス blob の FNV-1a 64bit が `header.weightsHash` と一致、の 4 点を検証してから
    `DmNet{ header, ops, bytes }` を返す (失敗はどれも false + *err、struct を
    memcpy しない)。`DmNetOpWeightFloat` (fp16→float 展開 + fp32 バイアスの取り出し)、
    `DmNetFnv1a64` (export.py の `fnv1a64` と同一定数) を公開。
  - `src\Engine\Engine\Modal\ModalInferenceBackend.h` (新規) — sub-05.md 記載のインター
    フェースをそのまま実装 (`Name`/`RunsOnWorkerThread`/`Prepare`/`Infer`)。
  - `src\Engine\Engine\Modal\CpuModalBackend.h/.cpp` (新規) — im2col + 4x4 ブロック GEMM
    による Conv3d/ConvTranspose3d (`Conv3dRaw`/`ConvTranspose3dRaw`、テスト用に低レベル
    API として公開)。ConvTranspose3d は「stride 散布 (dilate) + 前後パディング
    (k-1-pad / k-1-pad+outPad) + カーネル反転 + stride1/pad0 の Conv3dRaw」で実装
    (spec のヒントどおり)。`CpuModalBackend::Prepare` が全 op の重みを fp16→float へ
    一度だけ展開して保持し、`Infer` は op 表をそのまま辿るだけの薄い実行器 (buffer
    数はヘッダの `bufferCount`、無ければ ops から復元する保険つき)。
  - `src\Engine\Engine\Modal\ModalFeatureMap.h/.cpp` (新規) — `ModalFeatureMap` 構造体
    (version/modelHash/frame/validCount/cellSlot[4096]/feat)、`BuildFeatureMap`
    (backend.Infer → `modal::BuildCellSlotTable` で有効 cell 抽出 → fp16 へ詰める。
    有効性の規則は `cellSlot[cell]==cell` = 占有 voxel を含む cell、sub-04 の
    `_cell_valid_from_occupancy` と同じ)、`Serialize/DeserializeModalTable`
    (`.mcvx` と同型の key 昇順テーブル)。`ModalFeatureMap::RowOf`/`CellFeature`
    ([追加]、下記) も追加。
  - `src\Engine\Engine\Modal\ModalSoundLibrary.h/.cpp` (新規) — `ModalState` enum、
    `Init/SetBackend/SetBackendByName/LoadModel/ReloadModel/Request/Get/Header/Pump/
    Register/Clear/Shutdown/BakeSync`。ワーカーは `TextureLibrary::AsyncWorker` と
    同型 (mutex+cv+deque)。ボクセル化は常にワーカー、推論は
    `backend->RunsOnWorkerThread()` で分岐 (CPU=true は同じワーカースレッドで
    `BuildFeatureMap` まで完了、false は `Pump()` が 1 フレーム 1 ジョブでメイン
    スレッド推論を仕上げる — GPU 未実装なのでこの経路は選択テストのみ)。
    `.msfm` の読み書き (`LoadTable`/`SaveTable`/`UpdateTableEntry`) は `.mcvx` と
    同じ `CookedCache::ReadValidated/Write` を叩く。`namespace modalsound` (Install/
    Library/IsReady/Header) と `ResolveDeepModalPath` (プロジェクト→エンジンの
    2 ルート) も同ファイル。
  - `src\Engine\Core\AssetKeyResolver.h/.cpp` — `assetkey::SourcePathForSubAssetKey`
    を新設 (`ParseSubAssetKey` + `assetguid::ResolvePath` の中身をここへ 1 本化)。
  - `src\Engine\Engine\Physics\ConvexColliderLibrary.cpp` — `ConvexCookSourcePath` を
    `assetkey::SourcePathForSubAssetKey` への委譲に変更 (ビット中立、`AssetGuidResolver.h`
    の直接 include を削除)。
  - `src\Engine\Platform\PathUtil.h/.cpp` — `FindEngineDeepModalDir()` (`FindEngineShaderDir`
    と同型、単ルート)。
  - `src\Engine\Engine\HotReload\ReloadHub.h/.cpp` — `ReloadKind::ModalNet` を末尾に
    追加、`{ L".dmnet", ReloadKind::ModalNet, 6 }`、`ReloadModalNet` (パスの
    突き合わせはしない — 1 プロジェクト 1 `.dmnet` 前提。`modalsound::Library()->
    ReloadModel()` → `Clear()`)。`ReloadHubSelfTest.cpp` にケース追加。
  - `src\Engine\Engine\EngineCli.cpp` — `--modal-backend <cpu|d3d11cs>` (綴りだけ検査、
    実際の cpu 縮退は `ModalSoundLibrary::SetBackendByName` の役目)。
    `EngineCliSelfTest.cpp` に 5 ケース追加。
  - `src\Engine\Engine\EngineLoop.h/.cpp` — `EngineConfig::modalBackendName`
    (既定 `L"cpu"`)。`convexColliders` の隣に `ModalSoundLibrary modalSounds` を追加し、
    Init→SetBackendByName→Install→LoadModel の順で配線 (260 行台)、`Pump()` を
    `audioSources.Update` の**直前**に追加 (2257 行付近)、`modalsound::Install(nullptr)`
    → `Shutdown()` を `convexcol::Install(nullptr)` の隣に追加 (2530 行付近)。
  - `src\Editor\ModalTools.h/.cpp` — `RunModalBakeCli(projectDir)`: assets を再帰走査して
    `.fbx/.glb/.gltf` をヘッドレス登録 (`AssetDatabase::ScanAndSync/InstallAsKeyResolver`
    経由、`SubAssetMigration.cpp::RunMigration` と同じ手順) → 登録済み全メッシュを
    `BakeSync` → 1 行/メッシュ (`name state ms validCells`) + 合計行
    (`models= bakes= bakeMsAvg=`)。`CookedCache::Configure` の cookedDir 二経路は
    `EngineLoop.cpp` の起動配線と同じ式。
  - `src\Editor\EditorMain.cpp` — `--modal-bake` を `--modal-voxelize` と同じ「連鎖の
    手前」で拾って `RunModalBakeCli(projectDir)` へ (`--project` は既存の連鎖内で
    先に処理されるので、ここに来る時点で確定済み)。
  - `src\Engine\Engine\Modal\ModalSelfTest.cpp` — 6 節・44 チェックを追加
    (Conv3dRaw/ConvTranspose3dRaw を素朴な 6 重ループ参照実装と 1e-6 で照合 [計 5 ケース、
    奇数 k・stride1/2・pad・outPad]、fixture.dmnet+fixture_in.mvox の実推論を
    `fixture_out.bin` と 1e-3 で照合 [実測 max|Δ|=4.77e-07]、`.msfm` 表の往復
    memcmp + 破損拒否、`RowOf`/`CellFeature`、`ModalSoundLibrary` の
    Register/Get/Clear/NoModel/BakeSync(fixture+builtin cube)/失敗経路、
    `SourcePathForSubAssetKey` の builtin 空・guid 委譲一致、`SetBackendByName` の
    cpu/d3d11cs 縮退/綴り違い)。

仕様との差分:
  - [確認] sub-04 が `[追加]` として仮置きした 3 点を、実装・実測の両方で確定させた:
    (1) 256B ヘッダ + 9×uint32 reserved は `layout.py` の `DMNET_HEADER_FMT` と
    1 バイトも違わず読める (2) `weightOffset`/`biasOffset` はファイル先頭からの
    絶対オフセットとして実装し、fixture.dmnet で正しく解決できた (3) cell 有効性は
    ボクセル占有のみ (`cellSlot[cell]==cell`) で `_cell_valid_from_occupancy` と
    同じ規則にした。fixture 推論が `fixture_out.bin` と max|Δ|=4.77e-07 で一致した
    ことが、この 3 点がすべて Python 側と食い違っていないことの直接証拠になっている。
  - [追加] `ModalFeatureMap::RowOf`/`CellFeature` — spec/sub-05.md は「有効 cell だけを
    詰める」としか書いておらず、生の cell index から `feat` 内の行を引く手段が
    無かった。`cellSlot` (最寄り有効 cell の**生 index**) と `feat` の格納順
    (cell index 昇順) の間には rank 計算が要るので、sub-06 が接触点から音を
    引く際に必要になると判断して追加した (O(4096) の線形カウント、呼び出し頻度は
    衝突 1 件につき高々 1 回)。
  - [追加] `ModalSoundLibrary::SetBackendByName(name)` — 抽象クラス自体には無い
    ファクトリ関数。CLI (`--modal-backend`) の受け皿と、d3d11cs→cpu の WARN 付き
    縮退をライブラリ側の 1 箇所に閉じ込めるために追加した。
  - [追加] `EngineConfig::modalBackendName` を `std::wstring` にした (`particleBackendOverride`
    は int -1/0/1 だが、Deep-Modal は永続化の概念が無く将来バックエンド名が増える
    可能性があるため、CLI の綴りをそのまま持たせた方が単純と判断した)。
  - [追加] `ReloadHub::ReloadModalNet` はパスの突き合わせをしない (PhysMat の
    `Contains(path)` に相当する判定が無い)。1 プロジェクトに `.dmnet` は
    `assets\deepmodal\deepmodal.dmnet` の 1 本だけという前提を置いた設計判断
    ([追加])。将来複数 `.dmnet` を持つ想定が出たら見直しが要る。
  - [追加] `kDmNetMaxParamCount` (2,000,000、`model.MAX_PARAM_COUNT` と同値) を
    `check_rules.ps1` の `$constGroups` へは**登録していない** — spec §4.2 が明示
    登録を求めるのは 4 組 (kModalVoxelN 等) だけで、この定数は含まれていない。
    ドリフトすれば `LoadDmNet` が安全側 (拒否) に壊れるだけなので実害は小さいと
    判断したが、two-language 定数の重複ではあるので「不安・質問」にも記載する。
  - [実装中に発見・修正した実バグ] `BakeSync` が `.msfm` の cook テーブルを**確認せずに
    毎回焼き直す**実装になっていた (`Request()` は表を先に見るが `BakeSync()` は
    見ていなかった)。`--modal-bake` を 2 回連続実行して確かめる中で発覚
    (1 回目 176.6ms/mesh、2 回目も同じ 176-200ms/mesh = 全くキャッシュが効いて
    いなかった)。`BakeSync` の先頭で `cache_` → `.msfm` 表 (`modelHash` 一致) の順に
    見るよう修正し、再検証で 2 回目 0.38ms/mesh まで落ちることを確認した
    (下記「検証」参照)。sub-05.md 自身の受け入れ条件 #2 (「2 回目は全部 cached」) が
    無ければ見逃していた欠陥で、実測で確かめる価値を再確認した。

検証:
  - `pwsh -File tools\gen_project_files.ps1` → Engine.vcxproj(.filters) に新規 5 ファイル
    (DmNet/ModalInferenceBackend/CpuModalBackend/ModalFeatureMap/ModalSoundLibrary) が
    追加されたことを確認 (Editor/Runtime/GameLogic は既存ファイルの変更のみで
    リストは不変)
  - Debug ビルド (`/p:MyeWarnAsError=true`) → 0 エラー / 0 警告 (LNK4204 の imgui pdb
    のみ、既知で無害)
  - Release ビルド (同上) → 0 エラー / 0 警告
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → **exit 0**、全チェイン緑。
    `Modal (Voxelizer) self test` 節だけで **54 件 ALL PASS**
    (うち本サブ追加分は上記「実装」参照の 44 件)、`Engine CLI self test` /
    `ReloadHub self test` / `CookedCache self test` も含め全体で `FAIL:` 行 0 件
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `--modal-bake` 手動検証 (Release、fixture.dmnet [widths 4/8/8/8] を
    `assets\deepmodal\deepmodal.dmnet` へ仮置き、`bin\x64\Release\cache` を削除してから):
    1 回目 `models=19 bakes=382 bakeMsAvg=176.63` (全メッシュ Ready) →
    2 回目 (バグ修正後) `bakeMsAvg=0.38` (全 382 件が `.msfm` キャッシュ命中)。
    修正前は 1 回目 178.88ms / 2 回目 200.10ms とほぼ同じで、キャッシュが効いて
    いないことを確認してから直した (上記「仕様との差分」参照)
  - `--modal-bake` 時間計測 (Release、`python export.py --random-full` の基準構成
    [widths 16/32/64/96、paramCount=1,682,448] を仮置き、キャッシュ削除後の初回):
    実測 **約 4.75〜5.34 s/メッシュ** (n≈130 サンプルを採取した時点で強制終了。
    値は mesh の validCells に依らずほぼ一定 — 32³ 稠密ボリュームを畳み込むコストが
    支配的で疎な occupancy を利用できていないことの裏付け)。
    **受け入れ条件 #3 (bakeMsAvg ≤ 1500ms) を約 3.1〜3.6 倍超過**。
    sub-05.md 自身の指示どおり「不安・質問」へ上げる (下記)
  - fixture / random-full の一時配置は検証後に削除済み (`assets\deepmodal\` は
    リポジトリに残していない。`git status` で確認済み)

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1・2・4・5 はすべて満たした。条件 3 (時間、
    `bakeMsAvg ≤ 1500`) は基準構成の乱数重みで**約 3.1〜3.6 倍超過**しており未達
    (sub-05.md 自身が用意した「不安・質問」への escalation 経路をそのまま使う)。
    それ以外の設計判断はすべて sub-04 の `[追加]` 仮置きと整合させ、実測で確認済み
  正しさ: 5 — Conv3d/ConvTranspose3d を独立実装の素朴参照 (5 ケース、奇数 k・
    stride1/2・pad・outPad) と 1e-6 で照合、fixture の実推論を Python 生成の
    `fixture_out.bin` と 1e-3 (実測 4.77e-07) で照合、`.msfm` 往復 memcmp、
    `BakeSync` の validCount を `BuildCellSlotTable` の独立カウントと突き合わせ、
    `--modal-bake` を実機で 2 回走らせてキャッシュ挙動を確認 — と、あらゆる主張を
    実行結果で裏取りした。過程で自分の実装のバグ (BakeSync がキャッシュを見ない)
    を実測から発見し修正できたことも含め、5 とする
  コード品質: 4 — 既存の流儀 (AsyncWorker / CookedCache / .mcvx 表) を踏襲し、
    `ConvexCookSourcePath` の重複ロジックを `SourcePathForSubAssetKey` へ一本化した。
    nit: `Request()` と `BakeSync()` の「cache_→.msfm 表」判定ロジックが 2 箇所に
    重複している (コメントで「2 本目を書くと必ずずれる」と自戒は書いたが、
    共通ヘルパへ切り出す余地は残っている)。GPU 経路 (`Pump()` の
    `needsMainInfer` 分岐) は実装したが GPU バックエンドが無いため未運動 (差し込み口
    のみという spec の指示どおりだが、実地検証はできていない)
  テスト: 5 — ModalSelfTest に 6 節 44 チェックを追加し全緑、EngineCliSelfTest/
    ReloadHubSelfTest にも追加、さらに `--modal-bake` を実機で 3 種の条件
    (fixture 初回/2 回目、random-full 初回) で手動実行しログを実測した

不安・質問:
  - **最重要 (要 planner 判断): 受け入れ条件 #3 の時間予算超過。** 基準構成
    (widths 16/32/64/96、paramCount=1,682,448、乱数重み `--random-full`) の CPU
    推論が **約 4.75〜5.34 s/メッシュ** (目標 ≤1.5s の約 3.1〜3.6 倍)。sub-05.md の
    指示どおり「超過なら sub-04 の構造を縮める」の判断を仰ぎたい。参考データ:
    - 時間はメッシュの validCells (occupancy) にほぼ依存しない (6〜3375 cell の
      メッシュがすべて 4.7〜5.3s に収まっている) — ネットが 32³ の**稠密**ボリューム
      全体を畳み込むコストで、疎な occupancy を全く利用できていないことが原因と
      考えられる。ボトルネックは (実測はしていないが) おそらく `head1`
      (32³×32→64ch、spec のグラフでは最大の FLOPs を持つ層) 付近
    - fixture (widths 4/8/8/8、34,436 param) は 150〜200ms/mesh で目標内に収まって
      いる — 「小さいネットなら間に合う」ことは確認できている
    - 対処案 (planner 裁定を仰ぎたい): (a) sub-04 のネット幅を縮める (b) 疎な
      occupancy を利用する実装 (im2col を有効 cell 近傍だけに絞る等) へ
      `CpuModalBackend` を書き直す (c) 予算そのものを緩める (d) このサブは
      「配管が正しく動く」ことの確認までとし、時間予算の決着は M76h (本学習で
      実際のネット幅が決まった後) まで持ち越す。**筆者の見立ては (d)** — 正しさは
      確定できており、幅を決めるのは学習側 (sub-04/sub-08) の役割だと考えるため
  - `kDmNetMaxParamCount` (2,000,000) を Python の `model.MAX_PARAM_COUNT` と手で
    揃えている件 (`check_rules.ps1` の $constGroups には未登録、上記「仕様との
    差分」参照)。登録すべきか判断してほしい (spec §4.2 の明示リストには無いので
    今回は追加しなかったが、two-language 定数の重複はこのリポジトリの流儀では
    機械照合が原則)
  - `ReloadHub::ReloadModalNet` がパスの突き合わせをしない設計 (1 プロジェクト
    1 `.dmnet` 前提) でよいか。sub-06/07/08 を通してこの前提が崩れないか
    確認してほしい
  - GPU バックエンド用の `Pump()` 経路 (`needsMainInfer`) は実装したが、GPU
    バックエンドが存在しないため一度も通っていない。D3d11ModalBackend 着手時に
    必ず実地で確かめること

触ったファイル:
  - src\Engine\Engine\Modal\DmNet.h (新規)
  - src\Engine\Engine\Modal\DmNet.cpp (新規)
  - src\Engine\Engine\Modal\ModalInferenceBackend.h (新規)
  - src\Engine\Engine\Modal\CpuModalBackend.h (新規)
  - src\Engine\Engine\Modal\CpuModalBackend.cpp (新規)
  - src\Engine\Engine\Modal\ModalFeatureMap.h (新規)
  - src\Engine\Engine\Modal\ModalFeatureMap.cpp (新規)
  - src\Engine\Engine\Modal\ModalSoundLibrary.h (新規)
  - src\Engine\Engine\Modal\ModalSoundLibrary.cpp (新規)
  - src\Engine\Engine\Modal\ModalSelfTest.cpp
  - src\Engine\Core\AssetKeyResolver.h
  - src\Engine\Core\AssetKeyResolver.cpp
  - src\Engine\Engine\Physics\ConvexColliderLibrary.cpp
  - src\Engine\Platform\PathUtil.h
  - src\Engine\Platform\PathUtil.cpp
  - src\Engine\Engine\HotReload\ReloadHub.h
  - src\Engine\Engine\HotReload\ReloadHub.cpp
  - src\Engine\Engine\HotReload\ReloadHubSelfTest.cpp
  - src\Engine\Engine\EngineCli.cpp
  - src\Engine\Engine\EngineCliSelfTest.cpp
  - src\Engine\Engine\EngineLoop.h
  - src\Engine\Engine\EngineLoop.cpp
  - src\Editor\ModalTools.h
  - src\Editor\ModalTools.cpp
  - src\Editor\EditorMain.cpp
  - build\Engine.vcxproj (機械生成)
  - build\Engine.vcxproj.filters (機械生成)

申し送り:
  - sub-06 (ランタイム接続) は `modalsound::Request(mesh)` / `modalsound::IsReady(mesh)` /
    `modalsound::Header()` / `ModalFeatureMap::CellFeature(rawCell, out)` を使えば
    「特徴マップ 1 cell → `ModalCellFeature`」まで揃っている。`RestingImpulse` 抽出
    や `CollectModalImpacts` はこのサブでは触っていない
  - `--modal-bake` はプロジェクト assets 以下の `.fbx/.glb/.gltf` を再帰走査するだけで
    builtin 6 種は焼かない (spec の「builtin は毎起動焼く」設計どおり、CLI では
    明示要求しない限り対象外)
  - 時間予算超過 (上記「不安・質問」) が sub-04 のネット幅縮小につながる場合、
    fixture (widths 4/8/8/8) は変更不要 (テスト専用でありネット幅の縮小方針とは
    独立)

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: OK** (planner、2026-09-16)。fixture が `max|Δ| = 4.77e-07` で一致 = **推論の正しさが配管ごと担保された**のが最大の成果。唯一の未達だった時間予算は、planner が実測で切り分けて**仕様側を改訂**した (ネットは縮めない、≤ 6 s/メッシュ、spec §4.4 / §5 #11 / §7 / §8)。根拠: (a) MAC 内訳は高解像度側が支配 (`convT k4 64→32 →16³` 26.0% / `conv3 16→16 @32³` 11.0% / `res(32)@16³`×2 各 11.0% / `head conv3 32→64` 11.0%、総 2.06 GMAC) で、cell 有効性で飛ばせる head は **13.4% が上限** = 疎対応の書き直しは成立しない。「`validCells` に依らず一定」は畳み込みの性質として**正しい挙動**であって欠陥ではない (b) 実効 0.39–0.43 GMAC/s は単一スレッド・SIMD 無し (spec の設計どおり) の値で、SIMD / マルチスレッドに 10–50 倍の余地がある = 実装の問題 (c) 縮めると sub-04 の門 (R² ≥ 0.90) の測り直し = サブをまたぐ差し戻し + モデル品質の低下。`BakeSync` がキャッシュを見ずに毎回焼いていた実バグを 2 回実行で発見・修正 (176.6 ms → 0.38 ms) したのは good catch — 受け入れ条件にも 2 回実行を足した。
