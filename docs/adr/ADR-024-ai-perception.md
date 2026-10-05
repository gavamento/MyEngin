# ADR-024: AI の知覚 (AIPerception) — 状態をコンポーネントに置く

- 状態: **確定** (2026-10-05、M83a〜M83b)
- 出所: 依頼「このキャラクターはどこまで見えるとか、どんな音が聞こえるとかを設定できるように」。
  計画は `plans\ai-roadmap-m83-m86.md` (承認済みのロードマップの写し) の M83 節。
- 実体: `src\Engine\Engine\Perception\PerceptionSystem.{h,cpp}`、`PerceptionSelfTest.{h,cpp}`、
  `Components.h` の `AIPerceptionComponent` / `AIStimulusSourceComponent`

## 背景

既存の AI の知覚は AgentBrain 専用の 2 つだけだった。`AcousticListener` (音響の波が届いたか) と `LightSeeker`
(半径内の最も強い光) で、視野角・遮蔽・陣営・記憶は無い。後続のビヘイビアツリー (M85) が「見つけた敵へ向かう」
「見失った所を探す」を書けるよう、UE の AIPerception に相当する汎用の知覚を作る。

## 決定 1: 見る側と見られる側の 2 コンポーネント

- `AIPerception` (見る側): 視覚 (`sightRadius` / `loseSightRadius` / `fovDeg` / `eyeHeight` / `autoSuccessRange` /
  `losLayerMask`)、聴覚 (`hearingMode` / `hearingRange` / `hearingThreshold`)、ダメージ・接触の on/off、陣営
  (`faction` / `hostileMask` / `detectEnemies` / `detectNeutrals` / `detectFriendlies`)、記憶 (`forgetTicks`)、
  予測 (`predictionTicks`)。結果は固定 8 件の `percepts`。
- `AIStimulusSource` (見られる側): `faction`、見られる点の高さ、感覚ごとの可否。付いていない物は見えない
  (UE の StimuliSource と同じ。視覚の対象をシーンの全エンティティにしない = 走査量が設計者の意図で決まる)。
- 陣営は専用の番号 (0..31) と態度の判定 (同じ陣営 = 味方、`hostileMask` のビット = 敵、他 = 中立) にした。
  TagComponent で代用する案は、敵対関係をフィルタの組み合わせで毎回書くことになるので採らなかった (ユーザー選択)。

## 決定 2: 状態は全部コンポーネントに置く (システムは状態を持たない)

`PerceptionSystem` は毎 tick、コンポーネントだけを読んで書く。結果 (`percepts`) も、報告の保留欄
(`pendingNoise*` / `pendingDamage*`) もコンポーネントのフィールドで、ハッシュ対象・World 節に入る。

- 理由: SimSnapshot の節・SimSources・SimRefs の 3 点セット (XPBD / 音響 / Nav の前例) を足さずに、
  What-if・ロールバック・`--snapshot-stress` がそのまま通る。`PerceptionSelfTest` の「巻き戻して 120 tick が
  連続実行と一致」と replay_verify の `perception` ジョブがこれを確かめる。
- 却下: システム側の報告キュー。スクリプトの LateUpdate や OnCollision (知覚のフェーズの後) から報告すると
  tick 境界をまたいで残るので、スナップショットの節が要る。受け手のコンポーネントの保留欄に「強い 1 件」だけ
  溜める形なら、tick 境界をまたいでもコンポーネントとして運ばれる。
- 代償: 1 tick に届く音・ダメージは受け手ごとに最も強い 1 件だけ (同じ強さなら先に届いた方)。音響の耳
  (`AcousticListener`) の「大きいほうが勝つ」と同じ割り切り。

## 決定 3: tick の位置と読む状態

- フェーズ 3.4a: 音響 (3.4) の後・ナビメッシュ (3.4b) の前。音響のゲート (`ts.acoustic`) とは独立
  (音響ボリュームの無いシーンでも視覚は動く)。
- 読むワールド行列は前の tick の確定値 (音響・`RaycastWorld` と同じ)。接触は前の tick の物理のソリッド接触
  (`solidContacts`、音響の衝撃音と同じ入力) と、CharacterController どうしのカプセルの重なり
  (CC は物理の接触ペアに出ないため)。
- 前方 = ワールド行列の +Z、目 = 位置 + (0, `eyeHeight`, 0) (scale を掛けない)。
- 存在ゲート: `AIPerception` が 1 つも無ければ走査だけで何もしない (RNG を引かない、ハッシュが動かない)。
  replay_verify の既存 15 ジョブがこの変更の前後で PASS した。

## 決定 4: 視線は件数で絞る

