# ADR-027: 骨アニメのポーズはポーズプログラムに一本化し、純関数で評価する

- 状態: **確定** (2026-10-08、M89a。後続サブで層の書き手と尾部を足していく)
- 出所: M89 骨アニメの深化。計画は `plans\m89-skeletal-animation.md`。
- 実体: `src\Engine\Core\Ecs\Components.h` の `SkinnedMeshComponent` 末尾 (`poseLayerCount` / `poseClaim` / `poseLayers`)、
  `src\Engine\Renderer\Mesh\Skeleton.{h,cpp}` の `ComputeJointLocalsLayered`、
  `src\Engine\Engine\Animation\SkinningSystem.{h,cpp}` の claim 手順・`UsesLocalsPath`・`SamePoseInputs`・`SampleSkinnedLocals`。
  検証は `SkeletonSelfTest` の (10)。
- 番号: `kSimSnapshotVersion` 40 (M89d の決定 6 で 42)。ABI と TypeId は変更なし。

## 背景

骨アニメ (SkinnedMesh + SkinningSystem) はクリップ 1 本とクロスフェードしか表せず、ステートマシン (AnimatorController) と
BT / ABI からは骨クリップを切り替えられなかった。M89 ではコントローラが骨クリップも担当し (Unity 方式)、
ブレンドツリー・ルートモーション・IK を積む。描画・部位追従・ラグドールの 3 者は同じポーズを見なければならない
(どれか 1 つだけ混ぜ方を知らないと、骨に付けた部位やラグドールの未駆動の骨だけが飛ぶ)。

## 決定 1: ポーズの入力を「層の列」として sim 状態に置き、ポーズ自体は導出値にする

- `SkinnedMeshComponent` の末尾に最大 8 層の `{clip, timeQ (1/256 tick), weightQ (Q16)}` を持つ。値はすべて整数。
- ポーズ (局所行列) は層の列とモデルだけの純関数 `ComputeJointLocalsLayered` で毎回作る。前 tick のポーズを持たない
  (慣性補間・ばねは入れない)。スナップショットから戻しても、同じ入力から同じポーズになる。
- フィールド登録はしない (シーンに保存しない・Inspector に出さない)。毎 tick 書き直される値なので保存すると古い値が残る。
  コンポーネントは NoHash のままなのでハッシュは変わらず、生バイトがスナップショットに載るので版だけ上げた。
- 却下: ブレンド済みのポーズ (行列) を状態として持つ。状態が大きく、スナップショットとハッシュの対象が float の塊になる。

## 決定 2: 書き手は 1 か所、旧経路は claim で切り替える

- 層を書くのはコントローラ (と後続の IK の解決段) だけ。書いた tick には `poseClaim` を立てる。
- SkinningSystem は claim が立っていれば旧経路の時計を進めずに印を下ろす。立っていなければ `poseLayerCount` を 0 に戻して
  旧経路 (clip / timeTicks / クロスフェード) を進める。書き手が居なくなった次の tick に自動で旧経路へ戻る。
- 駆動中は `SkinnedMesh.clip` への直書きを無視する。SkinningSystem が `observedClip` を `clip` に揃えるので、
  駆動を外した後に遅れて切り替えとして効くこともない。
- `poseLayerCount = 0` の旧経路は 1 命令も変えない。描画は `UsesLocalsPath` が偽なら従来どおり `ComputeBonePalette` を直接呼ぶ。

## 決定 3: 混ぜ方は「先頭から順に畳む」、1 層は旧関数へ委ねる

- 層 i は、ここまでの結果と比 `w_i / (w_0 + ... + w_i)` で混ぜる (T / S は線形、R は slerp)。比は整数の和から作る。
  2 層なら `ComputeJointLocalsBlended` と同じ演算列になる (selftest で偏差 0 を確認)。
