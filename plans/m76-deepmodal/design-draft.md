# M76: Deep-Modal — 学習済み 3D-CNN による衝突音のモーダル合成 (Jin et al., ACM MM 2020)

## 実行手順 (ユーザー指示 2026-09-16: `/harness` で回す)

1. **WIP を先にコミット** (ユーザー決定): 作業ツリーの無関係な 3 件を内容ごとに分けて master へコミットする。
   コミット前に Debug ビルド (`/p:MyeWarnAsError=true`) + `Editor.exe --selftest` + `check_rules.ps1` を回す。
   - `PointerDeltaCarry` (Input.h/.cpp / InputActionsSelfTest.cpp / EngineLoop.cpp): 生マウスデルタとホイールを「実際に回った tick」へ 1 回だけ渡す
   - 敵 AI: safeRadius を持つ光を光センサーの誘引から除外 (AgentSystem.cpp / AcousticSelfTest.cpp)
   - スキンメッシュのフラスタム判定を保守化 (FrustumCull.h / RenderSystem.cpp / RenderSelfTest.cpp)
   - `plans/DeepModal/` (未追跡) は本計画の参考資料としてコミットに含める。
2. **ハーネス Phase 0**: `plans/m76-deepmodal/harness.md` を作り、基点コミットを記録する。
3. **Phase 1 (planner)**: 依頼原文 + 本ファイルの設計案を「補足」として渡す。planner がユーザーと往復して `spec.md` / `sub-NN.md` を確定する
   (本ファイルの A〜I 節とサブ分割は planner の出発点であり、planner の裁定で変わりうる)。
4. Phase 2〜4 はハーネスの手順どおり (coder ⟷ planner → reviewer → 完了報告)。

以下は planner へ渡す設計案。

**実装開始時**: 本ファイルを `plans\m76-deepmodal.md` へ複写してコミット対象にする。
1 サブ = 1 コミット (`M76a: ...` 形式の日本語件名) = 1 セッション + /clear。進捗の一次情報は git log。
作業は worktree + ブランチ `m76-deepmodal` (M75 が master で継続中のため)。

**再開手順**: `git log --oneline -5` で最後に完了した M76x を確認 → 本ファイルの該当節を読む →
共通検証 (Debug/Release ビルド `/p:MyeWarnAsError=true` 0 警告 → `Editor.exe --selftest` → `pwsh tools\check_rules.ps1`
→ `tools\replay_verify.bat`)。ソース追加時は `pwsh tools\gen_project_files.ps1`。Python 側は `cd tools\deepmodal && pytest`。

- 基点: master `8cdf44f`。末尾 TypeId = **60 (UIToggleGroup)** → ModalSound は **61** (62/63 は Cloth/SoftBody 予約。M75h の InputField が先に入れば末尾へ再 append)。`kCookVersion=3` 据え置き。ABI v19 不変。
- 参考: `plans\DeepModal\DeepModal_Implementation_Plan.md` (15 Phase / Checkpoint A〜M)、`plans\DeepModal\ACMMM20_ModalSound.pdf`。
- 機材: Python 3.11 / torch 2.11+cu128 (RTX 3060 12GB) / numpy / onnx あり。**scipy 無し** (requirements で入れる)。onnxruntime 無し (使わない)。

## Context

**何を作るか**: 3D メッシュ → 32³ バイナリボクセル → 学習済み 3D-CNN → 16³ × 192ch の Sound Feature Map (Mel 32 帯域 × {mask, log-amp} × 単位力 XYZ) をメッシュごとに 1 回焼いてキャッシュし、物理衝突のたびに接触点・法線・力積からモード列 (周波数 / 振幅 / 減衰) を組み立てて減衰正弦の和を合成、材質 (E, ρ, α, β) とサイズで後処理して鳴らす。録音済み SE を使わず「形状・接触位置・力の向きと強さ・材質・サイズで音が変わる」物理的整合性を成立させる。

**なぜ今の構造に載るか**: 音の経路は既に 2 段ある — 波 (`AcousticField`、sim 側、敵が聴く) と、その波を耳に出す一発再生 (`PendingWaveShot` → `MakeWaveShotPlay` → `ShapeAcousticSpatial`、出力レーン)。M76 は**耳に出す側だけ**を差し替える。波 (AI の聴覚) は 1 バイトも変えない。物理は `SolidContact` (接触点 / 法線 / tick 合計の法線力積) を既に出しており、`AcousticField::DrainImpacts` に静止支持力積の控除と材質選択の規則がある。CPU 側にメッシュの三角形と AABB が残っている (`Mesh::positions/indices/aabb`)。per-mesh のクック表は `.mcvx` (ConvexColliderLibrary) が雛形。

**ユーザー決定 (2026-09-16)**:
1. 再生 = **衝突ごとに ≤2 s のクリップを合成**して回転プール (`modal://slot#k`) へ `RegisterClip` → 既存の `Play` / 3D / 遮蔽 / リバーブ経路。ストリーミングの新レーンは作らない (持続接触は対象外)。
2. 推論 = **バックエンドを抽象化** (`ModalInferenceBackend`)。最初は C++ CPU 実装、将来 HLSL Compute 実装へ差し替えられる構造。外部ランタイム (ONNX/DirectML) は使わない (vendoring 方針)。
3. データセット = **Primitive → 小規模自前 → ModelNet10 → ModelNet40** の順で拡張。**小規模 Dataset への overfit (M76d) が成功するまで大規模生成 (M76h) を開始しない**。

**既存 WIP との関係**: `ImpactSynth.h:18-25` の「衝突のたびに呼ばない / Deep-Modal はやらない」は ImpactSynth (非モーダルの手続き音、事前焼き) の契約として残す。Deep-Modal は `ModalSound` コンポーネントを付けた物だけの opt-in で、無い物は従来どおり (WaveSound / 床材 / tone)。

