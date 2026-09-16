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
  - Editor: Inspector の面打ちプレビュー + WAV 書き出し、PhysMat 4 行、カタログ、ローカライズ
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
- **ボクセル化**: `L = max(aabb extent)`、`h = L/28`、`origin = center − 16.5h` (AABB は voxel 座標 2.5..30.5 = 軸平行面がボクセル中心を通り境界に乗らない、**かつ AABB 中心が voxel 16 の中心 (16.5) に乗る** = 中心対称な薄い特徴が 2 行に割れない。単位立方体 = 29³ = 24389。★`L/29` + `center − 16h` だと AABB 中心が voxel 15/16 の境界に乗り、厚さ ≪ h の板が必ず 2 行になる — sub-02 round 1 で判明。奇数個に割ると中心は必ず境界なので、偶数 28 に割る)。
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
- ★**FEM は「参照サイズ」で組む (sub-04 round 1 で判明した欠陥の修正)**: `dataset.py` は FEM の要素寸法に**メッシュ実寸の `grid.voxel_size` を渡してはいけない**。必ず `h_ref = L_REF / 28` (全メッシュ共通) を使う。
  理由 1 (学習が成立しない): ボクセル化は AABB の最長辺で正規化する = **入力はスケール不変**。一方 FEM に実寸を渡すと固有周波数が 1/L で動くので、**同じ入力ボクセルに異なる教師値**が生まれる。実測 (stage0): `cylinder_0` / `cylinder_3` / `cylinder_5` は占有 1305 で**ボクセル列がバイト一致**するのに実寸が L = 0.330 / 0.373 / 0.566 と違い、`feat` が最大 **7.0** 食い違う。どんなネットでもこの 3 本は同時に満たせない。
  理由 2 (ランタイムが二重にスケールする): 論文 §5.1 は「学習時は全物体を**同じ材質・同じスケール**にする」と明記し、サイズの違いは後処理の σ3 が担当する (§4.1 手順 6)。教師値に実寸が入っていると、`BuildModes` が σ3 = L_obj/L_ref を**もう一度**掛けることになり、参照サイズ以外の物体で音が系統的にずれる。**sub-05 / sub-06 の正しさに直結する**。
  修正後は上記 3 本の教師値が一致する (同じ入力 → 同じ出力) ので、重複は害のない冗長データになる。
- **固有値解法の 2 経路と品質指標 (sub-03 round 1 で確定。ユーザー追加指示 2026-09-16)**: 占有 voxel 数 ≤ `MAX_OCCUPIED_EXACT` (= 9000) は shift-invert `eigsh` (完全 LU)。超過分は不完全 LU 前処理の LOBPCG へ回す — 完全 LU の fill-in が要素数の 2 乗超で増える (実測 54000 DOF で nnz 4.0M → 230M = 57 倍) ため、満杯 29³ (81000 DOF) では空き RAM を超える。
  ★**採否は solver 名ではなく数値で決める** (ユーザー指示)。`method` はメタデータとして残すが、モードを捨てる / 重みを下げる判断に**使わない**。判断材料は次の 2 つ:
  1. **固有対ごとの相対残差** `r_i = ‖K x̂_i − λ_i M x̂_i‖₂ / ‖λ_i M x̂_i‖₂`。`x̂` は **M-正規化** (`x̂ᵀ M x̂ = 1`) した固有ベクトル — 正規化を固定しないと値が比較できない。**剛体 6 モードを除いた後の、帯域に残るモードについてだけ**計算する (λ → 0 では分母が退化して意味を失う)。
  2. **基準形状での直接法との突き合わせ** (下の受け入れ条件 21)。残差は「その固有対が正確か」しか言わず、**モードの取りこぼし**は検出できない (LOBPCG はブロック幅 m の都合でモードを丸ごと落としうる。落ちた分も残っている分は小さい残差を示す)。集合として完全かは直接法との比較でしか分からない。
  - **しきい値 (暫定。基準形状の実測で確定させる)**: `RESIDUAL_ACCEPT = 1e-5` 以下 = そのまま採用 / `1e-5 < r ≤ RESIDUAL_DROP = 1e-3` = 採用するが品質フラグを立てる / `r > 1e-3` = **そのモードを Σ\|a\| から除外**する (未励起しきい値と同じ場所で落とす)。根拠 (planner が 12³ ブロック 6591 DOF で実測、2026-09-16): 直接法の残差は max 1.6e-8 / median 1.5e-9、LOBPCG は max 4.8e-6 / median 9.6e-8 で、両者の周波数は相対 1e-11 で一致した。`1e-5` は「実測した LOBPCG の最悪値の約 2 倍」= その品質のモードは直接法と一致することが確かめられている水準。
  - **保存先は npz と stats.json の両方**:
    - solver metadata: `method` / パラメータ (m, maxiter, drop_tol, fill_factor, k, shift) / 反復回数 / 収束フラグ
    - convergence quality: **モードごとの残差配列** (≤256 float) と要約 (`residual_max` / `residual_median` / 除外したモード数)
  3. **Mel-band coverage (ユーザー指示 2026-09-16)**。残差も基準形状比較も「計算したモードが正しいか」しか言わない。**計算していないモード**は別の軸が要る。ただし判定を「固定モード数を達成したか」に置かない (**`spectrum_complete` を合否条件に使わない**) — 必要なのはモードの本数ではなく、**100–10000 Hz を Mel 32 帯域でどれだけ覆えているか**。
     - **定義 (正本)**: 帯域が「覆われている」= その cell で **3 力軸のいずれか**が mask を立てている。
       `cell_band_mask[cell][i] = OR_j mask[j][i]`。
       ★**OR にする理由**: ランタイムの `BuildModes` 手順 4 が `a_i = Σ_j |k_j|·H(mask_j,i)·amp_j,i` と**軸を足す**ので、1 軸でも立っていればその帯域は音になる。AND や平均にすると「実際に鳴る帯域」と指標がずれる。軸ごとの内訳は診断用に別途持つ。
     - **記録 (メッシュ単位)**: `band_coverage[32]` = 有効 cell 全体での `cell_band_mask` の平均 (帯域ごとの占有率)。**この 32 本の分布が「高域が系統的に空か」を見せる本体**。要約として `coverage_ratio` (32 帯域の平均) と `coverage_high` (上位 8 帯域 = index 24..31 の平均) を併記する。
     - **記録 (cell 単位)**: `cell_coverage[valid_cells]` (float32、`feat` の行と同じ順序) = その cell で覆えている帯域の割合。cell 単位の重み付け・除外を後から選べるようにする (数 KB なので保存コストは無視できる)。
     - ★**名前を 2 つ持たない**: 既存の `band_occupancy_ratio` は上の `coverage_ratio` と同義になるので、**どちらか 1 つに寄せる** (別定義の似た名前が 2 つあると、後で静かに食い違う)。
     - `modes_requested` / `f_top` / `spectrum_complete` / `mode_count` は**診断値として記録を続けてよい** (coverage が低いときに「予算で切れたのか、そもそもモードが無いのか」を切り分けられる)。ただし**採否・重み付けの判断には使わない**。
     - ★参考 (round 2 実測): 予算で切れるのは LOBPCG だけではない。直接法も `k` (既定 150) で頭打ちになっていた (capsule / sphere とも帯域フィルタ後がちょうど 150 本 = 制約は帯域ではなく k)。だから指標は**両経路で同じもの**を取る。
  - **粒度の使い分け** (ここを混ぜると死にフィールドになる): モード単位の除外は **npz を作る時点**でしか効かない (Mel 圧縮で個々のモードは消えるため)。学習時 (sub-04) に効かせられるのは**メッシュ単位の重み付け・除外**なので、そのための要約値を npz に持たせる。per-mode の残差配列は診断と再生成判断のために残す (per-channel の重みには使えない)。
  - ★**縮退モードの罠**: 対称形状 (立方体・球 = builtin と ModelNet の多く) は固有値が縮退する (planner の実測: 立方体で 10964 Hz が 2 重、14726 Hz が 3 重)。縮退した固有空間の中では**個々の固有ベクトルは一意に決まらない** (基底の取り方は任意) ので、解法間で per-mode の `a_ij` は**両方正しくても食い違う**。基準形状の突き合わせは**周波数と帯域集計 (Σ\|a\|) で比較し、生の固有ベクトルを直接比較しない**こと。
  - `builtin://` 6 種は受け入れ条件 7 が要求するので cap を超えても npz を書く (= LOBPCG 経路)。この非対称は意図的で、残差と metadata の記録がその可視化。しきい値と LOBPCG パラメータは M76h で実分布を見て再調整する。
