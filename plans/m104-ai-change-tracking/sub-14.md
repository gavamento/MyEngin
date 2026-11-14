# sub-14: レビュー (M104o)

- 依存: sub-12、sub-13
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **骨子** (spec §6.1)。Phase 4 はこの 1 本。

## やること

要求 §10、db §3.1 (reviews)、ADR-039 決定 11。完了条件は「承認情報が別 Revision へ誤って引き継がれない」。

1. サービス: op `review_set` (`role = editor` だけ。`mcp` / `hook` は `forbidden`)。`reviews` は**追記だけ**。今の状態は「その範囲の最新の行」。Reopen は `reopened` の行を足す。
2. 単位: Session / Operation / File (Revision) / Hunk / Entity / Component / Field。Session と Operation は「その下の全 Revision へ個別に記録する」か「親の単位の行を持つ」かを着手時に決める (db の `scope_kind` は revision / hunk / entity / component / field の 5 つで、親の状態は保存しない設計。前者が設計に沿う)。
3. 集約: 親 (セッション・操作・ファイル) の状態は子の最新の状態から問い合わせのたびに計算する。保存しない。一部だけ承認なら Partially Approved。集約の規則 (Rejected と Approved が混ざる、NeedsReview が 1 つある、など) を表にして db か attr に書く。
4. 新しい Revision は必ず Pending から。前の Revision の承認は前の Revision に残り、動かない。
5. Editor: `ReviewManager` と、AI Changes 窓の操作 (承認・却下・要確認・再開、メモ)、状態の表示、`list` の `review` フィルタ。
6. 承認は git の操作を何も起こさない。却下は変更を消さない (確認文と表示で明示する)。

## やらないこと (このサブでは)

- 復元 (sub-15、16)。却下から復元への自動の導線は「復元の画面を開ける」案内まで。
- MCP からの承認 (要求 §12.6)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `review.rs`、`tests\review.rs`、`C:\HAL\MyEngin\tests\track\`
- `C:\HAL\MyEngin\src\Editor\ChangeTracking\ReviewManager.*`、`AiChangesWindow.*`、`LocalizationTable.inl`

## 受け入れ条件 (このサブ)

1. Revision 1 を承認した後に同じファイルが変わると、Revision 2 は Pending で、Revision 1 の承認の行は変わらない → `cargo test` (要求 §16.4 の 4 番目)。
2. 承認 → 再開 → 却下で `reviews` に 3 行あり、どの行も書き換えられていない → `cargo test`。
3. Hunk 3 つのうち 1 つを承認するとファイルは Partially Approved、全部で Approved。集約の表の全行にテストがある → `cargo test`。
4. 却下の前後で、作業ツリーの全ファイルのハッシュと `.git\index` が変わらない → `cargo test` + スイート (§16.4 の 5 番目)。
5. `role = mcp` と `role = hook` の `review_set` が `forbidden` → `tools\track_verify.bat`。
6. Entity / Component / Field の単位の記録が、`documentId` と組で区別される → `cargo test`。
7. 窓の操作のスクショ (英日) → `--screenshot`。
8. A20 (Phase 3〜4 の横断): `tools\check_rules.ps1` が 0 error、`Editor.exe --selftest-full` が通る、追跡を有効にした場合と無効にした場合で `tools\replay_verify.bat` が一致、ABI・SimSnapshot・TypeId が変わっていない (spec §2 D20)。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`
- ビルド、`Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs`

## 実装メモ (coder が追記)

## フィードバック履歴
