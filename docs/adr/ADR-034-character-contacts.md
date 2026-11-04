# ADR-034: CC は運動量を交換しない。押し出し (tick 頭の姿勢、優先度) と接触の通知だけを足す

- 状態: 採用 (2026-11-04、M95 sub-02)
- 出所: 仕様 `plans\m95-action-combat\spec.md` §4.1 D (疑った点 #9 / #10 / #11)、台帳 `plans\m95-action-combat\harness.md`。
  解決する問題は [docs\dogfooding.md](../dogfooding.md) の 17

## 背景

`PhysicsSystem` は剛体を `bodies`、CharacterController (CC) を `chars` という別の配列で解く。CC は剛体 (静的 + 解決後の動的) からは押し出されるが、
CC 同士は素通りで、接触イベント (`SolidContact` → `CollisionSystem` → `OnCollisionEnter/Stay/Exit`) は剛体同士の拘束からしか生まれない。
そのため「ボスが雑魚を押し退ける」「CC が壁や別の CC に触れたのを知る」「車で CC を轢く」がエンジン側では書けず、ゲーム側が近傍を自前で調べていた。

## 決定

1. **押し出しと通知だけを足す。運動量の交換、CC が剛体を押す応答は作らない。** CC は従来どおり move-then-depenetrate のキネマティック移動体のまま。
2. **CC 同士の押し出しは「相手の tick 頭のカプセルから自分を全量押し出す」。** `SolveCharacters` の固定パスの中、`bodies` の走査の後に、
   `chars` を index 昇順に走査する。相手の姿勢は収集時に固定した tick 頭の位置 (`CharBody.px/py/pz`) なので、どの CC も同じ姿勢を見る = 走査順に依らない。
   (相手の解決後の位置を見ると、低 index の CC だけが余計に譲る順序依存が生まれる。)
3. **`CharacterControllerComponent` の末尾に 3 欄 (hash 対象、既定 off)。**
   `collideCharacters` (両者が true の組だけ押し出す)、`pushPriority` (相手の優先度が自分より低ければその相手を無視する = 低い側だけが押し出される。同値なら互いに止まる)、
   `reportContacts` (この CC が触れた相手を接触として出す)。接地・天井の判定は障害物と同じ分岐。plane2D の CC は z を押し出さない。
4. **接触の通知は `SolidContact` 列へ合流させる。** `reportContacts` の CC が押し出しのパス (床 / kinematic / 動的剛体 / 別の CC) で `Collide` が真になった相手を
   1 対 1 件、`impulse = 0`、key と法線は既存規約 (小 index が上位、法線は大 index → 小 index)、代表点は相手形状上で自分の中心に最も近い点で記録する。
   CC 同士はどちらか一方が `reportContacts` なら出る。接地プローブは状態を変えない探りなので記録しない。
   同じ key は最初の 1 件に畳み、剛体ソルバが既に出している key (CC + ソリッド capsule Collider と動的剛体など) は**剛体側を残す** = 二重配送しない。
   合流後も key 昇順 (`CollisionSystem::FindContactNormal` の二分探索の前提)。記録が 0 件なら出力列に触れず、`reportContacts` の CC が無ければ配列も確保しない。
5. **配送側は変えない。** `CollisionSystem` / `ScriptHost` / `GetContactInfo` (ABI v14) / デバッグ描画は合流した `SolidContact` をそのまま扱う。
6. **SimSnapshot v67。** 追加 3 欄は World 節のカラム生バイトに含まれる。

## 却下した案

- **半々の押し出し** — CC を index 昇順に逐次解くので、半々にすると低 index が相手の tick 頭、高 index が相手の解決後を見て順序依存になる。
- **「動いた側が止まる」(Unity / UE の pawn 同士)** — 動いた側の判定に前 tick との差分が要り、立ち止まった CC が一方的に押される。優先度の方が意図 (ボスの体当たり) を直接書ける。
- **CC を剛体ソルバの拘束に入れる** — 既存シーン (三校・HAL Collector) の演算順とビット一致を壊す。opt-in の旗が全て off ならこの経路に 1 命令も入らないことを優先した。
- **接触イベント専用の経路 (`SolidContact` と別の列)** — `CollisionSystem` / スクリプト / デバッグ描画 / ABI を全て複製することになる。

## 影響

- 既存シーン: 3 欄が全て off なら `SolveCharacters` の演算は従来とビット同一 (押し出しの適用を lambda にまとめただけで式と順序は同じ)。
- 1 tick 遅れ: 優先度で押される側は、相手の tick 頭の姿勢から押し出されるため、相手が動き続けると 1 歩ぶん重なった状態で終わる tick がある。
  隙間のある所から同優先度で歩み寄ると、重なりは最大で 1 歩 (速度 × dt) ぶん残り得る。接した状態からは重ならない。
- CC は運動量を持たない。車で CC を轢く判定は `reportContacts` を付けた CC の `OnCollisionEnter` で書ける (押し返しはゲーム側)。
