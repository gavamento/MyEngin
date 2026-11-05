# sub-10: 予約マーカーの整理と文書 (review-1 #3 / #4、#1 / #2 の文書)

- 依存: sub-08, sub-09 (両方の統合後の master)
- 状態: 未着手
- 往復: 0

## やること

1. 予約マーカーを整理する (spec §2 #28)
   - 中身の無いマーカーを消す: `SelfTestSuites.cpp:241` の `// ---- M95 ----`、`SkeletonSelfTest.cpp:1079` の `// ---- M95 sub-05 ----`、その他 `grep -rn -A1 "// ---- M95" src` で直下が空行や `};` のもの
   - 中身のあるマーカーは `// ---- M95: <機能名> ----` に書き換える (例: `// ---- M95: ヒットストップ ----`、`// ---- M95: CC 同士の押し出しと接触通知 ----`、`// ---- M95: ルートモーションの縦 ----`)。サブ番号と「末尾 append」は入れない
   - マーカーの直後にある説明コメント (`Components.h:825` 等) は残す
   - `LocalizationTable.inl` の `// ---- M95 ----` は文字列表の見出しとして残してよい
2. 文書を更新する
   - `engine_spec.md` §10.14
     - A: 停止中の縦 (rootMotionY の層があれば高さを保つ。無ければ重力)
     - D: 障害物への押し付け (CC 同士の押し出しは障害物への掃引で切る。押す側は相手が動ける分 (room) だけ進み、相手の手前で押し続ける) と既知の制限 3 つ (伝播は 1 段、壁沿いには滑らせない、押す側自身の移動は掃引しない = 高速では押す側が相手の向こうへ抜ける。受ける側が壁を抜けない保証は速さに依らない)
     - E: 「ヒットストップ中は高さを保つ」を 1 文
   - `docs\adr\ADR-034-character-contacts.md` の影響欄: 押し付けの規則と既知の制限
   - `docs\test_checklists.md` の M95 節: ボスの体当たりで壁際の相手が壁を抜けず、ボスが相手の手前で押し続けること、跳び込み斬り中のヒットストップで宙に止まることの目視 2 項目
3. 残り minor の文書 2 件 (review-1 #4 (5))
   - `docs\dogfooding.md:20` の台帳見出し「(M70d 時点)」を M95 時点へ
   - `engine_spec.md` §12.2 の期間「2026-07-19 → 2026-10-12」を M95 の完了日まで延ばす

## やらないこと (このサブでは)

- コメント・文書以外の変更 (コードの挙動は 1 命令も変えない)

## 触る場所 (planner の見立て)

- `grep -rn "// ---- M95" src` の全ヒット (統合後に sub-08 / 09 が足した行も含む)
- `engine_spec.md` / `docs\adr\ADR-034-character-contacts.md` / `docs\test_checklists.md` / `docs\dogfooding.md`

## 受け入れ条件 (このサブ)

spec §5 の 46。

- `grep -rn "// ---- M95 sub-" src` が 0 件
- 中身の無いマーカーが 0 件
- `git diff` のコード側の `+` / `-` 行がコメント行か空行だけ
- 文書が 42〜44 の規則と既知の制限を書いている
- 文書の参照先が実在する

## 検証コマンド

- `grep -rn "// ---- M95" src` と `grep -rn -A1 "// ---- M95" src` の出力を貼る
- `git diff -U0 -- src | grep -E '^[+-][^+-]' | grep -vE '^[+-]\s*(//|$)'` が空であること (コメントと空行以外の変更が無い)
- ビルド: Debug (コメントだけでもビルドが通ることの確認)
- `tools\check_rules.ps1`
- selftest / replay_verify は回さない (挙動の変更が無い。sub-08 / 09 の統合時の full 検証が最新)

## 実装メモ (coder が追記)

SELF_EVAL: sub-10 (round 1)
- マーカー 18 箇所を機能名へ (`M95: <機能名>`)、中身の無い 2 箇所 (SelfTestSuites.cpp / SkeletonSelfTest.cpp) を削除。文書: engine_spec §10.14 A/D/E と §12.2、ADR-034 影響欄、test_checklists M95 節に 2 項目、dogfooding 見出し。
- 検証: コード差分のコメント・空行以外の変更 0 行、`grep "M95 sub-" src` 0 件、Debug ビルド OK、check_rules 0 error / 50 warning。selftest / RV は仕様どおり未実行。


round 2 (司会が SELF_EVAL から転記): 既知の制限 (1) を「掃引は障害物だけを見て CC を含まない。ボス → A → B → 壁では B は壁を抜けないが、ボスは B で止まらずに A を B へ押し込み、A と B は重なり得る」に直した (engine_spec.md §10.14 D の英文と ADR-034 の影響欄)。旧文面は 0 件、check_rules 0 error。文書のみのためビルドと selftest は未実行 (planner 指定)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-11-05)。must 1 件: 既知の制限 (1) の説明が挙動と逆。engine_spec §10.14 D の "a CC pushed into a third CC does not push it on" と ADR-034 の「押された CC が第三の CC を押し出すことはない」は誤り。実際は掃引の相手が障害物だけで CC を含まないので、ボス → A → B → 壁では B は (受ける側の掃引で) 壁を抜けないが、ボスの room は A の前の障害物しか見ないので B で止まらず、ボスは A を B へ押し込み続け、A と B は通常の CC 同士の押し出しで押し合って重なり得る (spec §4.1 D の既知の制限の文)。両方の文書をこの意味に直す。それ以外 (マーカー 18 箇所の書き換え・空 2 箇所の削除、コード差分がコメントのみ、A / E / D の他の文、§12.2 の期間、dogfooding 見出し、test_checklists 2 項目) は OK。D の障害物の種類 (床・壁・kinematic・dynamic) は sub-08 のコード (bodies の solid を全部見る) と一致。round 2 の検証は grep で該当文の差分を貼るだけ (ビルド・CR は文書のみなので CR だけ)
- round 2: VERDICT OK (planner、2026-11-05)。既知の制限 (1) を engine_spec §10.14 D と ADR-034 の両方で「掃引は障害物だけを見て CC を含まない。ボスは B で止まらず A を B へ押し込み、A と B は重なり得る」に直したことを grep で確認、旧文面 0 件、CR 0 error
