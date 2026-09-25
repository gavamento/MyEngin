# M80 破壊物理 (Chaos Destruction 相当) — 仕様書

- slug: m80-destruction
- 状態: 確定 (2026-09-25、planner 裁定。ユーザー運用指示により AskUserQuestion なし。§7 の `[ユーザーに聞ける]` は Notion で回答待ち)
- 依頼原文: m80-destruction: UE の Chaos Destruction 相当の破壊物理を実装する。要件と事前調査は plans/m80-destruction/design-draft.md (要件節はユーザー確定済み)。運用: 質問は AskUserQuestion を使わず推奨で裁定し、Notion (https://app.notion.com/p/3e502024a4d481df8d01cbe69c013018) の表へ記録して先へ進む。作業ツリーの既存 WIP には触らない。

## 1. 目的 (なぜ作るか)

エンジンでゲームを作る人が、**メッシュ 1 つを選んでインスペクターでボタンを押すだけで「壊せる物」にでき**、
ぶつけた / スクリプトから損傷を与えたときに、**ぶつかった場所の近くから割れて破片が物理で飛び散る**状態にする。
しかもそれが、このエンジンの契約 (Debug / Release / replay / スナップショット / 巻き戻しのビット一致) を 1 ビットも壊さない。

達成したい状態:
- 静的メッシュ・スキンメッシュのどちらでも、エディタで事前に破片 (Voronoi 分割) を焼き、破片エンティティがシーンに最初から入っている
- 壊れる前は元のメッシュ 1 つで描かれ、1 つの剛体として振る舞う。当たった破片の近くの接着だけが切れ、塊ごとに剛体として分かれる
- 割れた後の破片の後始末 (残す / 消える / 沈む / 縮む / 静的化 / 上限) をインスペクターで選べる
- 閉じていないメッシュは既定で理由付きで拒否され、「ボクセル化を許容」を選べば閉じたメッシュにしてから割れる
- GameLogic (C++ DLL) と C# から、破壊イベントを受け取り、損傷を与えられる
- 破壊物を含まないシーンは、挙動・絵・replay が今と完全に同じ

## 2. 疑った点と結論

「ユーザーの判断」列: 要件 = design-draft の確定済み要件 (疑い直さない)。裁定 = planner 裁定 (ユーザー未確認)。`[聞ける #n]` = §7 で Notion に上げる論点。

| 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|
| 複合コライダーで破片を束ねられるか (破片の数だけ子形状が要る) | `kMaxCompoundShapes = 16` は関数ローカルのスタック配列 (`PhysicsSystem.cpp:1617-1634`)。17 個目以降は**質量・重心・慣性からだけ**黙って落ち、`subCount` は全数なので広域・接触では使われる (`:1585`, `:3229`, `:3598`)。警告なし | 裁定 | 上限を撤廃する (sub-05)。16 以下の既存経路は加算順を変えずビット一致を保つ |
| 凸包の子形状は複合で正しい質量特性になるか | 複合の合成は子の**形状原点を重心**とし (`:1611-1612` 「box / 球 / カプセルはいずれも原点対称」)、慣性は `LocalInertiaDiag` の対角近似 (`:1671-1673`)。凸包 (shape=5) の実重心・フル慣性 (`ConvexMassProperties`) は単体ボディの経路 (`:1700-1750`) でしか使われない。破片の塊は「凸包の子を持つ複合」そのもの | 裁定 | **複合の子が shape=5 のときだけ** `ConvexMassProperties` の体積・重心・フルテンソルを使う (sub-05)。既存デモの複合は box / カプセルのみ (`DemoContent.cpp:1936-1949`、`RagdollBuilder.cpp:326`) なので既存 replay は動かない。却下案: 破片メッシュを重心原点で焼くだけで済ませる = 慣性は対角近似のまま、スキン破片 (骨空間で焼く) では重心もずれる |
| どの破片が叩かれたかが分かるか | `SolidContact` はボディ対に 1 件へ畳まれ、子形状の情報が無い (`PhysicsSystem.h:22-35`, `.cpp:4347-4396`)。`ContactConstraint` にも子の index が無い (`:3518-3544`) | 裁定 | `SolidContact` は変えない (消費者 6 系統: CollisionSystem / スクリプト GetContactInfo / ModalAudio / AcousticField / DebugDraw / SelfTest)。**別の出力「形状単位の法線インパルス (形状を持つエンティティ, インパルス)」を足し、呼び出し側がポインタを渡したときだけ作る** (存在ゲート、sub-05)。物理は Fracture を知らない |
| 破片を最初から作っておくと、壊れる前の見た目と物理をどうするか | 可視性は `ActiveComponent` しか無く、これは物理・スクリプト・衝突からも外す (`Components.cpp:17-26`, `RenderSystem.cpp:1106`)。MeshRenderer に visible 欄は無い | 要件 (事前生成) + 裁定 | **root proxy 方式** (UE と同じ)。ルートに `Rigidbody(compoundColliders)` と元メッシュの MeshRenderer、破片は子エンティティで凸包 Collider + 破片メッシュ。**描画だけ**「壊れる前は破片を描かず、壊れた後はルートの元メッシュを描かない」を RenderSystem が `Destructible.broken` を読んで決める (車輪の見た目と同じ「描画はハッシュ済みの欄を読むだけ」)。却下案: ActiveComponent で隠す = 物理からも消えて当たらない。MeshRenderer.mesh を null にして退避 = 退避用の欄が要り、Inspector で触ると壊れる |
| 分かれた塊を誰が剛体として持つか | 複合は「Rigidbody を持つ最も近い祖先」で子形状を集める (`PhysicsSystem.cpp:1445-1478`)。構造変更は tick 末のコマンドバッファ (`World.h:19-25`, `World.cpp:747`, `TickRunner.cpp:491`)。`CreateEntity` は反復中でも即時で安全 (`World.cpp:27-52`) | 裁定 | **塊の中で破片 index 最小の破片を「リーダー」にし、Rigidbody を足してルートの親の下へ付け替え、他の塊メンバーはリーダーの子へ付け替える** (ワールド姿勢を保つ)。ルートに残る塊は「体積最大 (同値は index 最小を含む方)」。リーダーの塊が割れたときはリーダーを含む成分がリーダーに残る。事前に「塊用の空エンティティ」を N 個作る案は却下 (エンティティが倍、使わない Rigidbody の置き場が要る) |
| 接着の状態を ECS の外に置くか | ECS 外のシミュレーション状態は「プール + WorldHasher + SimSnapshot」の 3 点セットが必要で、1 つ漏れると replay は通るのに巻き戻しで割れる (`engine_spec.md` §11.3)。Reflection に配列型は無く、要素ごとに登録する前例あり (`Components.cpp:305-329`) | 裁定 | **全部コンポーネントの欄に置く**。破片ごとに `UInt32` のビットマスク 1 本 (隣接の切断状態、隣接は資産側で最大 32)、`damage`、`releaseTicks` 等。ハッシュ・スナップショット・What-if は自動で被覆 |
| 破片データを焼いてどこに置くか | 既存の焼き結果 (`.mterr` / `.mcvx` / `.msfm`) は `cache\cooked\` の CookedCache (`CookedCache.h:8-10`) で、**シーン側に依存が無い** (消えても焼き直せる)。破壊は違い、シーンの破片エンティティ (姿勢・メッシュ ID) が焼き結果の中身に依存する。焼き方式が将来変わると cache の再生成で既存シーンと食い違う | 裁定 `[聞ける #6]` | **`.mfrac` を assets 側に置き (`.meta` 付き、git で共有)**、ソース設定は Destructible コンポーネントの欄。AssetType を末尾追加。却下案: cache だけ = 別 PC / 封印配布で焼き直しになり、アルゴリズム変更で黙ってシーンとずれる |
| 破片メッシュの参照方法 | MeshRenderer は `AssetID` (登録名のハッシュ) だけを持つ (`Components.h:45-49`)。1 ファイル多メッシュは `guid://<16hex>#...` の登録名で表す前例 (`ModelLoader.cpp:396`, `ModalSoundLibrary.h:42-43`)。凸包は `ConvexColliderLibrary::Register(AssetID, data)` で直接登録でき、`Resolve` はキャッシュを先に見る (`ConvexColliderLibrary.h:39-41`) | 裁定 | 登録名 = `guid://<mfracGuid>#frag<i>` (外側面)、`#frag<i>#cap` (断面)、`#frag<i>#hull` (凸包)。`.mfrac` 読み込み時に MeshLibrary と ConvexColliderLibrary へ登録する。**ConvexColliderLibrary::Clear() の後に再登録されないと shape=5 が黙って無視され破片がすり抜ける** (`Get` が null を返す) ので、再登録の経路を sub-03 で必ず作る |
| 断面 (内部マテリアル) をどう描くか | MeshRenderer は 1 メッシュ 1 マテリアル、サブメッシュ index は無い (`Components.h:45-49`)。多マテリアルのモデルもプリミティブごとに子エンティティ (`ModelLoader.cpp:393-418`) | 裁定 `[聞ける #1]` | **破片ごとに断面用の子エンティティ `_cap` を 1 つ** (MeshRenderer のみ、Collider なし)。マテリアルは `Destructible.innerMaterial`、未設定ならルートのマテリアル。却下案: 断面を外側と同じメッシュに入れる = エンティティは半分で済むが断面の見た目を変えられない。MeshRenderer にサブメッシュを足す = 描画全経路 (Forward / Deferred / 影 / RT / インスタンシング) に波及する |
| 凹んだ破片の当たり | Voronoi セル ∩ 凹んだメッシュは凹み得るし、非連結にもなり得る (U 字の両腕をまたぐセル)。凸包は頂点 64 まで (`ConvexHull.h:31`) | 裁定 `[聞ける #7]` | **非連結な部分は別の破片に分ける。凹みは破片 1 つにつき凸包 1 個で近似**する (当たりが少し太る)。却下案: 凸分解 (V-HACD 相当) = サブ 1 本以上増え、破片 1 つが複数の子形状になり接着グラフも複雑化 |
| 閉じていない判定の定義 | 既存に判定は無い (design-draft) | 要件 (拒否既定 / ボクセル化) + 裁定 | 位置のビット一致で溶接 (−0 は +0 へ) → 無向辺ごとの使用数と向きを数える。**境界辺 (使用 1) / 非多様体辺 (使用 3 以上) / 向きの不一致** のどれかがあれば「閉じていない」。理由と件数を返す。自己交差は検出しない (後回し) |
| ボクセル化に既存 Voxelizer を使うか | `Voxelizer` は 32³ 固定で Deep-Modal の学習と実行時が共有する契約 (`Modal/Voxelizer.h:1-9, 21-34`、`ModalTypes.h:16-17`) | 裁定 | **流用しない**。破壊用に解像度可変の別関数 + surface nets を作る (sub-04)。WIP の deepmodal ファイルにも触れない |
| 破壊の荷重モデル | 関節の破断は「tick 全体のインパルス / dt = 平均反力」を tick 末に判定 (`PhysicsSystem.cpp:3911-3928, 4483-4517`、spec §10.5)。置いてあるだけの物にも重力分の接触インパルスが毎 tick 入る | 裁定 `[聞ける #4]` | 破片 i の荷重 `L_i = (その tick の接触法線インパルス合計)/dt + damage_i`。`damage_i` はスクリプトの ApplyDamage だけが**蓄積**する (永続)。接着 (i,j) は `max(L_i, L_j) ≥ S_ij` で切れる。`S_ij = strength × clamp(面積_ij / 平均面積, 0.25, 4)`。却下案: 接触も蓄積 (疲労) = 静置荷重で時間とともに勝手に割れる |
| 壊れた後にどの塊がルートに残るか (固定された壁など) | UE は Anchor で固定、RayFire は Connected Cluster の支持判定 | 裁定 `[聞ける #5]` | ルートが kinematic なら**ルートに残る塊は固定のまま** (= 壁が地面に立っている表現)、残る塊は体積最大。地面との接地判定によるアンカーは後回し |
| 「上限超過で古い順に消す」の範囲 | 挙動は Destructible ごとに選ぶ要件 | 要件 (6 種) + 裁定 `[聞ける #3]` | **Destructible ごと**の上限 (`maxDebris`)。古さ = 分離した tick の早い順、同値はエンティティ index 昇順。却下案: ワールド共通の上限 = 挙動を物ごとに選ぶ要件と噛み合わない |
| スキンメッシュの破片をどう描き、どう追従させるか | CPU の `Mesh` にはボーンウェイトが無く、`.mmdl` の `CookedMesh.vertices` だけにある (`GpuResources.h:41-56`, `ModelCook.h:29-33`)。`PartFollowSystem` は tick 内で骨に追従し `LocalTransform` を書く (決定的、直下の子が条件) (`PartFollowSystem.h:12-52`, `TickRunner.cpp:390`)。規約は `partLocal == jointGlobal` (spec §10.5 Ragdolls) | 要件 (スキン対象) + 裁定 `[聞ける #2]` | 焼きはバインドポーズ。各破片を**支配的な骨** (外側頂点のウェイト合計最大) に割り当て、**破片メッシュをその骨の空間で焼く** (inverseBind を掛ける)。破片 = SkinnedMesh 直下の子 + `PartComponent(joint)` で骨に追従、ルートは kinematic の複合。壊れた瞬間に Part を外して剛体化。**一度でも割れたら元のスキンメッシュは描かず、残りも剛体の破片として骨に追従して描く** (関節で継ぎ目が見える)。却下案: 破片ごとにスキン描画 = 破片の MeshRenderer にパレットを渡す経路が要りサブが 1 本増える |
| 閉じていない判定を「メッシュ全体」でするか (スキンの服など) | glb は UV 継ぎ目で頂点が割れるが位置は同一 → 位置溶接で閉じる。服や髪は本当に開いている | 要件 | 位置溶接で判定。開いていれば拒否 or ボクセル化 (要件どおり) |
| 多マテリアルのモデル全体を 1 つの破壊物にできるか | 多マテリアルはプリミティブごとの子エンティティ (`ModelLoader.cpp:393-418`) | 裁定 `[聞ける #8]` | **1 Destructible = そのエンティティの MeshRenderer のメッシュ 1 つ**。モデル全体を割るならプリミティブごとに付ける (互いに接着はしない)。統合は後回し |
| 新しい TypeId 64 / 65 で既存シーンが変わるか | 登録は末尾追加が規則 (`Components.cpp:36-42`)。スキーマ / スクリプトのコンポーネントは組込みの後に番号が振られるので**実行時の TypeId が 2 つずれる**。保存データは名前キーなので安全、ハッシュは `nameHash` を畳む (`WorldHasher.cpp:194-239`) | 裁定 | TypeId 64 = `Destructible`、65 = `FracturePiece`。スクリプト持ちのシーンでは**ハッシュのバイト列の並び**が変わり得るので、sub-06 以降の既存シーン確認は「前コミットの rep との照合」ではなく **replay_verify (Debug/Release) + shot_verify + 物理 SelfTest** で行う (spec §10.5 と同じ理屈: 挙動が動かないことを見る)。旧 rep 照合は物理だけを触る sub-05 で行う |
| ABI の足し方 | v14 以降の方針は「専用スロットを増やさず汎用 Get/SetComponentField で」(`docs/history/api-scripting-tools.md:14,17`)。一方要件は「破壊イベントと破壊 API (ABI 変更あり)」。イベントは `MyeScriptDesc` の末尾追加 (`ScriptTypes.h:49-67`) | 要件 + 裁定 `[聞ける #9]` | **スロット 1 本 `ApplyFractureDamage(entity, point, radius, amount)` + イベント `onBreak`**。状態の読み出し (`broken`、分離済み数) は汎用 `GetComponentField` で足りるので専用スロットは作らない。v22 / 126 スロット、最後のサブで 1 回だけ上げる |
| スクリプトの ApplyDamage を ECS 外のキューに積むか | ECS 外キュー = 3 点セットが要る | 裁定 | 呼ばれた瞬間に対象破片の `damage` 欄へ加算する (半径内の破片に距離で線形減衰、ワールド行列は直近の tick のもの)。スクリプト実行順は決定的なので結果も決定的。Update で呼べば同じ tick、LateUpdate で呼べば次の tick の判定に効く |
| 壊れた瞬間の生成 (B) への拡張点 | `CreateEntity` は反復中も安全 (`World.cpp:27-52`) | 要件 (拡張点を残す) | 「焼く (純関数) / 資産 (.mfrac) / 破片エンティティを組む関数 `BuildFracturePieces` / 破断ロジック (破片 index → エンティティの表だけを見る)」を分ける。B では破断の直前に `BuildFracturePieces` を呼べば足りる形にし、ADR に書く。インターフェース階層は作らない (AGENTS §5) |
| 分割コアの決定論 | 実行時の分割 (B) では Debug/Release で同じ破片が要る。凸包生成は既に「入力順に依らずビット同一」が契約 (`ConvexHull.h:20`) | 裁定 | 乱数は `Pcg32` (`Random.h`)、並列化しない、演算順固定、ハッシュコンテナの走査順を使わない。**同じ入力から同じバイト列**を SelfTest で確認し、Debug と Release の digest 一致を受け入れ条件にする |
| 焼き結果のバイト列を demo で使ってよいか | replay_verify は scene JSON をコードから作り直してから録る (`replay_verify.bat` の各 job) | 裁定 | `--fracture-demo` は**ビルド時にメモリ上で焼いて登録する** (ファイルなし、登録名 `fracture://demo...`)。Debug と Release が独立に焼いて replay が一致すること自体が「分割コアの構成間一致」の実行経路での証明になる |
| 60 Hz に収まる破片数 | 未計測。複合 1 つの子が N 個なら、相手 1 体あたり子形状 N 回の狭域判定。分かれた後はボディが最大 N 個 | 裁定 | ハード上限は 256 破片 / Destructible (隣接 32)。**推奨の既定値と Inspector の上限は sub-11 の実測で決める** |
| RT の BLAS 焼き直し | 参照メッシュの集合が変わると全 BLAS を焼き直す (`RtScene.cpp:64-120`)。壊れた瞬間に集合が変わる | 裁定 | sub-11 で計測し、スパイクが問題なら対策 (例: 破片メッシュを最初から集合に入れる) を入れる。RT は既定 off なので必須経路ではない |
| 作業ツリーの WIP | `WaterPass.cpp` / deepmodal 系 / ルート直下の一時ファイルが未コミット | 運用 (ユーザー指示) | **どのサブも触らない**。WIP と同じファイルを触る必要が出たら止めて報告 |

**押し切られた点**: なし (ユーザーとの往復なし。確定済み要件は疑い直していない)。

## 3. スコープ

### やる
- 分割コア (Engine 層の純関数): 閉じているかの判定、平面切断 + 蓋 (穴あき断面の三角形分割)、Voronoi 分割 (内部シード、セル多面体、非連結の分離、極小片の統合)、破片ごとの凸包、接着グラフ (隣接・接触面積)、決定論
- 閉じていない入力: 既定は理由付き拒否。「ボクセル化を許容」なら解像度可変のボクセル化 + surface nets で閉じたメッシュにしてから分割
- 破片資産 `.mfrac` (assets 側、`.meta` 付き) の形式・読み書き・登録 (メッシュ / 凸包)
- 物理: 複合の上限撤廃、凸包の子形状の正しい質量特性、形状単位の法線インパルス出力 (存在ゲート)
- コンポーネント `Destructible` (TypeId 64)、`FracturePiece` (TypeId 65)、破片エンティティ構築関数、root proxy の描画規則
- 破断: 荷重 → 接着切断 → 連結成分 → 塊の剛体化 (tick 末)、質量と速度の引き継ぎ、kinematic ルート (固定された壁)
- 割れた後の 6 挙動 (Tick 数で処理)
- エディタ: インスペクター (焼きの非同期実行、状態・理由の表示、生成 / 再生成の Undo、プレハブインスタンスでの生成禁止)、ローカライズ
- スキンメッシュ (ウェイト読み出し、骨の割り当て、骨空間で焼く、Part で追従、壊れたら剛体化)
- 計測: 物理内部のプロファイルスコープ、ベンチシーン、上限の決定、RT / 描画のスパイク対策
- ABI v22: `onBreak` イベント、`ApplyFractureDamage` スロット、C# ミラー、check_rules の表
- デモ `--fracture-demo` と replay_verify の job、`engine_spec.md` / ADR-021

### やらない (明示)
- 実行中の分割 (B)。拡張点を残すだけ
- 凸分解 (破片 1 つに凸包 1 個)
- 自己交差メッシュの検出
- 破片ごとのスキン描画 (割れた後は剛体の破片で描く)
- 複数メッシュ (多マテリアルモデル全体) を 1 つの破壊物として接着すること
- 地面接地によるアンカー判定、Field (UE の Anchor / Strain Field) 系
- 破片の衝突音 (ModalSound) や音響との連携
- WIP ファイル (`WaterPass.cpp`、deepmodal 系、ルート直下の一時ファイル) の変更

### 後回し
- 実行中の分割 (B)、壊れた瞬間の破片エンティティ生成
- 凸分解、自己交差検出
- 破片のスキン描画、壊れる前の骨の速度を破片へ引き継ぐこと (v1 はルートの速度だけ)
- 接地アンカー、ワールド共通の破片上限
- 多階層クラスタ (UE の Cluster level。v1 は破片 1 階層 + 接着グラフ)
- 破片メッシュの位相的な閉じ (T 字接合の解消・頂点の共有)。実行中の再分割 (B) で破片をもう一度切るときに要る

## 4. 仕様

### 4.1 振る舞い

#### 焼き (エディタ時、または demo のビルド時)
入力: ソースメッシュ (CPU の positions / indices / normals / uvs、スキンならウェイトも)、`seed`、`pieceCount` (目安)、`openMeshMode`、`voxelResolution`。
1. 位置のビット一致で溶接し閉じているか判定。閉じていて符号付き体積が負 (全面が内向きの巻き) なら**拒否せず全三角形を裏返して**外向きにそろえる (分割は外向きの入力を前提にする)。閉じていなければ `openMeshMode == 0` で拒否 (理由: 境界辺 n 本 / 非多様体辺 n 本 / 向き不一致 n 本)、`== 1` でボクセル化 + surface nets (占有の境界をまたぐ格子辺ごとに四角形、頂点はセルごとに 1 つで、そのセルの符号が変わる辺の中点の平均。反復の平滑化はしない) の閉じたメッシュへ置き換える。ボクセルの角だけで接する形 (曖昧な配置) は事前に占有を足して解消し、辺が 3 面以上で共有されないようにする
2. `Pcg32(seed)` で AABB 内の点を棄却法でメッシュ内部に `pieceCount` 個とる (内外判定はパリティ。試行回数に上限、足りなければ取れた数で進む)
3. 各シードの Voronoi セルを凸多面体として作り (AABB を膨らませた箱を二等分面で切る)、セルの面 (候補面) を求める。破片の**外側面**は元メッシュの三角形をセルの全候補面で三角形単位にクリップして作る。**断面 (蓋)** はシード対 (i,j) ごとに 1 回だけ作る: 二等分面 P_ij で**元の閉じたメッシュ**を 1 回切った断面 (sub-01 の切断、前提を満たす入力) を、i と j の他の候補面の和集合で三角形単位にクリップしたもの。i 側はそのまま、j 側は巻きを反転して使う (両側でビット同一、接着面積も対称)。破片は**幾何的に閉じた三角形の集まり**とし、位相的な閉じ (頂点の共有) は求めない: 外側面と蓋の継ぎ目は丸めで数 ulp ずれ、蓋どうしの継ぎ目 (Voronoi 辺) には T 字接合が残る。判定は「体積の保存」と「破片ごとのベクトル面積の和 ≈ 0」(面が欠けていない) で行う。蓋の UV は**断面平面への正射影** (平面の正規直交基底 t,b への射影、1 m = UV 1。平面上なので歪みなし)、法線は平面法線
4. 切った結果を連結成分に分け、別成分は別の破片にする
5. 体積が平均の `minVolumeRatio` (既定 0.1) 未満の破片は、接触面積が最大の隣へ統合する (同値は index 小)。平均は統合のたびに残った破片で数え直す
6. 破片ごとに体積重心を原点にしたメッシュ (外側 / 蓋の 2 本) と凸包 (`BuildConvexHull`) を作る。スキンなら原点は骨 (§4.1 スキン)
7. 接着グラフ: 蓋どうしが同じ平面上で重なる面積を隣接 (i,j) の面積とする。隣接が 32 を超える破片は面積の小さい順に落とす (落とした数を結果に記録)
8. 破片の並びは決定的な順 (シード index → 成分の最小頂点など、sub-02 で固定) で index を振る

出力はバイト列として決定的。上限: 破片 256 / 隣接 32 / 凸包頂点 64。

#### 破片エンティティ (事前生成)
`BuildFracturePieces(world, root, fractureAsset)` (Engine 層) が、ルートの子として破片 i ごとに:
- 破片エンティティ `Frag<i>`: `LocalTransform` = 破片原点 (ルート空間)、`MeshRenderer(mesh=#frag<i>, material=ルートのマテリアル)`、`Collider(shape=5, meshAsset=#frag<i>#hull)`、`FracturePiece(index=i, root=ルート)`
- その子 `_cap`: `MeshRenderer(mesh=#frag<i>#cap, material=innerMaterial or ルートのマテリアル)`
ルートには `Rigidbody(compoundColliders=true)` が必要 (無ければ付ける)。ルート自身の Collider は外す (生成時に警告)。

#### 壊れる前
- ルートの Rigidbody が破片凸包の複合として動く。描画はルートの元メッシュだけ (破片と `_cap` は描かない)
- `Destructible.broken == false`

#### 破断 (tick ごと、物理と衝突イベントの後)
1. 物理が出した形状単位インパルスから、各破片の接触荷重 `C_i = Σ impulse / dt` を作る
2. `L_i = C_i + damage_i`。接着 (i,j) (未切断、i<j) を i 昇順・隣接表順に見て、`max(L_i, L_j) ≥ S_ij` なら両側のビットを立てる
3. 切れた接着があった塊ごとに、未切断の接着で連結成分を作り直す (破片 index 昇順の union-find)
4. 成分が 2 つ以上になった塊を分ける: ルートの塊 → 体積最大の成分がルートに残る。リーダーの塊 → リーダーを含む成分が残る。分かれた成分ごとに index 最小の破片をリーダーにし、`Rigidbody` を足してルートの親の下へ (ワールド姿勢を保つ `LocalTransform`)、残りのメンバーはリーダーの子へ。すべて tick 末のコマンドバッファ
5. 質量: `ルートの全質量 × 成分の体積 / 全体積` (ルートが `useDensity` なら密度のまま)。残った塊の質量も同じ式で下げる
6. 速度: 元の塊の `v + ω × (成分重心 − 元の重心)` と `ω`。ルートが kinematic なら残る塊は kinematic のまま、分かれた塊は dynamic
7. 初めて割れた tick に `Destructible.broken = true`。以後ルートの元メッシュは描かず、全破片 (と `_cap`) を描く
8. 分かれた塊ごとに `onBreak` を発行 (ルートのスクリプトへ。ルート index → 新リーダー index 昇順)

#### 割れた後 (分かれた塊 = リーダー単位、`releaseTicks` を 1 tick ずつ進める)
| `afterBreak` | 挙動 |
|---|---|
| 0 残す | 何もしない |
| 1 N tick 後に消える | `releaseTicks ≥ afterBreakTicks` で塊ごと Destroy |
| 2 沈んで消える | `afterBreakTicks` 経過で塊の全 Collider の `mask = 0` (当たらなくなり重力で沈む)、さらに `fadeTicks` 後に Destroy |
| 3 縮んで消える | `afterBreakTicks` 経過から `fadeTicks` かけてリーダーの scale を線形に 0 へ、終わったら Destroy |
| 4 静止したら静的化 | リーダーの `Rigidbody.isSleeping` になったら Rigidbody を外す (塊の Collider は静的形状になる) |
| 5 上限超過で古い順に消す | この Destructible の分かれた塊が `maxDebris` を超えたら、`releaseTicks` が大きい順 (同値は index 小) に超過分を Destroy |
ルートに残った塊には適用しない。時間はすべて Tick 数 (実時間を使わない)。

#### スクリプト
- `ApplyFractureDamage(entity, point, radius, amount)`: `entity` は Destructible のルートまたは破片。その Destructible の破片のうち、原点が `point` から `radius` 以内のものの `damage` に `amount × (1 − d/radius)` を加える (radius ≤ 0 なら最寄りの 1 つに amount)。呼んだ瞬間に欄へ書く
- `onBreak(state, ctx, MyeEntityId piece, MyeVec3 point, float impulse)`: ルートにあるスクリプトへ、分かれた塊のリーダーと、その塊で最大の荷重の破片の原点、その荷重を渡す
- 状態は汎用 `GetComponentField` で `Destructible.broken` / `Destructible.detachedCount` を読む

#### エッジケース
- `.mfrac` が見つからない / 読めない / 破片数と子の `FracturePiece.index` が合わない → その Destructible だけ破断しない (壊れる前の 1 剛体のまま動く) + ERROR 1 回 + Inspector に赤字
- 破片 1 つ (pieceCount=1 相当) → 破断しない (壊れる前のまま)
- ルートが Rigidbody を持たない → 生成時に付ける。実行時に無ければ破断しない + ERROR
- 非一様スケールのルート: 破片の回転はルートと同じなので分離後も TRS で表せる。ルートの親に回転と非一様スケールが重なる場合は保証しない (制限として文書化)

### 4.2 データ・保存形式・互換性

#### `Destructible` (TypeId 64、ルート)
| 欄 | 型 | 既定 | 意味 |
|---|---|---|---|
| `fractureAsset` | AssetRef | null | `.mfrac` |
| `pieceCount` | Int32 | 16 | 破片数の目安 (2..256) |
| `seed` | UInt32 | 1 | 分割の seed |
| `openMeshMode` | Int32 | 0 | 0 = 拒否 / 1 = ボクセル化を許容 |
| `voxelResolution` | Int32 | 32 (sub-04 / sub-14 の計測。48 は開いた箱・16 破片で Release 10.8 秒) | ボクセル化の最長辺セル数 (16..256) |
| `innerMaterial` | AssetRef | null | 断面のマテリアル (null = ルートのもの) |
| `strength` | Float | 5000 | 接着の基準強度 [N] |
| `afterBreak` | Int32 | 0 | §4.1 の 0..5 |
| `afterBreakTicks` | Int32 | 300 | 挙動 1/2/3 の開始までの tick |
| `fadeTicks` | Int32 | 60 | 挙動 2/3 の長さ |
| `maxDebris` | Int32 | 64 | 挙動 5 の上限 |
| `broken` | Bool | false | 一度でも割れたか (ハッシュ対象) |
| `detachedCount` | Int32 | 0 | 分かれた塊の累計 (ReadOnly) |
焼きの入力 (`pieceCount` / `seed` / `openMeshMode` / `voxelResolution` とソースメッシュ) は焼き直しにだけ効く。実行時は `.mfrac` の中身が正。

#### `FracturePiece` (TypeId 65、破片)
| 欄 | 型 | 意味 |
|---|---|---|
| `root` | EntityRef | Destructible のルート |
| `index` | Int32 | `.mfrac` の破片 index |
| `brokenBonds` | UInt32 | 隣接表の k 番目が切れていれば bit k |
| `damage` | Float | ApplyDamage の蓄積 |
| `releaseTicks` | Int32 | 分かれてからの tick (リーダーだけが進める。-1 = 未分離) |
| `phase` | Int32 | 割れた後の段階 (0 通常 / 1 沈み・縮み中) |
全欄ハッシュ対象 (ReadOnly 表示)。

#### `.mfrac`
ByteWriter / ByteReader (`Engine/Core/ByteIo.h`) で、magic `"MFRC"`、版 1。中身: ソースの識別 (メッシュ登録名のハッシュ、焼き入力)、破片数、破片ごとに {原点 (ソース空間)、体積、外側メッシュ (MeshVertex + index)、蓋メッシュ、凸包 (`SerializeConvexHull`)、隣接 (相手 index, 面積) ×≤32、スキンなら骨名}、焼きの記録 (落とした隣接数、統合数)。境界検査付きで、壊れたファイルでも落ちない。書き出しは同じ入力で同じバイト列。AssetType に `Fracture` を末尾追加 (`AssetDatabase.h:13-32`)。

#### 互換性
- 既存コンポーネントの欄は変えない (`kSimSnapshotVersion` を上げるかは WaterWave (63) 追加時の前例に従う)
- 既存シーンの JSON・ハッシュの意味・replay・golden 画像は不変 (存在ゲート)
- ABI v21 → v22 (最後のサブで 1 回)。`MyeScriptDesc` 末尾に `onBreak`、テーブル末尾に `ApplyFractureDamage`

### 4.3 UI / ビジュアル
- Destructible の Inspector: 自動 UI の欄 (enum 相当は Int32 + ツールチップ) に加え、`DrawComponentNotes` で
  - 「破片を生成」ボタン (焼きはワーカースレッド。焼き中はボタン無効 + 「焼いています: <段階>」表示。完了したら `.mfrac` を書き、破片の子を組み直す。1 回の Undo で戻る)
  - 状態: 未生成 / 焼き中 / 生成済み (破片 n 個、隣接の切り捨て n、統合 n) / 失敗
  - 閉じていないメッシュで拒否したとき**赤字で理由** (境界辺 n 本 など) と「ボクセル化を許容に切り替えると割れます」
  - プレハブインスタンスでは生成ボタン無効 + 理由 (子の追加は上書きとして記録されない、`Prefab.h:95`)
- 保存先: プロジェクト assets の `Fracture/<ソース名>_<seed>_<pieceCount>.mfrac` (既存なら上書き確認なしで上書き、Undo は子エンティティ側だけ)
- 見た目: 壊れる前はルートのメッシュそのもの。割れた瞬間に破片へ切り替わっても**形が一致して見える** (破片の外側面は元の三角形を切っただけ)。断面は innerMaterial
- スクショで確認: `--fracture-demo` の割れる前 (tick 0 付近) と割れた後 (tick 120 付近)

### 4.4 非機能
- 決定性: 分割コアは同じ入力で同じバイト列 (Debug/Release 一致)。破断・塊の分離・割れた後の挙動はすべてハッシュ対象の欄と tick 数だけで決まる。順序は entity index / 破片 index 昇順
- 存在ゲート: Destructible が無いシーンでは形状単位インパルスを作らず、FractureSystem も走らない
- 性能: 60 Hz の tick 予算に収まる破片数を sub-11 で実測し、Inspector の推奨値と上限に反映
- ローカライズ: Inspector の文字列は `LocalizationTable.inl` の両言語、`###` 識別子一致、`Tr()` を printf の書式にしない
- レイヤー: 分割コアと `.mfrac` と FractureSystem は Engine 層、焼きの UI は Editor 層。物理は Fracture を知らない (形状単位インパルスは汎用の出力)

## 5. 受け入れ条件

1. 閉じているかの判定が、閉じたメッシュ (箱、トーラス、UV 継ぎ目で頂点が割れた箱) を通し、開いたメッシュ (蓋のない箱、平面) と非多様体を理由・件数付きで拒否する — `Editor.exe --selftest`
2. 平面切断 + 蓋で、凸・凹・穴あき断面 (トーラスを輪切り) の両側が閉じたメッシュになり、体積の和が元と相対 1e-4 以内 — `--selftest`
3. Voronoi 分割が、箱と凹形状 (L 字、トーラス) で破片がすべて幾何的に閉じ (ベクトル面積の和が破片の表面積の 1e-4 以下)、体積和が元と相対 1e-4 以内、非連結が分離され、極小片が統合され、隣接 ≤ 32 — `--selftest`
4. 同じ入力で分割結果のバイト列が 2 回一致し、SelfTest が出す digest が Debug と Release で一致する — Debug / Release の `--selftest` ログ比較
5. `.mfrac` の往復 (書く → 読む → 書く) がバイト一致し、壊れたファイル (切り詰め・magic 違い・版違い) で落ちずに失敗を返す。読み込みでメッシュと凸包が登録され、`ConvexColliderLibrary::Clear()` 後も再登録される — `--selftest`
6. 開いたメッシュ (蓋のない箱) が「ボクセル化を許容」で閉じたメッシュになって分割でき、解像度で細かさが変わる — `--selftest`
7. 複合の子が 17 個以上でも全子形状が質量・慣性に入り、shape=5 の子は凸包の重心とフル慣性で合成される — `--selftest`
8. 形状単位インパルスが、どの子形状 (エンティティ) が叩かれたかを返し、合計がボディ対の `SolidContact.impulse` と一致する — `--selftest`
9. 物理だけを触ったサブ (sub-05) の前後で、既存 replay (default / parts / physics / joints) の旧コミットで録った rep が新ビルドの Debug と Release で一致する — `--replay-verify`
10. 破壊物を含まないシーンは不変: `tools\replay_verify.bat` 全 job PASS、`tools\shot_verify.bat` 全枚 PASS (各サブで該当分)。shot_verify は、**基点コミットを WIP 抜きの worktree でビルドして同じ結果が出る枚は除外する** (2026-09-25、sub-05 で golden 4 枚が M80 着手前からずれている疑い。作業ツリーのビルドには WIP の `WaterPass.cpp` が入るので、shot_verify は WIP 抜きの worktree で比べる)
11. `--fracture-demo` で、壊れる前は元メッシュ 1 つ・1 剛体として動き、ぶつかった所から割れ、塊ごとに剛体化し、Debug/Release の replay が一致し `--snapshot-stress` も通る — replay_verify の `fracture` job
12. kinematic ルート (固定された壁) は、叩いた所だけ抜け、体積最大の塊が固定のまま残る — `--selftest` + デモのスクショ
13. 割れた後の 6 挙動がそれぞれ Tick 数どおりに動く (消える tick、沈み始めの mask、縮みの scale、静的化で Rigidbody が消える、上限で古い順) — `--selftest`
14. Inspector: 生成ボタンで非同期に焼け、焼き中は UI が固まらず、完了で破片の子ができ、Undo 1 回で戻る。開いたメッシュは赤字の理由が出る。プレハブインスタンスでは無効 — スクショ + `--selftest` (生成関数)
15. スキンメッシュ: 骨に追従した破片が複合としてアニメ中も当たり、割れた破片は剛体化し、割れた後は元のスキンを描かない。Debug/Release replay 一致 — `--selftest` + デモ (または専用シーン) の replay + スクショ
16. `.mfrac` 欠落・不一致でも落ちず、その Destructible だけが破断しない + ERROR — `--selftest`
17. ベンチで破片数ごとの物理時間を計測し、60 Hz に収まる推奨値・上限を決めて Inspector と文書に反映する。RT on で割れた瞬間のスパイクを計測し、必要なら対策 — 計測ログ (plans に記録) + `--rt` のスクショ
18. ABI v22: `onBreak` が C++ DLL と C# の両方で届き、`ApplyFractureDamage` で割れる。`check_rules.ps1` 規則 11 PASS (v22 = 126 スロット) — `--selftest` + デモのスクリプト + `tools\check_rules.ps1`
19. `engine_spec.md` に破壊の節、ADR-021 (方式・拡張点・却下案)、`--fracture-demo` を replay_verify に追加 — 文書差分 + `replay_verify.bat`
20. どのサブも WIP ファイル (`src/Engine/Renderer/WaterPass.cpp`、`tools/deepmodal/*`、`assets/deepmodal/*`、ルート直下の一時ファイル) を変更しない — `git diff --stat` / SELF_EVAL の触ったファイル

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | 閉じ判定と平面切断 + 蓋 (分割コアの土台) | なし | 1, 2, 20 | M80a: 破壊の分割コア — 閉じ判定と平面切断・断面の蓋 |
| sub-02 | Voronoi 分割・凸包・接着グラフ・決定論 | sub-01 | 3, 4, 20 | M80b: Voronoi 分割と破片の凸包・接着グラフ |
| sub-13 | 凸包生成の無限ループ修正とトーラスの焼き検証 (sub-02 から切り出し) | sub-02 | 3 (トーラス分), 4, 9 相当 (既存の凸包のビット一致), 20 | M80b2: 凸包生成が縮退面で止まらない不具合を直し、トーラスの焼きを確かめる |
| sub-03 | 破片資産 `.mfrac` と FractureLibrary | sub-02 | 5, 16 (読み込み側), 20 | M80c: 破片資産 .mfrac の形式と登録 |
| sub-04 | ボクセル化 + surface nets (開いたメッシュの経路) | sub-01 | 6, 20 | M80d: 開いたメッシュのボクセル化と surface nets |
| sub-14 | 断面の三角形分割を掃引法 (libtess2) に置き換える | sub-04 | 1, 2, 3, 6, 20 | M80d2: 断面の三角形分割を libtess2 の掃引法に置き換える |
| sub-05 | 物理: 複合の上限撤廃・凸包の子の質量特性・形状単位インパルス | なし | 7, 8, 9, 10, 20 | M80e: 複合コライダーの上限撤廃と形状単位の接触インパルス |
| sub-06 | コンポーネント・破片エンティティ構築・root proxy 描画・`--fracture-demo` (壊れる前) | sub-03, sub-05, sub-13 | 10, 11 (壊れる前), 16, 20 | M80f: Destructible / FracturePiece と破片の事前生成 |
| sub-07 | 破断と塊の分離 (tick 末)、kinematic ルート | sub-06 | 10, 11, 12, 20 | M80g: 接着の破断と塊の剛体化 |
| sub-08 | 割れた後の 6 挙動 | sub-07 | 10, 13, 20 | M80h: 割れた後の破片の後始末 6 種 |
| sub-09 | エディタ: Inspector・非同期焼き・生成の Undo・ローカライズ | sub-06, sub-04, sub-14 | 14, 20 | M80i: 破壊物のインスペクターと非同期の破片生成 |
| sub-10 | スキンメッシュの破壊 | sub-07, sub-09 | 15, 20 | M80j: スキンメッシュの破壊 (骨追従から剛体化) |
| sub-11 | 計測・ベンチ・上限の決定・RT / 描画のスパイク対策 | sub-08, sub-10 | 17, 10, 20 | M80k: 破壊の計測と破片数の上限 |
| sub-12 | ABI v22 (onBreak / ApplyFractureDamage)・デモ仕上げ・文書 | sub-11 | 18, 19, 10, 11, 20 | M80l: 破壊イベントと損傷 API (ABI v22) |

並列にできるもの: sub-05 は sub-01〜04 と独立。sub-13 は sub-03 / sub-04 / sub-05 と並列可 (sub-06 の前に要る)。sub-14 は sub-05〜08 と並列可 (sub-09 の前に要る)。sub-04 は sub-02 / sub-03 と並列可 (sub-01 の閉じ判定だけに依存)。

## 7. 未決事項・リスク

### `[ユーザーに聞ける]` (planner 裁定済み、Notion で回答待ち)
1. 断面の描き方: 破片ごとに断面用の子エンティティ (エンティティ数 2 倍) で内部マテリアルを分ける裁定。逆 (外側と同じメッシュ・マテリアル) なら sub-06 が軽くなり内部マテリアル欄が消える
2. スキン: 一度割れたら全部を剛体の破片で描く (関節で継ぎ目が見える) 裁定。逆 (破片ごとのスキン描画) ならサブが 1 本増える
3. 「上限超過で古い順に消す」は Destructible ごと、の裁定。逆 (ワールド共通) なら PhysicsEnvironment に欄が増え sub-08 の判定がワールド走査になる
4. 荷重: 接触は瞬間値、ApplyDamage だけ蓄積、の裁定。逆 (接触も蓄積) なら置いてあるだけで時間とともに割れる物になり、疲労の下限値の欄が要る
5. 固定された壁は「ルートを kinematic」で表し、体積最大の塊が残る、の裁定。逆 (接地アンカー) なら接地判定とアンカー欄が要りサブが 1 本増える
6. `.mfrac` を assets に置き git で共有、の裁定。逆 (cache のみ) なら焼き方式の変更で既存シーンと黙ってずれ、封印配布で焼けない
7. 凹んだ破片は凸包 1 個で近似、の裁定。逆 (凸分解) ならサブが 1 本以上増える
8. 1 Destructible = メッシュ 1 つ、の裁定。逆 (モデル全体を 1 破壊物) なら複数メッシュの接着と断面マテリアルの対応が要る
9. ABI は `ApplyFractureDamage` 1 本 + `onBreak`、状態は汎用 GetComponentField、の裁定。逆 (専用 getter も) ならスロットが増える
10. 「ボクセル化を許容」の見た目: surface nets の滑らかな形 (角が丸まる) の裁定。既定の解像度は焼き時間の実測で決める (Release で 10 秒以内に焼ける最大、64 か 32)。逆 (ブロック状のまま / 解像度を優先して遅くてもよい) なら、ブロック状は実装済みの形を残すだけになるが、断面の三角形分割の負荷は同じ対策が要る
11. 断面の三角形分割に外部ライブラリ libtess2 (SGI Free Software License B 2.0 = MIT 相当) を `external/` に取り込む裁定 (耳切りの失敗が 3 回目のため)。逆 (自作を続ける / 解像度 32 で打ち止め) なら、sub-14 は自作の掃引法または CDT になって規模が倍以上になる / 高ポリの実モデルや薄い壁の断面で焼きが失敗し得るまま残る
12. shot_verify の golden 4 枚 (parts / joints / acoustic_forward / acoustic_deferred) が M80 と無関係にずれている件 (基点の確認結果は司会が台帳に記録) / 裁定: M80 では golden を更新せず、除外して進める。更新するかどうかと原因の調査は M80 の外 / 逆 (M80 の中で更新する) を選ぶと、無関係な絵の変化 (影の有無など) を原因を調べずに正として固定することになる

### リスク
- 断面の三角形分割 (穴あき・縮退・ほぼ同一平面の頂点) の頑健さ — sub-01 で最初に潰す。失敗は「その切断を諦めて破片を統合」など安全側へ倒す規則を sub-01 で決める
- 焼き時間 (破片 256 × 高ポリ) — sub-02 で計測値を記録。遅ければ sub-09 のワーカーで吸収 (UI は固めない)
- 複合 1 つに子 N 個の狭域判定コスト、壊れた直後のボディ急増 — sub-11
- `ConvexColliderLibrary::Clear()` 後の再登録漏れ = 破片のすり抜け (黙った壊れ方) — sub-03 で `FractureLibrary::ReregisterAll()` を作り SelfTest で固定した。2026-09-25 時点で `Clear()` を呼ぶ本番経路は 0 件 (planner が grep で確認) なので、仕組みは足さない。**`Clear()` を本番経路で呼ぶ変更をするときは `fracturelib::Library()->ReregisterAll()` を対で呼ぶ**、を ADR-021 と `ConvexColliderLibrary::Clear()` の宣言コメントに書く (sub-12)
- 新 TypeId で実行時のスクリプト TypeId がずれる — 保存データは名前キーで安全。確認は replay_verify / shot_verify で行う
- スキン: `.mmdl` からのウェイト読み出し経路の有無 (CPU Mesh には無い) — sub-10 の最初に確認

## 8. 変更履歴
(確定後の変更のみ)
- 2026-09-25 (sub-01 VERDICT round 1、coder SELF_EVAL の不安・質問 1・3 から):
  - §4.1 焼き 1: 閉じているが内向きに巻かれたメッシュ (符号付き体積 < 0) は拒否せず裏返して外向きにする、を追加。切断関数の前提は「外向きの閉じたメッシュ」。理由: sub-01 で蓋の向き補正が要った根本原因が、SelfTest のトーラス生成が内向きに巻かれていたことだと判明 (planner が `FractureSelfTest.cpp` の `MakeTorus` を手計算で確認: θ=0, φ=0 で面法線 (−1,0,0)、外向き法線は (+1,0,0))。実データでも全面反転のモデルはあり得るので入口で正規化する
  - §4.1 焼き 3: 蓋の UV を「箱投影」から「断面平面への正射影」に変更。平面の蓋に対しては正射影が歪みのない投影で、箱投影 (3 軸から選ぶ) より良い。coder の実装をそのまま仕様にした
- 2026-09-25 (sub-02 VERDICT round 1、coder SELF_EVAL の不安・質問から):
  - §4.1 焼き 3: 蓋の作り方を変更。「切るたびに断面を集める」(逐次切断) をやめ、「対ごとに元メッシュを 1 回切った断面を、両セルの他の候補面でクリップして両側で共有」に。理由: round 1 の実装は、外側面だけを逐次クリップした (蓋のない = 開いた) メッシュに `CutMeshByPlane` を当てており、sub-01 で確定した前提「閉じた外向きの入力」を破っていた (`FractureBake.cpp:529-551` の `prefix = ClipMeshBySinglePlane(...)`)。断面ループが閉じず蓋が落ち、体積が不足していた。元メッシュは閉じているので、1 回切りなら前提を満たす
  - §4.1 焼き 3: 破片の仕上げに「微小距離の決定的な溶接 + T 字接合の分割」を追加 (外側面と蓋の境界点は丸めで数 ulp ずれ、蓋どうしの継ぎ目 (Voronoi 辺) には T 字接合が必ず出るため)
  - §4.1 焼き 5: 統合の平均を「統合のたびに数え直す」と明記 (coder の解釈を採用)
- 2026-09-25 (sub-02 VERDICT round 2): 破片の「閉じ」を**位相的 (溶接後に辺が 2 回ずつ) から幾何的 (ベクトル面積の和 ≈ 0 と体積保存) へ緩めた**。round 1 で入れた「微小距離の溶接 + T 字接合の分割」は廃止。理由: round 2 の実装では、溶接で潰れたスライバーや、ε 以内にある別シートの頂点で T 字分割が誤爆し、3 面が共有する辺が残った (`FractureBake.cpp:819-935` の `SplitTJunctions` は、境界頂点を ε 距離だけで辺へ挿入していて、どの面の頂点かを見ていない)。トーラスで終わらないのも、同じ関数が 1 パスにつき 1 三角形 1 辺ずつ、全体の溶接とソートを繰り返すためと見ている。v1 の下流 (描画・凸包・体積・重心・接着面積) は位相的な閉じを必要としない。必要なのは実行中の再分割 (B) だけなので後回しに回した。見た目の継ぎ目 (丸め程度の隙間) は sub-06 のスクショで確認する。受け入れ条件 3 の文言も変更した
- 2026-09-25 (sub-02 VERDICT round 3、差し戻し上限): sub-02 を箱・L 字で OK とし、トーラス分を**新サブ sub-13** へ切り出した。原因は既存の `ConvexHull.cpp:219-307` にある。地平線の新面が縮退すると `continue` するが、コメントに反して pick が `outside` に残るため、同じ点を永遠に選び続ける (無限ループ)。修正前にこの経路へ入った入力は必ず止まらなかったので、修正は既存の終了する入力に対してビット一致になる。sub-06 は sub-13 に依存させた (実メッシュの焼きで止まらないことが前提のため)
- 2026-09-25 (sub-04 VERDICT round 1): §4.1 焼き 1 の surface nets を具体化した (セルごとに 1 頂点・辺の中点の平均・曖昧な配置の事前解消)。round 1 の実装は占有境界の立方体の面をそのまま出すブロック状の抽出で、surface nets ではなかった。同じ平面上の大きな面が大量にでき、断面の輪郭が数千点の共線点の列になり、`EarClip` が失敗していた。§4.2 の `voxelResolution` の既定値を「sub-04 の計測で決める」に変更。`EarClip` の共線点の扱い (面積 0 の耳として外す) を sub-04 の範囲に加えた (sub-01 の部品だが、ボクセル化経路でしか顕在化しないため)。[ユーザーに聞ける] #10 を追加
- 2026-09-25 (sub-04 VERDICT round 2): sub-04 を OK にした (surface nets、解像度 32 の焼き成功)。開いた箱の解像度 48 / 64 の焼き失敗と、耳切りの遅さ (O(n³)) は**新サブ sub-14** に切り出した。断面の三角形分割を libtess2 の掃引法に置き換える。理由: 耳切りの失敗が sub-01 / 02 / 04 と同じ種類で 3 回出た。単純多角形の前提は、薄い壁の断面 (輪郭の接触) で原理的に破れる。`voxelResolution` の既定値を 32 にした (sub-04 の実測: 開いた箱、pieceCount 16 で Release 5.3 秒)。sub-09 を sub-14 に依存させた (Inspector で解像度を上げた利用者が失敗しないように)。[ユーザーに聞ける] #11 を追加
- 2026-09-25 (sub-14 VERDICT round 1): 断面の三角形分割を libtess2 (master @ 8dbd648、TESS_WINDING_ODD) に置き換えた。輪郭が縮退した入力 (新しい頂点・頂点の統合・三角形数がオイラーの公式と合わない) では、sub-01 の厳密な閉じの代わりに幾何的な閉じで判定する。既定の `voxelResolution` は 32 のまま (sub-14 の実測で 48 は Release 10.8 秒)
- 2026-09-25 (sub-05 VERDICT round 1): 受け入れ条件 10 の shot_verify に、「基点を WIP 抜きの worktree でビルドして同じ結果の枚は除外」を追加。[ユーザーに聞ける] #12 を追加。プロファイルスコープ (sub-05 のやること 4、任意) は sub-11 へ回す