**不変条件 (M76 全期間)**:
- sim 状態に 1 バイトも触れない。`ModalSoundComponent` は `kComponentNoHash`、push は `TickRunner.cpp` の `!ts.resim` ブロック内だけ、消費は `AudioSourceSystem::Update` (フレーム側)。replay 7 ペア / スクショ 24 枚は不変。
- sim に手が入るのは M76f の `RestingImpulse` 抽出 (AcousticField.cpp:984 の式を純関数へ) の 1 箇所だけ — **評価順を 1 文字も変えない**。replay_verify の acoustic ペアが証人。
- include の向き: **Engine/Audio → Engine/Modal → (Core / Renderer/GpuResources / Asset/CookedCache)** の一方向。`Modal/` は Audio / Acoustic / World を include しない。
- ボクセル化の実装は **C++ の 1 本** (`Modal/Voxelizer.cpp`)。Python は `Editor.exe --modal-voxelize` を呼ぶだけで自前実装を持たない (学習とランタイムの規則一致は「同じ関数」で担保)。
- ディスクに書く POD (.vox / .msfm / .dmnet / fixture) は **struct を memcpy しない** (Material の暗黙パディングで踏んだ罠)。フィールド単位で書く、固定長ヘッダ、版番号。
- 推論バックエンドの正しさは **1 つの fixture (Python が書いた voxel + 期待マップ)** で検査する。CPU / GPU どちらも同じ selftest を通る (許容誤差だけ違う)。

## 設計判断 (要点。詳細は M76h の ADR-021 へ)

### A. モーダル合成 (M76a) — `Engine/Audio/ModalSynth.h/.cpp`
- 純関数 `ModalSynthRender(const ModalModeSet&, AudioClip&)`。`ModalModeSet { int32_t count; float freqHz[32]; float amp[32]; float decay[32]; }` → mono / 44100 / int16。
- 実装は **2 次再帰共振器** (`y[n] = 2r cos(ω/fs)·y[n-1] − r²·y[n-2]`、`r = e^{−c/fs}`、double 状態) を 32 本加算。`AddDampedSine` (ImpactSynth.cpp:77) の per-sample sin/exp は衝突ごとには重い (2 s × 44100 × 32 ≈ 2.8M 回)。位相 0 (論文)、`AddDampedSine` と同じ 1 ms 線形アタック。乱数不使用。
- クリップ長 `T = clamp(max_i ln(a_i / kModalTailAmp) / c_i, 0.05, 2.0)`、`kModalTailAmp = 1/32768`。
- 音量は**絶対** (ピーク正規化しない)。float 加算 → `tanh` ソフトクリップ (|x| > 0.8 の領域のみ) → int16。同入力 → 同 PCM (selftest が memcmp)。
- 後処理 `modal::BuildModes(const ModalCellFeature&, const ModalPostParams&, ModalModeSet&)` の順序を **1 関数に固定**:
  1. fp16 → float。チャンネル順 `MaskCh(j,i) = j*64+i` / `AmpCh(j,i) = j*64+32+i` (`ModalTypes.h` が正本、Python `layout.py` と check_rules で照合)。
  2. log-amp 逆正規化 `ln a = logAmpMin + v·(logAmpMax − logAmpMin)`、`amp = exp(ln a)` (定数は `.dmnet` ヘッダ)。
  3. mask: `logit > ln(t/(1−t))` (sigmoid を計算しない)。t = `ModalSound.maskThreshold` (0 以下ならヘッダ既定)。
  4. 力の結合 `a_i = Σ_j |k_j|·H(mask_j,i)·amp_j,i` (式 9) × `hdr.ampScale · comp.gain · J_excess`。
  5. 帯域中心 (減衰) 周波数 `f_c = hdr.bandCenterHz[i]` → 参照材質の非減衰 λ: `c_ref = ½(α_ref + β_ref·(2πf_c)²)`、`λ = (2πf_c)² + c_ref²`。
  6. スケール `λ *= σ1/σ2/σ3²`、`a *= σ2^-½·σ3^-3/2` (式 12。ω ∝ √λ)。σ1 = E/E_ref、σ2 = ρ/ρ_ref、σ3 = L_obj/L_ref。
  7. 目標材質の減衰 `c_i = ½(α + βλ)`、`f_i = √(λ − c_i²)/2π`。過減衰 / `f ≥ 0.45 fs` / `a ≤ kModalTailAmp` は捨てる。
- Mel: `mel(f) = 2595·log10(1 + f/700)`、`mel(100)..mel(10000)` を 33 点で等分、中心は区間中点の逆変換。**実行時はヘッダの `bandCenterHz[32]`** を使う (学習時とバイト一致)。C++ の式は selftest がヘッダ値と 1e-2 Hz で照合 (ドリフト検知)。

### B. PhysMat の 4 フィールド (M76a)
`PhysMat` (PhysMatLibrary.h:33-67) 末尾に append: `youngsModulus = 0` (Pa。**0 = 参照材質 = σ1 = 1**) / `poissonRatio = 0.3f` / `rayleighAlpha = 0` (1/s) / `rayleighBeta = 0` (s)。ToJson は常に書く、FromJson は contains + 既定、Sanitize は E ∈ [0,1e13] / ν ∈ [0,0.49] / α ∈ [0,1e4] / β ∈ [0,1e-2]。**sim は読まない** (音レーン専用。σ2 は既存 `density`)。
`assets\physmats\*.physmat.json` 11 本の初期値 (耳で詰める出発点): metal 7.0e10/0.33/6/1e-7、steel 2.0e11/0.30/5/3e-8、glass 6.2e10/0.22/1/1e-7、tile 7.4e10/0.19/6/1e-7、wood 1.1e10/0.35/10/2e-6、rubber 5e6/0.49/60/5e-5、ice 9e9/0.33/8/5e-7、carpet/gravel/glue/water は 0。

