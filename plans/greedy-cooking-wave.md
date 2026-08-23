# M59〜M62: 超リアル物理 — 全体ロードマップ + M59 詳細計画

## Context

ユーザーの要求は「超リアルな物理演算を追加できるようにしたい」。対話で確定した中身は:
古典力学の修正 (空気抵抗・終端速度・マグヌス・ジャイロ項・浮力・風)、面ベース空力 (揚力・
失速・風見安定・翼面)、材料特性 (剛性・弾性・塑性・粘弾性・クリープ・応力緩和・強度・靭性・
脆性・延性・展性…)、関節・ラグドール・車両、XPBD 変形体、破壊、熱、流体、光学、電気導通。
**全機能は Inspector とスクリプトで ON/OFF・付け外し可能** (コンポーネント粒度 + bool
フィールド粒度の二段)。

### ユーザー決定 (確定)
1. **決定論は絶対維持** — 外部物理エンジン (Jolt/PhysX) は不採用。Jolt の決定論保証は
   同一バイナリ限定で、Debug/Release/WARP のビット一致という本リポジトリ最大の制約を満たせない。
2. 計画粒度 = **全体ロードマップ + M59 のみサブ詳細**。後続は着手時に詰め直す。
3. M59 の入口 = **空力・終端速度** (opt-in でビット互換維持)。ソルバ改装は M59 後半。
4. 材料 = **.physmat.json 資産 + コンポーネント側の個別上書き** (Unity 流二段構え)。
5. 熱・流体・光学・電気まで**全部ロードマップに含める** (粒子流体は実証スパイクのゲート付き)。

### 調査で確定した土台 (実ファイル裏取り済み)
- PhysicsSystem.cpp (1040行): 逐次インパルス固定8反復、蓄積インパルス無し・warm starting
  無し・スリープ無し・CCD 無し・サブステップ無し。重力 -9.81f ハードコード (:22)。
  ConstantForce 帯 (:535-563) = 「速度積分後・ソルバ前」の力適用ポイント。
  SpringJoint に Δv 100m/s クランプの前例 (:656-660)。ApplyImpulse (:217) = 作用点付き力積。
- **ソルバ数値変更の爆発半径は小さい**: golden 12 枚中、剛体が映るのは parts.png 1 枚のみ
  (tick3 で Drop 落下中、約2.4px/tol3)。.rep は毎回録り直しの使い捨て (cache/, gitignore)。
  壊れるのは PhysicsSelfTest の期待値 (約45項目、:315 にクォータニオン == 完全一致あり) が中心。
- 現最終 TypeId=35 (ReflectionProbe)。次は 36。登録順=TypeId、末尾 append のみ。
- 新資産種の触点 = 最小7/完全13箇所 (SoundLibrary 範型)。物理深部からの解決は
  meshcol::Install/Resolve 流儀 (MeshColliderLibrary.h:60-67)。
- 環境設定系の前例 = Skybox/Fog「entity.index 最小の active な 1 個」(RenderSystem.cpp:262)。
- ステートフルバックエンド追加の触点 22 箇所 (SimSnapshot 節/WorldHasher 3出口 約60呼び出し/
  Reset 2箇所/selftest)。**スリープ状態をコンポーネント末尾 append で持てばこの箱を開けずに済む**。
- CollisionSystem の prevPairs_/prevSolidPairs_ は SimSnapshot 被覆済み (非ハッシュ)。
  スクリプトへ渡る接触情報は相手 ID + 法線 (Enter のみ)。インパルス強度は渡らない。
- ABI v13=94 スロット。RemoveComponentByName / HasComponentByName は**無い** (v14 で追加)。
- 地形 (M58) は描画のみで当たり判定ゼロ。TerrainData::HeightAtTexel は sim から引ける
  (ただし uint32 wrap 罠 → TerrainSystem.cpp:31-38 の clamp ラッパ流儀を踏襲)。
- **発見: 静的コライダーの restitution は構造的に 0** (Collider に restitution フィールドが
  無く、Body 既定 0 のまま、結合則 e=min)。材料導入で「静的床で弾む」が新規能力になる。

---

## 実装開始時の手順

1. 本計画をリポジトリ `plans\greedy-cooking-wave.md` へコピーしてコミット対象にする (マイルストーン運用の定型) — 済。
2. 1 サブ = 1 コミット (`M59a1:` 形式の日本語件名) = 1 セッション + /clear。進捗の一次情報は git log、進捗表には計画外の事実・罠・申し送りのみ書く。