- 重みが正の層が 1 枚だけなら `ComputeJointLocals(clip, timeSec)` をそのまま呼ぶ。秒は `timeQ / (60 * 256)` で、
  `timeQ` が 256 の倍数なら旧経路の `timeTicks / 60.0f` とビット一致する (分子と分母に同じ 2 の冪を掛けた商は丸めが変わらない。
  `|timeQ| < 2^24` の範囲)。単一クリップのステートは旧経路と同じ絵になる。
- 却下: 全層の四元数を重み付きで足して正規化する (nlerp 和)。順序に依らない利点はあるが、2 層で既存のクロスフェードと
  一致しなくなり、決定 3 のビット一致の足場も失う。層の並びは書き手が決定的に決めるので、順序依存は問題にならない。

## 決定 4: 「同じポーズか」の判定も 1 か所に置く

部位追従のポーズキャッシュのキーは `SamePoseInputs` に任せる。キャッシュ側で欄を並べると、ポーズの入力を足したときに
キーだけが古いまま残り、別のポーズを使い回す。ポーズの入力を足すときは `SampleSkinnedLocals` と `SamePoseInputs` を一緒に直す。

## 決定 5: コントローラは骨クリップを名前で引き、部分木の SkinnedMesh をすべて駆動する (M89b)

- ステートの骨クリップは `.controller.json` v2 の `"skel":{"clip":"Walk"}`。SkinnedModel のクリップ名で持ち、
  SkinnedMesh ごとに `SkinnedModel::FindClipByHash` で index を引く。同じコントローラを、クリップの並びが違うモデルにも使える。
  見つからないモデルの層は `clip = -1` (バインドポーズ) で、層の数と重みは他のメッシュとそろえる。
- 駆動対象はコントローラの部分木の SkinnedMesh すべて (前順)。別の AnimatorController を持つ子孫の部分木は含めない。
- ステートの長さ (ループ・hasExitTime・BT の waitForEnd) は **主 SkinnedMesh** (駆動対象のうち entity index 最小) のモデルの
  骨クリップで決める (`ControllerStateLengthTicks`、`SkeletalClipTicks`)。走査順でなく index で選ぶのは、兄弟の並べ替えで
  ステートの長さが変わらないようにするため。骨クリップの長さが引けなければプロパティクリップの長さへ落ちる。
- プログラムは時刻を進めた後の値で書く (旧経路の「進めてから描画が読む」と同じ、その tick の終わりの姿勢)。
  遷移中は元 → 先の 2 層で、先の重みは `transitionTick * 65536 / duration` (切り捨て)、残りが元。片側だけが骨クリップを
  持つならそちらを重み満杯で出す。どちらも持たない tick は書かない = 旧経路へ戻る。
- コントローラの entity が非アクティブの間は、プログラムを持つメッシュの claim だけを立てて凍らせる。
- 既知の制約: ステートの長さが骨クリップ (= モデルの読み込み) に依存するので、モデルを読まない構成ではステートの時刻が
  進まない。プロパティクリップ (AnimationLibrary) と同じ依存で、replay_verify の `anim` ジョブ (Server.exe を含む) で
  全構成が同じ長さを読めることを確かめている。

## 決定 6: ブレンドツリーの子は 1 本の整数位相を共有する (M89d)

- ブレンドツリーのステートの再生位置は時刻 (tick) ではなく `uint32` の位相 (1 周 = 2^32)。`AnimatorControllerComponent` の
  末尾の `statePhase` / `transitionToPhase` に置く (フィールド登録 = 保存・ハッシュ対象、`kSimSnapshotVersion` 42)。
  子の時刻は `timeQ = (phase · L_i · 256) >> 32` で、長さの違うクリップ (歩き 60 tick と走り 30 tick) が同じ周期でそろう
  (足の接地がずれない)。`L_i` は各 SkinnedMesh が自分のモデルで引く。
