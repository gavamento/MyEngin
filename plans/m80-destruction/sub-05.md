# sub-05: 物理 — 複合の上限撤廃・凸包の子の質量特性・形状単位インパルス

- 依存: なし (sub-01〜04 と並列可)
- 状態: 未着手
- 往復: 0

## やること

破壊が載る物理側の 3 点を、**既存シーンのビット一致を保ったまま**入れる (spec §2 の最初の 3 行)。破壊のことは物理に書かない。

1. **複合の上限撤廃**: `PhysicsSystem.cpp:1617` の関数ローカル `kMaxCompoundShapes = 16` のスタック配列を撤廃し、子形状の数だけ扱う (ボディの外で確保した作業用 vector を使い回す等、tick ごとの確保を増やさない工夫は coder 判断)。**16 以下の経路は加算の順序・式を一切変えない** (ビット一致の要)
2. **凸包の子の質量特性**: 複合の子が `collidershape::kConvex` のときだけ、`ConvexMassProperties` の体積・重心 (子の姿勢で body 空間へ) ・フル慣性 (子の回転で回す) を合成に使う。box / 球 / カプセルの子は現状の式のまま。重心の体積加重も凸包の子は実重心を使う
3. **形状単位の法線インパルス**: `PhysicsSystem::Update` に任意の出力 (例 `std::vector<ShapeImpulse>* outShapeImpulses`、要素は「形状を持つエンティティ, 法線インパルス合計 [N·s]」) を足す。**null なら何も計算しない** (存在ゲート)。
   - 形状を持つエンティティ = 単体ボディなら本人、複合の子ならその子エンティティ (`CompoundShape.child`)
   - `ContactConstraint` に両側の形状のエンティティ (または子の index) を持たせ、`SolidContact` と同じ合計 (`lambdaNc + Σ pts.lambdaN`、全反復・全サブステップ、CCD / スリープ接触の扱いも `SolidContact` と揃える) を形状ごとに足す
   - 出力は entity index 昇順・決定的。両側に同じ量を入れる (A 側と B 側)
   - `SolidContact` とその全消費者 (CollisionSystem / GetContactInfo / ModalAudio / AcousticField / DebugDraw / SelfTest) は変えない
   - TickRunner はまだ null を渡す (sub-07 で Destructible があるときだけ渡す)
4. **プロファイル**: 物理の中に `MYE_PROFILE_SCOPE` を数か所 (収集 / 広域 / 狭域 / ソルバ / 書き戻し 程度) 足してよい (状態に影響しない。sub-11 で使う)

## やらないこと (このサブでは)

- コンポーネント・破断 (sub-06 / 07)
- `SolidContact` の変更

## 触る場所 (planner の見立て)

- `src/Engine/Engine/Physics/PhysicsSystem.cpp` — 複合の質量特性 `:1600-1697`、`CompoundShape` `:1409-1420`、`ContactConstraint` `:3518-3544`、接触の集約 `:4347-4396`、`MergeSubstepContacts` `:995`、`forEachShape` / `forEachShapePair` `:1995-2008`
- `src/Engine/Engine/Physics/PhysicsSystem.h` — `Update` の引数 (既定 null)、出力の構造体
- `src/Engine/Engine/PhysicsSelfTest.cpp` — ケース追加 (M60e の複合テスト `:5650` 付近が前例)
- **触らない**: `src/Engine/Engine/TickRunner.cpp` の呼び出し (null のまま。変えるなら null を明示するだけ)、WIP ファイル

## 受け入れ条件 (このサブ)

1. 子 20 個の複合で、全子形状の体積が質量・重心・慣性に入る (解析値と一致: 例 箱 20 個を並べた棒の慣性) — `--selftest`
2. 凸包の子 (箱の点群から作った凸包) 1 個の複合が、同じ形の凸包単体ボディと同じ重心・慣性になる (相対 1e-5)。原点が重心からずれた凸包の子でも重心が正しい — `--selftest`
3. 形状単位インパルス: 複合 (子 3 個) の 1 個だけが床に触れる配置で、その子のエンティティだけにインパルスが出る。全形状の合計がボディ対の `SolidContact.impulse` の合計と一致 (相対 1e-6) — `--selftest`
4. 出力ポインタ null のとき、2 回目の同シーン実行とハッシュ列が同じ (計算していない) — `--selftest`
5. **既存 replay のビット一致 (旧コミットの rep で照合)**: 変更前のコミット (sub-05 着手時の HEAD) を Debug ビルドし、`replay_verify.bat` の `:chain` と同じ録り方で `default` / `--parts-demo` / `--physics-demo` / `--joint-demo` の rep を録る。変更後の Debug と Release で `--replay-verify` し全 tick 一致 — コマンドと結果を SELF_EVAL に
6. `tools\replay_verify.bat` 全 job PASS、`tools\shot_verify.bat` の joints / physics 系 PASS — 実行ログ
7. `check_rules.ps1` PASS。WIP ファイル不変 — `check_rules.ps1`、`git status`

## 検証コマンド

```
（変更前: git stash は使わない — WIP があるため。git worktree で HEAD を別ディレクトリに出してビルドするか、
  変更前に一度ビルドして rep を録ってから実装する。どちらにしても WIP ファイルを動かさないこと）
（Debug|x64 と Release|x64 をビルド）
bin\x64\Debug\Editor.exe --selftest
bin\x64\Debug\Editor.exe --replay-verify <旧 rep> ...   (replay_verify.bat の :chain と同じ引数)
bin\x64\Release\Editor.exe --replay-verify <旧 rep> ...
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

旧 rep との照合が 1 つでも割れたら、コードを疑う前に「記録中に窓を触った / 止めた」を除外し (録り直して再照合)、それでも割れるなら 16 以下の経路の演算順を確認すること。

## 実装メモ (coder が追記)

## フィードバック履歴
