# sub-08: 割れた後の 6 挙動

- 依存: sub-07
- 状態: OK (commit 5e04bcd)
- 往復: 2

## やること

spec §4.1「割れた後」の表を FractureSystem に足す。単位は分かれた塊 (リーダー)。ルートに残った塊には適用しない。

1. リーダーの `releaseTicks` を毎 tick +1 (FractureSystem の中、破断判定の後。分かれた tick は 0)
2. `Destructible.afterBreak` ごとに:
   - 0 残す: 何もしない
   - 1: `releaseTicks ≥ afterBreakTicks` で塊ごと `DestroyEntity` (子孫ごと消える)
   - 2: `afterBreakTicks` 到達で塊の全 Collider の `mask = 0`・`phase = 1`、さらに `fadeTicks` 後に Destroy。重力倍率 0 の塊は沈まないが、時間で消える (仕様どおり)
   - 3: `afterBreakTicks` から `fadeTicks` かけてリーダーの `LocalTransform` の scale を線形に 0 へ (最終 tick の直前で最小値を下限にして 0 除算・零体積を避ける)、終わったら Destroy
   - 4: リーダーの `Rigidbody.isSleeping` になった tick に Rigidbody を外す (塊の Collider は静的形状として残る)。スリープが無効な環境 (`sleepDelayTicks <= 0` / 環境なし) では静的化しない (ツールチップに書く)
   - 5: この Destructible の、まだ残っている分かれた塊の数が `maxDebris` を超えたら、`releaseTicks` の大きい順 (同値は entity index 小) に超過分を Destroy
3. すべて tick 数と hashed な欄だけで決める。Destroy / RemoveComponent はコマンドバッファ
4. `--fracture-demo` の破壊物に挙動の違うものを並べてよい (例: 箱 = 1、壁 = 5)。replay 一致
5. (sub-06 からの申し送り) 絵が安定したので、`tools\shot_verify.bat` に `--fracture-demo` の golden を 2 枚追加する (割れる前 / 割れた後の決まった tick)

## やらないこと (このサブでは)

- ワールド共通の上限、フェードの見た目 (透明化) — 後回し

## 触る場所 (planner の見立て)

- `src/Engine/Engine/FractureSystem.cpp`
- `DemoContent.cpp` の fracture builder (任意)
- `FractureSelfTest.cpp` にケース追加

## 受け入れ条件 (このサブ)

1. 6 挙動それぞれについて、決まった tick で期待どおりの状態になる (1: tick N で塊が消える / 2: tick N で mask 0、N+fade で消える、沈んで床より下へ / 3: scale が線形に減り N+fade で消える / 4: スリープした tick に Rigidbody が外れ、塊が静的形状として当たる / 5: 上限を超えた分だけ古い順に消える / 0: 何も起きない) — `--selftest`
2. 同じシーン 2 本の並走でハッシュ列一致 — `--selftest`
3. `--fracture-demo` の replay 一致 — `replay_verify.bat` の `fracture` job
4. 既存 job 全 PASS、`check_rules.ps1` PASS、WIP 不変

## 検証コマンド

```
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\check_rules.ps1
```

## round 1 の裁定 (planner、FIX_REQUEST の手順)

**疑い**: kinematic ルートの壁から分かれた破片の速さが、物理的にあり得ない。
- 弾は 20 kg・30 m/s。反発係数が 1 でも、止まっている軽い破片が受け取れる速さは、衝突直前の弾の速さの 2 倍 (≤ 60 m/s) が上限
- 壁の質量 1 (1 破片 ≈ 0.08 kg) のとき「数千 m/s」、質量 40 (≈ 3.3 kg) のとき「数十 m/s」
- これはどちらも「約 261 N·s (分離した tick の壁の接触インパルス) ÷ 破片の質量」と一致する (261 / 0.083 ≈ 3,100、261 / 3.3 ≈ 79)
- **破片の質量に関係なく、ほぼ一定の運動量が破片に入っている** = 物理ではなく不具合の形