- 視線は自前で集めたコライダー (トリガーと無効なエンティティを除く) に `shapes::Raycast` を撃つ。
  `RaycastWorld` は呼ぶたびにコライダーを全部集め直すので使わない。集めるのは視覚の候補が出た tick に 1 回。
- 自分の体 (自分・祖先・子孫のコライダー) と相手の体 (相手と子孫) は遮蔽にしない。
- 1 体あたり 1 tick に撃つ本数は 16 本まで (距離と視野角で絞った候補を近い順、同距離はエンティティキー順)。
  時間で絞ると機種で結果が変わるので件数にした。溢れた相手はその tick は見えない扱い。
- 計測 (Release、`PerceptionSelfTest`): 見張り 50 体 × 相手 50 体 + 壁 20 枚で 1 tick 平均 0.21 ms
  (最悪 0.24 ms、視線 278 本)。Debug は約 7 ms。

## 決定 5: 聴覚は 2 方式。ReportNoise は Distance 専用

- `Distance`: `PerceptionReportNoise(pos, loudness, range, instigator)` (ABI v25) を、呼んだ時点で全
  `AIPerception` について距離の減衰 `loudness * (1 - 距離 / range)` で判定して保留欄に書く。壁は見ない。
- `Acoustic`: 同じエンティティの `AcousticListener` に音響 (フェーズ 3.4) が同じ tick に配った音を聞く
  (壁の回り込み・遮蔽は音響に任せる)。足音・衝撃音・`AcousticEmitter` が対象。
- `ReportNoise` を音響の波に変換する案は採らなかった。知覚から音響の場へ書き込む経路ができ、音響の波スロット
  (32 本) を知覚の都合で消費する。壁で回り込む音を出したいときは既存の `AcousticEmitter` で波を立てる。

## 決定 6: ダメージ・接触・予測

- ダメージ: エンジンに汎用のダメージ処理は無い (破壊物理の蓄積値だけ)。UE の ReportDamageEvent と同じく、
  ゲーム側が `PerceptionReportDamage(victim, instigator, amount, hitPos)` で知らせる。陣営で絞らない
  (攻撃されたら相手が誰でも気付く)。位置は攻撃者の位置、分からなければ当たった点。
- 予測: 視覚で続けて見た 2 回 (間隔 10 tick 以内) の位置の差から速度を求め、見失った後は
  `lastSensedPos + velocity × min(経過, predictionTicks) × dt` を `predictedPos` に書く。重力は見ない。
- 同じ tick に複数の感覚で知覚した相手の位置は、視覚 > 接触 > ダメージ > 聴覚の順で正確な方を採る。

## 決定 7: 記憶とスロット

- 8 件の固定長。並びは相手のエンティティキー順 (名乗らない音 = 無効なハンドルが先頭) で、スロットの埋まり方に
  結果を依存させない。
- 満杯のとき、新しい相手はこの tick に知覚していない中で最も古い記憶を追い出す。全員を知覚中なら捨てる。
  視覚の候補は近い順に入るので、10 体見えても近い 8 体が残る。
- `forgetTicks` を過ぎた記憶と、消えた相手 (`World::IsAlive` が偽) の記憶は消す (古いハンドルを残さない)。

## スクリプト API (ABI v25 = 144)

`PerceptionReportNoise` / `PerceptionReportDamage` / `PerceptionGetCount` / `PerceptionGet` (`MyePercept`、64 バイト) /
`PerceptionCanSee` (即時の判定、結果へ書かない)。C# は `Interop.cs` の位置ミラーと `MyeScript` の糖衣
(`ReportNoise` / `ReportDamage` / `PerceivedCount` / `GetPercept` / `PerceptTarget` / `CanSee`)。
スクリプトの Update が読む結果は前の tick の知覚のフェーズが書いた値。

## 既知の限界

- 視野は 3D の円錐 (上下も同じ全角)。SceneView のギズモは目の高さの水平面に扇形で描くだけ。
- 視線は目から見られる点 1 本だけで、体の一部が見えている状態は判定しない。
- 予測は直線 (曲がり角・重力・ナビメッシュを見ない)。BT の SearchArea (M85) がナビメッシュへ吸着して使う想定。
- `AgentBrain` (音響の敵) は変えていない。`AIPerception` を読むのは後続のビヘイビアツリー。

## 検証結果 (2026-10-05)

- `Editor.exe --selftest` (Debug / Release)、`Server.exe --selftest` に `PerceptionSelfTest` を追加。
- replay_verify に `perception` ジョブ (`--perception-demo`、Debug / Release / Server.exe・snapshot stress)。
- shot_verify に golden `perception` (frame 120、視線と予測位置の線)。
- C# レーンは一時プローブで 5 本を実走して確認 (恒久テストは無し。C# レーンは replay の被覆外)。