- データ段階の門: **`train.py --overfit 16 --epochs 300` が amp MSE < 1e-3 かつ mask acc > 99% を満たすまで ModelNet の生成コマンドを実行しない** (README に「実行禁止」と明記)。
- 参照材質 (初期値、stage0 統計で確定して `layout.py` と `.dmnet` ヘッダに固定): E=7.0e10, ρ=2700, ν=0.33, α=6, β=1e-7 (アルミ相当)、L_ref = 0.3 m。

### 4.2 データ・保存形式・互換性

- ディスクに書く POD は **struct を memcpy しない** (Material の暗黙パディングの罠)。フィールド単位、固定長ヘッダ、版番号。
- **`.mvox`**: **72 B** ヘッダ (18 フィールド × 4 B、フィールド単位で書く: magic 'MVOX' / version 1 / n=32 / pad / origin[3] / voxelSize / aabbMin[3] / aabbMax[3] / longestEdge / surfaceCount / interiorCount / reserved) + 32768 B 占有 = 32840 B。(策定時の「64 B」は planner の計算違い。sub-02 round 1 で訂正)
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
- **性能目標 (sub-09 で再改訂。ユーザー判断「今すぐ最適化する」)**: CPU 推論 (Release、フルサイズ ≤2M params) **≤ 0.6 s/メッシュ** (目標 ≤ 0.3 s、受け入れ条件 22)。最適化前の暫定値は ≤ 6 s だった (下記はその根拠で、**「縮めない」「疎対応しない」という結論は sub-09 でも維持される**)。
- (sub-05 round 1 時点の暫定値と根拠): CPU 推論 **≤ 6 s/メッシュ**。★旧「≤ 1.5 s」は planner が**何を守る数字か書かずに**置いた値で、実測 4.75–5.34 s に対して「ネットを縮める」判断を促す形になっていた。これが守るべきなのはフレーム時間ではなく**初回起動の体感**である — 焼きは (a) ワーカースレッドで走り (b) メッシュごとに**一度だけ** (2 回目以降は `.msfm` ヒットで実測 0.38 ms) (c) 焼けるまでは従来の音が鳴る段階移行 (§4.1)。エンジンの `assets` は 19 モデルなので初回の裏作業は合計 100 s 前後で、どのフレームも止めない。
  ★**構造を縮めて時間を作らないこと**。実測の実効スループットは **0.39–0.43 GMAC/s** (総 2.06 GMAC) で、これは単一スレッド・SIMD 無し (§4.4 の CpuModalBackend の設計どおり) の値。SIMD / マルチスレッドで 10–50 倍の余地があり、**アーキテクチャではなく実装の問題**。ネット幅を縮めると sub-04 の門 (R² ≥ 0.90) を測り直す = サブをまたぐ差し戻しになるうえ、モデル品質を落として未最適化カーネルを埋め合わせることになる。
  ★**疎な占有を使った書き直しは選択肢にならない**。畳み込みは 32³ を密に舐めるので占有率とコストは無関係 (実測でも `validCells` に依らずほぼ一定 = 正しい挙動)。cell 有効性で飛ばせるのは head だけで、MAC 内訳では **13.4%** が上限。支配項は高解像度側 — `convT k4 64→32 →16³` が 26.0%、`conv3 16→16 @32³` / `res(32)@16³` ×2 / `head conv3 32→64` が各 11.0%。
  ★最適化してよい根拠: fixture の selftest が `max|Δ| < 1e-3` で、加算順が変わる SIMD 化はこの許容内に収まる = **正しさを守ったまま後から速くできる**。合成 ≤ 3 ms/発。eigsh: 最悪ケース (満杯 30³ 立方体 = 約 90k DOF) < 600 s、primitive 中央値 < 60 s。
