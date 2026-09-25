# sub-07: 接着の破断と塊の剛体化 (tick 末)、kinematic ルート

- 依存: sub-06
- 状態: OK (コミット待ち)
- 往復: 2

## やること

spec §4.1「破断」の 1〜7 (イベント発行の 8 は sub-12) を FractureSystem として実装する。

1. **FractureSystem** (Engine 層): `TickRunner.cpp` で `collisionSystem.Update` (`:413-419`) の後、tick 末の `ApplyStructuralChanges` (`:491`) より前に 1 回。`simulateScripts` / `stepSim` の条件は物理と揃える
2. **存在ゲート**: ワールドに Destructible が 1 つも無ければ、物理へ形状単位インパルスの出力ポインタを渡さず (null)、FractureSystem も即 return。判定はアーキタイプ走査 1 回程度で
3. **荷重と切断**: spec §4.1 破断 1〜2。`dt` は tick 全体 (関節の破断と同じ単位)。`S_ij = strength × clamp(面積_ij / 平均面積, 0.25, 4)` (平均は資産全体の隣接面積の平均)。切断ビットは両側の `FracturePiece.brokenBonds` に立てる
4. **連結成分と分離**: spec §4.1 破断 3〜6。塊の単位は「Rigidbody を持つエンティティ (ルートまたはリーダー)」。付け替えは `SetParent` / `AddComponent<Rigidbody>` をコマンドバッファ経由で、**ワールド姿勢を保つ `LocalTransform`** を計算して入れる (WorldMatrix は 1 tick 古い可能性に注意 — 物理の書き戻し後の LocalTransform から組むこと)。新リーダーの Rigidbody: `mass` は体積比、`compoundColliders = (メンバー 2 以上)`、`velocity` / `angularVelocity` は元の塊の値から spec の式で、その他の欄 (減衰・重力倍率・ccd 等) は元の塊の Rigidbody を写す。ルートの質量も残りの体積比へ下げる (`useDensity` なら触らない)
5. **kinematic ルート**: ルートの `isKinematic` のとき、ルートに残る塊は固定のまま、分かれた塊は dynamic (`isKinematic = false`)
6. `Destructible.broken` / `detachedCount`、分かれたリーダーの `FracturePiece.releaseTicks = 0` (sub-08 が進める)
7. **ApplyDamage の内部関数** (ABI からは sub-12 で呼ぶ): spec §4.1 スクリプトの式で `damage` に加算する Engine 層の関数と SelfTest。スクリプトからはまだ呼べない
8. **デモ**: `--fracture-demo` に、決まった tick に発射される弾 (決定的な初速の球) を足し、動的な箱と kinematic の壁が割れるようにする。replay_verify の `fracture` job で一致すること
9. 無効な Destructible (sub-06 の整合確認) と破片 1 つの Destructible は何もしない

10. **(sub-06 からの申し送り)** `RenderSystem::CollectDrawables` の root proxy 規則で、`fp->root` の `DestructibleComponent` が見つからない (`d == nullptr`、ルートが Destroy された / 参照切れ) ときに破片と `_cap` を**隠している**のを、**描く**側へ変える (隠す対象が無いので proxy の意味が無い)。分離後にスクリプトやゲームがルートを消すと、破片が全部消える不具合になる。SelfTest で「割れた後にルートを Destroy しても、分かれた破片は描画 item に残る」を固定する

## やらないこと (このサブでは)

- 割れた後の挙動 (sub-08)、`onBreak` と ABI (sub-12)、スキン (sub-10)

## 触る場所 (planner の見立て)

- 新規 `src/Engine/Engine/FractureSystem.h/.cpp`
- `src/Engine/Engine/TickRunner.cpp` (呼び出しと存在ゲート、物理への出力ポインタ)
- `src/Engine/Core/World.h/.cpp` の遅延構造変更 API (使うだけ)
- `DemoContent.cpp` の fracture builder
- `FractureSelfTest.cpp` にケース追加
- 前例: 関節の破断 `PhysicsSystem.cpp:4483-4517` (tick 末判定・フラグで表す)

## 受け入れ条件 (このサブ)