### C. ボクセル化 (M76b) — `Engine/Modal/Voxelizer.h/.cpp`
- 定数 (`Modal/ModalTypes.h`): `kModalVoxelN=32`, `kModalMapN=16`, `kModalBands=32`, `kModalChannels=192`, `kModalVoxelPad=1`。
- **正規化**: `L = max(aabb extent)`、`h = L/29`、`origin = center − 16h`。AABB は voxel 座標 1.5..30.5 に収まる = 軸平行な面が**ボクセル中心**を通り境界に乗らない (単位立方体が正確に 30³ = 27000 個。`L/30` だと面が境界に乗って両側が立つ)。index = `x + 32(y + 32z)` (AcousticGrid の CellIndex と同じ x 最内)。座標系はメッシュローカル。
- **表面**: 三角形ごとに voxel 空間 AABB → その範囲の voxel と Akenine-Möller SAT (13 軸、箱半径 `h/2 + 1e-6h` で保守的)。退化三角形も辺として拾う。
- **内部充填**: pad リング (必ず空) から **6 近傍** flood-fill で外部を塗り、到達しない非表面 voxel = 内部。保守的表面は 26-分離なので漏れない。非 watertight は殻だけ残る (論文の「薄いものは 1 voxel 厚」と同じ扱い。`interiorCount` で観測可)。
- `VoxelFrame { origin[3]; voxelSize; aabbMin[3]; aabbMax[3]; longestEdge; }`、`VoxelGrid { frame; std::array<uint8_t,32768> occ; surfaceCount; interiorCount; }`、`bool VoxelizeMesh(const XMFLOAT3* pos, size_t n, const uint32_t* idx, size_t m, VoxelGrid& out)`。
- **.vox** (64 B 固定ヘッダ: magic 'MVOX' / version 1 / n / pad / origin / voxelSize / aabbMin / aabbMax / surfaceCount / interiorCount / reserved) + 32768 B。
- **接触点 → cell**: `v = clamp(floor((p − origin)/h), 0, 31)`、`cell = v >> 1`。無効 cell は `cellSlot[4096]` 表 (焼き時に総当たりで最寄り有効 cell、同値は小 index = 決定的) で引く。
- **OFF / OBJ 最小リーダ** (`Modal/TriangleSoup.h/.cpp`): ModelNet は OFF ("OFF" 直後に数字が続く癖に対応)。FBX / glTF は `ModelLoader` のヘッドレス登録 (`--migrate-subasset-ids` と同経路)、`builtin://cube` 等は `MeshLibrary`。

### D. 特徴マップと .msfm (M76e) — `Engine/Modal/ModalFeatureMap.h/.cpp`
- `ModalFeatureMap { version; uint64_t modelHash; VoxelFrame frame; validCount; uint16_t cellSlot[4096]; std::vector<uint16_t> feat /* validCount×192 fp16 */; }`。**有効 cell だけ**持つ (典型 200–500 KB)。
- `.msfm` = `.mcvx` と同型の表 `vector<pair<meshName, ModalFeatureMap>>` (key 昇順、1 モデルファイル 1 表、遅延で書き足す)。内部版 `kMsfmVersion=1` を持つので **`kCookVersion` は据え置き**。`modelHash` が現在の `.dmnet` と違う entry はミス扱いで焼き直す。
- クック元パスは `CookedCache::SourcePathForSubAssetKey(meshName)` を新設し `ConvexCookSourcePath` はこれに委譲 (bit 中立)。`builtin://` / 手続きメッシュはパス無し = 毎起動焼く (0.5 s 級)。
- `SerializeModalTable / DeserializeModalTable` は公開 (selftest が往復 memcmp)。

### E. 力の分解と音量 (M76f)
- `SolidContact.normal` は high → low。発音元 E が low index なら `nE = n`、high なら `−n`。
- ワールド → ローカル: `WorldMatrixComponent` (push は TransformSystem の後 = **今 tick の値**) の逆行列で接触点、回転部 (行ベクトル正規化 = `MakePoseFromMatrix` と同規約) で `nE`。`k = J_excess·nE_local`、`|k_x|,|k_y|,|k_z|` を式 9 へ。
- **|k| の理由**: 論文の式 7 は符号付き振幅の線形結合だが、Mel 帯域で Σ|a| → log にした時点で符号は表現できない。|k| が唯一整合する定義 (ADR に明記)。
- **音量校正**: `J_excess = impulse − RestingImpulse`、`RestingImpulse = (mA + mB)·|g|·dt·kImpactRestingMargin` (AcousticField.cpp:984 と同式を `acoustic::RestingImpulse(World&, EntityID, EntityID, float dt)` へ抽出し両者が呼ぶ)。`J_excess ≤ kImpactMinImpulse` (0.35) は鳴らさない。`hdr.ampScale` は export.py がデータセット統計から決める (J=1 N·s の中央値ピークが −12 dBFS)。上限はソフトクリップのみ。★耳確認で「絶対だが上限あり (`min(1, J/kImpactRefImpulse)` 圧縮)」が欲しくなる可能性が高い = M76f で決める。
- σ3 の `L_obj` = ローカル最長辺 × その軸のワールドスケール × `ModalSound.sizeScale`。
- **レート制限**: `kMaxModalShotsPerTick = 4`、entity ごとの `cooldownTicks` (既定 3 = 50 ms、側テーブル `ModalEntityState { EntityID; lastShotTick; }` = SourceState と同じく ECS の外)、キュー上限 `kMaxPendingModalImpacts = 64` (超過は捨てて数える = PushWaveShot と同型)。両側 ModalSound は両方鳴る。順序は contacts の key 昇順。
- **クリップ池**: `kModalClipSlots = 32`、id = `HashStr("modal://slot#k")`、ラウンドロビン。`RegisterClip` は同 id の voice を止める (AudioSystem.cpp:902) ので、`slot.endTick > now` のスロットしか無ければ捨てて数える (鳴っている音を切らない)。
- **wave の口封じ**: `ResolveWaveShotSound` (AcousticAudio.cpp:514) の先頭に (0): 発音元が `ModalSound` を持ち `muteWave != 0` かつ `modalsound::IsReady(mesh)` なら `shot.mute = 1`。焼けるまでは従来の音が鳴る = 段階移行。**波そのものは立つ**。
- spatial: `position = 接触点`、`minDistance = 1`、`maxDistance = ModalSound.maxDistance` (既定 30)、`rolloff = 0`、`dopplerScale = 0`、`reverbSend = AcousticAudio.waveReverbSend` (無ければ 0)。`AcousticAudio` + 場があれば `ShapeAcousticSpatial(..., nullptr, 1, &info)` (**規則は 1 本**)。無ければ Bypass = AcousticAudio の無いシーンでも鳴る。