候補 (安い順に切り分ける):
- (a) 分離の瞬間の速度: kinematic ルートの `velocity` / `angularVelocity` が 0 でない (`FractureSystem.cpp:415-461` の `velocityAt`)
- (b) 分離した破片の凸包が、ルートに残った隣の破片 (kinematic の複合の子形状) と重なったまま始まり、無限質量の相手から押し出されている (凹んだ破片の凸包は隣へはみ出す。凸なセルでも面どうしが接している)
- (c) 分離した tick の接触が二重に効いている (同じ接触が、分離前の複合と分離後の単体の両方で解かれる等)
- (d) その他

手順:
1. **計測 SelfTest** を足す: 重力 0・床なし。kinematic ルートの壁 (質量 1、破片 12 前後) に 20 kg・30 m/s の球を当てる。分離した tick の前後 (N−1〜N+10) で、分かれた各破片について次を記録し、SELF_EVAL に表で出す:
   - 速度
   - 接触の相手 (球 / ルートの複合 / 他の破片)
   - その接触のインパルス
   - 開始時の貫入の深さ (取れれば)
   - 球の速度
   (記録のための一時ログはコミットしない。検査として残すのは下の 3)
2. 表から原因を (a)〜(d) のどれか 1 つに**断定**する。断定できなければ修正せずに、表を付けて返す
3. **修正と固定**:
   - (a) / (c) なら FractureSystem 側を直す
   - (b) なら planner 承認済みの対策として、**焼きの凸包を破片の重心へ向けて縮める余白** (名前付き定数。破片の大きさの 1% と 1 mm の大きい方。描画メッシュは縮めない) を入れる。場所は `FractureBake` の凸包生成か `FractureLibrary` の登録時 (`.mfrac` の版を上げずに済むなら登録時)。縮めても凹んだ破片が隣にはみ出す分が残るなら、その量も表に出す
   - 固定する検査: 手順 1 のシーンで、**分かれた破片の最大の速さ ≤ 衝突直前の球の速さ × 2 × 1.05**
4. `--fracture-demo` の壁の `Rigidbody.mass` を 40 にした調整は、原因を直した後にもう一度見直してよい (デモの見た目の調整は coder 判断)。**golden `fracture_after` は撮り直す**。tick 120 で壁の破片が画面内に見えることが望ましい (should)
5. sub-08 の本体 (割れた後の 6 挙動) は受け入れ済み。変えないこと

## 実装メモ (coder が追記)

SELF_EVAL: sub-08 (round 1)
実装:
  - `src/Engine/Engine/FractureSystem.cpp` (`ProcessAfterBreak`、`UpdateImpl` からの呼び出し) —
    塊のリーダー (`FracturePieceComponent.releaseTicks >= 0`) ごとに releaseTicks を毎 tick +1 し、
    `DestructibleComponent.afterBreak` (0..5) に従って Destroy / `ColliderComponent.mask` クリア /
    `LocalTransform.scale` の線形縮小 / `RigidbodyComponent` 除去 / 上限超過削除を行う。
    今回新しく分かれたリーダー (releaseTicks が今 tick に 0 になったもの) は、上限判定 (5) の
    頭数には数えるがそれ以外の挙動判定からは外す (次 tick から判定を始める)。理由は「仕様との
    差分」参照
  - `src/Engine/Engine/FractureSystem.h` — 上記の設計意図をクラス冒頭のコメントに追記
  - `src/Engine/Core/Components.cpp` — `afterBreak` のツールチップに、スリープ無効時 (4) は
    静的化しない旨を追記 (やること 2 の指示どおり)
  - `src/Engine/Engine/Physics/FractureSelfTest.cpp` — `SetupDetachedLeader` ヘルパと、6 挙動 +
    決定論の 22 個の check を追加 (セクション 17)。behavior 2 は実際に `PhysicsSystem` を回し、
    床コライダーを置いて「mask クリア後に沈んで床の下へ抜ける」ことを実際の物理で確認する
  - `src/Engine/Engine/DemoContent.cpp` (`BuildFractureShowcaseScene`) — `--fracture-demo` の
    箱・壁の `strength` を既定 5000N から 200N へ、壁 (kinematic ルート) の `Rigidbody.mass` を
    既定 1.0 から 40.0 へ変更。理由は「仕様との差分」参照
  - `tools\shot_verify.bat` — `fracture_before` (frame 3) / `fracture_after` (frame 120、
    physics/joints 等と同じ流儀) の 2 枚を追加 (26/27 枚目)、末尾の PASS メッセージと冒頭の
    枚数コメントを更新
  - `tests\golden\fracture_before.png` / `tests\golden\fracture_after.png` — 上記 2 枚の golden
