# ADR-027: 骨アニメのポーズはポーズプログラムに一本化し、純関数で評価する

- 状態: **確定** (2026-10-08、M89a。後続サブで層の書き手と尾部を足していく)
- 出所: M89 骨アニメの深化。計画は `plans\m89-skeletal-animation.md`。
- 実体: `src\Engine\Core\Ecs\Components.h` の `SkinnedMeshComponent` 末尾 (`poseLayerCount` / `poseClaim` / `poseLayers`)、
  `src\Engine\Renderer\Mesh\Skeleton.{h,cpp}` の `ComputeJointLocalsLayered`、
  `src\Engine\Engine\Animation\SkinningSystem.{h,cpp}` の claim 手順・`UsesLocalsPath`・`SamePoseInputs`・`SampleSkinnedLocals`。
  検証は `SkeletonSelfTest` の (10)。
- 番号: `kSimSnapshotVersion` 40。ABI と TypeId は変更なし。

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
