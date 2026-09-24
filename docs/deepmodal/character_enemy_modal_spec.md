# キャラクター・エネミー向け Deep-Modal 特化音響仕様書

- 作成日時: 2026-09-23
- 対象バージョン: MyEngine M76 (Deep-Modal 音響システム)
- 関連コード:
  - コンポーネント定義: [`ModalSoundComponent`](file:///C:/HAL/MyEngin/src/Engine/Core/Components.h#L1621)
  - 物理材質定義: [`PhysMat`](file:///C:/HAL/MyEngin/src/Engine/Engine/Physics/PhysMat.h)
  - 合成ルーチン: [`BuildModes`](file:///C:/HAL/MyEngin/src/Engine/Engine/Audio/ModalSynth.cpp#L41)
  - 特化学習ツール: [tools/deepmodal/](file:///C:/HAL/MyEngin/tools/deepmodal/)

---

## 1. 概要と目的

本仕様は、キャラクター／エネミー（人型、モンスター、ロボット、異形クリーチャー等）の衝突・被弾・落下・打撃時において、従来の波形サンプリング再生（一様な「ドカッ」「バシッ」）を超え、**「どの部位（胴体、手足、角、頭部）に」「どの方向から」「どれほどの衝撃力で」ヒットしたかを 3D メッシュ形状と材質物理特性からリアルタイムモーダル合成する**ための運用仕様およびコンポーネント設定指針を規定する。

---

## 2. 特化学習データセットの構成（Thingi10K & ModelNet40）

Thingi10K の 3D プリント用マニホールドメッシュおよび ModelNet40 から抽出された **合計 1,484 件** の生物・キャラクター・メカニック系メッシュを学習データとして使用する。

| カテゴリ | 該当件数 | 代表的なメッシュ例 | 想定エネミー／音響特性 |
|---|---|---|---|
| **Robot / Mech** | 798 件 | ロボットアーム、二足歩行メカ、外骨格、ギア、四足ロボ | メカエネミー、ドローン、装甲兵、サイボーグ。高剛性・金属倍音・鋭い共振 |
| **Skull / Anatomy** | 402 件 | 頭蓋骨、顎骨、手骨、肋骨、脊椎、全身骨格 | アンデッド（スケルトン）、死霊系、弱点部位（頭部）。ガラガラと乾いた複数倍音 |
| **Humanoid / Figure** | 108 件 | 人型フィギュア、兵士、戦士、ポーズ付きミニチュア | プレイヤー、人型NPC、近接戦闘エネミー。手足・胴体の連成振動 |
| **Monster / Beast** | 76 件 | ドラゴン、ガーゴイル、ゴーレム、悪魔、ゾンビ、エイリアン | ボスエネミー、大型魔獣、甲殻生物、岩石ゴーレム。重厚な低域振動＋角・爪の打撃音 |
| **Animal / Dino** | 93 件 | 四足動物、恐竜、クモ（多脚）、蛇、オオカミ、熊 | 野生動物、多脚昆虫エネミー、爬虫類系。体躯に応じた低次モード |

---

## 3. キャラクター／エネミー用 コンポーネント設定項目

GameObject に付与する [`ModalSoundComponent`](file:///C:/HAL/MyEngin/src/Engine/Core/Components.h#L1621) および関連する [`ColliderComponent`](file:///C:/HAL/MyEngin/src/Engine/Core/Components.h#L100)（[`PhysMat`](file:///C:/HAL/MyEngin/src/Engine/Engine/Physics/PhysMat.h)）のパラメータ項目一覧。

### (1) `ModalSoundComponent` の設定項目

```cpp
struct ModalSoundComponent {
    AssetID mesh = {};          // 対象メッシュ (未指定時は自身および子孫の MeshRenderer を自動合体)
    float gain = 1.0f;          // 音量倍率 (0.0 〜 2.0)
    float maskThreshold = 0.0f; // モード生存判定の閾値 (0.0 = モデル既定 0.5 を使用)
    int32_t cooldownTicks = 3;  // 連続ヒット時の最短発音間隔 (tick = 1/60s 単位。3 = 50ms)
    float sizeScale = 1.0f;     // 代表寸法 L の倍率補正 (1.0 = 実メッシュ AABB 寸法そのまま)
    float maxDistance = 30.0f;  // 3D 音響の最大可聴距離 (m)
    bool muteWave = true;       // true: 既存の汎用 WaveSound 打撃音をミュートしてモーダルのみ再生
};
```

| フィールド名 | 型 / 既定値 | キャラクター／エネミーでの調整指針・役割 |
|---|---|---|
| **`mesh`** | `AssetID` (空) | 通常は **空（0）のままで良い**。MyEngine は階層構造（ルート → 手足・武器）を走査して自動的に 1 つの複合形状として解決する。特定のアーマーや頭部兜のみ別音にしたい場合はそのメッシュ ID を指定する。 |
| **`gain`** | `float` (1.0) | ボスエネミーや大型の打撃音を強調したい場合は `1.2`〜`1.5`、小型雑魚モンスターの接触音を控えめにしたい場合は `0.6`〜`0.8` に設定。 |
| **`maskThreshold`** | `float` (0.0) | 生体肉体エネミーなど「余計な倍音を鳴らさず鈍い音にしたい」場合は `0.6`〜`0.7` に上げてモード数を絞る。金属ロボットなど「細かい共振を豊かに鳴らしたい」場合は `0.3`〜`0.4` に下げる。 |
| **`cooldownTicks`** | `int32_t` (3) | 多段ヒット技やガトリング被弾時のマシンガン的な発音制限。`3`（50ms）〜`6`（100ms）を推奨。 |
| **`sizeScale`** | `float` (1.0) | **音色の巨小感コントロール**。大型ボスのメッシュであってもさらに重低音（$f \propto 1/L$）を響かせたい場合は `1.5`〜`2.0` を設定。小型クリーチャーの「甲高い小気味よい被弾音」にする場合は `0.3`〜`0.5` を設定。 |
| **`maxDistance`** | `float` (30.0) | プレイヤーからどれだけ離れた位置の被弾・衝突音が聞こえるか。ボスは `40.0`〜`50.0`m、雑魚は `20.0`m 程度。 |
| **`muteWave`** | `bool` (true) | 通常は `true`。従来のプリセット WAV 効果音とモーダル音をブレンドしたい特殊演出時のみ `false` にする。 |

---

## 4. エネミー種別ごとの推奨プリセット設定（データ対照表）

キャラクターやエネミーの物理材質（`PhysMat`）との組み合わせにより、同一の打撃でも劇的に異なる音響を生み出す。

### ① 生体・人型エネミー（ゾンビ、生身の敵兵、野生動物）
- **特性**: 衝撃を強く吸収し、鈍く短い「ドスッ」「バシッ」という生々しい打撃音。
- **推奨 `PhysMat` パラメータ**:
  - `youngsModulus` ($E$): `1.0e8` Pa（低剛性）
  - `density` ($\rho$): `1050.0` kg/m³（生体密度）
  - `rayleighAlpha` ($\alpha$): `20.0`（強い低域減衰）
  - `rayleighBeta` ($\beta$): `1.0e-5`（素早い高域消失）
- **`ModalSoundComponent` 設定**:
  - `maskThreshold`: `0.55`
  - `sizeScale`: `1.0`

### ② 装甲兵・メカニック・ロボットエネミー
- **特性**: 外装プレートやフレームが共振し、「ガキン！」「キィィン」と鋭く硬質な金属音が響く。
- **推奨 `PhysMat` パラメータ**:
  - `youngsModulus` ($E$): `7.0e10` Pa（アルミ・鋼鉄相当）
  - `density` ($\rho$): `7850.0` kg/m³
  - `rayleighAlpha` ($\alpha$): `6.0`（持続する余韻）
  - `rayleighBeta` ($\beta$): `1.0e-7`（クリアな高次倍音）
- **`ModalSoundComponent` 設定**:
  - `maskThreshold`: `0.40`
  - `gain`: `1.2`

### ③ スケルトン（骸骨）・アンデッド骨格
- **特性**: 硬質だが中空・多孔質特有の「カチャカチャ」「コトコト」と乾いた骨の衝突音。
- **推奨 `PhysMat` パラメータ**:
  - `youngsModulus` ($E$): `1.8e10` Pa（骨相当）
  - `density` ($\rho$): `1900.0` kg/m³
  - `rayleighAlpha` ($\alpha$): `12.0`
  - `rayleighBeta` ($\beta$): `8.0e-7`
- **`ModalSoundComponent` 設定**:
  - `maskThreshold`: `0.45`
  - `sizeScale`: `0.8`

### ④ 超大型ゴーレム・岩石魔獣
- **特性**: 地鳴りのような超重低音と、石塊がぶつかり合う重厚な崩落音。
- **推奨 `PhysMat` パラメータ**:
  - `youngsModulus` ($E$): `5.0e10` Pa（花崗岩・石材）
  - `density` ($\rho$): `2700.0` kg/m³
  - `rayleighAlpha` ($\alpha$): `15.0`
  - `rayleighBeta` ($\beta$): `5.0e-7`
- **`ModalSoundComponent` 設定**:
  - `sizeScale`: `2.5`（周波数を大幅に下げて地響き感を強調）
  - `gain`: `1.5`
  - `maxDistance`: `60.0`
