# Deep-Modal 自作ゲームエンジン実装計画

## 1. 目的

本計画の目的は、3Dオブジェクトの形状・材質・サイズ・接触位置・衝撃力から、事前録音された効果音を使わずに衝突音をリアルタイム生成できる物理ベース音響システムを自作ゲームエンジンへ統合することである。

最終的な処理系は以下を目標とする。

```text
3D Mesh
  ↓
Voxelization
  ↓
Deep-Modal 推論
  ↓
接触位置に対応する Sound Feature を取得
  ↓
衝撃力を反映
  ↓
材質・サイズ補正
  ↓
Modal Synthesis
  ↓
PCM / Audio Output
```

通常の剛体では推論結果をキャッシュし、衝突時には接触位置と力から音を生成する。破壊・変形などで形状が変化した場合のみ、Voxelization と Deep-Modal 推論を再実行する。

---

# 2. 理論全体

Deep-Modal は、従来の Modal Synthesis で必要だった高コストな Modal Analysis をニューラルネットワークで近似する手法である。

従来法では、物体ごとに質量行列と剛性行列を構築し、一般化固有値問題を解いて振動モードを求める。

\[
M\ddot{x}+C\dot{x}+Kx=f
\]

ここで、

- \(M\): 質量行列
- \(C\): 減衰行列
- \(K\): 剛性行列
- \(x\): 節点変位
- \(f\): 外力

Rayleigh damping を使う場合、

\[
C=\alpha M+\beta K
\]

と表す。

Modal Analysis では、

\[
KU=\Lambda MU
\]

を解き、

- 固有値 \(\lambda_i\)
- 固有ベクトル \(U_i\)

を得る。

各振動モードは減衰正弦波として表される。

\[
q_i(t)=a_i e^{-c_i t}\sin(2\pi\omega_i t)
\]

最終音は複数モードの加算で得られる。

\[
S(t)=\sum_i a_i e^{-c_i t}\sin(2\pi\omega_i t)
\]

Deep-Modal は、この Modal Analysis の結果に相当する音響特徴を3D形状から直接予測する。

---

# 3. Deep-Modal の基本構造

Deep-Modal では入力形状を Binary Voxel として扱う。

論文設定では、

- 入力 Voxel: \(32\times32\times32\)
- Sound Feature Map: \(16\times16\times16\)
- Mel sub-band 数: \(M=32\)
- 周波数範囲: 100 Hz ～ 10,000 Hz

である。

ネットワーク入力は3D形状全体で、出力は各接触位置に対応した音響特徴マップになる。

```text
32³ Binary Voxel
      ↓
3D Encoder
      ↓
Latent Feature
      ↓
3D Decoder
      ↓
16³ Sound Feature Map
```

Sound Feature Map の各セルは、その位置を叩いたときのモード情報を持つ。

---

# 4. Compact Mode Representation

物体ごとに振動モード数が異なるため、そのままではニューラルネットワーク出力を固定長にできない。

そこで振動モードを Mel Scale 上の固定数の sub-band にまとめる。

論文では、

\[
M=32
\]

の Mel sub-band を使用する。

各 sub-band は次の2つの値を持つ。

- Amplitude
- Mask

Mask は、その band に有効な振動モードが存在するかを表す。

```text
Mask = 1 : modeあり
Mask = 0 : modeなし
```

同じ Mel band 内に複数モードが存在する場合、Amplitude を統合する。

これにより、任意個数の Modal Data を固定長ベクトルへ変換できる。

---

# 5. 外力の分離

任意の衝撃力は3つの直交する単位力の線形結合として扱える。

\[
f=k_1f_1+k_2f_2+k_3f_3
\]

したがって、ネットワークは任意の力そのものを入力として学習する必要がない。

各接触位置について、

- X方向単位力
- Y方向単位力
- Z方向単位力

に対する Sound Feature を予測する。

各方向について、

- 32 Mask
- 32 Log-Amplitude

を持つため、総出力チャンネル数は

\[
3\times(32+32)=192
\]

となる。

したがってネットワーク出力は概念的に、

\[
16\times16\times16\times192
\]

の Sound Feature Map になる。