- **互換**: `kCookVersion` 据え置き、ABI v19 不変、Scene 版不変。旧 physmat JSON はキー無し = 0 = 参照材質 (σ1 = 1) で従来どおり無音側には影響しない (ModalSound が無ければそもそも読まれない)。
- **ローカライズ**: `Tr()` を printf の唯一の引数にしない、`###` 右辺一致・一意。
- **CI**: 新しい bat は作らない。`--modal-audio-log` の 2 run 一致はローカル検証 (CLAUDE.md の検証表に追記)。

## 5. 受け入れ条件

| # | 条件 | 検証手段 |
|---|---|---|
| 1 | `ModalSynthRender`: 1 モード (f=440, c=5, a=0.5) の零交差周波数が ±1%、`RmsIn` 比 [0.1,0.2]/[0.6,0.7] が e^{−2.5} ±5%、a×2 → ピーク×2、同入力 2 回 memcmp 一致、長さ規則 (0.05 / 2.0 の両端) | `Editor.exe --selftest` (ModalSynthSelfTest) |
| 2 | `BuildModes`: E×4 → f×2、ρ×4 → f/2 & a/2、L×2 → f/2 & a/2^1.5 (相対 1e-4)、mask 閾値で帯域が落ちる、k=0 → count 0、過減衰・ナイキスト超えが落ちる、`MelBandCenters` が double 期待値と 1e-2 Hz | 同上 |
| 3 | PhysMat 4 フィールド (E / ν / α / β): JSON 往復、Sanitize 範囲、キー無し旧 JSON → 既定 (E/α/β = 0、ν = 0.3)。11 本の JSON に初期値。`BuildModes` の署名に ν が無い。replay_verify 全ペア不変 | `--selftest` (PhysMatSelfTest) + `tools\replay_verify.bat` |
| 4 | Voxelizer: 単位立方体 → surface+interior == 24389 (29³)、中心 1、隅 0、pad リング全 0 / 厚さ 0.001 の板 → y 占有 index **ちょうど 1 種** / 蓋なし箱 → interior 0 / 2:1:0.5 AABB → 最長辺の占有 voxel 数 29 (h = L/28) / +X 面中心 → cell **(15, 8, 8)** / cellSlot: 有効は自身、無効は独立総当たりと一致 / `.mvox` 往復 memcmp / OFF/OBJ リーダ / 同入力 2 回 memcmp | `--selftest` (ModalSelfTest) |
| 5 | `Editor.exe --modal-voxelize --list tests\deepmodal\list_builtin.txt --out DIR` → 6 ファイル、exit 0。存在しない入力 → exit 1 | 手動 cmd (`cmd /c` 経由) |
| 6 | pytest 全緑: test_fem (Ke 対称・半正定、剛体 6 モードで K·r ≈ 0、集中質量総和 = ρh³) / test_modal (2×2×2 で E×4 → ω×2、ρ×4 → ω/2、h×2 → ω/2、1e-6。先頭 6 固有値 ≈ 0) / test_compact (単調・端点・Σ\|a\|・空帯域補間・mask) / test_layout (.mvox ヘッダ 72 B = 全体 32840 B、C++ cube で 24389 — Editor.exe 無ければ skip) / test_contact (cell を変えると励起が変わる) | `cd tools\deepmodal && pytest` |
| 7 | `dataset.py --stage primitives` が npz ≥ 20 本 + builtin 6 本と `stats.json` (メッシュごとの eigsh 秒、Mel-band coverage、モード数分布、解法名) を出す。満杯立方体 (builtin cube) < 600 s、exact 経路の中央値 < 60 s。**`stats.json` は再開 (resume) 実行で統計が消えないこと** (既存 stats と併合する。M76h の ModelNet10 は再開前提なので、消えると分布が取れない)。**npz と stats.json の両方に solver metadata (method / パラメータ / 反復回数 / 収束フラグ) と convergence quality (モードごとの残差 + `residual_max` / `residual_median` / 除外モード数) が入っていること**。★**残差しきい値による除外が効いていること** (`r > 1e-3` のモードが Σ\|a\| に入らない)。★`method` を**採否の判断に使っていない**こと (使ってよいのは数値のみ)。★**Mel-band coverage** が npz と stats に入ること (§4.1 の 3 番目の指標): メッシュ単位の `band_coverage[32]` / `coverage_ratio` / `coverage_high` と、cell 単位の `cell_coverage[valid_cells]`。stats には**帯域ごとの占有率の分布** (高域が系統的に空でないかが見える形) を出す。診断値 (`modes_requested` / `f_top` / `mode_count`) は記録してよいが**合否条件にしない**。L_ref / fMax の確定値を `layout.py` と spec §8 に記録 | 実行ログ + stats.json (再開実行の後に中身が残っていることまで) + npz の中身 |
| 8 | `check_rules.ps1` 全規則緑 (constGroups の C++ ⇄ Python 4 組を含む。片方を変えると赤くなることを 1 回確認) | `pwsh -File tools\check_rules.ps1` |
| 9 | **門 (確定、sub-04 round 2)**: `train.py --overfit 16 --epochs 300` → **mask acc > 99%** かつ **amp の説明率 `R² = 1 − MSE/var(target)` ≥ 0.90**。★根拠 (実測で「壊れた状態」と「直った状態」を分離できる位置に置いた): データ欠陥 2 件 (サイズ漏れ / `eigsh` の `v0` 乱数) があった round 1 は **R² ≈ 0.851**、両方直した round 2 は **R² = 0.9237**。0.90 はこの 2 つの間にあるので、同種の配管欠陥が再発すれば門が落ちる。★絶対 MSE の閾値 (旧「< 1e-3」) は撤回済み (§8)。★参考実測 (round 2、clean データ): N=1 → R² 0.9602 / N=4 → 0.9726 / N=16 → 0.9237、mask acc はいずれも 100%。**N をまたいだ R² の比較はできない** (var も MSE も標本集合ごとに変わる。N=4 が最良なのは選ばれた 4 形状の var が小さいため) | 実行ログ (MSE / var / R² を併記。`--overfit 1` も参考値として残す) |
| 10 | `export.py`: fixture 4 点 (`fixture.dmnet` / `fixture_in.mvox` / `fixture_out.bin` / `list_builtin.txt`) をコミット。`paramCount ≤ 2,000,000` の assert、`weightsHash` = FNV-1a、`bandCenterHz` = compact.py の値。`--random-full` でフルサイズ乱数重みの `.dmnet` (時間計測用、コミットしない) が出る | pytest (test_export) + 手動 |
| 11 | `CpuModalBackend`: fixture で 64 cell × 192 の `max\|Δ\| < 1e-3` (**インストール済みバックエンドに対して回す**) / Conv3d・ConvTranspose3d の小ケースが素朴 6 重ループと 1e-6 で一致 / フルサイズ乱数 `.dmnet` の Release 推論 **≤ 6 s/メッシュ** (`--modal-bake` の ms 欄。★旧「≤ 1.5 s」は根拠を書かずに置いた値で §4.4 で改訂した。**超過しても構造を縮めない** — 実装の SIMD / マルチスレッド化が正しい対処) + **2 回目の `--modal-bake` が `.msfm` ヒットで桁違いに速いこと** (実測 0.38 ms/mesh) | `--selftest` (ModalSelfTest) + `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` を 2 回 |
| 12 | `.msfm` 表の往復 memcmp / `SourcePathForSubAssetKey("builtin://cube")` が空で、guid キーでは `ConvexCookSourcePath` と同結果 / `CookedCacheSelfTest` 不変 | `--selftest` |
| 13 | `ModalSoundLibrary`: `Register` → `Get`、モデル無しで `NoModel`、`BakeSync` (fixture + 立方体) → `Ready` かつ validCount = voxel から独立に数えた有効 cell 数。`--modal-bake` が 1 行/メッシュ、2 回目は全部 cached。`--modal-backend d3d11cs` は WARN + cpu 縮退、綴り違いは exit 1 | `--selftest` + 手動 cmd + EngineCliSelfTest |
| 14 | ReloadHub: `.dmnet` → `ModalNet` (rank 6) | `--selftest` (ReloadHubSelfTest) |
| 15 | `CollectModalImpacts`: 箱 (ModalSound) と床の接触 1 件 → 1 impact、Y 90° 回転で k がローカル軸に乗る、`excess = impulse − RestingImpulse` / 両側 ModalSound → 2 impact、key 昇順 / `RestingImpulse` が抽出前の式とビット一致 / push 65 個目は dropped / cooldown / 合成マップ (全帯域 on) で k=(1,0,0) → count 32、k=0 → BelowMin、maxDistance = comp 値 / `ResolveWaveShotSound`: Ready → mute=1、未 Ready → 従来 soundKey / suspend 中の Update でキューが空になる | `--selftest` (ModalAudioSelfTest) |
| 16 | `Runtime.exe --modal-demo --modal-sync-bake --modal-audio-log 300 --synth-input --frames 300 > a.txt` を 2 回 → `[modal] t=` 行が byte 一致、`played > 0 && playFailed == 0`。`--no-audio` 併用で 0 行 | 手動 cmd (Release) |
| 17 | 共通検証: Debug/Release ビルド `/p:MyeWarnAsError=true` 0 警告 → `--selftest` 全緑 → `check_rules.ps1` → `replay_verify.bat` 全ペア緑 (M76f 以降は必須。golden は触らないので shot_verify は M76h で 1 回) | 各コマンド |
| 18 | Editor: Inspector の ModalSound 節 (状態 / cell 数 / 6 面ボタン + スライダ / Export WAV)、PhysMat 4 行、カタログ、en/ja。LocalizationSelfTest 緑。手動: 6 面で音が変わる、WAV が出て再生できる | `--selftest` + 手動 |
| 19 | stage1 (小規模自前 ≤ 100) で `dataset → train → export → --modal-bake → --modal-demo` が端から端まで通り、`assets\deepmodal\deepmodal.dmnet` (≤ 4 MB) をコミット。ModelNet10 の手順 (時間見積もり・実行禁止の門・再開方法) が README にある | 実行ログ + README |
| 20 | 文書: `engine_spec.md` §10.7、`docs\adr\ADR-0NN-deep-modal.md` (次の空き番号)、`README.md`、`CLAUDE.md` (末尾 TypeId 61 / Cloth・SoftBody 62/63 / CLI 6 本 / 検証表に `--modal-audio-log` 2 run 一致 / 「.dmnet を差し替えたら --modal-bake」/ constGroups の Python 組) | 目視 + check_rules |
| 21 | **基準形状の校正** (sub-03、ユーザー追加指示): 直接法が回せる基準形状 (cap 直下の占有と、余裕があれば cap 超の 1 本) で `eigsh` と LOBPCG を**両方**走らせ、`calibration.json` に (a) 各解法のモードごと残差の分布 (b) **周波数の突き合わせ** (相対誤差、昇順で対応付け) (c) **取りこぼしたモード数** (直接法にあって LOBPCG に無い周波数) (d) 帯域集計 Σ\|a\| の相対差 を書く。**生の固有ベクトルは比較しない** (縮退で基底が任意のため。§4.1 の罠を参照)。この実測から `RESIDUAL_ACCEPT` / `RESIDUAL_DROP` の暫定値 (1e-5 / 1e-3) が妥当かを判断し、違っていれば planner が §8 で改訂する。加えて小メッシュ (数百 DOF、1 秒未満) の pytest 1 本で両解法の一致を回帰的に固定する。★**両解法に同じモード数を要求して比較すること** (片方に k=150、他方に m=40 の既定を渡すと、単なる予算差が「取りこぼし」に見える — round 2 で実際に起きた)。要求数が違うまま比較するなら `modes_requested` を必ず併記し、差分を「予算差」と「真の取りこぼし」に分けて報告する | `python calibrate.py` (または同等) + `calibration.json` + pytest |
| 22 | **推論の高速化** (sub-09、ユーザー判断 2026-09-16): フルサイズ `.dmnet` の Release 推論が **≤ 0.6 s/メッシュ** (目標 ≤ 0.3 s。根拠: 現状 0.4 GMAC/s に対し AVX2 FMA は実効 10% でも 5.6 GMAC/s = 14 倍なので、8 倍は SIMD 単体で届く保守的な線)。0.6–1.0 s に着地したら**黙って合格にせず**報告し planner が裁定する。★**グローバルの `/arch` を変えないこと** (`Common.props` は「全構成で同一 = SSE2」— 上げるとエンジン全体の FP コード生成が変わり sim のビット一致契約を壊す)。AVX2 は `CpuModalBackend.cpp` 内の intrinsics + 実行時検出 + スカラー縮退に閉じる | `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` |
| 23 | **最適化しても結果が変わらない** (sub-09): fixture の `max\|Δ\|` が **AVX2・スカラーの両経路**で許容 1e-3 内 (現状 4.77e-07 から桁が大きく悪化しないこと) / **スレッド数を変えても `.msfm` がバイト一致** / 同じバイナリ・同じ入力で 2 回焼くと `.msfm` がバイト一致 / selftest が**両経路**を通る (スカラー縮退が腐らない) / `replay_verify` 全ペア緑 (sim に触っていない証明)。<br>★**スレッド数の検査は「SIMD 幅で割り切れない本数」を必ず含めること** (3 / 5 など)。1 / 2 / 4 / 8 だけでは**通ってしまう** — N (出力列数) は 32768 / 4096 / 512 / 64 でどれも 8 の倍数なので、2 冪のスレッド数ではチャンク境界が常に 8 に揃い、AVX2 ブロックとスカラー端数の境界が動かない (sub-09 round 1 で実際に見逃した)。<br>★**構造側の要件**: リダクション (K 次元) を割らないだけでは不十分。**AVX2 の 8 列ブロックとスカラー端数では丸めが違う** (FMA = 1 回丸め vs 乗算+加算 = 2 回丸め) ので、チャンク境界を **SIMD 幅の倍数へ量子化**して「どの列が AVX2 でどの列が端数か」をスレッド数から独立させること | `--selftest` + `--modal-bake` を **T = 1 / 3 / 4 / 5** で回して `.msfm` をバイト比較 |

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 (M76a) | モーダル合成器と材質パラメータ | なし | 1, 2, 3 | `M76a: モーダル合成器 (再帰共振器 / BuildModes / Mel 表) と PhysMat の音響材質 4 フィールド` |
| sub-02 (M76b) | ボクセライザと `--modal-voxelize` | なし (sub-01 と並列可) | 4, 5 | `M76b: 32³ ボクセライザ (.mvox) と OFF/OBJ リーダ、--modal-voxelize` |
| sub-03 (M76c) | Python: FEM / 固有値 / 接触励起 / Mel 圧縮 / データセット | sub-02 | 6, 7, 8 | `M76c: Deep-Modal データセット生成 (hex8 FEM / eigsh / 接触励起 / Mel 圧縮) と pytest` |
| sub-04 (M76d) | Python: モデル / 学習 / export / fixture (**大規模生成の門**) | sub-03 | 9, 10 (+ 品質フィルタは 21 の帰結) | `M76d: Deep-Modal のネット / 学習 / export (.dmnet + fixture)、overfit の門` |
| sub-05 (M76e) | C++ 推論 / バックエンド抽象 / .msfm / ModalSoundLibrary / `--modal-bake` | sub-02, sub-04 | 11, 12, 13, 14 | `M76e: .dmnet ローダと CPU 推論バックエンド、.msfm キャッシュ、ModalSoundLibrary、--modal-bake` |
| **sub-09 (M76e2)** | **CPU 推論の SIMD / マルチスレッド最適化** (実行順は sub-05 の後・sub-06 の前。ユーザー判断 2026-09-16) | sub-05 | 22, 23 | `M76e2: CPU 推論の AVX2 / マルチスレッド最適化 (結果はスレッド数に依存しない)` |
| sub-06 (M76f) | ランタイム接続 (ModalSound / 接触 → 音 / wave 口封じ / CLI / demo) | sub-01, sub-05, sub-09 | 15, 16, 17 | `M76f: ModalSound コンポーネントと衝突 → モーダル合成の接続、--modal-audio-log / --modal-demo` |
| sub-07 (M76g) | Editor (面打ちプレビュー / WAV / PhysMat 欄 / カタログ / 文字列) | sub-06 | 18 | `M76g: Inspector の ModalSound 面打ちプレビューと WAV 書き出し、PhysMat の音響材質欄` |
| sub-08 (M76h) | 本学習 (stage1) と文書 | sub-06, sub-07 | 19, 20, 17 | `M76h: Deep-Modal の stage1 学習済みモデルと文書 (engine_spec / ADR / README / CLAUDE.md)` |