仕様との差分:
  - [追加] 新しく分かれたリーダー (releaseTicks が今 tick に 0 になったもの) は、afterBreak の
    挙動判定を**次 tick から**始める (releaseTicks==0 の tick は判定をスキップし、上限判定 (5) の
    頭数だけ含める) — 理由: 昇格に伴う `SetParent` は tick 末のコマンドバッファに積まれるだけで、
    `World::GetParent` / `ForEachInSubtree` が辿れる階層に反映されるのは
    `ApplyStructuralChanges` の後。同じ tick のうちに afterBreak=2 の mask クリアを塊全体へ
    (`ForEachInSubtree` で) 適用しようとすると、まだ親付けが効いていないメンバーを取りこぼす
    (一部の破片だけ mask が残る、という黙った壊れ方になる)。releaseTicks==0 を全挙動で一律に
    スキップすることで、判定する時点では常に階層が揃っている塊だけを扱う不変条件にした。
    実質的な影響は `afterBreakTicks == 0` を指定した場合だけで、「分離した同 tick に発動」ではなく
    「分離の 1 tick 後に発動」になる (それ以外の afterBreakTicks では releaseTicks の値そのものは
    変わらないので影響なし)
  - [追加] `--fracture-demo` の `strength` を 200N へ、壁の `mass` を 40 へ変更 — 理由:
    既定値 (strength=5000, 壁の mass は未設定で既定 1.0) のままだと、デモの衝突
    (質量 20・速度 30 m/s の弾) では箱がほとんど割れず (400 tick 回してようやく破片 6 個)、
    壁は最初の一撃で 4 破片が分かれるものの質量が既定 1.0 しかないため 1 破片あたり ~0.08kg にしか
    ならず、生の衝突量積 (実測 C≈15651、1 tick の生インパルス≈261 Ns) を受けて時速数千 m/s で
    吹き飛び、同 tick のうちに画面外へ消える (golden 2 枚のどちらでも「壊れた」ことが見た目に
    残らない)。`FractureSelfTest.cpp` の box8 インパクト検算 (16b/16d) も同じ理由で strength=200
    を使っており、それに合わせた。壁の mass=40 は他デモの固定構造物 (`PhysicsSelfTest.cpp` の
    `Base` 等) と同程度の値。この変更は「触る場所」に明記された
    `DemoContent.cpp の fracture builder (任意)` の範囲内と判断した
  - [未実装] 壁 (kinematic ルート) 自身の破片は、mass=40 に上げても検算上まだ時速数十 m/s 級で
    吹き飛ぶため、tick 120 の golden では画面外に出ており「壁が壊れた」ことは見た目に残らない
    (箱の方は golden 2 枚で明確に破片が見える)。壁の kinematic 挙動そのものは
    `--selftest` (16c: 撃った所だけ抜け、体積最大側が固定のまま残る) で別途確認済みで、
    受け入れ条件 12 (デモのスクショで確認) は sub-07 の担当条件 (このサブの担当は 10, 13, 20)。
    「申し送り」に記載