ランタイムでは実際の衝撃力を3軸成分へ分解し、3方向の予測結果を線形結合する。

---

# 6. 接触位置

Deep-Modal は接触位置をネットワーク入力へ直接与える方式ではない。

ネットワークは全接触位置に対応する Sound Feature Map を一度に出力する。

```text
形状
 ↓
Network
 ↓
全接触位置の Sound Feature Map
```

衝突発生時には接触位置を Sound Feature Map 上のセルへ変換し、その位置の特徴ベクトルを取得する。

この方式により、同一形状のオブジェクトでは毎回ニューラルネットワーク推論を行う必要がない。

---

# 7. 材質・サイズの分離

Deep-Modal の学習時には、形状変化の学習へ問題を集中させるため、材質とサイズを固定する。

ランタイムでは Modal Data に対して後処理を行い、材質・サイズの違いを反映する。

基準物体に対して、

- stiffness ratio: \(\sigma_1\)
- density ratio: \(\sigma_2\)
- size ratio: \(\sigma_3\)

とすると、周波数は

\[
\omega
\leftarrow
\sigma_1^{1/2}
\sigma_2^{-1/2}
\sigma_3^{-1}
\omega
\]

Amplitude は

\[
a
\leftarrow
\sigma_2^{-1/2}
\sigma_3^{-3/2}
a
\]

として補正する。

この処理により、ネットワークを材質ごとに再学習せずに、

- 木材
- 金属
- ガラス
- 陶器
- プラスチック

などへ近似的に対応できる。

---

# 8. 減衰

Rayleigh damping を使用する。

\[
C=\alpha M+\beta K
\]

各モードの減衰係数は、

\[
c_i=\frac{1}{2}(\alpha+\beta\lambda_i)
\]

で得られる。

材質ごとに \(\alpha,\beta\) を持たせることで、材質による減衰特性を変化させる。

最終的な音は、

\[
S(t)=\sum_{i=1}^{M}
a_i e^{-c_i t}\sin(2\pi\omega_i t)
\]

で生成する。

---

# 9. 実装順序

## Phase 1: Modal Synthesizer

最初にニューラルネットワークとは独立して Modal Synthesis を完成させる。

目的は、既知の Frequency・Amplitude・Damping から正しく音を生成できることを確認すること。

確認項目:

- 周波数を変えると音程が変わる
- Amplitude を変えると音量が変わる
- Damping を変えると減衰時間が変わる
- 複数モードを加算できる

この段階で「Modal Data → 音声」の経路を確立する。

---

## Phase 2: Voxelizer

3D Mesh を固定解像度 Binary Voxel へ変換する。

目標解像度:

\[
32\times32\times32
\]

重要なのは、学習時とランタイムで全く同じ Voxelization 規則を使うことである。

統一すべき項目:

- 原点
- 座標軸
- スケーリング
- Bounding Box
- Padding
- Interior / Exterior 判定
- 薄い形状の扱い

Voxelizer は Deep-Modal 入力と Modal Analysis 用 Hexahedral Mesh の両方で使用する。

---

## Phase 3: Hexahedral FEM

Voxel を Hexahedral Finite Element として扱う。

各 Voxel を8節点 Hex Element とし、

- Mass Matrix
- Stiffness Matrix

を生成する。

Material Parameter として最低限、

- Density
- Young's Modulus
- Poisson Ratio

を扱う。

各 Element Matrix を Global Sparse Matrix へ Assemble する。

この段階では単純な Cube などで行列生成を検証する。

---

## Phase 4: Modal Analysis

Global Mass Matrix \(M\) と Stiffness Matrix \(K\) から、

\[
KU=\Lambda MU
\]

を解く。

必要なのは主に可聴帯域に存在する低次モードである。

ここはランタイム機能ではなく、Dataset 生成用 Offline Tool として実装する。

確認項目:

- 同形状で Material Parameter を変えたとき周波数が妥当に変化する
- サイズ変更で周波数が妥当に変化する
- 対称形状で極端に不自然な固有値が出ていない

---

## Phase 5: Contact Excitation

固有ベクトルと接触位置・単位外力から各モードの励起 Amplitude を求める。

各接触位置について、

- X Unit Force
- Y Unit Force
- Z Unit Force