- 1 tick の進みは `Δ = speed · 2^48 / Σ(wQ_i · L_i)` (int64、L_i は主 SkinnedMesh のモデル)。混ぜた周期の長さに反比例する
  ので、パラメータが動いても位相は飛ばない。ループは 2^32 で折り返し、非ループは 0..UINT32_MAX に張り付く
  (張り付いた位相は各子の末尾ちょうど)。
- 子の重みは float で 1 回だけ計算して Q16 へ切り捨て、端数を最大重みの子 (同値なら index の小さい子) に足す。
  遷移中は ステートの重み × 子の重み を Q16 で切り捨て、端数を最大重みの層に足す。どちらも和はちょうど 65536。
- 却下: 子ごとに別の時刻を持つ。状態が子の数だけ増え、長さの違う子の周期がずれる。
- 却下: 位相を float (0..1) で持つ。進みの累積で丸め誤差が溜まり、ハッシュとスナップショットに float が入る。

## 決定 7: 2D ブレンドツリーは Freeform Cartesian (gradient band) で、上位 4 本に絞る (M89e)

- `"skel":{"blend2d":{"paramX":i,"paramY":j,"children":[{"clip","x","y"}]}}`。子 i の影響は
  `h_i = min_j (1 - (p - p_i)·(p_j - p_i) / |p_j - p_i|^2)` を 0 以上に切ったもの (Unity の 2D Freeform Cartesian と同じ式)。
  h の大きい上位 4 本 (同値なら index の小さい子) を h の比で混ぜ、決定 6 と同じ Q16 化で和をちょうど 65536 にする。
  全体の Σh で割ってから選び直すのと、上位 4 本の比を取るのは同じ値なので、作業領域は 4 本ぶんで固定 (tick ごとの確保が無い)。
- 先の子と同じ位置の子は使わない (|p_j - p_i| = 0 で式が割れないように)。x か y が有限でなければ index 0 の子が満杯。
  有限の入力では最寄りの子の h が 1/2 以上になるので、全員 0 にはならない。
- 位相・長さ・層の書き方は決定 6 をそのまま使う (`ComputeBlendWeights` が 2D でも同じ形の重みを返すだけ)。
  コンポーネントの並びは変わらないので `kSimSnapshotVersion` は上げない。
- 却下: Simple Directional / Freeform Directional。方向と速さを分けて扱う利点はあるが、角度の計算 (atan2) が要り、
  子の配置の制約も増える。必要になったら `ControllerBlendType` に足す。
- 却下: 混ぜる本数を 8 本 (層の上限) まで許す。遷移の元と先が両方 2D のとき 16 層になり、ポーズプログラムに収まらない。

## 決定 8: 描画補間は層ごとの「前 tick の時刻 + 進み」で、描画だけが読む (M89f)

- 層 (`PoseLayer`) に `prevTimeQ` (前 tick のその層の時刻) と `stepQ` (この tick の進み、折り返す前) を足す。
  描画は `prevTimeQ + stepQ * interpAlpha` の時刻で引き (`SampleSkinnedLocalsInterpolated`)、alpha = 1 は `timeQ` そのもの。
  決定的撮影・編集中・記録/再生中は EngineLoop が alpha = 1 にするので、golden とリプレイは変わらない。
- ループの折り返しでは `prevTimeQ + stepQ` がクリップの末尾を越える。サンプラが末尾のコマでクランプするので、
  前 tick の姿勢 → 末尾 (= ループのクリップでは先頭と同じ姿勢) と滑らかに繋がる。ブレンドツリーの子は時刻の差を
  位相の進む向きで 1 周ぶん足し戻す (alpha = 1 の手前でちょうど `timeQ` に届く)。
- 書き手はコントローラだけ (時刻を進める前の値と進みを `StateClock` に残す)。遷移が終わる tick は先の時計を引き継ぐ。
  凍結中 (非アクティブ) は `stepQ = 0` に落とす — 残すと最後の tick の動きを毎 tick 繰り返して描く。