## 再開手順 (セッション跨ぎ用)

1. `git log --oneline -5` で最後に完了した M59x を確認。
2. 本ファイルの該当サブの節を読んで着手。列: a1 → a2 → b → b2 → e → c → d → g1 → g2 → f1 → f2 → h → i → j → k → l。
3. 全サブ共通検証: Debug/Release ビルド 0 警告 → `Editor.exe --selftest` 全 PASS → `tools\replay_verify.bat` PASS → `pwsh -File tools\check_rules.ps1` 0 error。ソース追加時は `pwsh -File tools\gen_project_files.ps1`。g2/l は `tools\shot_verify.bat`、k は `tools\build_managed.bat` 両構成も。
4. **a1〜e は既存シーンのビット同一を機械的に証明する** (PhysicsSelfTest の `[phys] hash @240` ログを変更前後で突き合わせ)。数値が動き始めるのは g1 から。
5. 新規 UI 文字列は `LocalizationTable.inl` に en/ja 両方 (規則 10)。TypeId は 36=PhysicsEnvironment / 37=Aero / 38=Buoyancy / 39=AeroSurface の末尾 append 厳守。
6. **確定済み (a1 冒頭の残確認事項)**: HashForPath = `assetkey::Resolve(NormalizePathKey(path))` で NormalizePathKey は `std::filesystem::absolute` を通す = **絶対パスハッシュ**。→ プリセット .physmat.json 自体はコミット可だが、**それを AssetRef で参照するシーン JSON はコミット不可** → デモは DemoContent 正本方式 (parts/flow と同じ)。

## 進捗表

| サブ | 状態 | コミット | メモ |
|---|---|---|---|
| M59a1 PhysMat 資産基盤 | 完了 | (本コミット) | 下記「M59a1 の申し送り」参照 |
| M59a2 材料の消費 | 未着手 | | |
| M59b PhysicsEnvironment + 等方空力 | 未着手 | | |
| M59b2 浮力 | 未着手 | | |
| M59e 可視化 + SolidContact 拡張 | 未着手 | | |
| M59c 面サンプリングカーネル | 未着手 | | |
| M59d 翼面 + ショーケース + replay 5 ペア目 | 未着手 | | |
| M59g1 ソルバ改装 (蓄積インパルス) | 未着手 | | |
| M59g2 サブステップ | 未着手 | | |
| M59f1 ジャイロ + COM オフセット | 未着手 | | |
| M59f2 静動摩擦 + 転がり抵抗 | 未着手 | | |
| M59h スリープ + アイランド | 未着手 | | |
| M59i 地形コライダー | 未着手 | | |
| M59j CCD | 未着手 | | |
| M59k ABI v14 束ね | 未着手 | | |
| M59l 仕上げ (golden physics.png) | 未着手 | | |
| (別件) net_verify DllReloader フレーク修理 | 未着手 | | 物理と無関係の独立コミット |

### M59a1 の申し送り (計画外の事実・罠)

1. **プリセット参照は .meta コミットで checkout 非依存になる (計画の仮定より緩い)。**
   HashForPath = 絶対パスハッシュだが、`assetkey::Resolve` は同伴 .meta の GUID を優先する
   (M30c のリネーム耐性)。別チェックアウトは「パスが変わった移動」と等価なので、**.meta を
   コミットした資産への AssetRef はシーン JSON に焼いてもコミット可**。プリセット 4 種の
   .meta (type "physmat") は replay_verify 実走で生成させてコミット済み。ただしデモは
   既存流儀どおり DemoContent 正本 (シーンファイルを置かない) で組む方針は維持。
2. **Sanitize は「非有限 = 既定値 / 有限の範囲外 = クランプ」の二段。** +inf をクランプ上限に
   落とすのは「ゴミを境界値という意味のある値に化けさせる」ので不採用 (selftest で固定)。
   density の NaN を 0 に落とさないのも同じ理由 (M59a2 の 1/m がゼロ除算相当に化ける)。
3. **既存の抜けを発見 (未修理・報告のみ)**: `SplitAssetName` の kCompound 表に
   `.terrain.json` / `.component.schema.json` が無い — これらをリネームすると複合サフィックスが
   剥がれて "y.json" (Unknown) になる。`.physmat.json` は追加済み。地形/スキーマの修理は別件。
4. 計画外の追加: Inspector の資産編集パネル (`DrawPhysMatInspector`、Sound 範型)。
   Save 時に Sanitize を通してから書く (手入力の NaN をファイルへ焼かない)。