の3ケースを生成する。

この結果を Deep-Modal の Ground Truth とする。

---

## Phase 6: Compact Mode Conversion

Modal Analysis の可変長モード列を固定長へ変換する。

処理:

1. 周波数を Mel Scale へ変換
2. 100 Hz ～ 10,000 Hz を32 bandへ分割
3. 各 mode を対応 band へ割り当て
4. band 内 Amplitude を統合
5. Mask を生成
6. Amplitude を Log-Amplitude 化
7. 学習用範囲へ Normalize

この段階でネットワーク学習データの出力形式が確定する。

---

## Phase 7: Dataset Generator

1つの3D Objectについて、

```text
Mesh
 ↓
Voxelization
 ↓
Hex FEM
 ↓
Modal Analysis
 ↓
全接触候補位置
 ↓
XYZ Unit Force Excitation
 ↓
Compact Mode Conversion
 ↓
Training Data
```

を自動実行できるツールを作る。

最初から大規模 Dataset を生成しない。

推奨順序:

1. 数個の単純形状
2. 数十～数百形状
3. 約1,000形状
4. ModelNet40級

Modal Analysis や Compact Mode に問題がある状態で大量生成すると Dataset 全体を作り直すことになる。

---

## Phase 8: Deep-Modal Network

3D Encoder-Decoder Network を構築する。

Encoder:

- 3D Convolution
- Residual Block
- Down Sampling

Decoder:

- 3D Transposed Convolution
- Skip Connection

Output:

\[
16\times16\times16\times192
\]

Loss は2種類を組み合わせる。

Log-Amplitude:

\[
L_{amp}=MSE
\]

Mask:

\[
L_{mask}=BinaryCrossEntropy
\]

総 Loss:

\[
L=\lambda_1L_{amp}+\lambda_2L_{mask}
\]

論文再現時は、

\[
\lambda_1=\lambda_2=1
\]

から開始する。

---

## Phase 9: Small Dataset Overfit Test

大規模学習の前に、小規模 Dataset へ意図的に Overfit させる。

目的は、

- Dataset が正しい
- Network Output の意味が正しい
- Loss が正しい
- Tensor Layout が正しい
- Contact Position Mapping が正しい

ことを確認すること。

この段階を通過するまでは大規模学習へ進まない。

---

## Phase 10: Large-Scale Training

小規模 Overfit が成功した後に Dataset を拡張する。

論文の基準設定:

- Input: \(32^3\)
- Output Map: \(16^3\)
- Mel Bands: 32
- Frequency Range: 100–10,000 Hz
- Optimizer: Adam
- Initial Learning Rate: 0.02
- Batch Size: 64
- Epoch: 100
- 20 Epoch ごとに Learning Rate を半減

ただし、完全再現よりもまず自作エンジン用途で十分な精度を優先する。

---

## Phase 11: Runtime Inference

学習済みモデルをエンジンで推論可能な形式へ変換する。

ランタイム処理:

```text
Mesh生成 / Mesh変更
 ↓
Voxelization
 ↓
Deep-Modal Inference
 ↓
Sound Feature Map
 ↓
Cache
```

通常の剛体では、この処理は形状変更時に一度だけ行う。

衝突ごとにニューラルネットワークを実行しない。

---

## Phase 12: Collision → Sound

Physics Collision から次を取得する。

- Contact Position
- Collision Normal
- Impulse / Impact Force

Contact Position を Object Local Space へ変換し、16³ Sound Feature Map の対応セルを選択する。

Impulse も Object Local Space へ変換し、

\[
k_1,k_2,k_3
\]

へ分解する。

3方向の Sound Feature を合成して最終 Amplitude を生成する。

その後、

1. Mask Threshold
2. Force Combination
3. Material Scaling
4. Size Scaling
5. Damping Calculation
6. Modal Synthesis

を行う。

---

## Phase 13: Material System

エンジン上の物理材質に音響パラメータを追加する。

最低限必要なパラメータ:

- Density
- Young's Modulus
- Poisson Ratio
- Rayleigh Alpha
- Rayleigh Beta

Deep-Modal が予測した Reference Material の結果を、これらの値によってランタイム補正する。