### F. 推論バックエンドとライブラリ (M76e) — `Engine/Modal/`
- **`.dmnet`** (`DmNet.h/.cpp`): 256 B 固定ヘッダ (`magic 'DMNT' / version / opCount / bufferCount / inN 32 / outN 16 / bands 32 / channels 192 / fMinHz / fMaxHz / logAmpMin / logAmpMax / ampScale / maskThreshold / refYoung / refDensity / refPoisson / refSizeL / refAlpha / refBeta / bandCenterHz f32×32 / weightsHash u64 / paramCount / reserved`) + op 表 `{ type (Conv3d=0 / ConvTranspose3d=1 / ReLU=2 / Add=3) / in0 / in1 / out / cin / cout / k / stride / pad / outPad / weightOffset / biasOffset }` + fp16 重み + fp32 バイアス。BN は export で畳み込み済み。**予算 ≤ 2.0M パラメータ = fp16 4 MB** (export.py が assert)。
- **ネット** (Python `model.py` と 1:1): enc `conv3(1→16)+ReLU, conv3(16→16)` @32³ → `s2 (16→32)`, res(32) @16³ → `s2 (32→64)`, res(64) @8³ → `s2 (64→96)`, res(96) @4³ → `convT k4 s2 (96→64)` + Add(enc64), res(64) @8³ → `convT (64→32)` + Add(enc32), res(32) @16³ → head `conv3(32→64)+ReLU, conv1(64→192)` (活性なし = mask は logit、amp は 0..1 回帰)。≈1.8M params、≈2.5 GFLOP/推論。
- **バックエンド抽象 (ユーザー決定 2)** — `Modal/ModalInferenceBackend.h`:
  ```cpp
  class ModalInferenceBackend {
  public:
      virtual ~ModalInferenceBackend() = default;
      virtual const char* Name() const = 0;                 // "cpu" / "d3d11cs"
      virtual bool RunsOnWorkerThread() const = 0;          // CPU = true / GPU = false (immediate context はメインスレッド専用)
      virtual bool Prepare(const DmNet& net, std::string* err) = 0;   // 重みの転置・GPU バッファ作成など (モデル差し替えごと 1 回)
      virtual bool Infer(const VoxelGrid& in, std::vector<float>& out192x4096, std::string* err) = 0;
  };
  ```
  - `CpuModalBackend` (`Modal/CpuModalBackend.h/.cpp`、M76e): im2col + 4×4 レジスタブロック GEMM (float、単一スレッド、SIMD なし)。ConvTranspose は「stride 散布 + 反転カーネルの Conv」。目標 0.3–1.0 s/メッシュ。
  - `D3d11ModalBackend` (将来 M76i、**本計画の範囲外だが差し込み口を今作る**): `RunsOnWorkerThread() == false`、`Prepare` で重みを StructuredBuffer へ、`Infer` は `conv3d.cs.hlsl` を op ごとに Dispatch + staging 読み戻し (VolumeTexture / GpuBufferUtil を流用。`RunFroxelVolumeProbe` が読み戻し検査の雛形)。**同じ fixture selftest を tol 1e-2 で通す**のが合格条件。
  - 選択は `ModalSoundLibrary::SetBackend(std::unique_ptr<ModalInferenceBackend>)` の 1 箇所 + CLI `--modal-backend <cpu|d3d11cs>` (`--particle-backend` と同型。既定 cpu。未実装の名前は WARN + cpu へ縮退)。
- **`ModalSoundLibrary`** (`Modal/ModalSoundLibrary.h/.cpp`、EngineLoop が所有):
  ```cpp
  enum class ModalState : int32_t { Missing=0, Baking=1, Ready=2, Failed=3, NoModel=4 };
  void Init(RenderResources*);  void SetBackend(std::unique_ptr<ModalInferenceBackend>);
  bool LoadModel(const std::wstring& path);   void ReloadModel();          // ReloadHub (.dmnet)
  ModalState Request(AssetID mesh);           // 非ブロッキング。未着手ならジョブを積んで Baking
  const ModalFeatureMap* Get(AssetID mesh) const;   const DmNetHeader* Header() const;
  void Pump();        // メインスレッド: 完了ジョブ取り込み + .msfm 書き。backend が RunsOnWorkerThread()==false なら**ここで**推論を回す (1 フレーム 1 ジョブ)
  void Register(AssetID, ModalFeatureMap);  void Clear();  void Shutdown();   // ワーカー join
  bool BakeSync(AssetID mesh);                // --modal-bake / --modal-sync-bake (メインスレッドで完走)
  ```
  ワーカーは `TextureLibrary::AsyncWorker` (GpuResources.cpp:720) と同じ mutex + cv + deque。ジョブは **enqueue 時に positions/indices をコピー**し `shared_ptr<const DmNet>` を掴む (ワーカーは RenderResources に触らない)。ボクセル化は常にワーカー (どのバックエンドでも)、推論だけバックエンドの申告で場所が変わる。`.msfm` の読み書きはメインスレッド。
  配線: EngineLoop.cpp:342 の隣 (`modalSounds.Init(&resources); SetBackend(...); modalsound::Install(&modalSounds); LoadModel(ResolveDeepModalPath()); audioSources.SetModalLibrary(&modalSounds);`)。`Pump()` は `audioSources.Update` の**直前** (EngineLoop.cpp:2257。Update は 0-tick フレームで早期 return するので中に置けない)。終了は `modalsound::Install(nullptr)` → `Shutdown()` を `convexcol::Install(nullptr)` の隣。
- モデルの置き場: `assets\deepmodal\deepmodal.dmnet` (コミット ≤ 4 MB)。解決は「プロジェクト assets → エンジンリポジトリ assets」の 2 ルート (`FindEngineDeepModalDir`、`FindEngineShaderDir` と同型)。ReloadHub に `{ L".dmnet", ReloadKind::ModalNet, 6 }`。

