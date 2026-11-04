# sub-03: 部位への球の重なり・カプセルの重なり・球の掃引 (ABI v32 の残り 3 本 = 193)

- 依存: sub-01 (ABI v32 の宣言とスロット順)、Wave 1 の統合 (Wave 2。sub-05 と並列)
- 状態: 未着手
- 往復: 0

## 作業場所の注意 (並列実装)

- **司会が渡した worktree の絶対パスでだけ作業する。master のチェックアウト (`C:\HAL\MyEngin`) へは 1 バイトも書かない。** 最初と最後に両方で `git status` を確認し、SELF_EVAL に貼る
- `PartSelfTest.cpp` への追記は `// ---- M95 sub-03 ----` の直下。`EngineAPI.h` / `Interop.cs` は sub-01 の 2 本の**次**に 3 本 (付録 A.1 の順)
- 同じ波の sub-05 のファイル (`AnimatorController.*`、`Components.*`、`PhysicsSystem.cpp`、`SkinningSystem.cpp`) を触らない

## やること

spec §4.1 B。`Parts::OverlapSphereParts` / `Parts::SphereCastParts` / `Parts::OverlapCapsuleParts` を `RaycastParts` と同じ候補収集・決定論規約で足し、
ABI v32 の 191〜193 本目を開け、C++ 糖衣 / C# ミラー / `MyeScript.cs` 糖衣を揃える。

## やらないこと (このサブでは)

- カプセルの掃引 (CapsuleCast)
- ABI 版の変更 (sub-01 で 32 済み)。`PartSelfTest` の v32 充填チェックを 5 本に増やすだけ
- `SphereCastWorld` の挙動変更。共通化するなら演算順を保つ (spec §7)

## 触る場所 (planner の見立て)

- `src\Engine\Engine\Animation\Parts.h:60-75` 宣言 3 本 (`PartRayHit` を再利用)
- `src\Engine\Engine\Animation\Parts.cpp:118-192` `RaycastParts` の候補収集 (`Cand` 構造体 → index 昇順 sort → root 配下 / アクティブ / 縮退の除外) を関数に切り出して 4 本で共有してよい (切り出しは `RaycastParts` の結果を変えないこと)
  - 重なり: `shapes::Overlap(pose, MakePartBoundsPose(...))` (`Shapes.h:91`)。球 pose は `shape = collidershape::kSphere`、`identityRot = 1`、`radius`。カプセル pose は `shape = kCapsule`、中心 = 中点、`halfSeg = |p1 − p0| / 2`、軸 = `normalize(p1 − p0)` を**カプセルの軸の基底**に入れる (どの基底が軸かは `Shapes.cpp:314` `CapsuleCapsule` / `:383` `CapsuleBox` を読んで合わせる。CC は単位基底で Y 軸 = 軸なので `by` の見込みだが確認する)。残り 2 軸は軸に直交する正規直交基底を決定的に作る (例: 軸と最も離れた座標軸との外積)。`|p1 − p0| < 1e-6f` は球
  - 掃引: `src\Engine\Engine\Physics\Rigid\PhysicsQueries.cpp:166-215` の対象 1 件分 (sphere = 膨張レイ、box = 32 回の保守的前進、`1e-4f`、`ClosestPointOnShape`、始点が形状内は報告しない) を同じ定数で書く
- `src\Shared\EngineAPI.h` `AnimatorGetPause` の次に 3 本 (順: `OverlapSphereParts` / `SphereCastParts` / `OverlapCapsuleParts`)、版コメント v32 の行を確認 (sub-01 が 5 本書いていなければ追記)
- `src\Engine\Engine\Script\EngineApiTable.cpp:1019-1035` `RaycastParts` の隣に実装 (総数 / cap の規約は `FindPartsByTag` `:1005-1017`)
- `src\Shared\ScriptAPI.h:728` `MyeRaycastParts` の隣に糖衣 3 本 (`MyePartTag` でタグ名 → ハッシュ)
- `src\Scripting\Interop.cs` の `// ---- v32 (M95) ----` の下、sub-01 の 2 本の次に関数ポインタ 3 本と静的糖衣 (`:904-915` の様式)、`src\Scripting\MyeScript.cs:730-737` の隣に糖衣 (root 省略版 / あり版)
- `src\Editor\SelfTest\PartSelfTest.cpp` の `// ---- M95 sub-03 ----` 直下に 14 / 15 / 31、sub-01 の v32 充填チェックを 5 本に
- `tools\check_rules.ps1:746` の `$apiVersionSlots` の `32 = 190` (sub-01 が追加) を `32 = 193` へ (規則 11-c。版とスロット数の表を同時に更新しないと check_rules が error で止まる。sub-01 VERDICT round 1 で追記)

## 受け入れ条件 (このサブ)

spec §5 の 14, 15, 16, 31。
- 14: 球 / 箱の部位を並べ、重なる集合が index 昇順、tag と root の絞り込み、非アクティブ・縮退 (halfExtents 0) の除外、`radius <= 0` で 0 件、`out = null` で総数だけ、cap 切り捨てで戻り値は総数
- 15: 球部位 (解析値と一致) と箱部位 (1e-3 以内) のヒット、同距離 2 部位で低 index、`radius <= 0` が `RaycastParts` と同じ `distance`、`maxDist` 超えで 0
- 31: カプセル (軸が斜め) で球・箱の部位にヒット、箱の角で当たる / 当たらないの境界、`p0 == p1` が球と同じ集合、`radius <= 0` で 0 件、総数と cap
- 16: ABI 経由で同じ結果 (`PartSelfTest` の ABI 様式、v32 = 193 スロット)、`Interop.cs` の順の機械照合 (sub-01 と同じレシピ) を SELF_EVAL に貼る

## 検証コマンド

- ビルド Debug / Release (worktree。GameLogic.dll も)
- 往復中: `$env:MYE_SELFTEST_ONLY='part,twodabi'; bin\x64\Debug\Editor.exe --selftest`
- SELF_EVAL を返す前: `bin\x64\Debug\Editor.exe --selftest` (既定層)
- VERDICT OK の直前 (最後の round で 1 回): `bin\x64\Debug\Editor.exe --selftest-full` と `bin\x64\Release\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat` (`SphereCastWorld` を共通化した場合は必須。しなかった場合も回す)

## 実装メモ (coder が追記)

## フィードバック履歴
