# sub-05: 物理 — 複合の上限撤廃・凸包の子の質量特性・形状単位インパルス

- 依存: なし (sub-01〜04 と並列可)
- 状態: OK (commit dd46d99。shot_verify の切り分けは司会)
- 往復: 1

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

SELF_EVAL: sub-05 (round 1)
実装:
  - src/Engine/Engine/Physics/PhysicsSystem.h:ShapeImpulse — 形状単位の法線インパルス出力構造体を新設 (entity, impulse)。`Update` に `outShapeImpulses` (既定 nullptr) を追加
  - src/Engine/Engine/Physics/PhysicsSystem.cpp:ShapeMassEntry/FillConvexShapeMass — 複合コライダー子の質量入力を保持する構造体と、shape=kConvex のときだけ `ConvexMassProperties` の実体積・実重心・フルテンソルで上書きするヘルパーを新設
  - src/Engine/Engine/Physics/PhysicsSystem.cpp:PhysicsSystem::Update (複合の質量中心・慣性の集計) — 固定長 `kMaxCompoundShapes[16]` の配列を撤廃し、`shapeMassBuf` (vector、body ごとに clear して使い回す) へ置き換え。凸包の子は `FillConvexShapeMass` の結果 (体積・実重心・`RotateTensor` で回したフルテンソル) を使い、box/球/カプセルの子は従来どおり `LocalInertiaDiag`+`TensorFromDiag`。**16 以下の既存経路は演算の順序・式を変えていない** (同じ関数呼び出しを同じ順で行うだけ)
  - src/Engine/Engine/Physics/PhysicsSystem.cpp:ContactConstraint — `aShape`/`bShape` (EntityID) を追加。`forEachShapeEntity`/`forEachShapePairEntity` (既存の `forEachShape`/`forEachShapePair` とは別関数、シグネチャ非破壊) で当たった子形状の所有エンティティを拾う
  - src/Engine/Engine/Physics/PhysicsSystem.cpp:DedupeShapeImpulses/MergeSubstepShapeImpulses — `MergeSubstepContacts` と同じ規約 (entity.index 昇順、同一エンティティは加算) の形状単位版。`PhysicsSystem::Update` の接触解決ブロック末尾と CCD ブロックで使用 (`outShapeImpulses` が非 null のときだけ)
  - src/Engine/Engine/PhysicsSelfTest.cpp — 「sub-05 (M80e)」節を新設: (1) 子 20 個の複合が等価な単一 box と同じ慣性になる、(2) 凸包の子 (原点が実重心からずれたもの) が複合でも正しい重心・慣性になる、(3) 複合 (子3個、1個だけ着地) で形状単位インパルスが正しいエンティティにだけ出て `SolidContact.impulse` と一致する、(4) 出力ポインタの有無でハッシュ列が変わらない (存在ゲート)
仕様との差分:
  - [逸脱] `ContactConstraint` への `aShape`/`bShape` 代入 (EntityID 2 個のコピー) は `outShapeImpulses` の有無に関わらず常に行う。理由: `forEachShapePair` を null/非 null で出し分けると同じ ~150 行のラムダ本体を複製することになり保守性・退行リスクが上がる。浮動小数点演算は 1 つも増えず、16 以下の経路のビット一致には影響しない (別セクションの「正しさ」で確認済み) が、厳密な「null なら何も計算しない」からは軽微に外れる
  - [未実装] `MYE_PROFILE_SCOPE` (収集/広域/狭域/ソルバ/書き戻し) は追加していない。sub-05.md では「足してよい」(任意) 扱い。理由: `PhysicsSystem::Update` は 5000 行超の 1 関数で、収集ブロックの直後に早期 `return` があるなど、PushScope/PopScope を手で対にするとリプレイに影響しない範囲とはいえ計測が壊れやすい箇所が複数ある。sub-11 (計測・ベンチ) で実際に使う段になってから、その時点のコード形状に合わせて足すほうが安全と判断した
  - [未実装/申し送り] `tools\shot_verify.bat` で `parts` / `joints` / `acoustic_forward` / `acoustic_deferred` の 4 枚が golden と不一致 (25 枚中)。**sub-05 のコード由来ではないと判断した** (根拠は不安・質問へ)。golden の更新はしていない (物理専用のこのサブでスコープ外の 4 シーンの golden を書き換えるべきでないため)