### G. Python (M76c / M76d / M76h) — `tools\deepmodal\`
```
requirements.txt  numpy scipy torch pytest   (torch の cu128 index URL は README に)
README.md         手順 / 環境変数 MYE_EDITOR_EXE (既定 bin\x64\Release\Editor.exe) / データ段階の門
layout.py         定数 (VOXEL_N / MAP_N / MEL_BANDS / CHANNELS / F_MIN / F_MAX / チャンネル順 / struct 文字列 / 参照材質 / L_REF)
meshio.py         .vox 読み、モデル列挙 (OFF は Editor.exe が読む)
voxelize.py       subprocess `cmd /c "<Editor.exe> --modal-voxelize --list F --out DIR"` (バッチ 200 件)
primitives.py     Primitive 段: 箱 / 板 / 円柱 / 球 / 中空箱 / L 字 / 穴あき板 を寸法乱数で OBJ 生成 (seed 固定)
fem.py            hex8 の Ke (24×24, 2×2×2 Gauss) / Me (集中質量) を単位立方体で 1 回作り h, E, ν, ρ でスケール。
                  占有 voxel の節点を compact 化 → COO scatter-add → CSR
modal.py          eigsh(K, k, M=M, sigma=0, which='LM') → 剛体 6 モードを落とす → ω=√λ/2π、100–10000 Hz 外を落とす。k ≤ 256、timeout 300 s
contact.py        16³ cell ごとの接触節点 (cell 内占有 voxel の節点で中心に最寄り、同値は最小 index)、a_ij = |U[dof_j(node), i]| / ω_i
compact.py        Mel 区切り / 帯域割当 / Σ|a| / mask / ln / 空帯域は最寄り非空の値 (論文の trick) / 未励起 (|a| < 1e-3·max) は無し
dataset.py        1 メッシュ → data\<stage>\<name>.npz {vox u8[32³], valid u8[16³], feat f16[valid,192]}。multiprocessing (OMP_NUM_THREADS=1)、再開可、stats.json
model.py          F 節と 1:1 (学習時 BN あり)
train.py          Adam lr 1e-3 (--lr で論文 0.02 に戻せる) / batch 16 / 100 epoch / 20 epoch ごと半減 / loss = MSE(amp, valid cell) + BCEWithLogits(mask)
                  --overfit N: N 形状 300 epoch → amp MSE < 1e-3 & mask acc > 99% を assert (= 大規模生成の門)
export.py         BN 畳み込み → fp16 → .dmnet + fixture (tests\deepmodal\)
tests\            test_fem / test_modal / test_compact / test_layout / test_contact / test_export
```
- **データ段階 (ユーザー決定 3)**: (0) Primitive (`primitives.py` 数十 + `builtin://` 6 種) → (1) 小規模自前 (`assets\models` + 三校 / HAL Collector のモデル、合計 ≤ 100) → **門: `train.py --overfit 16` 合格 + C++ 推論 fixture 一致 (M76d/e)** → (2) ModelNet10 (4.9k、M76h) → (3) ModelNet40 (12.3k、時間が許せば)。門を越えるまで (2) 以降の生成コマンドは README で「実行禁止」と明記。
- **eigsh の時間**: 60k DOF の shift-invert は SuperLU の fill-in が支配 (20–60 s/メッシュ見込み)。緩和: k ≤ 256 / 占有 > 20k voxel は skip / 12 プロセス並列 / 1 メッシュ 1 npz で再開可 / L_ref = 0.3 m + 参照材質 E=7.0e10, ρ=2700, ν=0.33, α=6, β=1e-7 (アルミ相当)。ModelNet10 ≈ 4899 × 40 s / 12 ≈ 4.5 h。L_ref と fMax は stage0/1 の統計 (帯域内モード数の分布) で確定してからヘッダに固定する。

### H. Editor (M76g)
- Inspector の `ModalSound` 節末尾: 状態 (Missing/Baking/Ready/NoModel/バックエンド名) と有効 cell 数、**6 面ボタン** (`+X −X +Y −Y +Z −Z`: 接触点 = 面中心、力 = 内向き法線 × スライダ [N·s] 0.1..20) → `MakeModalShotPlay` (**ランタイムと同じ関数**) → `RegisterClip(HashStr("modal://preview"))` → `Play` on `kBusUi` (SoundGenWindow.cpp:181-202 と同型)。**Export WAV** は `SoundGenWindow::Save` の WAV 書き出しを `AudioClip.h` の `SaveWav` に切り出して共用。
- PhysMat インスペクタ (InspectorWindow.cpp:2058-2077) に 4 行。`EditorComponentCatalog.cpp` に `ModalSound` (Audio 分類)。`LocalizationTable.inl` に en/ja 8 本。

### I. CLI
- `--modal-voxelize --list F --out DIR` (Editor、ヘッドレス): 1 行 1 パス (`.off/.obj/.gltf/.fbx/builtin://cube`)。出力 `DIR\<stem>#meshN#primM.vox`。exit 0/1。
- `--modal-bake [--project DIR]` (Editor): `.dmnet` を読み AssetDatabase のモデルをヘッドレス登録 → 各 prim を `BakeSync` → `.msfm`。1 行/メッシュ + 合計。exit 0/1/2 (モデル無し)。
- `--modal-backend <cpu|d3d11cs>` (両 Main、EngineCli.cpp の表)。
- `--modal-audio-log N` (両 Main): tick < N の間 `[modal] t=…` を 1 行ずつ + 終了時 summary。`--no-audio` で 0 行。**耳を使わない唯一の検査口** (`--acoustic-audio-log` と同型)。
- `--modal-sync-bake` (両 Main): `Request` を `BakeSync` に倒す (検証 run の決定性用)。
- `--modal-demo` (Showcase 表): 木 / 金属 / ガラスの箱 3 個が金属板と木の床に落ちる小シーン (builtin メッシュ + physmat + ModalSound)。golden / replay ペアは**足さない** (出力レーンのみで主張が無い)。
- ★EditorMain の else-if 連鎖は MSVC の入れ子上限 (C1061) に達している — 新フラグは `--cook-font-metrics` と同じく**連鎖の手前で拾って `continue`**。

## サブ分割 (実行順)

| サブ | 内容 | 主な検証 |
|---|---|---|
| **M76a** | ModalSynth (再帰共振器 / BuildModes / Mel 表) + PhysMat 4 フィールド + physmat JSON 11 本 | ModalSynthSelfTest / PhysMatSelfTest / replay_verify 不変 |
| **M76b** | Voxelizer + .vox + TriangleSoup (OFF/OBJ) + 接触→cell + cellSlot + `--modal-voxelize` | ModalSelfTest (voxel) |
| **M76c** | Python: layout / primitives / voxelize / fem / modal / contact / compact / dataset + pytest + check_rules constGroups (C++⇄Python 4 組) | pytest / スケール則 / stage0 生成 |
| **M76d** | Python: model / train (**overfit の門**) / export (.dmnet + fixture) | overfit 合格 / fixture コミット |
| **M76e** | DmNet loader + `ModalInferenceBackend` + `CpuModalBackend` + ModalFeatureMap/.msfm + ModalSoundLibrary + `--modal-bake` / `--modal-backend` + EngineLoop 配線 + ReloadHub | ModalSelfTest (fixture 一致 / msfm / library) |
| **M76f** | ModalSound (61) + PendingModalImpact + drain + クリップ池 + wave 口封じ + `RestingImpulse` 抽出 + `--modal-audio-log` / `--modal-sync-bake` / `--modal-demo` | ModalAudioSelfTest / replay_verify / log 2 run 一致 |
| **M76g** | Inspector strike プレビュー + WAV 書き出し + PhysMat 欄 + カタログ + 文字列 | LocalizationSelfTest / AudioSelfTest (SaveWav) / 手動 |
| **M76h** | 小規模自前 → ModelNet10 本学習 → `deepmodal.dmnet` 更新 + 耳合わせ + 文書 (engine_spec §10.7 / ADR-021 / README / CLAUDE.md) | 共通検証 全部 |
| (M76i) | 範囲外・差し込み口のみ: `D3d11ModalBackend` (conv3d.cs.hlsl) を同じ fixture で tol 1e-2 | — |