5. DrawAssetRef の physMat 分岐は **"mesh" より前** に置いた (計画は "material"/"mat" より前と
   だけ言っていたが、既定の混合リスト落ちを避ける最安全は先頭)。照合は "physMat"/"physmat" 両方。

---
## 全体ロードマップ

```
M59  剛体の基礎力学と空力 (16 サブ、下に詳細)
      a1→a2→b→b2→e→c→d→g1→g2→f1→f2→h→i→j→k→l
  ↓
M60  関節と機構 (剛体トラック)          M60' XPBD 変形体 (並行可)
      拘束一般化 (Jacobian 行 + 蓄積λ)        ステートフルバックエンド様式の確立
      → ボール/ヒンジ → リミット/モータ       → 粒子+拘束基盤 → ロープ/鎖 → 布
      → 固定/スライダ/コーン                  (空力カーネル共有) → ソフトボディ
      → 複合コライダー + 凸包 (クック時生成)   → 塑性/粘弾性/クリープ/応力緩和
      → ラグドール (Parts/Skeleton 接続)
      → 車両 (レイキャストサス + タイヤ)
  ↓
M61  破壊 — 事前フラクチャクック (ボロノイ、CookedCache) → 接続グラフ + 破断
      (強度/靭性/脆性/延性 = 材料資産、入力は M59e の累積インパルス) → 破片剛体化
  ↓
M62  熱・流体・光学・電気
      温度場 (伝導・比熱) → 熱膨張 (拘束 rest 長) → 融解 (相変化)
      → 浮力/水面/波の拡充 → [実証スパイク] 粒子流体 (決定論固定順近傍探索の
      go/no-go 判定) → 光学 (屈折・吸光 = 描画側・ハッシュ外) → 電気 (導通グラフ)
```

### 材料特性 → 実現機構の対応 (どの特性がどこで実現されるか)

| 特性 | 機構 | 時期 |
|---|---|---|
| 密度・摩擦 (静/動)・反発・転がり抵抗・Cd | .physmat + 剛体ソルバ | **M59** |
| 粘着性 | 法線インパルス下限を負まで許可 | M60 |
| 剛性・弾性・圧縮性・粘性 | XPBD compliance / 体積・距離拘束 | M60' |
| 塑性・粘弾性・クリープ・応力緩和 | 拘束 rest 値更新 + Maxwell 要素 | M60' |
| 強度・靭性・脆性・延性・展性 | 破断閾値・累積エネルギー・塑性量 | M61 |
| 熱伝導率・比熱・熱膨張率・融点 | 温度場 + rest 長変調 + 相変化 | M62 |
| 流動性・表面張力・凝集性・濡れ性 | 粒子流体 (スパイクゲート) | M62 |
| 屈折率・透明度・吸光度 | 描画 (SSR/キューブマップ延長、**決定論外**) | M62 |
| 電気伝導率・抵抗率 | 接続グラフ導通 (ゲーム的近似) | M62 |
| 硬さ (局所押込) | へこみマップ近似 (将来候補) | 保留 |
| 多孔性・透過性・誘電率 | 投資対効果が成立しない | **対象外** |

### マイルストーンを跨ぐ予約事項 (今決めておかないと後で全部やり直しになるもの)

1. **ステートフルバックエンドの導入様式** — M59 では箱を開けない (スリープはコンポーネント
   常駐で実現)。M60' の XPBD 着手時に、WorldHasher の `const CpuParticleBackend*` 名指し
   シグネチャ (~60 呼び出し) を「sim 状態源の集約 struct」へ置換する**準備コミットを独立で
   切る**。以後のバックエンド追加は SimSnapshot 節 1 個の追加に縮む。
2. **SolidContact の拡張形** (M59e で確定) — 「ペアキー + 法線 + 代表接触点 + 法線インパルス
   合計」。消費者は可視化 (M59e)・接触 ABI (M59k)・破壊の応力入力 (M61)・衝撃音 (将来) の 4 者。
3. **面サンプリングカーネルの API** (M59c で確定) — 「表面要素 (点・法線・面積・その点の速度)
   → 力・トルク蓄積」の純関数。M60' の布が第 2 の呼び手になることを前提に署名を切る。
4. **サブステップ数の置き場** (M59g2 で確定) — PhysicsEnvironment のフィールド (ハッシュ対象、
   既定 4)。車両 (M60) が 8 を要求しがちなため定数にしない。