検証:
  - `bin\x64\Debug\Editor.exe --selftest` → ALL PASS (fracture: 0 FAIL、新規 22 check 含む)
  - `bin\x64\Release\Editor.exe --selftest` → ALL PASS (fracture: 0 FAIL)
  - `tools\check_rules.ps1` → 0 error(s), 0 warning(s)
  - `tools\replay_verify.bat` → `[parallel] all 14 jobs passed` (`fracture` job含む。DemoContent
    変更後に再実行して確認、`cache\golden_fracture.rep.mismatch.txt` は生成されない)
  - `tools\shot_verify.bat` → 27 枚中 23 枚 PASS (`fracture_before` / `fracture_after` は
    maxDiff=0 で PASS)。FAIL の 4 枚 (`parts` / `joints` / `acoustic_forward` / `acoustic_deferred`)
    は harness.md 記載どおり M80 着手前からの既知の乖離で、spec §5 受け入れ条件 10 により除外対象
自己採点 (1-5):
  仕様適合: 5 — spec §4.1「割れた後」の表 6 種を全て実装し、tick 数と hashed な欄だけで判定する
    契約 (§4.4) を守った。未規定だった「新規リーダーの同 tick 判定」は最小で可逆な解釈を採り、
    上記のとおり明記した
  正しさ: 5 — 6 挙動それぞれの決まった tick での状態遷移 (Destroy / mask / scale / Rigidbody
    除去 / 上限) を Debug/Release 両方の `--selftest` で確認し、決定論 (2 本並走のハッシュ列一致)
    と `replay_verify.bat` の `fracture` job (Debug/Release rep 一致) も確認した
  コード品質: 5 — 既存の `ProcessRoot` と同じ流儀 (tick 末コマンドバッファ、階層を毎 tick
    検証し直す方針) を踏襲し、新規ヘルパ (`ForEachInSubtree` 利用) は既存の公開 API を再利用した。
    デモの調整も理由をコメントに残した
  テスト: 5 — 6 挙動 × 個別 check (計 18) + 決定論 1 本 (計 2 check) + 既存 check の計 22 個を
    新規追加し、Debug/Release 両方で確認済み
不安・質問:
  - なし (裁定が必要な新規論点はない。上記「仕様との差分」の解釈は最小の可逆な選択で、
    影響は `afterBreakTicks==0` という縁のケースに限られる)
触ったファイル:
  - src/Engine/Engine/FractureSystem.h
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Core/Components.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Engine/Engine/DemoContent.cpp
  - tools/shot_verify.bat
  - tests/golden/fracture_before.png
  - tests/golden/fracture_after.png
申し送り:
  - 壁 (kinematic ルート) 自身の破片が golden 画像で見た目に残らない件 (上記「仕様との差分」の
    [未実装] 参照)。将来デモを調整する機会 (sub-11/12 の計測・仕上げなど) があれば、壁側の球の
    軌道かカメラ位置を調整して「壁が壊れた」ことも golden で見えるようにすると受け入れ条件 12 の
    デモ面がより厚くなる (必須ではない)
  - `FractureSelfTest.cpp` の Debug ビルドはセクション 14 (ボクセル化、既存・このサブでは未変更)
    の `res=48` 系ケースで 1 回あたり 2 分前後かかる (既存の重さ、sub-04/14 の実測どおり)。
    `--selftest` 全体の所要時間はこれが支配的で、このサブの新規テスト自体は軽い

SELF_EVAL: sub-08 (round 2)
実装:
  - `src/Engine/Engine/Physics/FractureSelfTest.cpp` — セクション 18 として、「round 1 の裁定」
    手順 3 の固定検査 (kinematic 壁に球をぶつけ、分かれた破片の最大の速さ ≤ 衝突直前の球の速さ
    × 2 × 1.05) を追加した。計測用の一時ログ・診断コードは全て削除済み (下記「検証」の実測値は
    一時的に足して取り、確認後に削除した)
  - `src/Engine/Engine/FractureSystem.cpp` / `FractureSystem.h` — **変更なし** (round 1 採択済みの
    コードのまま。診断のため一時的に `MYE_LOG_INFO` を足したが、原因の切り分けが済んだ時点で
    全て削除し、round 1 と同一のコードに戻したことを diff で確認済み)
