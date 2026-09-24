# sub-07: 接着の破断と塊の剛体化 (tick 末)、kinematic ルート

- 依存: sub-06
- 状態: 未着手
- 往復: 0

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

## 実装メモ (coder が追記)

## フィードバック履歴