- sim・部位追従・ラグドールは読まない。`SamePoseInputs` も見ない (ポーズの入力ではない)。部位は tick 境界の姿勢に付くので、
  補間中の骨と最大 1 tick ぶんずれる (部位のワールド行列は M36b の行列補間で別に補間される)。
- 補間するのは時刻だけで、重み (遷移・ブレンド) は補間しない。旧経路 (`poseLayerCount = 0`) も補間しない (1 命令も変えない原則)。
- 層の大きさが変わるので `kSimSnapshotVersion` を 43 に上げる。欄は生バイトで載るので、snapshot から戻した直後の描画も途切れない。
- 却下: 描画側で前 tick のポーズ (行列) を覚えて行列を補間する。前 tick のポーズに依存する処理を持たない原則 (背景) に反し、
  エンティティごとの履歴の寿命 (生成・破棄・snapshot) を別に管理することになる。
- 却下: 前 tick の層の並びをまるごと持つ (層 16 本)。遷移の始まりで層が増減しても、層ごとに前の時刻を持てば足りる。

## 決定 9: アニメイベントは「時計が 1 tick に通った区間」で判定し、入った tick だけを sim 状態で覚える (M89h)

- 定義はコントローラの `clipEvents` (骨クリップの名前がキー、位置は tick)。モデルごとにクリップの index が違ってよい (決定 5 と同じ)。
- 判定は純関数 `ClipEventPassDistance` 1 本。区間は old を含まず old + step を含む (順再生 `(old, new]`、逆再生 `[new, old)`)。
  ループは cycle を法とし (末尾ちょうど = 先頭 = 1 周に 1 回)、1 tick で 1 周以上なら全位置を 1 回ずつ。非ループは端に張り付くので終端は 1 回。
  返り値の「old からの距離」で層の中の発火順を決める (同じ位置はイベントの並び順)。
- 入った tick は old (= 0) も含める。これは時刻だけからは決まらない — ループが 0 ちょうどへ折り返した次の tick と、
  入った tick はどちらも「0 から進む」。そこで `AnimatorControllerComponent::stateEntered` (末尾 append) を足し、
  最初の tick と `AnimatorPlay` の即切り替えで 1、進めたら 0 にする。遷移先は `transitionTick == 0` で分かるので使わない。
  コンポーネントの並びが変わるので `kSimSnapshotVersion` を 44 に上げる (計画では j = v44、l = v45 だったものが 1 つずつずれる)。
- 層と重みはポーズプログラムと同じ考え方: 今のステート (遷移中は元 → 先)。ブレンドツリーは最大重みの子 1 本だけを位相で判定する
  (歩きと走りの足音が二重に鳴らない)。重みは遷移の `transitionTick / duration` × 子の重み。重み 0 の層 (遷移を終える tick の元) と
  `minWeight` 未満は発火しない。長さは主 SkinnedMesh のモデルで引く (ステートの長さと同じ。モデルが無ければ発火しない)。
- 出口は `AnimatorControllerSystem::FiredEvents()` (sim 状態ではない作業領域)。TickRunner が同じ tick に
  `BehaviorTreeSystem::SendEvent` へ自分宛てで積み、次の tick の冒頭で配られる (`BtEventCount` / `BtGetEvent` / BB の `eventName`)。
  AnimatorController は BT を知らない。エンジンが直接処理する種類 (sound / effect / noise) は M89i で同じ出口に足す。
- 却下: 時刻 0 から進む tick を常に「入った tick」とみなす (状態を増やさない)。ループが 0 ちょうどへ折り返す速さ (speed 1 なら毎周) で
  位置 0 のイベントが 2 回鳴る。
- 却下: 区間を `[old, new)` にして入った tick を自然に含める。発火がポーズ (進めた後の時刻) より 1 tick 遅れ、非ループの終端を別扱いにする必要がある。
