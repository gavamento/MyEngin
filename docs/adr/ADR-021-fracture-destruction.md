# ADR-021: 破壊 (Chaos Destruction 相当) — 事前分割 + 接着グラフ + root proxy

- 状態: 採用 (2026-09-26、M80a〜M80l)
- 出所: 「UE の Chaos Destruction 相当の破壊物理を実装する」(依頼原文)。要件と事前調査は
  `plans\m80-destruction\design-draft.md`、疑った点と結論は `plans\m80-destruction\spec.md` §2。
  仕様の本文は `engine_spec.md` §10.8。

## 決定 1: エディタで事前に分割し、破片エンティティを最初からシーンに置く (root proxy)

実行中に割る (UE の Chaos と同じ「壊れる瞬間に切る」方式) は、切断・凸包生成・接着グラフ構築を
毎回 tick 予算の中で走らせる必要があり、決定論を保ったまま Voronoi 分割 (数百 ms 級) を
フレーム内に収めるのは非現実的。**エディタで焼いて `.mfrac` に固定し、破片エンティティ
(`FracturePiece`) を Destructible の子として事前生成**する v1 を採る。

壊れる前の見た目と物理は「root proxy」方式 (UE と同じ): ルートに `Rigidbody(compoundColliders)`
と元メッシュの `MeshRenderer` を残し、破片は子エンティティとして凸包 `Collider` + 破片メッシュを
持つ。**描画だけ** `Destructible.broken` を見て「壊れる前は破片を描かず、壊れた後はルートの
元メッシュを描かない」を切り替える (`RenderSystem::CollectDrawables` が
`ShouldHideUnbrokenFracturePiece` を読むだけ、車輪の見た目と同じ「描画はハッシュ済みの欄を読む
だけ」の形)。

却下:
- **ActiveComponent で破片を隠す**: 物理・スクリプト・衝突からも外れてしまい、割れた瞬間に
  当たり判定を持つ剛体として持ち上げられない。
- **MeshRenderer.mesh を null にして退避**: 退避用の欄が新設され、Inspector で触ると壊れる状態
  異常を作り込む。

## 決定 2: 破片データは資産 (`.mfrac`、assets 側 + `.meta`) として git 共有し、cache には置かない