---

### M76a — モーダル合成器と材質パラメータ (Checkpoint A + Phase 13 のデータ)

**変更ファイル**
- 新規 `src\Engine\Engine\Modal\ModalTypes.h` (定数 / `DmNetHeader` POD / `ModalCellFeature { float v[192]; }` / `MaskCh` `AmpCh` / `MelBandCenters(float out[32])`)
- 新規 `src\Engine\Engine\Audio\ModalSynth.h/.cpp` (`ModalModeSet` / `ModalPostParams { young, density, sizeL, alpha, beta, maskThreshold, gain, impulse }` / `BuildModes` / `ModalSynthRender` / `ModalClipSeconds`)
- 新規 `src\Engine\Engine\Audio\ModalSynthSelfTest.h/.cpp`
- `src\Engine\Engine\Physics\PhysMatLibrary.h/.cpp`、`PhysMatSelfTest.cpp`、`assets\physmats\*.physmat.json` (11 本)
- `src\Engine\Engine\Audio\ImpactSynth.h:23` のコメント更新 (「モーダル経路は ModalSynth が別に持つ」)
- `src\Editor\EditorMain.cpp:427` の後ろに `RunModalSynthSelfTest()`

**要点**: `BuildModes` に World も PhysMat も渡さない (値だけ) = selftest がデバイスもワールドも無しで全経路を叩ける。

**テスト (ModalSynthSelfTest)**: (1) 1 モード f=440, c=5, a=0.5 → 零交差で周波数 ±1%、`RmsIn` (ImpactSynthSelfTest.cpp:37 と同型) で [0.1,0.2] と [0.6,0.7] の比が `e^{−5·0.5}` ±5% / (2) a×2 → ピーク×2 / (3) 同入力 2 回 → memcmp 一致 / (4) 長さ規則 (下限 0.05 / 上限 2.0) / (5) `MelBandCenters` が double 期待値と 1e-2 Hz / (6) BuildModes: E×4 → f×2、ρ×4 → f/2 & a/2、L×2 → f/2 & a/2^1.5 (1e-4 相対) / (7) mask 閾値で帯域が落ちる / (8) k=0 → count 0 / (9) 過減衰・ナイキスト超えが落ちる。PhysMatSelfTest: JSON 往復 + Sanitize + 旧 JSON (キー無し) が既定へ。

### M76b — ボクセル化と CLI (Checkpoint B)

**変更ファイル**: 新規 `Modal\Voxelizer.h/.cpp` (`VoxelizeMesh` / `TriBoxOverlap` / `FloodFillInterior` / `LocalPointToCell` / `BuildCellSlotTable` / `SerializeVox` / `DeserializeVox`)、新規 `Modal\TriangleSoup.h/.cpp`、新規 `src\Editor\ModalTools.h/.cpp` (`RunModalVoxelizeCli`。モデルのヘッドレス登録は `SubAssetMigration.cpp` を流用)、新規 `Modal\ModalSelfTest.h/.cpp` (M76e/f で積み増す)、`EditorMain.cpp`。

**要点**: C 節。flood-fill は明示スタックで固定順。

**テスト (ModalSelfTest)**: (1) 単位立方体 12 三角 → `surfaceCount + interiorCount == 27000`、中心 1、隅 0、pad リング全 0 / (2) 厚さ 0.001 の板 → y の占有 index が 1 種類 / (3) 蓋なし箱 → interiorCount == 0 / (4) 2:1:0.5 の AABB → 最長辺 29 voxel / (5) +X 面中心 → cell (15, 7|8, 7|8) / (6) cellSlot: 有効は自身、無効は最寄り (独立の総当たりと一致) / (7) .vox 往復 memcmp / (8) OFF/OBJ リーダ / (9) 同入力 2 回 → memcmp。

**検証**: `cmd /c "bin\x64\Debug\Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out %TEMP%\vox"` → 6 ファイル。

### M76c — Python: FEM / 固有値 / 接触励起 / Mel 圧縮 / データセット (Checkpoint C–G)

**変更ファイル**: `tools\deepmodal\*` (G 節)、`.gitignore` に `tools/deepmodal/data/`、`tools\check_rules.ps1` の `$constGroups` に `kModalVoxelN⇄VOXEL_N` / `kModalMapN⇄MAP_N` / `kModalBands⇄MEL_BANDS` / `kModalChannels⇄CHANNELS`。

**要点**: `Ke(h,E,ν) = h·Ke_unit(ν)·E`、`Me = ρh³·Me_unit_lumped` (1 回だけ数値積分)。節点 index `(x + 33(y + 33z))` を占有 voxel の 8 頂点だけ compact 化。eigsh は M を渡す一般化問題。

**テスト (pytest)**: `test_fem` Ke 対称・半正定、剛体 6 モードで `K·r ≈ 0`、集中質量の総和 = ρh³ / `test_modal` 2×2×2 で E×4 → ω×2、ρ×4 → ω/2、h×2 → ω/2 (式 12 の根拠、1e-6)、先頭 6 固有値 ≈ 0 / `test_compact` 単調・端点・Σ|a|・空帯域補間・mask / `test_layout` .vox が 64 B、C++ の cube .vox で 27000 (`@pytest.mark.editor`、Editor.exe 無ければ skip) / `test_contact` cell を変えると励起ベクトルが変わる (Checkpoint E)。

**検証**: `pytest` / `python dataset.py --stage primitives --out data\stage0` → npz 数十本 + stats.json / `pwsh tools\check_rules.ps1`。

### M76d — Python: モデル / 学習 / export / fixture (Checkpoint H = **大規模生成の門**)

**変更ファイル**: `model.py / train.py / export.py / tests\test_export.py`、新規 `tests\deepmodal\fixture.dmnet` (幅 4/8/8/8 の小ネット、乱数重み ≈ 50 KB) / `fixture_in.vox` / `fixture_out.bin` (64 cell × 192 float32 + cell index 表) / `list_builtin.txt`。