5. **乱流風の乱数** — M59 は一様定数風のみ。乱流は「PCG32 専用ストリーム + tick とセル座標
   からの純関数導出」と方針だけ予約 (規則 8)。
6. **TypeId 追加順** — M59: 36=PhysicsEnvironment / 37=Aero / 38=Buoyancy / 39=AeroSurface。
   M60 以降は各計画で末尾 append を厳守 (途中挿入不可)。
7. **ABI は各マイルストーン末に 1 回束ねる** — M59k=v14、M60 末=v15…。規則 11 の
   $apiVersionSlots 表と Interop.cs を同時更新。
8. **挙動バージョニングはしない** — g1 以降は物理シーンのビットが変わる。「既存シーン
   ビット同一」の約束は a1〜e (資産未割当・env 無し) にスコープされる。旧ソルバ互換
   スイッチは持たない (golden 再記録文化に従う)。engine_spec に明文化。

---

## M59 詳細 (16 サブ、1 サブ = 1 コミット = 1 セッション)

**列: a1 → a2 → b → b2 → e → c → d → g1 → g2 → f1 → f2 → h → i → j → k → l**

順序の根拠:
- 可視化 (e) を面空力 (c/d) より前に — 面ごとの力ベクトルが見える状態で開発する。
- **ソルバ改装 (g) を回転力学 (f) より前に** — f の静動摩擦分離・転がり抵抗はソルバループ
  本体に書くため、f 先行だと g で書き直しになる。サブステップ (g2) は実効 dt を変えるので
  dt 依存チューニング (ジャイロ・摩擦・kRestitutionVelThreshold「~2g·dt」) は g2 の後に 1 回で済ます。
- golden (physics.png) は挙動が固まる最後 (l) に 1 回だけ記録。
- a1〜e は既存シーンをビット同一に保つ (機械的証明つき)。**数値が動き始めるのは g1 から**。

### M59a1: PhysMat 資産基盤 (sim 非接触)
- `.physmat.json` 資産種 + `PhysMatLibrary` (SoundLibrary 範型: HashForPath/LoadFromFile/
  Register/Get/Enumerate 名前昇順/ToJson/FromJson) + `physmat::Install/Resolve`
  (meshcol:: 流儀、EngineContext を汚さない)。
- スキーマは **f2 の分まで最初に定義**: density / staticFriction / dynamicFriction /
  restitution / rollingResistance / dragCoefficient。将来の熱・破壊系キーは「後から足せる」
  ことをコメントで明文化。パース時に NaN/負値をサニタイズ (min/max 結合に NaN が入ると壊れる)。
- 触点: AssetType enum (append) / ClassifyPath (複合サフィックスは単一拡張子より先) /
  TypeName / ParseTypeName / AssetBrowser フィルタ+ラベル / LocalizationTable /
  起動スキャン (DemoContent.cpp:1265) / ReloadHub / Create メニュー + CreateXxxAsset /
  InspectorWindow::DrawAssetRef に **"physMat" 分岐を "material"/"mat" 系より前に**挿す
  (照合は大文字小文字区別 — "physMaterial".find("material") は npos で混合リストに落ちる)。
- プリセット 4 種 (鋼/木/ゴム/氷) — **HashForPath の正規化方式 (絶対パスか) を冒頭で確認**。
  絶対パスハッシュならプリセット参照シーンはコミット不可 → デモは DemoContent 正本方式
  (parts/flow と同じ) で回避し README に明記。
- 検証: ClassifyPath selftest / パース+サニタイズ+ラウンドトリップ / replay_verify 素通り。
- gen_project_files.ps1 実行 (新規ファイル)。

### M59a2: 材料の消費 — Collider 拡張 + ソルバ解決
- ColliderComponent 末尾 append: `physMaterial` (AssetRef) + `materialOverrideBits` (UInt32)。
  RigidbodyComponent 末尾 append: `useDensity` (Bool = 密度→質量導出 opt-in、質量 = ρ×形状体積)。
- **材料解決は収集時の純粋な値選択** (fp 演算を挟まない)。優先順位:
  overrideBits のビット → 既存フィールド (上書き値の格納庫として再利用) /
  材料割当あり → .physmat 値 / 未割当 → 既存フィールド。
  未割当シーンは「同じメモリを同じ経路で読む」ためビット同一が自明に成立。
- 結合則を明文化して固定: μ = sqrt(μa·μb) 維持 / e = min 維持。静的側も材料から restitution
  を得られるようになる (= 静的床で弾むのは**新規能力**。engine_spec に現状仕様と変更を明記)。
