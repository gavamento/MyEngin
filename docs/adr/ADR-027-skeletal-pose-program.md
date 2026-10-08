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

## 決定 10: エンジンが直接処理するイベントは既存の出口へ積み、位置は「ジョイント × 前 tick の WorldMatrix」で求める (M89i)

- イベントに `kind` (`script` / `sound` / `effect` / `noise`) を持たせる。省略は `script` (M89h の定義はそのまま読める)。未知の kind は警告して読まない。
  種類別の欄は `sound` (+ `volume` / `pitch`)、`prefab`、`loudness` / `range`。どの種類も `joint` (主 SkinnedMesh のモデルのジョイント名) で位置を指定できる。
- 位置はプログラムを書いた後 (Update の最後) に、`JointGlobalFromLocals(今 tick のプログラムのポーズ) × 主 SkinnedMesh の WorldMatrix` の平行移動で求める
  (部位ソケットと同じ式)。`joint` が空・見つからなければコントローラのエンティティの WorldMatrix の平行移動。WorldMatrix は TransformSystem の前に読むので
  前 tick の確定値 (エンティティの移動ぶん 1 tick 遅れる。足音・土煙の用途では許容する)。
- 振り分けは TickRunner が `FiredEvents()` の順に行う。AnimatorController は BT・オーディオ・知覚を知らない (決定 9 と同じ)。
  - `script`: BT イベント。`vec` に発火位置を入れる (M89h では 0 だった)。
  - `sound`: `ScriptAudioOp::PlayAtPoint` を出力レーン (ハッシュ後に drain) へ。`ReserveAudioHandle` は使わず handle 0 (= タグ無し)。
    スクリプトの再生ハンドルの採番列を変えないため。止める口は要らない (ワンショット)。
  - `effect`: `EffectSpawnRequest` (親なし、ワールド位置) を tick 末の生成へ。sim 状態なのでハッシュに入る。
  - `noise`: `PerceptionReportNoise` を直接呼ぶ (出した者 = コントローラのエンティティ)。聞き手の保留欄に書かれ、次の tick の知覚で消費される。
- 却下: ポーズを TransformSystem の後で引き直して今 tick の WorldMatrix を使う。発火 (フェーズ 3.5) と配る場所を分けることになり、BT のイベントだけ別の tick に積む順序の問題が出る。
- 却下: 種類別の欄を `value` / `intValue` に詰め込む。意味がアセットごとに変わり、読めないデータになる。
## 決定 11: ルートモーションはプログラムと同じ層から速度を測り、適用先を 1 つ選んで毎 tick 書く (M89j)

- 測るジョイントは主 SkinnedMesh のモデルの「親を持たない最初のジョイント」(`FindRootJoint`)。名前の指定は持たない。
  FBX は非ジョイントの祖先もジョイントに含める (M48a) ので、動かない祖先が選ばれて速度は 0 になる (既知の制約)。
- この tick の移動は、ポーズプログラムと同じ層・時計・重み (`BuildSkeletalSources` の結果) で、層ごとに
  `T(今の時刻) − T(進める前の時刻)` をサンプルし、ループの折り返しには周回数 × `T(末尾) − T(先頭)` を足して重みで混ぜる。
  周回数は単一クリップなら `floor((prevTime + timeStep) / L)`、ブレンドツリーの子なら位相の `floor((prevPhase + phaseStep) / 2^32)`。
  プログラムの `stepQ` (描画用) は読まない。
- ワールドへは主 SkinnedMesh の LocalTransform の連鎖 (回転の積と、軸ごとの拡大の積。物理の `ComposeParentFrame` と同じ近似) で回す。
  WorldMatrix は前 tick の値なので使わない。y を捨て、× 60 を `rootMotionVelocity` (hash 対象) に書く。`applyRootMotion` に関わらず毎 tick 書く
  (スクリプトが自分で動かすときに読める。Unity の deltaPosition に当たる)。
- 適用先は 1 つ: NavMeshAgent が `updatePosition` で位置を握っている → 何もしない / 非 kinematic の Rigidbody → 水平速度 (縦は物理) /
  CharacterController → `moveInput` / それ以外 (kinematic の Rigidbody を含む) → LocalTransform.position (親空間へ共役と割り算で戻す)。
  物理より前 (フェーズ 3.5) に書くので、同じ tick の物理が使う。適用中は骨を駆動しない tick も 0 を書く — `moveInput` は保持される値なので、
  書かない tick があると最後の速度で滑り続ける。