`cache\cooked\` の既存の焼き結果 (`.mterr` / `.mcvx` / `.msfm`) はシーン側に依存が無く、消えても
焼き直せる。破壊は違い、**シーンの破片エンティティ (姿勢・メッシュ ID) が焼き結果の中身に依存する**
— 焼き方式が将来変わったときに cache だけを正としてしまうと、既存シーンと黙ってずれる。
`.mfrac` を assets 側に置き `.meta` 付きで git 共有し、AssetType へ `Fracture` を末尾追加した。

却下:
- **cache のみに置く**: 別 PC でのチェックアウトや封印配布 (焼き直し不可の環境) で破片が
  再現できなくなる。アルゴリズムを直しても既存シーンが古い焼き直し結果のまま黙って動く。

## 決定 3: 破断・分離・割れた後の状態は ECS のコンポーネント欄だけに置く (ECS 外の状態を作らない)

ECS 外のシミュレーション状態は「プール + `WorldHasher` + `SimSnapshot`」の 3 点セットが必要で、
1 つ漏れると replay は通るのに巻き戻し (タイムトラベル / ネットのロールバック) だけ壊れる、
という実績のある壊れ方 (`engine_spec.md` §11.3)。破片の隣接切断状態は `FracturePiece.brokenBonds`
(隣接表の k 番目に対応する 1 bit のビットマスク、資産側で隣接上限 32) として、`damage` /
`releaseTicks` / `phase` と一緒に**全部コンポーネントの欄**に持たせた。これで replay / snapshot /
what-if の被覆を新しいプール実装なしに自動で得ている。

## 決定 4: 破片の当たりは凸包 1 個で近似する (凸分解はしない)

Voronoi セル ∩ 元メッシュは凹み得るし、U 字の両腕のように非連結にもなり得る。非連結な部分は
別破片へ分けるが、**凹みは破片 1 つにつき凸包 1 個で近似**する (当たりがわずかに太る)。

却下:
- **凸分解 (V-HACD 相当)**: サブが 1 本以上増え、破片 1 つが複数の子形状を持つことになり
  接着グラフ・質量特性・root proxy の構築が全て複雑化する。v1 の要件 (壊れる物理として遊べる)
  に対して割に合わない。

## 決定 5: 荷重は「接触は毎 tick の瞬間値、蓄積するのはスクリプトの `ApplyFractureDamage` だけ」

関節の破断は「tick 全体のインパルス / dt = 平均反力」で判定する既存の仕組み (`PhysicsSystem.cpp`)
があり、置いてあるだけの物にも重力分の接触インパルスが毎 tick 入る。接触インパルスまで
蓄積すると、静置しているだけの破壊物が時間経過だけで勝手に割れる「疲労」挙動になり、下限値の
調整という新しい設計面を持ち込む。**破片 i の荷重 `L_i = C_i + damage_i`** (`C_i` は今 tick の
形状単位接触インパルス合計 / dt、`damage_i` はスクリプトの `ApplyFractureDamage` だけが加算する
永続値) とし、接着 `(i,j)` は `max(L_i, L_j) >= S_ij` (`S_ij = strength * clamp(面積比, 0.25, 4)`)
で切れる。

## 決定 6: 固定された壁は「ルート kinematic」で表す (接地アンカーは後回し)

UE の Anchor や RayFire の Connected Cluster 支持判定に相当する「地面に接しているか」の判定は、
地形・傾斜・複数接地点の扱いなど v1 の要件を超える複雑さを持ち込む。**ルートが kinematic なら
壊れても残る塊はそのまま kinematic、分かれた塊は常に dynamic** とし、残る塊は体積最大 (先着優先で
同値は index 最小) にした。「叩いた所だけ抜けて残りは固定のまま立っている」という UE の壁破壊の
見た目を、新しい判定系を作らずに表現している。

## 決定 7: `.mfrac` を読み込むたびに `ConvexColliderLibrary` / `MeshLibrary` へ登録し直す

破片の凸包は `ConvexColliderLibrary::Register(AssetID, data)` で直接登録し、物理の広域・狭域判定は
`Get` が返すキャッシュを見るだけ (`shape=5`)。**`ConvexColliderLibrary::Clear()` の後に
`FractureLibrary::ReregisterAll()` を対で呼ばないと、shape=5 の子が黙って無視され破片がすり抜ける**
(`Get` が null を返すだけでエラーにならない — 一番気づきにくい壊れ方)。2026-09-25 時点で `Clear()`
を呼ぶ本番経路は 0 件 (grep で確認済み) なので、自動で対にする仕組みはまだ足していない。
**`ConvexColliderLibrary::Clear()` を本番経路で呼ぶ変更をするときは、必ず
`fracturelib::Library()->ReregisterAll()` を対で呼ぶこと** — この契約は
`ConvexColliderLibrary::Clear()` の宣言コメントにも 1 行で書いてある (`ConvexColliderLibrary.h`)。

## 決定 8: ABI は `ApplyFractureDamage` 1 本 + `onBreak` イベント 1 本 (v22)

v14 以降の方針 (`docs\history\api-scripting-tools.md`) は「専用スロットを増やさず汎用
`GetComponentField`/`SetComponentField` で足りるものは増やさない」。状態の読み出し (`broken`、
`detachedCount`) はこの汎用アクセサで足りるので専用 getter は作らない。損傷を与える書き込みだけは
「半径内の破片へ距離減衰で加算する」という**書き込み側の意味論**を持つため、汎用フィールド書込みで
表現できず 1 本のスロットにした。分かれた瞬間の通知 (`onBreak`) はイベント配信なので同様に
`MyeScriptDesc` 末尾へ 1 本追加。これで v21 (125 スロット) → v22 (126 スロット)。

`onBreak` の配信順序 (spec §4.1 破断 8: ルート index → 新リーダー index 昇順) を保証するため、
`FractureSystem::UpdateImpl` は Destructible の処理順を `ForEachArchetype` の行順ではなく
**root の `EntityID.index` 昇順**へ明示的に並べ替えている。単一の Destructible しか壊れない
既存シーン (`--fracture-demo` を含む) ではこの並べ替えは観測できない差分だが、複数の
Destructible が同一 tick に同時に割れる構成 (`--fracture-bench` の 8 個同時ベンチ等) では
スクリプトへ届く順序を構成非依存にする。

## 決定 9: `ApplyFractureDamage` は Engine 層の内部関数として sub-07 で先に実装し、ABI は最後にまとめて 1 回だけ bump する

「ABI は各マイルストーン末に 1 回」という既存の運用 (v12/v14/v21 も同じ) に従い、sub-07 の時点で
`FractureSystem.h` に Engine 層の内部関数 `ApplyFractureDamage(World&, EntityID, XMFLOAT3, float,
float)` を先に実装し (SelfTest はこれを直接呼んで荷重蓄積の挙動を検算していた)、ABI テーブルへの
公開 (`EngineApiTable.cpp` の `out.ApplyFractureDamage` がこの内部関数を呼ぶだけ) は sub-12 まで
遅らせた。これにより ABI bump の回数を M80 全体で 1 回に抑えている。

## 拡張点: 実行中の再分割 (B) への足場

v1 は「事前分割のみ」だが、壊れた瞬間に破片を生成する B 機能を見据え、焼き (純関数
`BakeFracture`) / 資産 (`.mfrac`、`FractureAsset`) / 破片エンティティを組む関数
(`BuildFracturePieces(World&, EntityID root, const FractureAssetHandle&)`) / 破断ロジック
(`FractureSystem` — 破片 index → エンティティの表だけを見て、焼き方式を一切知らない) の 4 層に
分けてある。`CreateEntity` は反復中でも安全 (`World::IsIterating()` 下でも即時) なので、B は
「破断の直前に `BuildFracturePieces` を呼ぶ」形で足せるはずで、`FractureSystem` 側の変更は
不要という設計になっている。インターフェース階層 (`IFractureStrategy` のような抽象化) は
現時点で使い手が 1 つしか無いため作っていない (AGENTS §5)。

## 未解決のまま残した後回し項目 (spec §3 の「後回し」の再掲)

- **破片の位相的な閉じ** (T 字接合の解消・頂点の共有)。v1 は幾何的な閉じ (体積保存 + ベクトル
  面積 ≈ 0) だけを保証する。実行中の再分割 (B) で破片をもう一度切るときに必要になる見込み。
- **打ち抜き**: 割れた tick の衝突は分かれる前の塊 (kinematic なら無限質量) に対して解かれるため、
  固定された壁を撃った弾はその tick で止まる/跳ね返る。弾の運動量で破片を弾き飛ばす見た目
  (UE 風) にするには、割れた tick に物理をもう一度分かれた後の質量で解き直す段取りが要り、
  `TickRunner` の順序変更と replay 一致の再検証を伴うサブが 1 本増える。「受けたインパルスの
  一定割合を分かれた塊の速度として与える」簡易案は運動量が保存しない不物理な調整値になるため
  推奨しない。
- **地面接地によるアンカー判定** (決定 6 の却下案そのもの)、**ワールド共通の破片上限**
  (v1 は Destructible ごとの `maxDebris`)、**多階層クラスタ** (v1 は破片 1 階層 + 接着グラフ)、
  **凸分解**・**自己交差メッシュの検出** (決定 4 の却下案)。

## 観測可能性: RT の BLAS 再構築

割れた瞬間に描画対象メッシュの集合が変わるため、RT (既定 off) では全 BLAS を焼き直す
(`RtScene.cpp`)。sub-11 の計測ではフレーム時間へのスパイクは実測の範囲で問題化しなかった
(`plans\m80-destruction\bench.md`)。RT を使うプロジェクトが多数の破壊物を同時に割る構成を
組む場合は、破片メッシュを最初から BLAS 集合に入れておく対策が候補になる (未実装、必要が
出た時点で追加する)。
