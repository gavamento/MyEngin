# sub-10: M66j: 仕上げ — engine_spec §14 Source control / README / CLAUDE.md / 規則の記載

- 依存: sub-01〜sub-09 (全部 OK 後)
- 状態: 未着手
- 往復: 0

## やること

§5 の 15 + 全検証緑。

1. **`engine_spec.md`**: §9 の表に「Source Control」行 (Changes / Branches / History、利用不可の理由) → 新節「§14 Source control (M66)」(in-process Rust DLL + JSON C ABI 6 関数、
   トランザクション、段階 A / B / C の表 (spec §4.1 の確定版)、ゲートの 13 要因、対の規則、背景 fetch の env、`error.code` 一覧、v1 の対象外) → §11.2 の表に**規則 11 (ABI ミラー、S12 の抜け) と規則 12 (Editor 層封じ込め)** → §13 の ADR 一覧に ADR-015 (sub-01 で入れていなければ)。
   既存の §13 が「ADR」なので新節番号は **§14** で、Appendix A の前に置く。
2. **`README.md`**: 「主要機能」に Source Control の段落、「ビルドと実行」に `build_managed.bat` (既存の抜け) と `build_collab.bat` + **rustup (stable) 前提** の 1 行、「CLI (検証/CI 用)」に `collab_verify.bat` / `collab_fixture.ps1`。
3. **`CLAUDE.md`**: ビルド節に `build_collab.bat` (sln 外、rustup 前提、DLL は両構成へ)、検証表に `collab_verify.bat` の行、「43 スイート」→ 44、横断チェックリストに「Collab の op を足す = `ops.rs` + `CollabProtocol.h` の op 名 + `error.code` の `Tr()` 表 + proto 版 bump は `$constGroups`」の 1 項目、環境の罠に「エディタ実行中は `MyeCollab.dll` を上書きできない」。
4. **`plans\quiet-merging-harbor.md`** の進捗表: 各サブのコミットハッシュと申し送り (計画外の事実だけ) を埋める。冒頭に「実装形態は spec (`plans\m66-git-collab\spec.md`) §4.0 が正 (別プロセス → in-process DLL)」の 1 行。
5. `.github\workflows\ci.yml` のステップ名コメントを実態に合わせる (sub-01 で書いたものの見直し)。

## やらないこと (このサブでは)

- コードの変更 (ドキュメントと注釈のみ。コードに手を入れたくなったら差し戻して該当サブへ)。

## 触る場所 (planner の見立て)

- `engine_spec.md`、`README.md`、`CLAUDE.md`、`plans\quiet-merging-harbor.md`、`.github\workflows\ci.yml` (コメントのみ)、`docs\adr\ADR-015-*.md` (追記があれば)。

## 受け入れ条件 (このサブ)

1. reviewer が spec §4.1 の表と `engine_spec.md` §14 を並べて食い違いが無い。
2. `CLAUDE.md` の検証表の全コマンドが実在し、記載のとおりに緑: `--selftest` (両構成) / `replay_verify.bat` / `shot_verify.bat` / `check_rules.ps1` / `collab_verify.bat` / `cargo test`。
3. `check_rules.ps1` 規則 10 (README / spec は対象外だが `LocalizationTable.inl` の最終状態で緑)。

## 検証コマンド

```
cd tools\collab && cargo test
tools\collab_verify.bat
cmd /c bin\x64\Debug\Editor.exe --selftest
cmd /c bin\x64\Release\Editor.exe --selftest
tools\replay_verify.bat
tools\shot_verify.bat
pwsh -File tools\check_rules.ps1
```

## 実装メモ (coder が追記)

## フィードバック履歴

## planner 追記 (sub-05〜07 の判定から)

- コードの衛生 (DocumentDirty 改名 / Scm_ComingSoon 削除 / merge_in_progress 分類) は **sub-08 へ移した**。本サブは引き続きドキュメント専用。
- engine_spec §14 に書く内容は spec §4.0 (DLL / C ABI / FreeLibrary を呼ばない理由) と §4.1 の確定版 (段階表 / 対の規則 / commit 周り / ブランチ周り / リモート周り / 競合周り / ゲート 13 要因) を正とする。元計画 `plans/quiet-merging-harbor.md` の記述ではなく spec を写すこと。
- README の「初回認証はターミナルで一度 `git push`」(GCM の GUI は背景 fetch から出ない仕様) を忘れずに。