- ポーズからは、駆動する各 SkinnedMesh のプログラムの `poseRootJoint` / `poseRootUp` (末尾 append、snapshot v45) を見て
  `SampleSkinnedLocals` が水平分を抜く。抜く量は層ごとの `T(サンプル時刻) − T(クリップの先頭)` を重みの比で混ぜたもの
  (時刻 0 の姿勢は変わらず、1 周の間にルートが水平へずれない)。上はメッシュごとに、そのエンティティの連鎖の回転の共役で
  ワールドの上を戻して求める (Z-up の glTF は Z_UP ノードがエンティティ側に載る。b の下調べ)。縦の動き (弾み) は残す。
  描画補間の時刻でも同じ式で抜くので、前進中に体が前後に震えない。部位追従のポーズキャッシュのキー (`SamePoseInputs`) にも入れる。
- 却下: 前 tick のポーズからの差分で測る (前 tick のポーズに依存する処理は入れない原則に反し、スナップショットから戻した直後に 1 tick 狂う)。
- 却下: 抜く基準をバインドポーズにする。先頭でルートがずれているクリップ (攻撃の踏み込みなど) が入った瞬間に飛ぶ。
## 決定 12: ルートモーションのヨーは「クリップの先頭からのひねり」を半角の (cos, sin) で測り、移動は回したぶん戻して表す (M89k)

- ヨーはルートジョイントの回転の、クリップの先頭からの変化 `R(t)·R(0)⁻¹` のうち、ルートの親空間の上まわりのひねり
  (swing-twist の twist) とする (`SampleJointYaw`)。値は四元数の半角 `(cos(θ/2), sin(θ/2))` のまま持ち、sqrt と四則だけで扱う
  (同じ軸まわりなので積は複素数の積で可換)。回らないクリップは `(1, 0)` ちょうどになり、M89j の結果をビットも変えない。
- この tick のヨーは層ごとに `Z(終わり)·Z(始め)⁻¹·Z(末尾)^周回数` を求め、`cos >= 0` にそろえて重みで足して正規化する (nlerp)。
  ワールドへは主 SkinnedMesh の連鎖の回転が上をワールドの上へ移すので角度はそのまま Y 軸まわりになる (拡大の積が負なら逆回り)。
  `rootMotionDeltaRotation` (hash 対象、末尾 append、snapshot v46) に毎 tick 書く。
- エンティティを回すのは `applyRootMotion` で、NavMeshAgent が `updateRotation` で向きを握っていないときだけ (`RootYawFollows`)。
  回すときは LocalTransform.rotation に親空間の上まわりで掛け、駆動する各 SkinnedMesh の `poseRootYaw` (末尾 append) を立てて
  `SampleSkinnedLocals` がひねりを抜く (ジョイントの位置で回し、平行移動の行は変えない)。Rigidbody も角速度ではなく回転を直接回す
  (角速度にするには角度 = atan2 が要る)。
- 回すときは、クリップの座標での移動を区間の頭までに回ったぶん (`Z(始め)⁻¹`) 戻して今のエンティティの向きで表す。
  エンティティの向きは「周の頭の向き · Z(t)」なので、これで 1 周の中のワールドの通り道がクリップのルートの通り道に一致し、
  曲がりながら歩くクリップで移動が二重に回らない。折り返しをまたぐ tick は `Σ_{k<n} z^k·C + z^n·(終わり − 先頭) − (始め − 先頭)`
  (C = 1 周の移動、z = 1 周のヨー) で、和は倍々で求める (1 tick で何周しても反復は高々 64 回)。回さないとき (Nav が握っている、
  `applyRootMotion` が切れている) はポーズがひねりを残すので、移動もクリップの座標のまま出す。
- 却下: 角度 (ラジアン) で持って重みで混ぜる。角度にするには atan2 が要り、CRT の超越関数は CPU の機能で経路が変わりうる。
- 却下: ブレンド後の回転 (slerp の連鎖) からひねりを取る。sim 側は層ごとに測っているので、ポーズ側も層ごとに混ぜてそろえる
  (差は周ごとに積もらない: ポーズで抜く量はクリップの先頭からの値で、毎 tick 測り直す)。
## 決定 13: 2 ボーン IK は解決段が目標をメッシュの空間へ直してポーズ入力に書き、SampleSkinnedLocals の最後で解く (M89l)