- Inspector: overrideBits はプロパティ別チェックボックスの小さな専用描画 (Collider.mask の
  ビットマスクポップアップ InspectorWindow.cpp:847-887 が前例)。
- 検証: (i) 既定同値材料シーン ⇔ 未割当シーンの並走 per-tick ハッシュ一致 (値経路の等価性証明)、
  (ii) overrideBits 優先順位、(iii) 密度→質量の解析値 (球 ρ·4/3πr³)、(iv) e=0.8 材料の静的床で
  h·e² 付近まで弾む (新テスト)、(v) PhysicsSelfTest の `[phys] hash @240` ログが変更前後で一致
  (fast-path ビット同一の機械的証明 — a1〜e の全サブで使う)。

### M59b: PhysicsEnvironment + 等方空力
- `PhysicsEnvironmentComponent` (TypeId 36): gravity (Float3 既定 0,-9.81,0) / airDensity
  (1.225) / windVelocity (Float3) / waterPlaneY / waterDensity。消費は Skybox/Fog と同じ
  「entity.index 最小の active な 1 個」規約。登録は消費者と同一コミット (未検証コンポーネント
  を漂わせない)。
- `AeroComponent` (TypeId 37): enableDrag / enableMagnus / enableAngularDrag (Bool 群) +
  Cd 上書き / 投影面積スケール / マグヌス係数。
- 実装は ConstantForce 帯の直後。等方二次抗力は**閉形式 implicit**: `v ← v/(1+(k·|v|/m)·dt)`
  (除算のみ、無条件安定)。風は相対速度 `v−v_wind` を抗力に食わせる。マグヌス `F = S(ω×v)`。
  **角速度二次抗力も入れる** (無いとマグヌスで回る球が永遠に回り続ける — angularDamping は
  非物理の定率なので置き換え候補として併存)。
- ★**fast-path は値ゲートでなく存在ゲート**: env コンポーネント不在 → 現行の
  `b.vy += kGravity * gravityScale * dt` (:507) を 1 文字も変えずに通す分岐。
  理由: -0.0f + 0.0f = +0.0f でビットが変わるため、gx=0 でも無条件ベクトル加算は不可。
  「env を置いた = 挙動変更に opt-in」が契約。AeroComponent も「装着 = 新数式に opt-in」の
  1 段で切る (係数 0 でのビット中立は約束しない)。bool OFF は項の計算ごとスキップ。
- **CharacterController は M59 では env に従わない** (kGravity 直参照 :261 + Y 軸前提の
  接地判定のまま) — 文書化して据え置き。任意重力ベクトルは CC の意味論を壊すため。
- 検証: 終端速度の閉形式一致 (v_t = √(mg/k))、風中の抗力平衡、マグヌスの曲がり方向 (符号断言)、
  Aero 非所持ボディ混在シーンで非所持側ビット不変 (並走ハッシュ)、fast-path ログ照合。

### M59b2: 浮力
- `BuoyancyComponent` (TypeId 38)。env の waterPlaneY/waterDensity を参照。
  没水体積: 球 = 球冠公式 (多項式のみ) / OBB・カプセル = 高さ比近似。水中抗力係数。
- 検証: 中性浮力の平衡没水深 (解析値)、浮上・沈降の定性、並走ハッシュ。

### M59e: 物理デバッグ可視化 + SolidContact 拡張
- **SolidContact を拡張**: ペアキー + 法線 + **代表接触点 + 法線インパルス合計** (予約事項 2)。
  per-tick 出力のままステートレス維持。CollisionSystem の既存消費は互換。
- 可視化 経路A (ts.debugLines → GameView にも出る): 接触点 (十字 glyph)・法線・速度ベクトル。
  経路B (SceneViewWindow::BuildOverlays): AddWireBox/Sphere/Capsule で形状・接触の常設ギズモ。
  トグルは SceneView の showGizmos_ 系に追加。
- 検証: 可視化 ON/OFF で並走ハッシュ一致 (収集 read-only の証明)、resim 中の多重蓄積なし
  (TickServices::resim ゲート)、fast-path ログ照合。

### M59c: 面サンプリングカーネル + 平板空力
- 新規 `AeroSampling.h/.cpp` — **純関数**: 「表面要素 (点・法線・面積・要素速度) → 力・トルク
  蓄積」。面ループは固定順 (OBB = 基底順 ±X±Y±Z の 6 面 / カプセル = kAeroCapsuleSegments
  固定 N 分割 / 球 = 解析 1 発)。**加算順序固定・並列化永久禁止をコメントに明記**。
