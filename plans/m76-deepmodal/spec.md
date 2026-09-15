# M76: Deep-Modal — 学習済み 3D-CNN による衝突音のモーダル合成 — 仕様書

- slug: m76-deepmodal
- 状態: 確定 (2026-09-16。司会経由でユーザーが確定。`[ユーザーに聞ける]` 3 件の回答は §2 #4 / #14 と §8 に反映)
- 依頼原文: "C:\HAL\MyEngin\plans\DeepModal" これを参考にDeepmodalを使った音作成をエンジンに実装する計画を立てて
- 参考: `plans/DeepModal/DeepModal_Implementation_Plan.md` (ユーザーの 15 Phase 計画)、`plans/DeepModal/ACMMM20_ModalSound.pdf` (Jin et al., ACM MM 2020)、`plans/m76-deepmodal/design-draft.md` (司会の設計案。本 spec が優先し、食い違いは §2 に理由を書いた)
- 基点: master `99803eb`。末尾 TypeId 60 (UIToggleGroup) → ModalSound = **61**。`kCookVersion = 3` 据え置き。ABI v19 不変。

## 1. 目的 (なぜ作るか)

録音済み SE に頼らず「**形状・接触位置・力の向きと強さ・材質・サイズで音が変わる**」衝突音を、
物理衝突から自動で鳴らせる状態にする。達成したい状態は次の 6 点 (ユーザー計画 §12):
形状で / 接触位置で / 衝撃方向と強さで / 材質で / サイズで音が変わる、
そして (後回し) 実行時に生まれた新形状でも音が出る。「現実音の完全再現」は目標にしない。

既存の音経路は 2 段ある — 波 (`AcousticField`、sim 側、敵 AI が聴く) と、その波を耳に出す一発再生
(`PendingWaveShot` → `MakeWaveShotPlay`、出力レーン)。M76 は**耳に出す側だけ**を差し替える。
波 (AI の聴覚) と sim 状態には 1 バイトも触れない。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| 1 | Python の FEM / 学習パイプラインまで「エンジンに実装」の範囲か | ユーザー計画 Phase 3–10 が明示的にオフライン工具として含める。データ段階の決定 (台帳) も前提にしている | 決定済み (台帳「ユーザー判断」3) | **範囲内**。`tools\deepmodal\` に置く (`tools\collab` = Rust と同じ「sln の外」の流儀) |
| 2 | 再生を衝突ごとの ≤2 s クリップ合成 + `RegisterClip` にしてよいか | `AudioSystem::RegisterClip` (AudioSystem.cpp:902) は同 id の voice を止めるだけで軽い。`SoundGenWindow::Preview` (181-202) が同型を既に使っている | 決定済み (台帳 1) | 採用。合成は v1 ではメインスレッド、`kMaxModalShotsPerTick = 4` と長さ上限で最悪 ~10 ms に抑える。`MakeModalShotPlay` を純関数にしてワーカーへ移せる形にしておく |
| 3 | ONNX/DirectML を使わず自前推論でよいか | 外部ランタイムは vendoring 方針に反する。ネットは ≤2M パラメータに絞れば CPU で 1 s 級 | 決定済み (台帳 2) | 採用。`ModalInferenceBackend` 抽象 + `CpuModalBackend`。GPU 実装 (`D3d11ModalBackend`) は差し込み口だけ |
| 4 | PhysMat に `poissonRatio` を足すべきか (ユーザー計画 Phase 13 は「最低限必要」に列挙) | 論文 §4.3 (式 10) は「ポアソン比の影響は複雑なので E と ρ だけスケールする」と明言。現行ランタイムのどのコードも ν を読まない | planner は「足さない」と裁定したが、**ユーザーが逆を選択**: 「poissonRatio は FEM / 教師データ生成と reference material metadata 用。現行 Deep-Modal の runtime material scaling には使用しない。将来 Poisson 比を考慮するモデルへ拡張可能な形で保持する」 | **足す** (E / ν / α / β の 4 つ)。JSON 往復 + Inspector 欄あり。**`BuildModes` / 材質スケーリングは ν を読まない** (読み始めるときは ADR を改訂する)。現時点で ν を読むのは Python 側の FEM (参照材質 = `.dmnet` ヘッダの `refPoisson`) だけ。planner の反対意見 (死にフィールドになる) は記録のみ、以後蒸し返さない |
| 5 | 力の結合を論文の式 9 (符号付き k_j) でなく \|k_j\| にしてよいか | 論文 §3 は帯域内モードを「振幅の和」で 1 本に畳み log を取る = 非負しか表せない。符号付き k を非負振幅に掛けると、逆向きの力で帯域が「打ち消される」が、物理では位相が変わるだけで消えない | 未確認 → planner 裁定 | **\|k_j\| と Σ\|a\|** で統一 (Python `compact.py` と C++ `BuildModes` の両方)。論文からの逸脱として ADR に明記。聴感への副作用 (同帯域で打ち消し合う 2 モードの過大) は許容 |
| 6 | ネット構造を論文どおり再現するか | 論文 Fig.3/4 は残差ブロック構成しか示さず、チャンネル数は本文に無い。CPU 推論 ≤1 s とリポジトリ ≤4 MB の制約が優先 | — | 自前の U-Net 型 (§4.2 F)。**予算 ≤ 2.0M パラメータ**を export が assert。構造は M76h の本学習まで変えてよい (M76e の時間計測で超過したら縮める) |
| 7 | 学習率 0.02 (論文) を既定にするか | BN 付き Adam で 0.02 は発散しやすい。既定 1e-3、`--lr 0.02` で論文値に戻せる | — | 既定 1e-3。行動が変わらない差なので聞かない |
| 8 | ボクセル化を Python にも持つか | 学習時とランタイムの規則不一致が最大の静かな壊れ方 (ユーザー計画 Phase 2 が「全く同じ規則」と強調) | — | **C++ の 1 本だけ**。Python は `Editor.exe --modal-voxelize` を `cmd /c` で呼ぶ (Editor は GUI サブシステム — CLAUDE.md 環境の罠)。代償: データ生成に Release ビルドの Editor.exe が要る |
| 9 | 衝突→音の差し込み点と tick の関係 | `TickRunner.cpp:608-645` の `!ts.resim` ブロックは波スロットを舐めるが、`solidContacts` (225 で束縛) は**今 tick**の接触。`DrainImpacts` (344) は**前 tick**の接触を消費する | — | modal は 608 のブロック内で今 tick の接触を直接読む = 同じ接触の波より **1 tick 早く鳴る**。波の耳出しは `ResolveWaveShotSound` の先頭で `mute` (Ready のときだけ) |
| 10 | サブの順序 — 最大リスク (eigsh の時間 / CPU 推論の時間) を先に潰すべきでは | deep-reasoning の原則。ただしユーザー計画 §11 は「1. Modal Synthesizer → 2. Voxelizer → 3. FEM …」を「崩さない方がよい」と明記 | ユーザー計画の順序 | **ユーザーの順序を維持** (M76a = 合成器)。代わりにリスクは**受け入れ条件で前倒し**: M76c に eigsh の最悪ケース時間、M76e にフルサイズ推論の時間を測る条件を置き、超過時は M76h 前にネットを縮める |
| 11 | `builtin://` メッシュの特徴マップをキャッシュしないでよいか | `ConvexCookSourcePath` (ConvexColliderLibrary.cpp:31-41) が `builtin://` を「クック対象外」にしている前例。ワーカー焼きは 0.5 s 級 | — | v1 はキャッシュしない (前例踏襲)。検証 run は `--modal-sync-bake` で決定的にする。後回し候補 (§3) |
| 12 | 音量を絶対 (ピーク正規化しない) にしてよいか | 「強く落とすと大きい」はユーザー計画 Checkpoint J の主張そのもの。正規化すると消える | — | 絶対音量 + `tanh` ソフトクリップ。「軽い接触が聞こえない / 重い衝突が張り付く」が出たら M76f の耳確認で `min(1, J/kImpactRefImpulse)` 圧縮を足すか決める (`[ユーザーに聞ける]` になる見込み、今は聞かない) |
| 13 | `SaveWav` を AudioClip.h に切り出す必要があるか (draft H 節) | `WriteWavToFile` / `WriteWavToMemory` が既に `SynthCore.h:43-46` にある | — | **切り出し不要**。M76g はそれを呼ぶだけ |
| 14 | M76h (ModelNet10 生成 ≈ 4.5 h + 学習 + 耳合わせ) をハーネスの 1 サブで閉じられるか | 長時間ジョブと「耳」はユーザーの機材と判断が要る。coder が待てるのは 1 セッション | **ユーザー確定**: 「stage1 で .dmnet コミットまで」 | M76h の受け入れは「**stage1 (小規模自前) で端から端まで通り、`.dmnet` をコミットし、ModelNet10 の手順が README で再現できる**」まで。ModelNet10 の実走はユーザーが README どおりに回し、結果の `.dmnet` 差し替えは通常コミット |
| 15 | `.vox` という拡張子 | MagicaVoxel の `.vox` と衝突し、関連付けで別ツールが開く | — | **`.mvox`** (magic 'MVOX' はそのまま) |
| 16 | 破壊 (Phase 15 / Checkpoint M) を入れるか | 破壊システム自体が未実装 (M61 予定)。ライブラリが「登録済みメッシュなら何でも焼く」形なら破片は自動で乗る | — | **範囲外**。設計上の担保だけ: `ModalSoundLibrary::Request(AssetID)` はメッシュの出自を問わない |

## 3. スコープ

- やる:
  - モーダル合成器 (再帰共振器) と後処理 `BuildModes` (材質・サイズ・減衰) — C++
  - PhysMat の音響材質 4 フィールド (E / ν / α / β。ν は保持のみ) + physmat JSON 11 本の初期値
  - C++ ボクセライザ (32³、表面 SAT + 6 近傍 flood-fill) と `.mvox`、OFF/OBJ 最小リーダ、`--modal-voxelize`
  - Python: hex8 FEM / 一般化固有値 / 接触励起 / Mel 圧縮 / データセット生成 / モデル / 学習 / export (`.dmnet` + fixture)
  - C++ 推論 (`ModalInferenceBackend` 抽象 + CPU 実装)、`.dmnet` ローダ、`.msfm` キャッシュ、`ModalSoundLibrary` (非同期焼き)、`--modal-bake`
  - ランタイム接続: `ModalSound` (TypeId 61)、接触 → impact → クリップ合成 → 回転プール → `Play`、wave の口封じ、レート制限、`--modal-audio-log` / `--modal-sync-bake` / `--modal-demo`
  - Editor: Inspector の面打ちプレビュー + WAV 書き出し、PhysMat 3 行、カタログ、ローカライズ
  - 本学習 (stage1 まで + ModelNet10 の手順)、文書 (engine_spec §10.7 / ADR / README / CLAUDE.md)
- やらない (明示的に外したもの):
  - 波 (`AcousticField`) と sim 状態への変更 (唯一の例外は `RestingImpulse` の**ビット中立な抽出**)
  - ストリーミング / 持続接触 (擦り・転がり) の音 (台帳 1)
  - ONNX / DirectML / 外部推論ランタイム (台帳 2)
  - ランタイムの材質スケーリングで `poissonRatio` を読むこと (§2 #4。フィールドは保持するが `BuildModes` は E / ρ / L / α / β しか使わない)
  - GPU 推論の実装 (`D3d11ModalBackend`) — 差し込み口と同じ fixture テストの設計だけ
  - 破壊 / 実行時生成メッシュの自動追従 (§2 #16)
  - Python 側の独自ボクセライザ (§2 #8)
  - golden スクショ / replay ペアの追加 (`--modal-demo` は出力レーンのみで主張が無い)
- 後回し:
  - `builtin://` メッシュの `.msfm` キャッシュ (頂点ハッシュをキーに)
  - 合成のワーカー移動 (スパイクが問題になったら)
  - ModelNet40、`D3d11ModalBackend`、絶対音量の圧縮 (M76f の耳確認で決める)

## 4. 仕様

### 4.1 振る舞い

**経路 (ランタイム)**
```
SolidContact (今 tick、TickRunner.cpp:608 の !ts.resim ブロック内)
 → CollectModalImpacts: ModalSound を持つ側ごとに PendingModalImpact (接触点・ローカル k・J_excess)
 → AudioSourceSystem::PushModalImpact (上限 64、溢れは数える)
 → AudioSourceSystem::Update (フレーム側): cooldown → Library::Request(mesh) が Ready か →
   MakeModalShotPlay (cell 選択 → BuildModes → ModalSynthRender) → 回転プール RegisterClip →
   PlayDesc{bus=SE, priority=128} + AudioSpatial (接触点、rolloff 0、doppler 0) →
   AcousticAudio + 場があれば ShapeAcousticSpatial (規則は 1 本) → Play
```
- **ModalSound を持つ物だけ** opt-in。無い物は従来どおり (WaveSound / 床材 / tone)。
- 両側が ModalSound なら両方鳴る。順序は contacts の key 昇順、entity ごとに `cooldownTicks` (既定 3)。
- `J_excess = impulse − RestingImpulse(world, ea, eb, gMag, dt)`。`RestingImpulse` は AcousticField.cpp:984-985 の式
  `(dynamicMass(ea) + dynamicMass(eb)) * gMag * dt * kImpactRestingMargin` を `acoustic::` の純関数に抽出したもので、
  `DrainImpacts` もそれを呼ぶ (**評価順を 1 文字も変えない**、replay_verify の acoustic ペアが証人)。
  `J_excess ≤ kImpactMinImpulse (0.35)` は鳴らさない。
- 法線: `SolidContact.normal` は大 index → 小 index。発音元 E が小 index なら `nE = n`、大なら `−n`。
  ワールド → ローカルは `WorldMatrixComponent` の逆行列 (今 tick の値: TransformSystem は 409 で確定済み)。
  `k = J_excess · nE_local`、式 9 へは `|k_x|, |k_y|, |k_z|`。
- 接触点 → cell: `v = clamp(floor((p_local − origin)/h), 0, 31)`、`cell = v >> 1`。無効 cell は `cellSlot[4096]` で最寄り有効 cell へ。
- **wave の口封じ**: `ResolveWaveShotSound` の先頭 (0) — 発音元が `ModalSound` を持ち `muteWave != 0` かつ `modalsound::IsReady(mesh)` なら `shot.mute = 1`。焼けるまでは従来の音が鳴る (段階移行)。波そのものは立つ。
- **後処理 `BuildModes`** (1 関数に順序固定):
  1. fp16 → float。チャンネル順 `MaskCh(j,i) = j*64+i` / `AmpCh(j,i) = j*64+32+i` (j = 力軸 0..2、i = 帯域 0..31)
  2. log-amp 逆正規化 `ln a = logAmpMin + v·(logAmpMax − logAmpMin)`、`amp = exp(ln a)`
  3. mask: `logit > ln(t/(1−t))`。t = `ModalSound.maskThreshold` (≤0 ならヘッダ既定)
  4. `a_i = Σ_j |k_j|·H(mask_j,i)·amp_j,i × hdr.ampScale × comp.gain`
  5. 帯域中心 (減衰) 周波数 `f_c = hdr.bandCenterHz[i]` → 参照材質の非減衰 λ: `c_ref = ½(α_ref + β_ref·(2πf_c)²)`、`λ = (2πf_c)² + c_ref²`
  6. スケール (式 12): `λ *= σ1/(σ2·σ3²)`、`a *= σ2^-½·σ3^-3/2`。σ1 = E/E_ref (E=0 → 1)、σ2 = ρ/ρ_ref、σ3 = L_obj/L_ref
  7. 目標材質 `c_i = ½(α + βλ)`、`f_i = √(λ − c_i²)/2π`。過減衰 / `f ≥ 0.45 fs` / `a ≤ kModalTailAmp` は捨てる
- **合成 `ModalSynthRender`**: 2 次再帰共振器 (double 状態) × ≤32 本、位相 0、1 ms 線形アタック、乱数不使用、
  mono / 44100 / int16。長さ `T = clamp(max_i ln(a_i/kModalTailAmp)/c_i, 0.05, 2.0)`、`kModalTailAmp = 1/32768`。
  音量は絶対 (正規化しない)、float 加算 → `tanh` ソフトクリップ (|x| > 0.8 のみ) → int16。同入力 → 同 PCM。
- **クリップ池**: `kModalClipSlots = 32`、id = `HashStr("modal://slot#k")`、ラウンドロビン。
  slot の `endTick > now` (自前の終了予定) しか無ければ捨てて `poolFull` を数える (鳴っている音を切らない)。
- **Mel**: `mel(f) = 2595·log10(1 + f/700)`、`mel(100)..mel(10000)` を 33 点等分、中心は区間中点の逆変換。
  実行時はヘッダの `bandCenterHz[32]` を使い、C++ の式は selftest がヘッダ値と 1e-2 Hz で照合 (ドリフト検知)。
- **焼き (ModalSoundLibrary)**: `Request(mesh)` は非ブロッキング (未着手なら positions/indices をコピーしてジョブ投入 → Baking)。
  ボクセル化は常にワーカー。推論はバックエンドの `RunsOnWorkerThread()` が true ならワーカー、false なら `Pump()` (メインスレッド、1 フレーム 1 ジョブ)。
  `.msfm` の読み書きはメインスレッド。失敗は `Failed` + WARN 1 回。モデル無しは `NoModel`。
- **ボクセル化**: `L = max(aabb extent)`、`h = L/29`、`origin = center − 16h` (AABB は voxel 座標 1.5..30.5 = 軸平行面がボクセル中心を通る。単位立方体 = 30³ = 27000)。
  index `x + 32(y + 32z)`。表面 = Akenine-Möller SAT (箱半径 `h/2 + 1e-6h`)、内部 = pad リングから 6 近傍 flood-fill で外部を塗った残り。
  非 watertight は殻だけ (論文「薄いものは 1 voxel 厚」と同じ扱い)。明示スタック、固定順、同入力 → 同出力。

**経路 (オフライン、Python)** — ユーザー計画 Phase 3–10 / データ段階 (台帳 3)
```
メッシュ列挙 → Editor.exe --modal-voxelize (C++ の同じ規則) → .mvox
 → hex8 FEM (単位 Ke/Me を h,E,ν,ρ でスケール) → eigsh(K, k≤256, M, sigma=0) → 剛体 6 モード除去 → 100–10000 Hz
 → 16³ cell ごと接触節点 (cell 内占有 voxel の節点で中心に最寄り、同値は最小 index) → a_ij = |U[dof_j(node), i]| / ω_i
 → Mel 32 帯域に Σ|a| → mask / ln / 0..1 正規化 / 空帯域は最寄り非空の値 (論文の trick) → npz
 → model.py (3D U-Net、≤2M params) → train.py (Adam 1e-3, batch 16, 100 epoch, 20 epoch ごと半減、MSE(amp) + BCEWithLogits(mask))
 → export.py (BN 畳み込み → fp16 → .dmnet + fixture)
```
- データ段階の門: **`train.py --overfit 16 --epochs 300` が amp MSE < 1e-3 かつ mask acc > 99% を満たすまで ModelNet の生成コマンドを実行しない** (README に「実行禁止」と明記)。
- 参照材質 (初期値、stage0 統計で確定して `layout.py` と `.dmnet` ヘッダに固定): E=7.0e10, ρ=2700, ν=0.33, α=6, β=1e-7 (アルミ相当)、L_ref = 0.3 m。

### 4.2 データ・保存形式・互換性

- ディスクに書く POD は **struct を memcpy しない** (Material の暗黙パディングの罠)。フィールド単位、固定長ヘッダ、版番号。
- **`.mvox`**: 64 B ヘッダ (magic 'MVOX' / version 1 / n=32 / pad / origin[3] / voxelSize / aabbMin[3] / aabbMax[3] / longestEdge / surfaceCount / interiorCount / reserved) + 32768 B 占有。
- **`.msfm`** (cooked cache、`.mcvx` と同型): `vector<pair<meshName, ModalFeatureMap>>` key 昇順、1 モデルファイル 1 表、遅延で書き足す。
  `ModalFeatureMap { version; uint64 modelHash; VoxelFrame frame; validCount; uint16 cellSlot[4096]; vector<uint16> feat /* validCount×192 fp16 */ }` (有効 cell だけ、典型 200–500 KB)。
  内部版 `kMsfmVersion = 1` を持つので `kCookVersion` (= 3) は据え置き。`modelHash ≠ 現在の .dmnet の weightsHash` はミス扱いで焼き直す。
  クック元パスは `assetkey::SourcePathForSubAssetKey(meshName)` (新設、`Asset/`) で引き、`ConvexCookSourcePath` はこれに委譲 (ビット中立)。`builtin://` はパス無し = 毎起動焼く。
- **`.dmnet`**: 256 B ヘッダ (magic 'DMNT' / version / opCount / bufferCount / inN 32 / outN 16 / bands 32 / channels 192 / fMinHz / fMaxHz / logAmpMin / logAmpMax / ampScale / maskThreshold / refYoung / refDensity / refPoisson / refSizeL / refAlpha / refBeta / bandCenterHz f32×32 / weightsHash u64 (FNV-1a) / paramCount / reserved)
  + op 表 `{ type (Conv3d=0 / ConvTranspose3d=1 / ReLU=2 / Add=3) / in0 / in1 / out / cin / cout / k / stride / pad / outPad / weightOffset / biasOffset }` + fp16 重み + fp32 バイアス。BN は export で畳み込み済み。
  置き場 `assets\deepmodal\deepmodal.dmnet` (≤ 4 MB、コミット)。解決は「プロジェクト assets → エンジンリポジトリ assets」の 2 ルート (`FindEngineDeepModalDir` は `FindEngineShaderDir` と同じく単ルートを返し、呼び手が 2 ルートにする — EngineLoop.cpp:251-259 と同型)。
  ReloadHub: `{ L".dmnet", ReloadKind::ModalNet, 6 }` (整数は**rank**)。差し替えたら Library を Clear → 遅延で焼き直し。
- **PhysMat** (PhysMatLibrary.h:33-67 末尾 append、この順): `youngsModulus = 0` (Pa、**0 = 参照材質 = σ1 = 1**) / `poissonRatio = 0.3f` (無次元。**保持のみ** — 現行ランタイムは読まない。FEM / 参照材質メタデータ用、将来 ν を考慮するモデルのために持つ。ユーザー判断 §2 #4) / `rayleighAlpha = 0` (1/s) / `rayleighBeta = 0` (s)。ToJson は常に書く、FromJson は contains + 既定、Sanitize は E ∈ [0, 1e13] / ν ∈ [0, 0.49] / α ∈ [0, 1e4] / β ∈ [0, 1e-2]。**sim は読まない**。
  初期値 (E / ν / α / β、耳で詰める出発点): metal 7.0e10/0.33/6/1e-7、steel 2.0e11/0.30/5/3e-8、glass 6.2e10/0.22/1/1e-7、tile 7.4e10/0.19/6/1e-7、wood 1.1e10/0.35/10/2e-6、rubber 5e6/0.49/60/5e-5、ice 9e9/0.33/8/5e-7、carpet/gravel/glue/water は E/α/β = 0、ν = 0.3。
  `ModalPostParams` / `BuildModes` の引数に ν は**含めない** (読んでいないことを型で示す)。
- **ModalSoundComponent** (TypeId 61、`kComponentNoHash`、`RegisterBuiltinComponents()` 末尾に append): `AssetID mesh` (空 = 同 entity の `MeshRenderer.mesh`) / `float gain = 1` / `float maskThreshold = 0` / `int32 cooldownTicks = 3` / `float sizeScale = 1` / `float maxDistance = 30` / `int32 muteWave = 1`。NoHash なので Scene / .rep / snapshot 版は不変。
- **fixture** (`tests\deepmodal\`): `fixture.dmnet` (幅 4/8/8/8 の小ネット、乱数重み ≈ 50 KB) / `fixture_in.mvox` / `fixture_out.bin` (cell index 表 + 64 cell × 192 float32。**fp16 に丸めた重みで fp32 計算**した期待値) / `list_builtin.txt`。`.gitattributes` に `*.dmnet binary` / `*.mvox binary` / `*.msfm binary` / `tests/deepmodal/*.bin binary` を足す (autocrlf=true 前提のリポジトリ)。
- `.gitignore`: `tools/deepmodal/data/`、`tools/deepmodal/**/__pycache__/`、`tools/deepmodal/.pytest_cache/`、`tools/deepmodal/runs/`。
- `check_rules.ps1` の `$constGroups` に C++ ⇄ Python の 4 組 (`kModalVoxelN ⇄ VOXEL_N` / `kModalMapN ⇄ MAP_N` / `kModalBands ⇄ MEL_BANDS` / `kModalChannels ⇄ CHANNELS`)。規則 9 は「1 ファイル 1 整数」の正規表現照合なので Python 側もそのまま乗る (check_rules.ps1:515-537)。

### 4.3 UI / ビジュアル

- Inspector の `ModalSound` 節末尾: 状態 (Missing / Baking / Ready / Failed / NoModel + バックエンド名) と有効 cell 数、
  **6 面ボタン** (`+X −X +Y −Y +Z −Z`: 接触点 = AABB 面中心、力 = 内向き法線 × スライダ [N·s] 0.1..20) → `MakeModalShotPlay` (**ランタイムと同じ関数**) → `RegisterClip(HashStr("modal://preview"))` → `Play` on `kBusUi` (SoundGenWindow.cpp:181-202 と同型)。
  **Export WAV** は `WriteWavToFile` (SynthCore.h:46) を呼ぶ。Baking 中はボタン disabled。
- PhysMat インスペクタに 4 行 (E / ν / α / β。ν のツールチップに「現行ランタイムは未使用 (FEM / 参照材質用)」)。`EditorComponentCatalog` に `ModalSound` (Audio 分類)。`LocalizationTable.inl` に en/ja。
- スクショ golden は増やさない (音の機能なので絵に出ない)。

### 4.4 非機能

- **決定論**: sim 状態に触れない。`ModalSound` は NoHash。push は `!ts.resim` ブロック内 + `audioSystem.IsReady() && !IsSuspended()` (TickRunner.cpp:622 と同じ門)。消費はフレーム側。replay 8 ペア / golden 24 枚は不変。乱数不使用。`MYE_CHECK`、`#ifdef _DEBUG` 分岐なし、宣言時初期化、unordered でバイト列を作らない (表は sorted vector)。
- **include の向き**: Engine/Audio → Engine/Modal → (Core / Renderer/GpuResources / Asset)。`Modal/` は Audio / Acoustic / World を include しない。`Audio/ModalAudio.cpp` が Acoustic (`RestingImpulse`) と Modal を include するのは許容。
- **スレッド**: ワーカーは `TextureLibrary::AsyncWorker` (GpuResources.cpp:720) と同じ mutex + cv + deque。ジョブは enqueue 時に頂点をコピーし `shared_ptr<const DmNet>` を掴む (ワーカーは RenderResources に触らない)。終了は `Shutdown()` で join (`convexcol::Install(nullptr)` の隣、EngineLoop.cpp:2530 付近)。
- **性能目標**: CPU 推論 (Release、フルサイズ ≤2M params) ≤ 1.5 s/メッシュ — M76e で計測し超過なら M76d の構造を縮めてから M76h へ。合成 ≤ 3 ms/発。eigsh: 最悪ケース (満杯 30³ 立方体 = 約 90k DOF) < 600 s、primitive 中央値 < 60 s。
- **互換**: `kCookVersion` 据え置き、ABI v19 不変、Scene 版不変。旧 physmat JSON はキー無し = 0 = 参照材質 (σ1 = 1) で従来どおり無音側には影響しない (ModalSound が無ければそもそも読まれない)。
- **ローカライズ**: `Tr()` を printf の唯一の引数にしない、`###` 右辺一致・一意。
- **CI**: 新しい bat は作らない。`--modal-audio-log` の 2 run 一致はローカル検証 (CLAUDE.md の検証表に追記)。

## 5. 受け入れ条件

| # | 条件 | 検証手段 |
|---|---|---|
| 1 | `ModalSynthRender`: 1 モード (f=440, c=5, a=0.5) の零交差周波数が ±1%、`RmsIn` 比 [0.1,0.2]/[0.6,0.7] が e^{−2.5} ±5%、a×2 → ピーク×2、同入力 2 回 memcmp 一致、長さ規則 (0.05 / 2.0 の両端) | `Editor.exe --selftest` (ModalSynthSelfTest) |
| 2 | `BuildModes`: E×4 → f×2、ρ×4 → f/2 & a/2、L×2 → f/2 & a/2^1.5 (相対 1e-4)、mask 閾値で帯域が落ちる、k=0 → count 0、過減衰・ナイキスト超えが落ちる、`MelBandCenters` が double 期待値と 1e-2 Hz | 同上 |
| 3 | PhysMat 4 フィールド (E / ν / α / β): JSON 往復、Sanitize 範囲、キー無し旧 JSON → 既定 (E/α/β = 0、ν = 0.3)。11 本の JSON に初期値。`BuildModes` の署名に ν が無い。replay_verify 全ペア不変 | `--selftest` (PhysMatSelfTest) + `tools\replay_verify.bat` |
| 4 | Voxelizer: 単位立方体 → surface+interior == 27000、中心 1、隅 0、pad リング全 0 / 厚さ 0.001 の板 → y 占有 index 1 種 / 蓋なし箱 → interior 0 / 2:1:0.5 AABB → 最長辺 29 voxel / +X 面中心 → cell (15, 7\|8, 7\|8) / cellSlot: 有効は自身、無効は独立総当たりと一致 / `.mvox` 往復 memcmp / OFF/OBJ リーダ / 同入力 2 回 memcmp | `--selftest` (ModalSelfTest) |
| 5 | `Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out DIR` → 6 ファイル、exit 0。存在しない入力 → exit 1 | 手動 cmd (`cmd /c` 経由) |
| 6 | pytest 全緑: test_fem (Ke 対称・半正定、剛体 6 モードで K·r ≈ 0、集中質量総和 = ρh³) / test_modal (2×2×2 で E×4 → ω×2、ρ×4 → ω/2、h×2 → ω/2、1e-6。先頭 6 固有値 ≈ 0) / test_compact (単調・端点・Σ\|a\|・空帯域補間・mask) / test_layout (.mvox 64 B、C++ cube で 27000 — Editor.exe 無ければ skip) / test_contact (cell を変えると励起が変わる) | `cd tools\deepmodal && pytest` |
| 7 | `dataset.py --stage primitives` が npz ≥ 20 本 + builtin 6 本と `stats.json` (メッシュごとの eigsh 秒、帯域占有率、モード数分布) を出す。満杯 30³ 立方体の eigsh < 600 s、primitive 中央値 < 60 s。L_ref / fMax の確定値を `layout.py` と spec §8 に記録 | 実行ログ + stats.json |
| 8 | `check_rules.ps1` 全規則緑 (constGroups の C++ ⇄ Python 4 組を含む。片方を変えると赤くなることを 1 回確認) | `pwsh -File tools\check_rules.ps1` |
| 9 | **門**: `train.py --overfit 16 --epochs 300` (stage0 + stage1 の 16 形状) → amp MSE < 1e-3 かつ mask acc > 99%。ログをコミットメッセージ本文に残す | 実行ログ |
| 10 | `export.py`: fixture 4 点 (`fixture.dmnet` / `fixture_in.mvox` / `fixture_out.bin` / `list_builtin.txt`) をコミット。`paramCount ≤ 2,000,000` の assert、`weightsHash` = FNV-1a、`bandCenterHz` = compact.py の値。`--random-full` でフルサイズ乱数重みの `.dmnet` (時間計測用、コミットしない) が出る | pytest (test_export) + 手動 |
| 11 | `CpuModalBackend`: fixture で 64 cell × 192 の `max\|Δ\| < 1e-3` (**インストール済みバックエンドに対して回す**) / Conv3d・ConvTranspose3d の小ケースが素朴 6 重ループと 1e-6 で一致 / フルサイズ乱数 `.dmnet` の Release 推論 ≤ 1.5 s (`--modal-bake` の ms 欄) | `--selftest` (ModalSelfTest) + `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` |
| 12 | `.msfm` 表の往復 memcmp / `SourcePathForSubAssetKey("builtin://cube")` が空で、guid キーでは `ConvexCookSourcePath` と同結果 / `CookedCacheSelfTest` 不変 | `--selftest` |
| 13 | `ModalSoundLibrary`: `Register` → `Get`、モデル無しで `NoModel`、`BakeSync` (fixture + 立方体) → `Ready` かつ validCount = voxel から独立に数えた有効 cell 数。`--modal-bake` が 1 行/メッシュ、2 回目は全部 cached。`--modal-backend d3d11cs` は WARN + cpu 縮退、綴り違いは exit 1 | `--selftest` + 手動 cmd + EngineCliSelfTest |
| 14 | ReloadHub: `.dmnet` → `ModalNet` (rank 6) | `--selftest` (ReloadHubSelfTest) |
| 15 | `CollectModalImpacts`: 箱 (ModalSound) と床の接触 1 件 → 1 impact、Y 90° 回転で k がローカル軸に乗る、`excess = impulse − RestingImpulse` / 両側 ModalSound → 2 impact、key 昇順 / `RestingImpulse` が抽出前の式とビット一致 / push 65 個目は dropped / cooldown / 合成マップ (全帯域 on) で k=(1,0,0) → count 32、k=0 → BelowMin、maxDistance = comp 値 / `ResolveWaveShotSound`: Ready → mute=1、未 Ready → 従来 soundKey / suspend 中の Update でキューが空になる | `--selftest` (ModalAudioSelfTest) |
| 16 | `Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --frames 300 > a.txt` を 2 回 → `[modal] t=` 行が byte 一致、`played > 0 && playFailed == 0`。`--no-audio` 併用で 0 行 | 手動 cmd (Release) |
| 17 | 共通検証: Debug/Release ビルド `/p:MyeWarnAsError=true` 0 警告 → `--selftest` 全緑 → `check_rules.ps1` → `replay_verify.bat` 全ペア緑 (M76f 以降は必須。golden は触らないので shot_verify は M76h で 1 回) | 各コマンド |
| 18 | Editor: Inspector の ModalSound 節 (状態 / cell 数 / 6 面ボタン + スライダ / Export WAV)、PhysMat 4 行、カタログ、en/ja。LocalizationSelfTest 緑。手動: 6 面で音が変わる、WAV が出て再生できる | `--selftest` + 手動 |
| 19 | stage1 (小規模自前 ≤ 100) で `dataset → train → export → --modal-bake → --modal-demo` が端から端まで通り、`assets\deepmodal\deepmodal.dmnet` (≤ 4 MB) をコミット。ModelNet10 の手順 (時間見積もり・実行禁止の門・再開方法) が README にある | 実行ログ + README |
| 20 | 文書: `engine_spec.md` §10.7、`docs\adr\ADR-0NN-deep-modal.md` (次の空き番号)、`README.md`、`CLAUDE.md` (末尾 TypeId 61 / Cloth・SoftBody 62/63 / CLI 6 本 / 検証表に `--modal-audio-log` 2 run 一致 / 「.dmnet を差し替えたら --modal-bake」/ constGroups の Python 組) | 目視 + check_rules |

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 (M76a) | モーダル合成器と材質パラメータ | なし | 1, 2, 3 | `M76a: モーダル合成器 (再帰共振器 / BuildModes / Mel 表) と PhysMat の音響材質 4 フィールド` |
| sub-02 (M76b) | ボクセライザと `--modal-voxelize` | なし (sub-01 と並列可) | 4, 5 | `M76b: 32³ ボクセライザ (.mvox) と OFF/OBJ リーダ、--modal-voxelize` |
| sub-03 (M76c) | Python: FEM / 固有値 / 接触励起 / Mel 圧縮 / データセット | sub-02 | 6, 7, 8 | `M76c: Deep-Modal データセット生成 (hex8 FEM / eigsh / 接触励起 / Mel 圧縮) と pytest` |
| sub-04 (M76d) | Python: モデル / 学習 / export / fixture (**大規模生成の門**) | sub-03 | 9, 10 | `M76d: Deep-Modal のネット / 学習 / export (.dmnet + fixture)、overfit の門` |
| sub-05 (M76e) | C++ 推論 / バックエンド抽象 / .msfm / ModalSoundLibrary / `--modal-bake` | sub-02, sub-04 | 11, 12, 13, 14 | `M76e: .dmnet ローダと CPU 推論バックエンド、.msfm キャッシュ、ModalSoundLibrary、--modal-bake` |
| sub-06 (M76f) | ランタイム接続 (ModalSound / 接触 → 音 / wave 口封じ / CLI / demo) | sub-01, sub-05 | 15, 16, 17 | `M76f: ModalSound コンポーネントと衝突 → モーダル合成の接続、--modal-audio-log / --modal-demo` |
| sub-07 (M76g) | Editor (面打ちプレビュー / WAV / PhysMat 欄 / カタログ / 文字列) | sub-06 | 18 | `M76g: Inspector の ModalSound 面打ちプレビューと WAV 書き出し、PhysMat の音響材質欄` |
| sub-08 (M76h) | 本学習 (stage1) と文書 | sub-06, sub-07 | 19, 20, 17 | `M76h: Deep-Modal の stage1 学習済みモデルと文書 (engine_spec / ADR / README / CLAUDE.md)` |

推奨実行順: 01 → 02 → 03 → 04 → 05 → 06 → 07 → 08 (01 と 02 は入れ替え可)。

## 7. 未決事項・リスク

(策定時の `[ユーザーに聞ける]` 3 件は回答済み — poissonRatio は保持する (§2 #4)、M76h は stage1 まで (§2 #14)、計画は確定。詳細は §8)

- 実装中に判明する見込み (coder が「不安・質問」で拾う):
  - 絶対音量に上限圧縮を足すか (sub-06 の耳確認で決める。§2 #12)
  - L_ref / fMax の確定値 (sub-03 の stats で決める。決めたら §8 に積む)
  - フルサイズ推論が 1.5 s を超えたときのネット縮小幅 (sub-05 → sub-04 差し戻し)
  - `ModalSound.mesh` (AssetID) の FieldType — `MeshRenderer.mesh` と同じ widget が使えるか
  - eigsh の shift-invert (SuperLU fill-in) が 90k DOF で 600 s を超える場合の代替 (LOBPCG / k の削減 / 占有上限)
- リスク:
  - TypeId 61 の衝突: M75h (InputField) が先に master へ入れば 62 (登録順なのでコード位置を後ろへ動かすだけ)
  - `|k|` と Σ|a| は論文からの逸脱 (§2 #5)。同帯域で打ち消し合う 2 モードが過大になる。ADR に明記
  - 合成のメインスレッド負荷: 2 s × 32 モード ≈ 1–3 ms/発、4 発/tick で最悪 ~10 ms のスパイク。`MakeModalShotPlay` は純関数なのでワーカーへ移せる
  - wave との二重発音: 初回接触 (NotReady → wave) の 1 発だけ両方鳴り得る。段階移行の意図どおり、WARN で `--modal-bake` を促す
  - `builtin://` は毎起動焼く (0.5 s 級 × 6)。検証 run は `--modal-sync-bake` 必須
  - fp16 の特徴量: fixture は fp16 に丸めた値で期待値を作る
  - GPU バックエンドは `Infer` がメインスレッド専用になる — `Pump()` の 1 フレーム 1 ジョブで吸収する設計にしてある

## 8. 変更履歴

- 2026-09-16 (ユーザー回答、司会経由): `poissonRatio` を PhysMat に**保持フィールドとして追加** (planner 裁定「足さない」を却下)。理由: FEM / 教師データ生成と reference material metadata 用、将来 ν を考慮するモデルへ拡張可能な形で持つ。ランタイムの `BuildModes` は読まない。反映: §2 #4 / §3 / §4.2 / §4.3 / §5 #3・#18 / §6 sub-01 件名 / sub-01・sub-07。
- 2026-09-16 (ユーザー回答): M76h の範囲は「stage1 で .dmnet コミットまで」で確定 (planner 裁定どおり)。計画全体を確定。