**要点**: export は (1) BN を conv に畳む (2) fp16 に丸めて**から** fp32 で fixture 期待値を計算 (C++ と同じ重み、差は加算順だけ) (3) `paramCount ≤ 2,000,000` assert (4) `weightsHash` = FNV-1a (5) `bandCenterHz` は compact.py の値そのもの。

**合格条件 (門)**: `train.py --overfit 16 --epochs 300` (stage0 + stage1 の 16 形状) → amp MSE < 1e-3 & mask acc > 99%。これを越えるまで ModelNet の生成 (M76h) を始めない。

### M76e — C++ 推論 / バックエンド抽象 / .msfm / ModalSoundLibrary / --modal-bake (Checkpoint I)

**変更ファイル**
- 新規 `Modal\DmNet.h/.cpp`、`Modal\ModalInferenceBackend.h`、`Modal\CpuModalBackend.h/.cpp` (Conv3d / ConvTranspose3d / Gemm)、`Modal\ModalFeatureMap.h/.cpp` (`BuildFeatureMap(backend, net, grid, out)`)、`Modal\ModalSoundLibrary.h/.cpp` (`namespace modalsound { Install / Library / IsReady / Header }`)
- `Asset\CookedCache.h/.cpp` (`SourcePathForSubAssetKey`)、`Physics\ConvexColliderLibrary.cpp` (委譲)
- `EngineLoop.h/.cpp` (所有 / Init / SetBackend / Install / LoadModel / Pump / Shutdown)、`Platform\PathUtil.h/.cpp` (`FindEngineDeepModalDir`)、`EngineCli.cpp` (`--modal-backend`)
- `HotReload\ReloadHub.h/.cpp` (`ReloadKind::ModalNet`)、`ReloadHubSelfTest.cpp`
- `src\Editor\ModalTools.cpp` (`RunModalBakeCli`)、`EditorMain.cpp` (`--modal-bake`)、連鎖末尾に `RunModalSelfTest()`

**要点**: F / D 節。ワーカーの失敗は `Failed` にして WARN 1 回。`Request` は mesh 未登録 / positions 空なら `Missing` (キャッシュしない)。バックエンドが `RunsOnWorkerThread()==false` のときはボクセル化だけワーカーで済ませ、推論は `Pump()` で 1 フレーム 1 ジョブ。

**テスト (ModalSelfTest)**: (1) fixture.dmnet + fixture_in.vox → 64 cell × 192 で `max|Δ| < 1e-3` (**インストール済みバックエンドに対して回す** = 将来の GPU も同じ検査) / (2) Conv3d / ConvTranspose3d の小ケースを素朴 6 重ループと 1e-6 で一致 / (3) .msfm 表の往復 memcmp / (4) Library: `Register` → `Get`、モデル無しで `NoModel`、`BakeSync` (fixture + 立方体) → `Ready` かつ validCount = voxel から独立に数えた有効 cell 数 / (5) `SourcePathForSubAssetKey("builtin://cube")` が空、`ConvexCookSourcePath` と同結果。

**検証**: `cmd /c "bin\x64\Debug\Editor.exe --modal-bake"` (fixture モデルを `assets\deepmodal\deepmodal.dmnet` に仮置き) → 行数 = メッシュ数、2 回目は全部 cached / `CookedCacheSelfTest` 不変。

### M76f — ランタイム接続 (Checkpoint J / K / L)

**変更ファイル**
- `Core\Components.h/.cpp` (`ModalSoundComponent` を末尾 61 に `kComponentNoHash`。`AssetID mesh` (空 = 同 entity の MeshRenderer.mesh) / `gain=1` / `maskThreshold=0` / `cooldownTicks=3` / `sizeScale=1` / `maxDistance=30` / `muteWave=1`)
- 新規 `Audio\ModalAudio.h/.cpp` (`PendingModalImpact` POD / `ModalAudioStats` / `ModalShotResult { Played, NotReady, NoModel, Cooldown, BelowMin, PoolFull, PlayFailed }` / 純関数 `CollectModalImpacts(World&, const vector<SolidContact>&, float dt, uint64_t tick, vector<PendingModalImpact>&)` / `MakeModalShotPlay(map, header, impact, comp, physmat*, AudioClip&, AudioSpatial&, info*)`)
- `Acoustic\AcousticGrid.h` / `AcousticField.cpp` (`acoustic::RestingImpulse` 抽出、評価順不変)
- `TickRunner.cpp:645` の直後 (同じ `!ts.resim` 内、wave の後): `CollectModalImpacts(world, solidContacts, ...)` → `audioSources.PushModalImpact`
- `Audio\AudioSourceSystem.h/.cpp` (`SetModalLibrary` / `PushModalImpact` (上限 64) / drain を wave shot の後ろ / 側テーブル / クリップ池 / `ModalStats` / `Reset`)
- `Audio\AcousticAudio.cpp:514` (`ResolveWaveShotSound` の (0))
- `EngineCli.cpp` (`--modal-audio-log N` / `--modal-sync-bake`)、`EngineLoop` (配線 + 終了 summary)、`EngineCliSelfTest.cpp`、`ShowcaseScenes.cpp` / `DemoContent.cpp` (`--modal-demo`)
- 新規 `Audio\ModalAudioSelfTest.h/.cpp`、連鎖末尾

**drain 手順 (1 impact)**: cooldown → `lib->Request(mesh)` (Ready でなければ `NotReady` を数えて続行しない = wave の音がそのまま鳴る) → `physmat::Resolve` (null = 参照材質) → `MakeModalShotPlay` → 池から slot → `RegisterClip` → `PlayDesc { bus=SE, volume=1, priority=128 }` → acOn なら `ShapeAcousticSpatial` → `Play`。
log: `[modal] t=<tick> src=<idx> mesh=<16hex> cell=<cx>,<cy>,<cz> slot=<n> k=<kx>,<ky>,<kz> J=<excess> modes=<n> f0=<Hz> f1=<Hz> peak=<dBFS> len=<sec> class=<Direct|Detour|Occluded|Bypass> gain=<g> r=<result>`。summary: `[modal] summary impacts= played= notReady= cooldown= belowMin= dropped= poolFull= playFailed= bakes= bakeMsAvg=` (**追加は末尾**)。初回 `notReady` で「`--modal-bake` を促す WARN」を 1 回。