- `TwoBoneIKComponent` (TypeId 80) は SkinnedMesh と同じエンティティに付け、鎖を 4 本まで持つ。先端は名前で指し、中間・根は親を辿る
  (腕・脚の 3 関節を名前 3 つで指させない)。目標はエンティティ (そのローカル座標の点と回転) か、無ければワールドの値。
  `poleHint` はキャラ自身のローカル座標の点で、(0, 0, 0) は今のポーズの曲げ面のまま (ワールドの pole はキャラが向きを変えると追従しない)。
- `TwoBoneIkSystem` が SkinningSystem の後・PartFollowSystem の前に、目標を LocalTransform の連鎖 (この tick の値) で組んだ行列で
  メッシュの空間へ直し、SkinnedMesh の末尾の `poseIk` (snapshot v47) に書く。`SampleSkinnedLocals` の引数は変えず、どの経路の結果にも
  最後に鎖を書かれた順に解く。描画・部位追従・ラグドールが同じ関数を通る原則のまま、部位は IK 後の関節に付く。
  鎖があるメッシュは `UsesLocalsPath` が真になり、描画もパレット直作りの経路を通らない。部位追従のキー (`SamePoseInputs`) にも入れる。
- ソルバ (`SolveTwoBoneIk`) は純関数。中間の位置を余弦定理の長さ (根から目標方向の成分 `(l1² − l2² + d²) / 2d` と垂直な成分) で
  直接求め、根と中間を「今の向き → 新しい向き」の最短の弧で回す。弧は半角公式 `(u×v, 1 + u·v)` の正規化で作り、acos / atan2 を使わない。
  届かない距離は伸び切り / 縮み切りで止める。weight は目標・曲げ面・先端の回転を今の値から線形に寄せる (0 の近くで膝が反対側へ飛ばない)。
- ラグドールの作動中は書かない (骨は物理が決める)。イベントの位置 (M89i) はコントローラの段 (フェーズ 3.5 の先頭) で引くので、
  IK は前 tick の値が乗る (1 tick 遅れ。許容)。
- 却下: ソルバを解決段で回して結果の局所行列を持つ。ポーズが時刻の純関数でなくなり、描画補間の時刻で引き直せない。
- 却下: 目標をワールド座標のまま持ち、`SampleSkinnedLocals` で WorldMatrix を掛けて戻す。引数が増え、WorldMatrix は前 tick の値になる。
## 決定 14: 足の接地は TwoBoneIK の mode 3 として持ち、解決段の最後に足首の目標と骨盤の下げ幅を決める (M89m)

- 設定は新しいコンポーネントを作らず `TwoBoneIKComponent` に寄せた: 鎖の `mode = 3 (kGround)` と、末尾の `pelvisJoint` /
  `pelvisMaxDrop` / `groundProbe` / `groundLayerMask` (snapshot v48)。足も 2 ボーンの鎖なので、先端の名前・pole・weight をそのまま使える。
- `FootIkSystem` は `TwoBoneIkSystem` が持ち、通常の鎖を書いた後に呼ぶ (順序を 1 か所で固定する)。足首のアニメだけのポーズ (IK 前) の
  ワールド位置を LocalTransform の連鎖 (この tick) で求め、足元の面 (メッシュのエンティティの原点の高さ) の ± `groundProbe` を真下へ
  `RaycastWorld` で探す。足首はアニメの高さのまま地面の高さへずらす (振り上げた足は浮いたまま)。地面が足元の面より低い足があれば、
  骨盤をその差 (`pelvisMaxDrop` まで) だけ下げる (`poseIkPelvisJoint` / `poseIkPelvisOffset`、鎖より先に `OffsetJointGlobal` で効かせる)。
- 自分の体 (メッシュの祖先 = CharacterController のカプセル等、子孫 = 骨に付いた部位) のコライダーは、`RaycastWorld` に足した
  除外の関数で収集段階から除く (既定は従来どおり)。
- 計画は「前 tick の WorldMatrix で足の位置を決める」だったが、目標はこの tick の連鎖でメッシュの空間へ直すので、足の位置も同じ連鎖で求める
  (前 tick の値で探すと、移動中は 1 tick 分の移動だけ足が遅れる)。当てる地面のコライダーは WorldMatrix (前 tick) のままで、静止した地面が前提。
- 却下: 骨盤の下げ幅を前 tick から滑らかに追う。前 tick の値に依存する状態を持たない原則に反し、スナップショットから戻した直後に 1 tick 狂う。
  段差を越える瞬間は 1 tick で切り替わる (既知の制約)。