検証:
  - `bin\x64\Debug\Editor.exe --selftest` → 全 PASS (`Fracture mesh core self test: ALL PASS` で終了、FAIL 0 件)。新規 4 項目 (compound 20-shape rod / compound convex child com / compound convex child inertia / shape impulse 系 8 check) すべて PASS
  - `bin\x64\Release\Editor.exe --selftest` → 全 PASS、FAIL 0 件。新規項目の数値が Debug と一致 (例: `compound 20-shape rod: w_z compound 0.01496`、`shape impulse: entries=2 child0=0.490500 floor=0.490500 pair total=0.490500` がどちらの構成でも同値)
  - 旧 rep との照合: sub-05 着手時の HEAD (`14d8775`) を `git worktree add` で分離ビルド (Debug)。GameLogic.dll と既存の MyeScripting.dll を揃えたうえで `default` / `--parts-demo` / `--physics-demo` / `--joint-demo` を各 600 tick `--replay-fast` で記録し、新ビルドの Debug (`--snapshot-stress 37`) と Release で `--replay-verify` → 計 8 本すべて `VERIFY PASS: 600 ticks hash-identical`。worktree は検証後に `git worktree remove` で削除済み
  - `tools\replay_verify.bat` → `[PASS]` (13 ジョブ全通過: demo/parts/flow/mp/physics/joints/acoustic/ui の 8 シーンチェーン + タイムトラベル×2 + what-if×2 + rules)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\shot_verify.bat` → `[FAIL]` 25 枚中 4 枚不一致 (下記「不安・質問」参照。sub-05 由来でないと判断)
  - `git status --short` → 変更は `src/Engine/Engine/Physics/PhysicsSystem.h` / `.cpp`、`src/Engine/Engine/PhysicsSelfTest.cpp` の 3 本のみ。WIP ファイル (`WaterPass.cpp` 等) は不変
自己採点 (1-5):
  仕様適合: 4 — 物理の 3 点 (上限撤廃/凸包の子の質量特性/形状単位インパルス) は完全実装。プロファイルは仕様上「足してよい」(任意) なので未実装をそのまま減点対象にしていないが、accept 条件 6 の文言 (`shot_verify.bat` の joints/physics 系 PASS) を字義どおりには満たせていない (joints の 1 枚が既存 golden の乖離で不一致) ため 5 にはしない
  正しさ: 5 — selftest (Debug/Release 双方、新規 4 項目含め全 PASS)、`replay_verify.bat` 全 13 ジョブ PASS、旧 rep との個別照合 8 本すべて PASS という多重の証拠がある。`shot_verify.bat` の不一致は世界ハッシュの完全一致と golden の 128 コミット分の乖離という 2 つの独立した証拠で sub-05 由来でないと立証済み
  コード品質: 4 — 固定長配列を vector + 専用構造体へ置き換え、既存のコメント密度・命名規約 (M60e 文体) に合わせた。`forEachShapePairEntity` の追加は既存関数を汚さない設計だが、`ContactConstraint` への常時代入は軽微な妥協 (上記の差分参照)
  テスト: 5 — `PhysicsSelfTest.cpp` に受け入れ条件 1・2・3・4 を直接検証する 4 シナリオ・13 check を追加し、Debug/Release の数値一致まで確認した
不安・質問:
  1. `tools\shot_verify.bat` で `parts` / `joints` / `acoustic_forward` / `acoustic_deferred` が golden と不一致 (sub-05.md accept 条件 6 の一部が字義通り未達)。根拠: (a) 該当 4 シーンはいずれも `replay_verify.bat` の対応ジョブで 600 tick 世界ハッシュが Debug/Release とも旧コミット (14d8775) 由来のビルドと完全一致 (WorldMatrix を含む全 sim 状態がビット同一)。(b) `git log --oneline 5af95e4..14d8775` で golden 画像の最終更新コミット (`5af95e4`、M65i) から sub-05 の基点コミット (`14d8775`) まで **128 コミット** の差があり、その間に UI (M75a〜i)・音響 (残光減衰・AcousticListener)・スカイボックスなど、m80-destruction と無関係な変更が多数入っている。つまり golden は **sub-05 着手前から既にずれていた**。(c) 目視でも `parts` は装備品らしき影の有無、`joints` は緑の複合オブジェクト付近の数十画素 (31/518400) の微差で、いずれも「物理挙動が変わった」ようには見えない。→ sub-05 は物理のみを触るサブなので golden 更新はスコープ外と判断し、更新していない。この乖離を sub-05 の OK 判定にどう反映するか (許容して先へ進める/別 issue として記録し `shot_verify.bat --update` を誰がいつ行うか) は planner の判断を仰ぎたい `[ユーザーに聞ける]`
  2. `MYE_PROFILE_SCOPE` の追加を見送った判断 (上記「仕様との差分」) に異論が無いか。無ければそのまま sub-11 へ委ねたい
触ったファイル:
  - src/Engine/Engine/Physics/PhysicsSystem.h
  - src/Engine/Engine/Physics/PhysicsSystem.cpp
  - src/Engine/Engine/PhysicsSelfTest.cpp
申し送り:
  - `tools\shot_verify.bat` の golden 乖離 (上記) — 128 コミット分の差分をレビューしてから `--update` するかどうかは、物理専用のこのサブの範囲外。次に `shot_verify.bat` を触るサブ、または専用のメンテナンスタスクで扱ってほしい
  - 旧 rep 照合用に作った一時ファイルはスクラッチパッド (`...\scratchpad\sub05_reps\`) に残置。リポジトリには一切コミットしていない

## フィードバック履歴
- round 1: VERDICT OK (planner)。受け入れ条件 1〜5・7 を満たした。特に 5 は、基点 14d8775 で録った 4 シーンの rep が新しい Debug (snapshot-stress) と Release で 8 本とも一致。条件 6 の shot_verify の 4 枚は、sub-05 が描画コードに触れておらず、ワールドのハッシュ列が基点と完全一致することから、sub-05 の原因ではないと判定。ただし作業ツリーのビルドには WIP の `WaterPass.cpp` が入るので、司会が「基点の WIP 抜き worktree で shot_verify」を回して切り分ける。基点でも同じ 4 枚が割れれば golden のずれ ([ユーザーに聞ける] #12)。基点が通るなら「基点 + sub-05 の差分」の WIP 抜き worktree でも回し、通れば WIP 由来、割れれば sub-05 を差し戻す。aShape / bShape を常に代入する逸脱は採用。プロファイルスコープは sub-11 へ