- 平板モデルは超越関数を避けベクトル代数で組む (`sinα·cosα` = 内積の積)。揚力・風見安定は
  面積分の別成分として自然に出る。方向を変える項 (揚力/マグヌス) には SpringJoint 前例の
  「1 tick Δv 上限の決定論的クランプ」を必須にする。
- 検証: 純関数ユニットテスト — 一様流中の球の全抗力 = 解析値 / 対称 OBB 正面流でトルク厳密 0 /
  平板揚力が α=0°,90° で 0・符号正 / 2 回実行のビット一致。

### M59d: 翼面 + 物理ショーケース + replay 5 ペア目
- `AeroSurfaceComponent` (TypeId 39): 翼面 (位置・法線・コード・面積・失速角つき CL/CD 近似)。
  子エンティティに複数置く流儀 (Parts と同じ)。
- `--physics-demo` ショーケース (DemoContent 正本、シーンファイルは gitignore): 羽根と鉄球 /
  紙飛行機 / カーブボール / 浮き / 材料プリセット割当を全部載せる。
- **replay_verify.bat に 5 ペア目** (--physics-demo 600tick)。bat 編集は CRLF 維持・
  `if !ERRORLEVEL! NEQ 0` 流儀 (CLAUDE.md 環境の罠)。golden はここでは録らない (l で 1 回)。
- 検証: 5 ペア目が本丸。snapshot 往復 1 本を selftest に明示追加。

### M59g1: ソルバ改装 (構造) — 蓄積インパルス
- マニフォールド生成を反復ループの外へ (現行 :736 は毎反復生成)。蓄積インパルス
  (λ 累積 + クランプ)。位置補正を速度ソルバと分離した別パスへ。
- **縛り: 物理モデル (μ=sqrt積 / e=min / 閾値) を 1 つも変えない** — 構造だけ変える。
  期待値の再基準化を「静定位置の窓ずれ」程度に抑える。kBroadphaseMargin の根拠コメント更新。
- **ここから既存物理シーンの数値が動く** (replay ペア 2/3/5 は使い捨てなので自動追随。
  壊れるのは PhysicsSelfTest 期待値と parts.png のみ)。
- 検証: PhysicsSelfTest 全 45 項目の再基準化 (:315 の == 比較は窓比較に書き直し)、
  **10 段スタック 1200 tick を新設** (h の warm starting 計測ゲートを先に仕込む)、
  2 体衝突の運動量保存 (解析値)、broadphase 等価性 (test 24) 維持、並走ハッシュ維持。
- 最重量サブ。溢れそうなら「位置補正分離」を g2 に送る。

### M59g2: サブステップ
- kSubsteps は PhysicsEnvironment のフィールド (既定 4、ハッシュ対象 = sim 入力)。
- **dt 依存定数の再スケール**: kRestitutionVelThreshold (「~2g·dt」設計 :26)、ジャイロ・摩擦の
  チューニング前提。コメント更新。
- parts.png 再記録 (golden-diff-triage の 4 点計測をしてから --update)。
- 検証: スタックドリフト定量化、e=1 反発の頂点保存 ~99%、既存項目再基準化。

### M59f1: ジャイロ項 + 質量中心オフセット
- ジャイロ項 ω×Iω は **implicit** (陽的だと発散)、固定反復 kGyroIterations。
  テニスラケット定理 (中間軸不安定) が出る。
- 質量中心オフセット (材料/形状導出 or 明示フィールド)。ApplyImpulse の r 系が波及範囲。
- 検証: |ω| 非増加 (エネルギー断言)、中間軸回転の他軸成分獲得 (定性)、COM オフセット振り子の静定。

### M59f2: 静止/動摩擦分離 + 転がり抵抗
- .physmat の staticFriction/dynamicFriction/rollingResistance を消費 (スキーマは a1 で定義済)。
- 検証: μd < tanθ < μs の斜面ヒステリシス (静止からは動かず、初速を与えると滑り続ける)、
  転がり抵抗で球が N tick 内に完全静止 (h のスリープの前提を作る)。

### M59h: スリープ + アイランド
- スリープ状態は **Rigidbody 末尾 append** (sleepTicks: Int32 / isSleeping: Int32) —
  hash/JSON/snapshot/DLL 移行が自動被覆。**SimSnapshot 節・WorldHasher 変更なしで済む**
  (warm starting との決定的な差)。
