# M80 破壊物理 — 設計の叩き台 (司会の事前調査)

> planner の出発点。裁定で変えてよい。作成 2026-09-25。
> ユーザー確定済みの要件は「要件」節。それ以外は叩き台。

## 要件 (ユーザー確定済み)
ユーザーが「UE にあるような、物を壊せる物理」を依頼した。相談の結果、次の要件で確定した (2026-09-25):
- 方式は A (事前の Voronoi 分割 + 接着グラフ + クラスタ剛体)。B (実行中の分割) へ拡張できる形にする
- 割れた後の挙動はインスペクターで選ぶ: 残す / N 秒後に消える / 沈んで消える / 縮んで消える / 静止したら静的化 / 上限超過で古い順に消す (この選択肢で足りるとユーザー確認済み)
- 破片の数はインスペクターで指定する (目安の数)
- 複雑な素材まで対象にする。**スキンメッシュも含める**
- 閉じていないメッシュの扱いはインスペクターで選ぶ: 「エラー表示して拒否」(**既定**) / 「ボクセル化を許容」
- 破片エンティティは**最初から作っておく**方式。「壊れた瞬間に生成」を後で足せるよう拡張点を残す
- ゲームロジック (DLL / C#) から破壊イベントと破壊 API を使えるようにする (ABI 変更あり)

マイルストーン規模 (10 サブ以上) なので、メモの運用どおり `/harness` に載せる。司会の事前調査は `plans/m80-destruction/design-draft.md` として planner の出発点に渡す。


### 調査で確かめた事実
- **物理**は tick ごとに作り直すステートレス型。永続状態はコンポーネントのフィールドに置く (ハッシュとスナップショットは自動で被覆)。body の順序は entity.index 昇順。構造変更は tick 末のコマンドバッファ経由。イテレーション中にアーキタイプを新規作成すると `MYE_CHECK` で落ちる (`src/Engine/Core/World.cpp:275`)。
- **複合コライダー**: `kMaxCompoundShapes=16` はスタック配列の大きさで、17 個目以降の子形状は「当たるのに慣性へ入らない」黙った不整合になる (`PhysicsSystem.cpp:1617-1645`)。接触は 1 ボディペア 1 件に畳まれ、どの子形状 (破片) が叩かれたかが分からない (`:4348-4389`)。
- **凸包**: 頂点は 64 まで。`BuildConvexHull` / `ConvexMassProperties` / `ConvexColliderLibrary::Register` (`src/Engine/Engine/Physics/ConvexHull.h`、`ConvexColliderLibrary.h:41`)。初回は tick 内で遅延生成される → 破片は事前にクックまたは登録する。
- **破断の型**: 関節の breakImpulse を集計し、tick 末に判定して `broken` フラグを立てる (`PhysicsSystem.cpp:3911-3928, 4483-4517`)。応力の入力として `SolidContact.impulse` が破壊用に予約済み (`PhysicsSystem.h:22-35`)。
- **メッシュ**: CPU 側の `Mesh` には positions / indices / normals / uvs だけが残り、**ボーンウェイトは残らない** (ウェイト込みの頂点はクック blob `.mmdl` の `CookedMesh.vertices` にだけある)。CPU スキニング関数は無い。ボーンパレットは CPU で計算している (`src/Engine/Renderer/Skeleton.h`)。
- **閉じているかの判定は無い**。ボクセル→メッシュ化も無い。`Voxelizer` は 32³ 固定で Deep-Modal と契約を共有しているので、流用せず別関数にする。
- **描画**: 同じ mesh・同じ material が続くときだけインスタンシングされる。RT は参照メッシュの集合が変わると連結 BLAS を全部焼き直す (`RtScene.cpp:64-99`)。
- **エディタ**: Reflection の自動 UI (enum 型は無く Int32 で代用) と `DrawComponentNotes` の手書き付記。非同期ベイクの前例は ModalSoundLibrary (worker + 状態列挙 + ボタン無効化)。数値の進捗バーは前例なし。
- **ABI**: 現在 **v21 / 125 スロット** (`src/Shared/EngineAPI.h:37`、`tools/check_rules.ps1:676`)。イベントは `MyeScriptDesc` の末尾へ追加する (`src/Shared/ScriptTypes.h:49-67`)。C# は位置ミラー (`src/Scripting/Interop.cs`)。規則 11 で機械照合。ABI はマイルストーンの最後に 1 回だけ上げる運用 (`docs/history/api-scripting-tools.md:14`)。
- **番号**: 次は M80。空いている TypeId は 64 から。旧ロードマップの破壊計画は `plans/greedy-cooking-wave.md:606-607`。
- **UE の方式**: Geometry Collection (破片とクラスタ階層)、Connection Graph と Damage Threshold、断面の内部マテリアル、Remove on Sleep / Break、`OnChaosBreakEvent`。入力は water tight (閉じていること) が必須と明記されている。Unity 本体には機能が無く、RayFire が Prefragment / Connected Cluster / Skinned 対応の方式。

### 方式 (推奨)
1. **分割コア (Engine 層の純関数)**: 入力はメッシュ + seed + 破片数の目安。出力は破片メッシュ、凸包、接着グラフ。
   - seed は `Pcg32`。浮動小数点の演算順を固定し、並列化はしない (B で実行時に使うため、ビット一致させる)。
   - 処理: 閉じているかの判定 (位置で溶接 → 境界辺と非多様体辺を数える) → Voronoi 平面でセルごとに切る → 切り口に蓋を作る (内部マテリアル枠、box 投影の UV) → 極小片は隣へ統合 → 破片ごとに凸包 (凹みが大きい片は小さく分けるかを planner が裁定)。
   - 閉じていない入力: 既定は拒否 (理由をインスペクターに赤字で表示)。「ボクセル化を許容」なら、解像度可変のボクセライザ + surface nets で閉じたメッシュを作ってから切る。
2. **破片アセット** (例 `.mfrac`): 破片メッシュ、凸包、接着グラフ (隣接、接触面積、強度)、スキン情報を持つ。assets 側に置いて git で共有する (cache だけだと別 PC で作り直しになり、封印配布で書けない)。ソース (設定) と焼き結果の二本立ては地形 (`.terrain.json` / `.mterr`) と同じ形。
3. **コンポーネント** (TypeId 64〜): `Destructible` (ソースメッシュ、破片数、seed、閉じていないときの扱い、割れた後の挙動と時間 (Tick 数)、上限数、強度) と、破片ごとの状態 (接着の切断ビットマスク、損傷の蓄積、割れた後の経過 Tick)。
   - 接着の状態は、破片ごとに固定長のビットマスクで持つ (隣接数の上限を資産側で保証する)。こうすれば ECS 外のプールと、ハッシュ / スナップショット / プールの 3 点セットが要らない。
4. **実行時**: 壊れる前はルートの剛体 1 つに `compoundColliders` で破片を束ねる。見た目は元のメッシュ 1 つで描き、破片は非表示 (UE の root proxy 相当)。
   - 接触インパルスを破片に配分する (SolidContact に子形状の index を足す。**既存シーンのビット一致は保つ**)。
   - tick 末に閾値を超えた接着を切り、連結成分を破片 index 順に再計算し、新しいクラスタへ付け替える (コマンドバッファ経由、アーキタイプは事前に用意しておく)。
   - `kMaxCompoundShapes` の上限を外す (16 以下の既存経路はビット一致のまま)。
   - 「壊れた瞬間に生成」は、破片を供給する層 (事前生成 / 生成時) を差し替えられる形にしておくことで拡張点とする。
5. **スキンメッシュ**: 焼きはバインドポーズで行い、ウェイトは `.mmdl` から読む。各破片を支配的なボーンへ割り当てる。壊れる前は元のスキンメッシュを描き、破片は非表示のままボーンに追従させる (既存の PartFollow を流用)。壊れた瞬間に、そのポーズのまま剛体化する。実行中のポーズで切り直す処理は B に回す。
6. **割れた後の挙動**: 上の 6 種類を Tick 数で処理する (実時間は使わない)。消すときは tick 末に Destroy する。
7. **ABI v22**: `MyeScriptDesc` の末尾に `onBreak` を追加し、API スロットとして ApplyDamage / 破壊状態の取得などを追加する。C# のミラーと `check_rules.ps1` の表を同時に更新する。サブの最後で 1 回だけ上げる。
8. **計測**: 物理の内部にプロファイルスコープを足し、破片 N 個のベンチシーンを作る。60 Hz の Tick 予算に収まる上限を実測で決め、インスペクターの上限値に反映する。

### 想定サブ (planner が再分割してよい)
- a 閉じているかの判定 + 分割コア (静的メッシュ) + SelfTest (同じ seed で同じバイト列、閉じていない入力を拒否)
- b 切り口の蓋、UV、内部マテリアル + 破片アセットの形式と保存
- c 可変解像度ボクセル化 + surface nets (「ボクセル化を許容」の経路)
- d コンポーネント + インスペクター (非同期ベイク、状態表示、エラー表示) + Undo / プレハブ
- e 複合コライダーの上限撤廃 + 子形状単位の接触インパルス (既存のビット一致を確認)
- f 接着グラフの破断 + クラスタ分離 (tick 末) + replay
- g 割れた後の 6 挙動
- h スキンメッシュ (ウェイトの読み出し、ボーン割り当て、追従から剛体化へ)
- i 計測、ベンチ、上限値の決定、RT と描画のスパイク対策
- j ABI v22 (onBreak、API、C# ミラー) + デモシーン + engine_spec と ADR の更新

## 主要ファイル
- 物理: `src/Engine/Engine/Physics/PhysicsSystem.cpp`, `PhysicsSystem.h`, `ConvexHull.h`, `ConvexColliderLibrary.*`
- ECS: `src/Engine/Core/Components.h/.cpp`, `src/Engine/Engine/TickRunner.cpp`
- メッシュ: `src/Engine/Renderer/GpuResources.h`, `src/Engine/Engine/Asset/ModelCook.h`, `src/Engine/Renderer/Skeleton.h`, `src/Engine/Engine/PartFollowSystem.cpp`
- エディタ: `src/Editor/Windows/InspectorWindow.cpp`, `src/Editor/EditorComponentCatalog.cpp`, `src/Engine/Core/LocalizationTable.inl`
- ABI: `src/Shared/EngineAPI.h`, `src/Shared/ScriptTypes.h`, `src/Scripting/Interop.cs`, `src/Engine/Engine/Script/EngineApiTable.cpp`, `tools/check_rules.ps1`
- 新規: 分割コア (`src/Engine/Engine/Physics/Fracture*` 想定)、`plans/m80-destruction/`

## 検証
- サブごと: `Editor.exe --selftest` (分割の決定性、閉じているかの判定、接着の破断、クラスタ分離、ABI)、`tools\check_rules.ps1`
- 物理に触るサブ: 既存の `--physics-demo` / `--joint-demo` の replay がビット一致すること
- 全体: 破壊デモシーンで `tools\replay_verify.bat` (Debug / Release 一致)、`--screenshot` で割れる前後の画、ベンチの計測値
- 最後に harness-reviewer の 4 軸レビュー