**テスト (ModalAudioSelfTest)**: (1) 箱 (ModalSound) と床の接触 1 件 → 1 impact、箱を Y 90° 回転して k がローカル軸に乗る、`excessImpulse = impulse − RestingImpulse` / (2) 両側 ModalSound → 2 impact、key 昇順 / (3) `RestingImpulse` が抽出前の式とビット一致 / (4) push 65 個目は dropped / (5) 合成マップ (全帯域 mask on) → k=(1,0,0) で count 32、k=0 で BelowMin、maxDistance = comp 値 / (6) cooldown / (7) `ResolveWaveShotSound`: Ready → mute=1、未 Ready → 従来 soundKey / (8) suspend 中の Update でキューが空になる / (9) CLI 行。

**検証**: 共通検証 + `replay_verify.bat` (RestingImpulse 抽出の証明) + `cmd /c "bin\x64\Release\Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --frames 300 > a.txt"` を 2 回 → `[modal] t=` 行が byte 一致 + `--no-audio` で 0 行 + `played > 0 && playFailed == 0`。**耳確認**: 面で音が変わる / 強く落とすと大きい / 材質を metal ↔ wood で変えると減衰が変わる。ここで「絶対音量に上限圧縮を足すか」を決める。

### M76g — エディタ

`InspectorWindow.cpp` (ModalSound 節 / PhysMat 4 行)、`AudioClip.h/.cpp` (`SaveWav` 切り出し)、`SoundGenWindow.cpp` (委譲)、`EditorComponentCatalog.cpp`、`LocalizationTable.inl`。プレビューは `MakeModalShotPlay` と**同じ関数** (2 本目を書かない)。Baking 中はボタン disabled。テスト: LocalizationSelfTest / `AudioSelfTest` に `SaveWav` → 既存 WAV ローダで読み戻して samples 一致 / 手動 (6 面で音が変わる、WAV が出る)。

### M76h — 本学習と文書

- 順序: stage1 (小規模自前 ≤ 100) で `train.py` フル → 耳確認 → `dataset.py --stage modelnet10 <dir> --jobs 12` (≈ 4.5 h) → `train.py --epochs 100` → `export.py --out assets\deepmodal\deepmodal.dmnet` (≤ 4 MB) → `Editor.exe --modal-bake` → `--modal-demo` で `ampScale` / physmat α, β を詰める。ModelNet40 は時間が許せば同手順。
- 文書: `engine_spec.md` §10.7 (経路図 / .vox .msfm .dmnet の版と配置 / 後処理順 / レート制限 / バックエンド / CLI) / `docs\adr\ADR-021-deep-modal.md` (per-collision 合成 vs ストリーミング、CPU 推論 + バックエンド抽象 vs ONNX/DirectML、C++ 単一ボクセライザ、|k| と Σ|a|、絶対音量、wave 口封じの段階移行、L_ref と参照材質、データ段階の門) / `README.md` / `CLAUDE.md` (末尾 TypeId 61、Cloth/SoftBody 予約繰り下げ、CLI 6 本、検証表に `--modal-audio-log` 2 run 一致、「.dmnet を差し替えたら `--modal-bake`」、constGroups の Python 組) / `plans\m76-deepmodal.md` 申し送り。

## CLAUDE.md の規則で効くもの

- **コンポーネント append-only**: 61 を `RegisterBuiltinComponents()` 末尾へ。NoHash なので Scene / .rep / snapshot 版は不変 (WaveSound=51 と同じ扱い)。
- **kCookVersion**: 据え置き (.msfm は新拡張子 + 内部版)。
- **constGroups**: C++ ⇄ Python の 4 組を `check_rules.ps1` へ。
- **UI 文字列**: `LocalizationTable.inl` en/ja、`Tr()` を printf の唯一の引数にしない。
- **gen_project_files**: M76a/b/e/f/g で新規 .cpp/.h を足すたびに実行。
- **include の向き**: Audio → Modal → (Core / Renderer / Asset)。`AcousticAudio.cpp` が `modalsound::` を呼ぶのは Audio → Modal で許容。
- **決定論**: `MYE_CHECK`、`#ifdef _DEBUG` 分岐なし、宣言時初期化、unordered でバイト列を作らない (表は sorted vector)、乱数不使用。ワーカーの結果は出力レーンにしか入らない。
- **Editor.exe の起動**: Python から `cmd /c` (GUI サブシステム)。`.bat` を足すなら CRLF。
- **テスト**: 機能の隣に `*SelfTest.cpp`、連鎖の末尾に append。ECS (61) / シリアライズ (PhysMat JSON) / アセット (.msfm) / ローカライズ / ホットリロード (.dmnet) の全部に触るので各サブで必須。
- **コミット**: `M76a: ...` 日本語件名、ABI 変更なし。

## 懸念 (実装中に判断が要る点)

1. **TypeId 61 の衝突**: M75h (InputField) が先に master へ入れば ModalSound は 62。マージ時に末尾へ再 append (登録順で決まるのでコード位置を後ろへ動かすだけ)。
2. **|k| と Σ|a| は論文からの逸脱**: 同帯域で打ち消し合う 2 モードが過大になる。聴感上は問題になりにくい。ADR に明記。
3. **絶対音量**: 軽い接触が聞こえず重い衝突がソフトクリップに張り付く幅がある。M76f の耳確認で `min(1, J/kImpactRefImpulse)` 圧縮を足すか決める。
4. **eigsh の時間**: ModelNet40 全量は 12 並列でも 10 h 超。ModelNet10 で止めても「形状で音が変わる」は示せる。k ≤ 256 で帯域内モードを切り捨てる形状 (大きな平板) が出る — L_ref / fMax は stage0/1 の統計で決める。
5. **合成のコスト**: 2 s × 32 モードの再帰合成 ≈ 1–2 ms/発、`kMaxModalShotsPerTick = 4` で最悪 ~8 ms のスパイク。v1 はメインスレッド。`MakeModalShotPlay` が純関数なのでワーカーへの移動は容易。
6. **`builtin://` メッシュは .msfm を持てない** → 毎起動ワーカーで焼く。`--modal-demo` の箱が builtin なので検証 run は `--modal-sync-bake` 必須。
7. **wave の 1 tick 遅れとの二重発音**: 初回接触の 1 発だけ (NotReady → wave) 両方鳴り得る。段階移行の意図どおり、WARN で `--modal-bake` を促す。
8. **fp16 の特徴量**: fixture は fp16 に丸めた値で期待値を作る (計画に入れた)。
9. **GPU バックエンド**: `Infer` がメインスレッド専用になるので `Pump()` の 1 フレーム 1 ジョブで吸収する設計にしてある。`D3d11ModalBackend` 自体は範囲外 (M76i)。