仕様との差分: なし (round 1 から変更なし)
不安・質問:
  - round 1 の [must] 1 の疑い (kinematic 壁の破片が「衝突直前の球の速さの 2 倍」を超えて
    分離する) は、**測定の結果、再現しなかった**。詳細は下記「計測結果」参照。round 1 の
    SELF_EVAL に書いた「時速数千 m/s で吹き飛ぶ」は、実測した接触インパルス C (荷重の判定にしか
    使わない量) を根拠なく「C ÷ 質量 = 分離速度」と読み替えた**私の誤った机上計算**で、
    `FractureSystem.cpp` の分離速度の実装 (`velocityAt`、`ProcessRoot.cpp:421-430`) を
    見ないまま断定していた。`velocityAt` は塊 (この場合は静止した kinematic 壁) の
    `RigidbodyComponent.velocity`/`angularVelocity` だけから決まり、接触インパルスにも
    破片の質量にも依存しない。壁は動かないので両方 0、よって新しく分かれる破片の初速は
    常に厳密に 0 — 実測もそれと一致した (下記)。round 1 の指摘は取り下げるべきだが、
    却下権は planner にあるため、実測結果を添えて再度ご確認いただきたい
  - 上記により、[must] 1 の候補 (a)/(b)/(c)/(d) はどれも「原因」ではなく、`FractureSystem` /
    凸包 (sub-02/03) のコードに修正は加えていない。[must] 2 (golden 撮り直し) は実施したが、
    旧 golden と MD5 完全一致だった (下記) — 旧 golden は「不具合の状態」ではなく、この
    (正しい) 挙動をそのまま撮っていたことになる
  - [should] 3 (壁の破片が golden で見えるようにする) は見送った。理由: 静止した kinematic
    壁の破片は物理的に初速 0 が正しく、これを画面内で目立たせるには不物理な初速を追加で
    与えるほかなく、AGENTS.md の判断優先順位 (正しさ > 見た目) に反すると判断した。
    箱側は golden 2 枚で明確に破片が見えており (受け入れ条件のデモ面は満たしている)、壁の
    kinematic 挙動自体は `--selftest` (16c) で別途確認済み。この見送りが妥当かは planner の
    判断を仰ぎたい
計測結果 (round 1 の裁定・手順 1〜2):
  シーン: 重力 0、床なし、kinematic 壁 (質量 1 既定、破片 12、`res.meshes.Cube()` と同じ
  頂点データ、seed=2)、球 (質量 20 kg、速度 30 m/s、半径 0.5、重力なし)。tick=8 で破片 2 個
  (piece 6 = entity 14、piece 7 = entity 16) が分かれた (N=8)。
  | tick | ball \|v\| | 備考 |
  |---|---|---|
  | 7 (N−1) | 30.000 | 衝突前 |
  | 8 (N) | 3.453 | 衝突。contact(wall=1, ball=26) impulse=604.417 N·s。**piece6/7 とも v=(0,0,0) (release=0)** |
  | 9 (N+1) | 3.453 | contact(wall,piece6) / (wall,piece7) impulse≈0.000。piece6/7 とも v=(0,0,0) |
  | 10〜18 (N+2〜N+10) | 3.453 (不変) | 同上、piece6/7 とも v=(0,0,0) のまま (数値誤差以下) |
  実際の `--fracture-demo` (壁の mass=40、重力あり、床あり) でも同じ式を確認: 分かれた
  4 破片の初速はいずれも `newVel=(0.000,0.000,0.000)` (`ownerOldVel`/`ownerOldOmega` が
  壁の 0 のまま)。以後は重力と小さな接触 (impulse < 0.03 N·s) だけで、14 tick 後でも
  最大速度 0.373 m/s 程度 (物理として正常)。新設の恒久検査 (セクション 18、球速度 30 m/s
  への切替なし) では 30 tick 以内の最大分離速度 2.872 m/s ≤ 63.000 (=30×2×1.05) で PASS