---

## Phase 14: Runtime Optimization

最適化対象:

- Voxelization
- Deep-Modal Inference
- Sound Feature Map Memory
- Modal Oscillator
- 同時発音数
- Collision Event Filtering

特に衝突音では、小さすぎる衝撃を無視し、同一物体の短時間多重衝突を制御する必要がある。

ニューラルネットワーク推論より、同時に大量発生する Modal Voice の管理が最終的な Audio Runtime では重要になる。

---

## Phase 15: Dynamic Fracture

最後に破壊オブジェクトへ対応する。

形状が変化した場合、

```text
Fracture
 ↓
New Fragment Mesh
 ↓
Voxelization
 ↓
Deep-Modal Inference
 ↓
New Sound Feature Map
```

を実行する。

これにより事前に存在しなかった破片形状にも新しい衝突音を割り当てられる。

Deep-Modal の最大の利点は、このようなランタイム生成形状に対して高コストな Modal Analysis を実行しなくてよい点にある。

---

# 10. 実装チェックポイント

## Checkpoint A

既知の Modal Data から減衰音を生成できる。

## Checkpoint B

任意 Mesh を32³ Binary Voxelへ変換できる。

## Checkpoint C

単純形状から FEM の Mass / Stiffness Matrix を生成できる。

## Checkpoint D

一般化固有値問題から妥当な振動モードが得られる。

## Checkpoint E

接触位置を変更すると励起される Modal Amplitude が変化する。

## Checkpoint F

Modal Data を32 Mel Bandsの Compact Modeへ変換できる。

## Checkpoint G

少数形状 Dataset を自動生成できる。

## Checkpoint H

Deep-Modal が小規模 Dataset に Overfit できる。

## Checkpoint I

未知形状に対して Sound Feature Map を推論できる。

## Checkpoint J

衝撃方向を変えると生成音が変化する。

## Checkpoint K

材質とサイズ変更によって音が変化する。

## Checkpoint L

Physics Collision から自動的に音が生成される。

## Checkpoint M

破壊後に生成された新形状から新しい音を生成できる。

---

# 11. 推奨開発順序まとめ

```text
1. Modal Synthesizer
2. Voxelizer
3. Hexahedral FEM
4. Modal Analysis
5. Contact Excitation
6. Compact Mode Representation
7. Dataset Generator
8. Deep-Modal Network
9. Small Dataset Overfit
10. Large Dataset Training
11. Runtime Inference
12. Physics Collision Integration
13. Material / Size Scaling
14. Runtime Optimization
15. Dynamic Fracture
```

この順序を崩さない方がよい。

特に、

```text
Deep Learning
```

から先に始めないこと。

Deep-Modal は Modal Analysis の Ground Truth を学習するため、

```text
Physics / FEM
 ↓
Dataset
 ↓
Deep Learning
 ↓
Runtime
```

の依存関係になる。

---

# 12. 最終システム

完成時の構造は以下になる。

```text
                    ┌───────────────┐
Mesh ─────────────→ │   Voxelizer   │
                    └───────┬───────┘
                            │
                           32³
                            │
                            ▼
                    ┌───────────────┐
                    │  Deep-Modal   │
                    │    Network    │
                    └───────┬───────┘
                            │
                       16³ × 192
                            │
                          Cache
                            │
Physics Collision ──────────┤
       │                    │
       ├─ Contact Position ─┤
       │                    ▼
       │              Feature Select
       │                    │
       └─ Impact Force ─────┤
                            ▼
                     Force Combination
                            │
                            ▼
                    Material / Size
                       Post-Process
                            │
                            ▼
                      Compact Modes
                            │
                            ▼
                    Modal Synthesizer
                            │
                            ▼
                       Audio Output
```

最初の完成目標は「現実音の完全再現」ではない。

まず、

- 形状で音が変わる
- 接触位置で音が変わる
- 衝撃方向と強さで音が変わる
- 材質で音が変わる
- サイズで音が変わる
- 破壊後の新形状でも音が生成される

という一連の物理的整合性を成立させる。

その後、必要に応じて Dataset 品質、Network 構造、Acoustic Transfer、Propagation、Radiation Model などを追加して精度を上げる。