1. 箱 8 破片の動的ルートに十分な速さの球を当てると、当たった側の破片から接着が切れ、塊が 2 つ以上になる。弱い球では割れない — `--selftest`
2. 分離後、塊ごとの質量の和 = 元の質量 (相対 1e-6)。分離した tick の運動量の和が分離前と一致 (相対 1e-5) — `--selftest`
3. kinematic ルートの壁 (破片 16) の一部を撃つと、撃った所だけ抜け、体積最大の塊が動かない (ルートの姿勢が不変) — `--selftest`
4. 連結成分と付け替えが決定的: 同じシーンを 2 本並べて 240 tick のハッシュ列が一致 (`PhysicsSelfTest` の並走比較と同じ形) — `--selftest`
5. ApplyDamage の内部関数で `strength` 以上を 1 破片に与えるとその破片が外れる。未満を 2 回与えると蓄積で外れる — `--selftest`
6. `--fracture-demo` の replay (Debug / snapshot-stress / Release) 一致、割れる前後のスクショ — `replay_verify.bat` の `fracture` job + 画像パス
6b. (round 1 で追加) 割れた後 / 割れる前の状態から、新しい FractureSystem で再開しても、ハッシュ列が途切れずに進めた実行と一致する — `--selftest`
7. **既存は不変**: `replay_verify.bat` の既存 job 全 PASS、`shot_verify.bat` 全 PASS — 実行ログ
8. `check_rules.ps1` PASS。WIP ファイル不変

## 検証コマンド