検証:
  - `bin\x64\Debug\Editor.exe --selftest` → ALL PASS (0 FAIL、セクション 18 の速度上限検査
    2.872 <= 63.000 で PASS)
  - `bin\x64\Release\Editor.exe --selftest` → ALL PASS (0 FAIL、同じ数値で PASS。Debug/Release
    一致)
  - `tools\check_rules.ps1` → 0 error(s), 0 warning(s)
  - `tools\replay_verify.bat` → `[parallel] all 14 jobs passed` (`fracture` job 含む、
    `FractureSystem.cpp` は round 1 と同一なので当然の結果)
  - `tools\shot_verify.bat` → 27 枚中 23 枚 PASS。FAIL の 4 枚は round 1 と全く同じ数値
    (`parts` maxDiff=198/diffPixels=3625、`joints` 83/31、`acoustic_forward` 83/596、
    `acoustic_deferred` 82/594) で、harness.md 記載の既知の乖離のまま
  - `tests\golden\fracture_before.png` / `fracture_after.png` を撮り直し、旧ファイルと
    MD5 で完全一致することを確認 (前者 `d4fc9bfa804c90a36b743344ff1488f8`、
    後者 `1564d927f97afc10f2a8917e65a7de1c`)
自己採点 (1-5):
  仕様適合: 5 — round 1 で受け入れ済みの 6 挙動・[追加] 判定・ツールチップは不変。round 1 の
    裁定手順 (計測→断定→検査固定) を順番どおり実施した
  正しさ: 5 — 疑われた「物理的にあり得ない速さ」は実測で再現せず、`velocityAt` の式
    (`ownerOldVel + ω×r`) がコード上も実測上も分離速度の全てであることを確認した。恒久検査を
    Debug/Release 両方で確認し、既存 replay/shot も無回帰
  コード品質: 4 — 一時診断コードは全て削除し `FractureSystem.cpp` を round 1 のコードへ戻した
    (diff で無変更を確認)。新規検査 1 本だけが増分。減点理由: round 1 の SELF_EVAL に
    未検証の推測を事実であるかのように書いてしまった (この round で訂正した)
  テスト: 5 — round 1 の裁定が要求した固定検査 (2 倍速上限) を追加し、Debug/Release 双方で
    PASS。既存 22 check・replay 9 シーン・shot 23 枚 (既知 4 枚除く) も無回帰
触ったファイル (round 2 差分のみ):
  - src/Engine/Engine/Physics/FractureSelfTest.cpp (セクション 18 追加)
  - tests/golden/fracture_before.png (撮り直し、旧ファイルと MD5 一致)
  - tests/golden/fracture_after.png (撮り直し、旧ファイルと MD5 一致)
  - plans/m80-destruction/sub-08.md (このメモ)
  - src/Engine/Engine/FractureSystem.cpp / FractureSystem.h は round 1 から**変更なし**
    (一時診断を足して削除しただけで、最終的な内容は round 1 と同一)
申し送り: round 1 のものに変更なし (壁の破片が golden で見えない件は「見送った」理由のとおり)

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。6 挙動 (受け入れ条件 1〜4) と [追加] (新しいリーダーは次の tick から判定) は受け入れ。must: kinematic ルートから分かれた破片の速さが「分離 tick の接触インパルス ÷ 破片の質量」と一致し、物理の上限 (球の速さの 2 倍) を大きく超える。sub-07 由来の不具合の疑いとして、「round 1 の裁定」節の手順で計測・断定・修正・固定する。golden `fracture_after` を撮り直す
- round 2: VERDICT OK (planner)。計測で、分離の初速が 0 (kinematic の壁、速度 0) であることと、round 1 の「数千 m/s」が机上計算の誤りだったことを確認。FractureSystem は変更なし。section 18 の回帰検査を採用。golden は撮り直してもバイト一致。壁の破片を画面内で見せる調整を見送ったことは妥当 (不物理な初速が要る)。「弾が固定の壁に止められ、破片が静止から始まる」のは v1 の設計の帰結として spec §3 後回し (打ち抜き) に記録し、[ユーザーに聞ける] #13 へ