推奨実行順: 01 → 02 → 03 → 04 → 05 → 06 → 07 → 08 (01 と 02 は入れ替え可)。

## 7. 未決事項・リスク

(策定時の `[ユーザーに聞ける]` 3 件は回答済み — poissonRatio は保持する (§2 #4)、M76h は stage1 まで (§2 #14)、計画は確定。詳細は §8)

- 実装中に判明する見込み (coder が「不安・質問」で拾う):
  - 絶対音量に上限圧縮を足すか (sub-06 の耳確認で決める。§2 #12)
  - L_ref / fMax の確定値 (sub-03 の stats で決める。決めたら §8 に積む)
  - (解決済み: 推論時間は sub-05 round 1 で実測 4.75–5.34 s/メッシュ。**ネットは縮めない**と裁定し予算を ≤ 6 s へ改訂した。§4.4 / §8 を参照。速度が要るなら SIMD / マルチスレッド化が対処で、fixture の許容 1e-3 がその安全網)
  - `ModalSound.mesh` (AssetID) の FieldType — `MeshRenderer.mesh` と同じ widget が使えるか
  - (解決済み: eigsh の代替は sub-03 round 1 で「cap 9000 + 不完全 LU 前処理 LOBPCG + 残差による採否」に確定。§4.1 と §8 を参照)
  - **残差しきい値 `RESIDUAL_ACCEPT = 1e-5` / `RESIDUAL_DROP = 1e-3` は暫定** (planner が 12³ ブロック 1 本で実測した値からの外挿)。受け入れ条件 21 の校正結果で確定させる。分布が重なっていたら planner が §8 で改訂する
  - 縮退モード (対称形状) で per-mode の `a_ij` が基底依存になる件 (§4.1 の罠)。Σ\|a\| は帯域内の縮退群をまとめて足すので影響は小さいが、**厳密には基底不変ではない** (Σa² なら不変)。学習の教師データとしては許容するが、再生成のたびに値が揺れうる点は M76h の ADR に残す
  - coverage のしきい値は **stage0/stage1 では既定 off で確定** (§8、round 3 の実測で決着)。M76h の ModelNet10 前に**適応予算**を入れるかを再検討する
  - **L_REF (0.3 m) と fMax (10000 Hz) は M76h の前に再検討する** (sub-04 round 2)。サイズ漏れの修正で全メッシュを参照サイズで解くようになった結果、帯域内のモードが減った: `mode_count` 中央値 **30.5 → 14**、`coverage.mean_ratio` **0.45 → 0.336**。§8 で「stage0 統計で確定」とした根拠は**バグ入りデータの統計**だったので失効している。L_ref はランタイムの σ3 が吸収する**自由なパラメータ**なので、「教師データが最も豊かになる値」を選んでよい (大きくすると周波数が下がり帯域内のモードが増える)。変更時は `.dmnet` ヘッダの `refSizeL` と stage0 の再生成がセット。sub-04 の門の合否とは切り離す (今変えると門の実測をやり直すことになるため)
  - 実測メモ (planner、2026-09-16): 一辺 0.13 m 程度の**中実**アルミ塊は基本周波数が 10 kHz 超 = 帯域に 1 本も入らない。帯域に入るのは大きい物と薄い / 細長い物。stage1 で形状を選ぶときに効く (中実の小塊ばかり集めると教師データが空になる)
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
- 2026-09-16 (coder SELF_EVAL sub-02 round 1、不安・質問 #1): ボクセル正規化を `h = L/29, origin = center − 16h` → **`h = L/28, origin = center − 16.5h`** に変更。理由: 旧式は AABB 中心が voxel 15/16 の境界に乗る構造 (奇数個に割ると必ずそうなる) で、厚さ ≪ h の中心対称な板が必ず 2 行になり、受け入れ条件 4 の「1 種」と「+X 面中心 → cell (15, 7|8, 7|8)」の不定が消せなかった。新式は面がボクセル中心 (2.5 / 30.5) を通り、中心もボクセル中心 (16.5) に乗る。数値の変更: 単位立方体 27000 → 24389、最長辺 29 voxel (占有数、h は L/28)、+X 面中心 → (15, 8, 8) に一意。データセット生成前 (sub-03 未着手) なので影響はテスト値だけ。反映: §4.1 / §5 #4 / sub-02。
- 2026-09-16 (coder SELF_EVAL sub-02 round 1、不安・質問 #2): `.mvox` ヘッダは 64 B ではなく **72 B** (18 フィールド × 4 B。planner の計算違い)。反映: §4.2 / §5 #6 / sub-03。
- 2026-09-16 (coder SELF_EVAL sub-03 round 1、不安・質問 #1): 固有値解法を **2 経路 (cap 9000 + LOBPCG)** に確定。planner 裁定: 機構は採用するが、**npz に解法名を記録する**ことを条件にする (LOBPCG の高次モード精度が未検証のまま学習へ黙って入るのを防ぐ。coder の 申し送りが挙げたリスクに sub-04 が対処できる形にする)。`[ユーザーに聞ける]` として司会へ回す。反映: §4.1 / §5 #7 / §7 / sub-03。
- 2026-09-16 (**ユーザー判断**、sub-05 の [ユーザーに聞ける] への回答): 提示した 3 択のうち **「今すぐ最適化する」**を選択。planner の裁定 (「許容する。最適化は必要になってから」) とは異なるが、**ネットを縮めない / 疎対応は成立しない / 正しさの安全網は fixture** という round 1 の技術的結論はそのまま維持される (遅さの原因が実装側だという切り分けが、むしろ「実装を直せば解決する」ことの根拠になる)。**sub-09 (M76e2) を新設**し、実行順を sub-05 → **sub-09** → sub-06 にした。受け入れ条件 **22 / 23 を末尾に追加** (既存番号は不動)。planner が仕様で固定した要点: (a) **グローバルの `/arch` を変えない** — `Common.props` は「`/arch` は全構成で同一 (既定 = SSE2)」と明記しており、上げるとエンジン全体の FP コード生成が変わって **sim のビット一致契約を壊す**。AVX2 は `CpuModalBackend.cpp` 内の intrinsics + 実行時検出 + スカラー縮退に閉じる (b) **マルチスレッドは出力側だけを分割し、GEMM のリダクション (K 次元) をスレッドで割らない** ⇒ **スレッド数が変わっても `.msfm` がバイト一致**する (4 コア機と 16 コア機で cooked が変わらない。CLAUDE.md の「暗黙パディングで cooked のバイト列が run ごとに変わる」罠と同型の予防) (c) 速度の合格線 **≤ 0.6 s/メッシュ** は根拠つきで置いた (現状 0.4 GMAC/s、AVX2 FMA は実効 10% でも 5.6 GMAC/s = 14 倍なので 8 倍は保守的)。★1e-3 や 1.5 s で 2 度繰り返した「根拠を書かずに閾値を置く」失敗を避けるため、**根拠を条件文に併記**した。
- 2026-09-16 (sub-05 round 1、推論時間の裁定): **ネットを縮めず、予算を ≤ 1.5 s → ≤ 6 s/メッシュへ改訂**。旧 1.5 s は planner が「何を守る数字か」を書かずに置いたもので、実測 4.75–5.34 s に対して構造縮小を促す形になっていた。切り分け: (a) MAC 内訳は高解像度側が支配 (`convT k4 64→32 →16³` 26.0% / `conv3 16→16 @32³` 11.0% / `res(32)@16³`×2 各 11.0% / `head conv3` 11.0%、総 2.06 GMAC) で、**cell 有効性で飛ばせる head は 13.4% が上限** = 疎対応の書き直しは成立しない。`validCells` に依らず一定という実測は畳み込みの性質として**正しい** (b) 実効 **0.39–0.43 GMAC/s** は単一スレッド・SIMD 無し (spec の設計どおり) の値で、**SIMD / マルチスレッドに 10–50 倍の余地**がある = アーキテクチャではなく実装の問題 (c) 縮めると sub-04 の門 (R² ≥ 0.90) の測り直し = サブをまたぐ差し戻し + モデル品質の低下で、未最適化カーネルを埋め合わせることになる。守るべきは**初回起動の体感**で、焼きはワーカー・メッシュごと 1 回・`.msfm` ヒット 0.38 ms・焼けるまで従来の音 (段階移行) なので、エンジンの 19 モデルで裏作業 100 s 前後・フレーム停止なし。**後から速くしてよい根拠**は fixture selftest の許容 1e-3 (加算順が変わる SIMD 化はこの中に収まる)。
- 2026-09-16 (sub-04 round 2、門の確定 + **planner の round 1 の主張の訂正**):
  (1) **門を確定**: `mask acc > 99%` かつ **`R² ≥ 0.90`** (受け入れ条件 9)。0.90 は「壊れた状態 (R² ≈ 0.851)」と「直った状態 (R² = 0.9237)」を分離する位置に置いた = 同種の配管欠陥が再発したら落ちる門。ユーザーが形式 (R² + mask acc) を承認し、具体値は planner に委任された。
  (2) ★**planner が round 1 で述べた「下限 ≈ 0.002 は構造的で、ネットは既に到達可能上限にいる」は誤りだった**。clean データで測り直すと、隣接 cell 間二乗差から見積もった「下限」を**実測 MSE が 3 例とも下回る** (N=1: 見積 0.00196 に対し実測 0.000583 / N=4: 0.000615 → 0.000139 / N=16: 0.003551 → 0.001661)。つまりあの見積もりは下限として妥当ではなく、cell 単位の変動はかなりの部分が形状から**予測可能**だった。round 1 で頭打ちしていた真因は**データ欠陥 2 件**であって表現力の限界ではない。1e-3 を撤回した結論自体は変わらない (N=16 は clean でも 0.001661 > 1e-3) が、**理由は「到達不能だから」ではなく「16 形状という標本集合に対して現容量では届かないから」**。N=1 / N=4 では clean データで 1e-3 を下回る。
  (3) **`eigsh` の `v0` 乱数** (coder が round 2 で発見・修正): `v0` 省略時 ARPACK が乱数初期ベクトルを使うため、縮退のある対称形状で固有ベクトルの基底が実行ごとに変わり、**同じ入力から同じ教師値が出なかった**。sub-03 §4.1 で「縮退空間の基底は任意」と警告していた性質が、オフラインのデータ生成の再現性として顕在化したもの。`seed` 既定 0 で `v0` を固定して解消 (`solver_params` に seed を記録)。修正後は重複入力 3 本の `feat` が **bit-identical (max|diff| = 0.0)**。
- 2026-09-16 (sub-04 round 1、planner の切り分け): **2 件の欠陥を確定**。
  (1) **サイズの漏れ**: `dataset.py:83` が FEM に `grid.voxel_size` (メッシュ実寸) を渡していた。ボクセル化は最長辺で正規化する = 入力はスケール不変なので、**同じ入力に異なる教師値**が生じる。実測: `cylinder_0/3/5` はボクセル列がバイト一致 (占有 1305) なのに L = 0.330/0.373/0.566 で `feat` が最大 7.0 違う。さらに論文 §5.1「学習時は同じスケール」+ 後処理 σ3 の設計から、**ランタイムが σ3 を二重適用する**ことになり sub-05/sub-06 の正しさに直結する。→ §4.1 に「FEM は `h_ref = L_REF/28` で組む」を明記。**must**。
  (2) **overfit の門の閾値 (amp MSE < 1e-3) を撤回**。planner が根拠なく置いた値で、**表現上の下限を下回っていた**。切り分けの実測: 重複入力は選ばれた 16 本に含まれない (原因ではない) / **1 サンプルだけ** (有効 240 cell × 96 値 = 23k を 1.68M パラメータで学習 = 73 倍の過剰パラメータ) でも 0.00109 で頭打ち → 容量でも最適化でも標本数でもなく**構造的**。目標場の「隣接 cell 間の二乗差」は 0.00392 = 予測不能な cell 単位成分の分散 ≈ 0.00196 で、**観測された 16 本の下限 0.001935 とほぼ一致**。原因は畳み込みが並進同変で、内部の cell は受容野の中身が同一になり区別できないこと (`contact.py` が cell ごとに代表節点を 1 つ選ぶため、目標場には cell 単位のジッタが乗る)。現状のネットは **R² = 0.851 に対し到達可能な上限が ≈ 0.849** = **学習可能な信号はほぼ全部取れている**。→ 門を「mask acc > 99% + amp の説明率 R²」に組み替え、閾値はサイズ修正後に再計測して確定する (受け入れ条件 9)。
- 2026-09-16 (sub-03 round 3 の実測を受けた planner 裁定): **coverage しきい値は stage0 / stage1 では既定 off** (フィルタを掛けない)。実測: `coverage_high` は exact 経路 35 本の平均 0.789 に対し **lobpcg 経路 3 本 (builtin cube / cylinder / sphere) が一律 0.000**、`f_top` 3463–4740 Hz。Mel 帯域 24 の下端が 4895 Hz なので **0.000 は指標として正しい** (独立に計算した `f_top` と整合)。ただし両群は完全に分離しているので、この段階で `--quality-min-high-coverage` を有効にすると**「lobpcg のメッシュを除く」と数値的に同義**になり、ユーザー指示「method で決めない」の趣旨に反する結果を coverage 経由で招く。原因は指標ではなく**固定モード数の予算**なので、正しい対処は「データを捨てる」ことではなく予算を直すこと。よって: (1) stage0/stage1 は既定 off のまま全 38 本を使う (基本形状 3 つを落とす損の方が大きい。overfit の門は汎化ではなく配管の検証が目的) (2) **M76h の ModelNet10 では「適応予算」を検討する** — `f_top ≥ f_max` に達するまで、または実時間上限に当たるまで m / k を上げる。大きいメッシュが増える段階ではこれが根本対処になる。しきい値を入れるならその後、分布を見て決める。
- 2026-09-16 (**ユーザー追加指示**、sub-03 round 2 の [ユーザーに聞ける] への回答): 3 つ目の品質指標を「スペクトルの完全性 (固定モード数の達成)」から **「Mel-band coverage」** へ差し替え。「1 で固定のモード数達成を必須条件にせず、100–10000 Hz に対する Mel-band coverage を各サンプルで記録する。学習時の採否・重み付けは mode count ではなく band coverage と residual 品質で決定する」。planner の裁定 (予算を上げず測って記録し、学習側が選ぶ) の方向はそのままだが、**判定軸が本数から被覆率に変わった**。planner が定義を確定: 軸の畳み方は **OR** (ランタイムの `BuildModes` が `Σ_j |k_j|·H(mask)·amp` と軸を足すので、1 軸でも立てば鳴る = OR が唯一ランタイムと整合する)、メッシュ単位の `band_coverage[32]` を本体として高域の空きが見える形にし、`coverage_ratio` / `coverage_high` / cell 単位の `cell_coverage` を併記。`spectrum_complete` 等は診断値へ降格 (合否に使わない)。`band_occupancy_ratio` との名前の重複は 1 つに寄せる。反映: §4.1 / §5 #7 / §7 / sub-03 / sub-04。
- 2026-09-16 (coder SELF_EVAL sub-03 round 2 + planner の再分析): (上記で coverage へ差し替わったが、原因分析は有効) **モード数の予算差**の発見。round 2 の校正で「LOBPCG が 150 本中 116 本を取りこぼす」と報告されたが、planner が `calibrate.py:83-84` を読んで**原因は解法の質ではなくモード数の予算差**と判定した — 直接法に `k=150`、LOBPCG に既定 `m=40` を渡しており、40 − 剛体 6 = 報告された 34 とちょうど一致する。capsule (16200 DOF) と sphere (45864 DOF) で件数が**完全に同一** (34 / 116) なのも、収束の失敗ではなくブロック幅で頭打ちになった証拠。さらに**直接法も `k=150` で頭打ち**だった (両形状とも帯域フィルタ後がちょうど 150 = 帯域ではなく要求数が制約)。よって truncation は LOBPCG 固有ではなく両経路の問題として扱う (この分析は上のユーザー指示の後も有効で、coverage が低いメッシュの**原因説明**として使う)。受け入れ条件 21 の「両解法に同じモード数を要求する / `modes_requested` を併記する」もそのまま有効。
- 2026-09-16 (**ユーザー追加指示**、sub-03 の [ユーザーに聞ける] への回答): fallback の採否を **solver 名ではなく固有対の residual と基準形状での直接法比較に基づかせる**。planner の round 1 裁定 (「npz に `method` を記録して sub-04 が除外・重み下げを選ぶ」) は**判断基準が solver 名のままだった**ので、より正しい形に差し替えた: `method` はメタデータに降格し、採否は相対残差 (M-正規化、剛体除去後) で決める。モードごとの残差 + 要約 + solver metadata を npz と stats.json の両方に保存。受け入れ条件 **21 を新設** (基準形状の校正ジョブ + 小メッシュの pytest)。暫定しきい値 1e-5 / 1e-3 は planner の実測 (12³ ブロック 6591 DOF: 直接法 max 1.6e-8 / median 1.5e-9、LOBPCG max 4.8e-6 / median 9.6e-8、周波数一致 1e-11) から置いた仮値で、校正で確定させる。あわせて**縮退モードの罠** (対称形状では固有ベクトルの基底が任意 = per-mode の `a_ij` を解法間で直接比較してはいけない) を §4.1 に明記。反映: §4.1 / §5 #7・#21 / §7 / sub-03 / sub-04。
- 2026-09-16 (coder SELF_EVAL sub-03 round 1、不安・質問 #2): **参照材質と L_ref を初期値のまま確定** (E=7.0e10 / ρ=2700 / ν=0.33 / α=6 / β=1e-7、L_ref=0.3 m、fMax=10000 Hz)。根拠: stage0 の統計で帯域占有率 平均 0.45 / モード数 中央値 30.5 と偏りが無かった (round 1 実測)。spec §4.1 が「stage0/1 の統計で確定してからヘッダに固定する」としていた項目の決着なので §8 に記録する。stage1 (sub-08) で分布が偏れば再訪してよい。