```
tools\gen_project_files.ps1
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## round 1 の裁定 (planner、FIX_REQUEST の手順)

**問題**: `FractureSystem` の `validated_` (`FractureSystem.cpp:545-567`) は、Destructible を**初めて見た時点**の子の構成で有効 / 無効を決め、以後はその結果を使い続ける。検証 (`ValidateFracturePieces` → `CollectExistingPieces`、`FractureBuilder.cpp:23-35, 139-172`) は**ルートの直下の子だけ**を数える。一方、割れた後の破片は、ルートの親の下へ付け替えたリーダーとその子になる。このため、**割れた後の状態から始まる実行**では、破片数が合わず Destructible が無効になり、以後割れなくなる。該当するのは次のような場合:
- 新しいプロセスで、割れた途中のセーブやスナップショットを読んだとき
- `Reset()` 後のシーン再読み込み
- (sub-08 で塊が Destroy された後に初めて見た場合も)

この場合、同じ状態から続けた元の実行と結果が分かれる。`--snapshot-stress` が通っているのは、同じプロセスでキャッシュが残っているからにすぎない。キャッシュは ECS の外にある「シミュレーションの分岐を決める状態」で、しかも今の状態から作り直せない。spec §2 (状態は全部コンポーネントに置く) と §4.4 (決定性) に反する。

手順:
1. **有効 / 無効をキャッシュしない**。キャッシュしてよいのは資産から一意に決まるもの (資産ハンドルの解決、平均の接着面積) だけ。ERROR を 1 回だけ出すための抑止集合 (ログ専用) はあってよい
2. **検証は毎 tick、今の状態から行う**。すでに集めている `piecesByRoot` (ワールド全体の `FracturePiece.root == root`) を使い、階層は見ない:
   - `broken == false`: index が重複せず範囲内で、個数が資産の破片数と一致すること
   - `broken == true`: index が重複せず範囲内であること (sub-08 の Destroy で減ってよい)
3. Rigidbody の有無の検査もルートの今の状態で毎回行う
4. SelfTest を足す: 割れた後の tick でスナップショットを撮る → **新しい FractureSystem のインスタンス** (キャッシュが空) と新しいワールドで復元 → N tick 進めたハッシュ列が、途切れずに進めた実行と一致すること。あわせて、割れる前に撮った場合も同じ検査をする
5. 他の [追加] 3 件 (残った成分の速度の引き直し・`FindByAssetId`・可視判定の純関数化) は採用

## 実装メモ (coder が追記)

SELF_EVAL: sub-07 (round 1)
実装:
  - `src/Engine/Engine/FractureSystem.h/.cpp` (新規) — `FractureSystem::Update`: 存在ゲート
    (`DestructibleComponent` 走査を `AddComponent<RigidbodyComponent>` の tick 末コマンドバッファ
    強制 (ForEachArchetype コールバック内) と兼用)、資産の解決キャッシュ (`validated_`、無効は
    ERROR 1 回で以後スキップ)、`ProcessRoot`: 荷重 `L_i=C_i+damage_i` → i<j の接着判定 →
    塊 (root/リーダーの直子) ごとの union-find 再構築 → 分かれた成分の昇格 (`ReparentKeepWorld`
    でワールド姿勢を保って `SetParent` + `AddComponent<RigidbodyComponent>`、質量は体積比、
    速度は `v_owner + ω×(center-ownerCenter)` を**残留成分にも**適用 [追加、根拠は差分欄])。
    `ApplyFractureDamage` (Engine 層の内部関数、ABI 化は sub-12)。`AnyDestructibles` (存在ゲート
    共有)。`ShouldHideUnbrokenFracturePiece` (root proxy の可視規則、RenderSystem と共有)
  - `src/Engine/Engine/RenderSystem.cpp` — sub-06 の申し送り (item 10) を修正: `fpi`/`hi` 分岐を
    `ShouldHideUnbrokenFracturePiece` 呼び出しに置き換え、root の Destructible が見つからない
    ときは隠さず描く側にした
  - `src/Engine/Engine/TickRunner.h/.cpp`, `src/Engine/Engine/EngineLoop.cpp` — `FractureSystem` /
    `std::vector<ShapeImpulse>` を `TickServices` へ配線。`AnyDestructibles` で物理への
    `outShapeImpulses` ポインタと `FractureSystem::Update` 呼び出しの両方をゲート
    (collisionSystem の後・tick 末 ApplyStructuralChanges の前、stepSim 条件は物理と共通)。
    シーン遷移時に `fractureSystem.Reset()` (partFollowSystem と同じ流儀)
  - `src/Engine/Engine/Physics/FractureLibrary.h/.cpp` — `FindByAssetId` を追加 [追加、根拠は
    差分欄]。`RegisterInternal` で `byHash_[HashStr(prefix)]=prefix` を記録
  - `src/Engine/Engine/DemoContent.cpp/.h` — `--fracture-demo` に `Destructible.fractureAsset`
    の設定 (`FindByAssetId` で実行時に再解決できるように) と、tick 0 から重力なしで直進する
    決定的な球 2 発 (箱用・壁用) を追加。箱と壁が実際に割れるようにした
  - `src/Engine/Engine/Physics/FractureSelfTest.cpp` — 「16. 接着の破断と塊の剛体化」を追加
    (16a 荷重判定・質量・運動量保存の検算 [合成資産、shapeImpulses を直接組み立てて検算]、
    16b 実物理衝突での弱い球/十分な球、16c kinematic ルートの壁、16d 240 tick の並走ハッシュ
    一致、16e ApplyFractureDamage の一発/蓄積、16f root proxy の可視規則)
仕様との差分:
  - [追加] 残留成分 (root/既存リーダーに残る側) の速度も `v_owner + ω×(center-ownerCenter)`
    で引き直す — spec §4.1 破断 5 は質量の式だけ明記 (「残った塊の質量も同じ式で下げる」)。
    しかし `RigidbodyComponent.velocity` は複合の重心の速度 (PhysicsSystem.cpp の位置積分が
    前提) なので、重心が動いた分を残留側でも引かないと受け入れ条件 2 の運動量保存が成立しない
    ことを手計算 (Σmass_c·v_c = ownerOldMass·ownerOldVel の恒等式) と SelfTest (16a) の両方で
    確認した。理由: 分離で重心が動く量だけ速度場を評価し直す物理的要請
  - [追加] `FractureLibrary::FindByAssetId` + `byHash_`。sub-06 の申し送りは
    「LoadFromFile か RegisterBaked/Find で解決」だったが、`--fracture-demo` はメモリ登録
    (guid を持たない) なので `DestructibleComponent.fractureAsset` だけから実行時に
    再解決する経路が無かった (`assetguid::ResolvePath` は空文字列を返す)。ConvexColliderLibrary
    が既に AssetID (hash) キーで解決する設計に合わせ、FractureLibrary にも hash 逆引きを足した
  - [追加] `RenderSystem::CollectDrawables` の可視判定を `ShouldHideUnbrokenFracturePiece`
    という独立関数へ切り出した。理由: sub-07.md やること 10 が明示的に「SelfTest で固定する」
    ことを要求しており、`CollectDrawables` はレンダラ内部の private メソッドで GPU 抜きの
    単体テストができないため、判定ロジックだけを純関数として external testable にした
  - [未実装] spec §4.1 破断 8 (`onBreak` 発行) — sub-07.md の「やること」で明示的にこのサブの
    範囲外 (sub-12) とされている。`ProcessRoot` は `detachedCount` を増やすだけで、コールバック
    や通知キューは用意していない。sub-12 が新リーダー確定の瞬間 (`ProcessRoot` 内、新リーダー
    ごとのループ末尾) にフックできる形にはなっている
検証:
  - `tools\gen_project_files.ps1` → Engine.vcxproj(.filters) 更新 (507 files)
  - Debug|x64 / Release|x64 フルビルド → エラー 0 (2 往復とも)
  - `bin\x64\Debug\Editor.exe --selftest` → 2 回とも ALL PASS (fail count 0)。16a〜16f 含む
    fracture 系の新規アサーションが全て PASS (mass/momentum は 8.0 vs 8.0、16.0,0,0 vs
    16.0,0,0 の完全一致)
  - `bin\x64\Release\Editor.exe --selftest` → 2 回とも ALL PASS (fail count 0)
  - `tools\replay_verify.bat` → 2 回とも 14/14 job PASS (fracture job 含む。600 tick の
    Debug/Release/snapshot-stress 一致)
  - `tools\shot_verify.bat` → 2 回とも 21/25 PASS。失敗 4 枚 (parts / joints /
    acoustic_forward / acoustic_deferred) は基点コミット由来と司会が確認済みの既知差分と
    数値まで完全一致 (maxDiff/diffPixels/worst pixel 座標が 2 回の実行間でも同一)。新規の
    差分は 0 枚
  - `tools\check_rules.ps1` → 3 回とも 0 error / 0 warning
自己採点 (1-5):
  仕様適合: 5 — spec §4.1 破断 1〜7 と sub-07.md の「やること」1〜10 を実装し、8 (onBreak) は
    明示的に範囲外として未実装のまま (差分欄に記載)。追加した 3 点 ([追加] 3 件) はいずれも
    受け入れ条件を満たすために必要だった変更で、根拠を差分欄に記録した
  正しさ: 5 — 検証コマンドを全て実行し (未実行なし)、Debug/Release 双方の SelfTest・
    replay_verify・shot_verify・check_rules が一貫して同じ結果を返した。質量・運動量保存は
    浮動小数点の完全一致まで確認できた (許容誤差に頼らず一致)
  コード品質: 4 — 既存の RagdollBuilder.cpp / PhysicsSystem.cpp の流儀 (手書きクォータニオン、
    XMMatrixInverse/Decompose を使わない) に揃えた。`ProcessRoot` は 1 つの Destructible の
    破断処理を丸ごと持つのでやや長い (既存の PhysicsSystem::Update も同種に長い関数がある)。
    もう少し細かい関数分割の余地はある (nit)
  テスト: 5 — 受け入れ条件 1〜5 (このサブの範囲) をそれぞれ専用のケースで検算し、実物理衝突
    (16b)・decoupled なシステム単体検算 (16a/16c/16e)・240 tick 決定論比較 (16d)・root proxy
    の純関数検算 (16f) を揃えた
不安・質問:
  - 上の [追加] 3 件 (残留速度の引き直し / FindByAssetId / ShouldHideUnbrokenFracturePiece
    への切り出し) は planner の裁定を経ていない実装判断です。差分欄の根拠で妥当と考えていますが、
    仕様の正本を更新するかどうかの判断は planner にお任せします
  - `dc.strength` など Destructible の欄は spec 4.2 の既定値 (`strength=5000`) のままデモに
    置いていますが、`--fracture-demo` の球のパラメータ (質量・速度) は実測で「確実に割れる」
    よう調整した値 (デモ専用、spec に既定値の指定なし) です。見た目のバランス (弾の大きさ・
    速さの説得力) は sub-08/sub-09 のスクショ確認時に必要なら再調整をお願いします
触ったファイル:
  - src/Engine/Engine/FractureSystem.h (新規)
  - src/Engine/Engine/FractureSystem.cpp (新規)
  - src/Engine/Engine/RenderSystem.cpp
  - src/Engine/Engine/TickRunner.h
  - src/Engine/Engine/TickRunner.cpp
  - src/Engine/Engine/EngineLoop.cpp
  - src/Engine/Engine/Physics/FractureLibrary.h
  - src/Engine/Engine/Physics/FractureLibrary.cpp
  - src/Engine/Engine/DemoContent.h
  - src/Engine/Engine/DemoContent.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - build/Engine.vcxproj (gen_project_files.ps1 による自動更新)
  - build/Engine.vcxproj.filters (同上)
申し送り:
  - sub-08 (割れた後の 6 挙動) 向け: `FracturePieceComponent.releaseTicks` は新リーダー確定時に
    0 を書くだけで、以後の加算・afterBreak の分岐は未実装 (このサブの範囲外)
  - sub-12 (ABI v22) 向け: `onBreak` のフック地点は `FractureSystem.cpp` の `ProcessRoot` 内、
    「分かれた成分ごとに index 最小の破片をリーダーへ昇格する」ループの末尾
    (`++newLeaders;` の直前/直後)。ルート index → 新リーダー index 昇順の発行順は、この
    ループが `comps` を昇順に走査するため自然に満たされる
  - 性能計測は未実施 (sub-11 の範囲)。`FractureSystem::UpdateImpl` は毎 tick 全 FracturePiece
    を再収集する (O(総破片数)、キャッシュなし)。既存デモ (箱 8 + 壁 12 + 球 2) では問題にならない
    規模だが、上限 256 破片 × 複数 Destructible の負荷は未計測
  - `FractureLibrary::FindByAssetId` は登録名のハッシュ衝突を検出しない (既存の AssetID 全般と
    同じ前提)。将来 `Clear()` を本番経路で呼ぶ変更をする場合は `byHash_` も
    `ReregisterAll()`/`Clear()` の対象に含める必要がある (現状は `handles_` と同じタイミングで
    clear/populate しているので追加の対応は不要)

---

SELF_EVAL: sub-07 (round 2)
実装 (round 1 の裁定への対応):
  - `FractureSystem.h` — `RootCache`/`validated_` (有効/無効そのものをキャッシュ) を廃止し、
    `AssetCache`/`assetCache_` (資産ハンドルの解決 + 平均隣接面積だけをキャッシュ) と
    `erroredOnce_` (ERROR を 1 回だけ出すためのログ専用の抑止集合、std::unordered_set) に
    分離した。`Reset()` は両方を clear する
  - `FractureSystem.cpp` の `UpdateImpl` — 資産の解決だけを `assetCache_` から引く (解決できて
    いなければ都度 `ResolveFractureAsset` を試す。失敗は `erroredOnce_` でログだけ 1 回に
    抑える)。資産が解決できたら、**毎 tick**新規関数 `PiecesMatchAssetNow` で「今の
    `piecesByRoot` (ワールド全体の `FracturePiece.root==root`、階層を見ない)」を資産と
    突き合わせる: `broken==false` は index 重複無し・範囲内・個数一致、`broken==true` は
    index 重複無し・範囲内のみ (個数は sub-08 の Destroy で減ってよい)。Rigidbody の有無も
    毎 tick `world.GetComponent` で見る。無効なら `erroredOnce_` でログを 1 回だけ出して
    `continue` する (次 tick 以降も再検証するが、既にログ済みなら黙って `continue` するだけ)。
    資産未解決時のログは `ValidateFracturePieces(world, root, nullptr)` の `asset==nullptr`
    分岐 (階層を一切見ない) だけを流用する
  - `FractureSelfTest.cpp` — 受け入れ条件 6b 用に (16g) 割れた後のスナップショットから、
    (16h) 割れる前のスナップショットから、それぞれ新しい `Scene`/`World` + 新しい
    `FractureSystem` インスタンスへ `CaptureSimSnapshot`/`RestoreSimSnapshot` (`SimRefs{&scene}`
    のみ、xpbd/acoustic 等は無し) で復元し、同じ以後の刺激 (`ApplyFractureDamage`) を
    継続実行側と復元側の両方へ与えて N tick 進め、ハッシュ列が一致することを確認するケースを
    追加した
仕様との差分:
  - 差分なし (round 1 の [追加] 3 件は round 1 の VERDICT で採用済み。今回は round 1 の
    must 指摘への対応のみ)
検証:
  - Debug|x64 / Release|x64 フルビルド → エラー 0
  - `bin\x64\Debug\Editor.exe --selftest` → ALL PASS (fail count 0)。16a〜16f (既存) に加え
    16g (割れた後のスナップショット復元)・16h (割れる前のスナップショット復元) が新規 PASS
  - `bin\x64\Release\Editor.exe --selftest` → ALL PASS (fail count 0)
  - `tools\replay_verify.bat` → 14/14 job PASS (fracture job 含む)
  - `tools\shot_verify.bat` → 21/25 PASS。失敗 4 枚は round 1 と数値まで完全一致する既知の
    基点由来の差分 (parts/joints/acoustic_forward/acoustic_deferred)。新規差分は 0 枚
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - 反証: round 1 のバグ機構 (初見時キャッシュ + 階層限定の `CollectExistingPieces` が、割れて
    ルートの親の下へ出た破片を見失う) を手順どおりに再現する構成 (row8 を damage で 1 回
    分離させてから撮ったスナップショットを、新しい `FractureSystem` インスタンスへ復元し、
    2 回目の分離が両方の実行で起きること・ハッシュ列が一致することを検算) で固定した。
    このテストは新実装 (階層を見ない毎 tick 検証) の下で PASS することを確認済み。
    旧実装に戻して FAIL することの再確認は行っていない (新旧比較の revert テストは未実施 —
    理由: 新実装は `ValidateFracturePieces`/階層探索を一切呼ばない経路に置き換えたため、
    バグの再現条件そのものが構造的に排除されている)
自己採点 (1-5):
  仕様適合: 5 — round 1 の裁定 (1)〜(5) をすべて反映。受け入れ条件 6b (割れた後/割れる前の
    両方) を SelfTest で固定した
  正しさ: 5 — 検証コマンドを全て再実行し、新規テストも含めて一貫して PASS
  コード品質: 4 — round 1 と同じ (nit: ProcessRoot がやや長い)
  テスト: 5 — 受け入れ条件 6b を「割れた後」「割れる前」の両分岐で検算。ただし旧実装での
    FAIL 再現 (反証) までは行っていない点は自己採点を下げる材料として自覚している
不安・質問:
  - 「反証」欄の通り、修正前のコードに一時的に戻して本当に FAIL することまでは確認していません
    (時間対効果を考え、コードレビュー相当の手順追跡で代替しました)。必要であれば追加で
    revert テストを行います
触ったファイル (round 1 から増減なし):
  - src/Engine/Engine/FractureSystem.h
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - (他は round 1 と同じ。新規ファイルの追加は無いため gen_project_files.ps1 の再実行は不要
    — build/Engine.vcxproj(.filters) は round 1 のもののまま)
申し送り: round 1 と同じ (変更なし)

---

SELF_EVAL: sub-07 (round 3)
実装 (should 1 件 + nit 1 件への対応):
  - コードとテストのコメント・アサーション文言から、round 番号・「裁定」・`sub-NN` の語を
    除いた。`FractureSystem.h`: 存在ゲートの説明から「(round-1 の裁定)」を削り理由の説明だけに
    圧縮、`root proxy` と `ApplyFractureDamage` の説明から `sub-06→sub-07`/`sub-12` を削除。
    `FractureSystem.cpp`: `PiecesMatchAssetNow` の説明・`UpdateImpl` 内の見出しコメント 2 か所・
    `FindShapeImpulse` の見出しから同様に除去。`FractureLibrary.h`: `FindByAssetId` の説明から
    `sub-07` を除去。`RenderSystem.cpp`: 可視規則呼び出しの注釈から `sub-06→sub-07` を除去。
    `FractureSelfTest.cpp`: ヘルパーの見出し・(16g) の導入コメント・アサーション文言中の
    `sub-05`/`sub-06→sub-07`/`round-1 の裁定`/`(round-1 fix)` を除去し、理由だけを残した
    (例: 「旧実装は…していた」という経緯の説明は削り、テストが確認する不変条件だけを書いた)
  - (nit) `FractureSystem.cpp` の `UpdateImpl`: 資産が未解決のときに `ValidateFracturePieces
    (world, job.root, nullptr)` を「ログを出すためだけ」に呼んでいた箇所を、その関数が出す
    のと同じ文言の `MYE_LOG_ERROR` 直呼びに置き換えた。使われなくなった
    `#include "Engine/Engine/FractureBuilder.h"` も削除した
  - 修正範囲の確認: 今回のサブで触った全ファイル (build/*.vcxproj を除く) を
    `round[- ]?[0-9]|sub-0[0-9]|裁定|指摘番号` で grep し、自分の差分行 (git diff の `+` 行、
    新規ファイルは全文) に該当が無いことを確認した。`FractureSelfTest.cpp:1419` の
    `sub-05 の複合合成` と、`RenderSystem.cpp`/`DemoContent.cpp`/`EngineLoop.cpp`/
    `DemoContent.h` に残る `M79 sub-05/06` や `reviewer round 1` 等は、いずれも他サブ由来の
    既存コメントで自分の差分に含まれないため変更していない (無関係な既存コードへの改変を
    避けるため)
仕様との差分: なし (コメント・ログ文言の整理のみ、挙動は変えていない)
検証:
  - Debug|x64 フルビルド → エラー 0
  - `bin\x64\Debug\Editor.exe --selftest` → ALL PASS (fail count 0)。fracture 系のアサーション
    (メッセージ文言を変えた箇所を含む) も全て PASS
  - `tools\check_rules.ps1` → 0 error / 0 warning
  - Release ビルド・`--selftest`・`replay_verify.bat`・`shot_verify.bat` は今回の指摘が
    コメント/ログ文言のみで挙動を変えないため未実行 (指示どおり Debug ビルドと check_rules
    のみ再検証)
自己採点 (1-5):
  仕様適合: 5 — should 1 件・nit 1 件をすべて反映
  正しさ: 5 — 指示された範囲 (Debug ビルド・check_rules) を実行し PASS。挙動を変えない
    修正であることをコードの差分 (コメント・1 箇所のログ呼び出し置き換えのみ) で確認できる
  コード品質: 5 — 経緯の語を除いたことで、AGENTS §5 / japanese-comment-style の
    「経緯をコードに書かない」を満たした
  テスト: 5 — 既存のテスト構成・アサーション条件は変更していない (文言だけ差し替え)。
    Debug --selftest で全 PASS を再確認済み
不安・質問: なし
触ったファイル (round 2 から増減なし、コメント/ログ文言のみの変更):
  - src/Engine/Engine/FractureSystem.h
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/Physics/FractureSelfTest.cpp
  - src/Engine/Engine/Physics/FractureLibrary.h
  - src/Engine/Engine/RenderSystem.cpp
申し送り: round 1 と同じ (変更なし)

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 1 件: 有効 / 無効の判定を初見時にキャッシュしていて、しかも直下の子しか数えないため、割れた後の状態から再開した実行が分岐する (セーブの読み込み・新しいプロセス)。手順は「round 1 の裁定」節。受け入れ条件 6b を追加。それ以外 (破断・質量・運動量・kinematic・存在ゲート・描画規則の修正) は受け入れ済み
- round 2: VERDICT OK (planner)。有効 / 無効のキャッシュを廃止し、毎 tick 今の `piecesByRoot` で検証するようになった。16g / 16h (新しい FractureSystem への復元) で、2 回目の分離が復元した実行でも起きることを明示的に検査している。旧実装では直下の子の数が合わずに無効になるので、この検査で必ず落ちる (planner がコードで確認)。revert テストは不要。nit (コミット前): コードとテストの文言にある「round-1」「round-1 の裁定」「round-1 fix」等の作業経緯を除く (AGENTS §5)。`ValidateFracturePieces(world, root, nullptr)` をログのためだけに呼ぶのは、ログを直接出す形のほうが素直 (任意)