- 判定は int tick カウンタ (float 秒累積は不可)。**入眠時に velocity/angularVelocity を
  厳密 0.0f へ書く** — スリープ中の WorldHash が完全静止し検証が安価になる + 残留ビットが
  構成差を運ぶ余地が消える。スリープ中は重力積分・damping もスキップ (しないと即起床)。
- 起床は走査内決定論: 候補ペア (小,大) 昇順走査中に「片方覚醒で接触 → その場で起床」。
  1 パスで届かない伝播は次 tick 繰越 (決定論的、1/60s 遅れは許容)。
  スクリプトの AddForce/AddImpulse/SetVelocity/AddTorque は起床トリガに配線。
- アイランドは**ソルバ走査順を変えないブックキーピングに限定** (島の全員が閾値未満のとき
  だけ入眠)。union-find は候補ペア昇順処理 + 根を最小 index に正規化 = 入力順非依存。
- **★スリープ中の OnCollisionExit 誤発火対策**: 睡眠ペアはソルバ出力から消えるため、
  CollisionSystem に「両者睡眠のペアは前 tick 接触集合から Stay として繰越す」規則を追加
  (prevSolidPairs_ は SimSnapshot 被覆済みなので snapshot 整合は既存流儀のまま)。
- warm starting は**計測ゲート**: g1 で仕込んだ 10 段スタック 1200 tick がサブステップだけで
  安定するなら延期 (予約事項 1 の箱を開けない)。入れる場合は必ず別コミット。
- 検証: 入眠 tick 断言 / 入眠中 WorldHash 完全不変 / velocity ビット厳密 0 / 投擲物で起床 /
  閾値 0 設定 ⇔ 導入前コードのビット同一 / test 24 拡張 (アイランドラベルも総当たり⇔sweep で
  一致) / スリープ中 OnCollisionStay 継続の回帰テスト。

### M59i: 地形ハイトフィールドコライダー
- ShapePose shape=4。TerrainData 注入は meshcol:: 流儀 (POD 維持のポインタ参照)。
- Collide/Manifold/Raycast/DistanceToShape/ComputeAabb の dispatch 追加。CC も地形を歩ける。
- **HeightAtTexel を直接呼ばない** (uint32 wrap 罠) — TerrainSystem.cpp:31-38 の符号付き
  clamp ラッパ流儀を踏襲。play 中の地形編集は sim に反映されない契約を明文化 (予約事項 6)。
- 検証: 平坦/斜面/谷の解析ハイトフィールドで接地・転がり・Raycast/SphereCast/CC 歩行、
  地形端 (負座標) clamp 回帰、並走ハッシュ。

### M59j: CCD
- opt-in per Rigidbody (Bool 末尾 append)。swept sphere/capsule 解析 + box 保守的前進
  (SphereCastWorld の既存実装を再利用)。固定最大反復 + 決定論的打切り。
- サブステップ後で正しい位置 (サブステップが CCD 需要の大半を先に潰すため)。
- 検証: 薄壁 + 高速弾 (CCD off で抜ける現状断言 → on で止まる)、混在並走ハッシュ。

### M59k: ABI v14 束ね
- 追加スロット案: RemoveComponentByName (構造変更 = コマンドバッファ tick 末適用、
  AddComponentByName の経路をミラー) / HasComponentByName / AddForceAtPosition /
  GetContactInfo (M59e の拡張 SolidContact を返す) / SampleWind / SampleTerrainHeight /
  WakeRigidbody / IsSleeping。
- EngineAPI.h + EngineApiTable.cpp 全充填 + Interop.cs 位置ミラー + MYE_API_VERSION 14 +
  check_rules $apiVersionSlots 表を**同時に** (規則 11)。tools\build_managed.bat 両構成。
- 検証: C# 一時 probe で新スロット全数実走 (C# レーンは replay 被覆外のため)。

### M59l: 仕上げ
- golden `physics.png` を shot_verify に追加 (12→13 枚、bat と CI 判定数の更新、tol は
  parts.png 前例 = 3)。parts.png 最終確認。engine_spec / README / CLAUDE.md の検証表更新。
- 全量検証: フルリビルド警告 0 / selftest / check_rules / shot_verify / replay_verify (5 ペア) /
  crash_verify。

---

## 決定台帳 (M59 の設計判断)

| # | 論点 | 決定 |
|---|---|---|
| 1 | ビット互換の契約 | 「コンポーネント装着 = 新数式に opt-in」の存在ゲート。値ゲート (係数0で中立) は -0.0+0.0=+0.0 のため約束しない |
| 2 | 材料の優先順位 | overrideBits → 既存フィールド (格納庫再利用) / 材料 → .physmat / 未割当 → 既存フィールド。解決は fp 演算なしの値選択 |
| 3 | 結合則 | μ=sqrt(μa·μb)、e=min を維持。静的 restitution=0 の現状仕様と「材料で弾むようになる」変更を engine_spec に明記 |
| 4 | 単位系 | SI (m/kg/s/N) 現行踏襲。実材料からの換算表をプリセットのコメントに書く。GPa 級の剛性は M60' の XPBD compliance 側で扱う (float 直値は発散) |
| 5 | 資産の中身とリプレイ | .physmat の値はワールドハッシュ非対象 (メッシュコライダーと同クラス = 「再生時に同じ資産がある」前提)。ホットリロードは sim を変える既存資産クラスに合流 |
| 6 | スリープの状態置き場 | コンポーネント末尾 append (SimSnapshot 節を開けない)。入眠時に速度を厳密 0 化 |
| 7 | warm starting | 計測ゲート (10 段スタック 1200 tick)。必要になった時のみ・別コミット・SimSnapshot v4 |
| 8 | CC と env | M59 では CC は env に従わない (文書化)。将来 Y 成分のみ追従を検討 |
| 9 | linearDamping/angularDamping | 残す (既存シーン互換)。空力使用時は 0 推奨をドキュメントに。angularDamping はスリープ導入後に役割縮小 |
| 10 | ON/OFF 二段 | 構造的 = コンポーネント付け外し (Inspector ポップアップ / ABI v14 Add/Remove/Has)、フィールド的 = Bool チェックボックス (自動) / SetComponentField (ABI 追加ゼロ)。毎 tick の付け外しは非推奨 (アーキタイプ移動)、常用は bool |

## 壊れるもの台帳

| 対象 | いつ | 内容 |
|---|---|---|
| PhysicsSelfTest 期待値 | g1, g2, f1, f2 | 約 45 項目の再基準化 (== 比較は窓比較へ) |
| tests\golden\parts.png | g2 | 再記録 (golden-diff-triage の 4 点計測後に --update) |
| TypeId | b, b2, d | 36-39 を末尾 append |
| ABI | k のみ | v13→v14、束ねて 1 回 |
| ReplayFile / SimSnapshot 版 | **bump なし見込み** | スリープをコンポーネント常駐にしたため。warm starting 導入時のみ SimSnapshot v4 |
| 既存シーンのビット | **a1〜e は不変** (機械的証明つき) / g1 以降は物理シーンのみ変わる | .rep は使い捨てなので再記録作業は無い |

## 検証の柱

1. **fast-path ビット同一の機械的証明** (a1〜e): PhysicsSelfTest の `[phys] hash @240` +
   body ビットパターンログを変更前後で突き合わせ。
2. **並走ハッシュ決定論** (全サブ): 同一シーン 2 個並走の per-tick ハッシュ一致 (既存流儀)。
3. **等価性の常時検証** (h): sDisableSleepForTest 追加。総当たり⇔sweep のアイランドラベル一致。
4. **replay_verify 5 ペア目** (d〜): --physics-demo 600 tick、Debug⇔Release。
5. **解析解との一致** (b, c, f): 終端速度・浮力平衡・運動量保存・ジャイロのエネルギー非増加。
6. **golden は最後に 1 回** (l): physics.png 追加。

## 残確認事項 (実装時に最初に潰す)

- a1 冒頭: PhysMatLibrary::HashForPath の正規化方式 (絶対パスか) → プリセット参照シーンの
  コミット可否を確定。不可なら DemoContent 正本方式で回避 (既定の想定)。
- 別件: net_verify の DllReloader 排他オープン競合フレーク (M54-58 から M59 送りになっている
  もの) は物理と無関係の独立修理 — M59 のどこかで単独コミットとして回収する。

## 実装の進め方

- この計画をリポジトリ `plans\` へコミットしてから着手 (M52/M54-58 と同じ流儀)。
- 1 サブ = 1 コミット (`M59a1: ...` 形式の日本語件名) = 1 セッション + /clear。進捗の一次情報は git log。
- 各サブ完了時: selftest 両構成 → check_rules → replay_verify → (該当時 shot_verify)。
- ソースファイル追加時は `pwsh -File tools\gen_project_files.ps1`。bat は CRLF。
- M60 以降の詳細計画は M59 完了時に本ファイルの予約事項を前提に策定。
