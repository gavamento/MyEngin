# sub-10: M66j: 仕上げ — engine_spec §14 Source control / README / CLAUDE.md / 規則の記載

- 依存: sub-01〜sub-09 (全部 OK 後)
- 状態: OK (commit: M66j — ハッシュは harness.md のサブ進捗表)
- 往復: 1

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

### round 1 (SELF_EVAL の写し)

実装:

- `engine_spec.md` §14 Source Control (M66) — §13 の後・Appendix A の前に新設。spec §4.0〜§4.4 の
  確定版を写した: 14.1 実装形態 (図 / C ABI 6 関数 / JSON / proto 版と bump 規則 / `catch_unwind` /
  `FreeLibrary` を呼ばない理由 / 両構成へ配置 / rustup 前提と NoService 縮退) / 14.2 op 一覧
  (Handshake 1 + Read 10 + Write 12 とタイムアウト) + event 4 + `error.code` 18 + 疑似 `timeout` +
  Unavailable 8 / 14.3 ゲート 13 要因の表・トランザクションの順序・段階 A/B/C の表・対の規則・
  commit 周り / 14.4 リモートと競合 / 14.5 検証 5 手段 / 14.6 既知の制約。
- `engine_spec.md` §9 — 「Source Control」行 (3 タブ + Diff 窓 + ゲート理由の全列挙 + 利用不可 8 値)。
  Asset Browser 行に git バッジの 1 句。
- `engine_spec.md` §11.2 — 規則 11 (ABI ミラー = S12 の抜け) と規則 12 (Editor 層封じ込め) を追加。
- `engine_spec.md` §13 — ADR-015 の参照を「(M66)」→「(§14)」。
- `README.md` — 「ビルドと実行」に sln 外の 2 本 (`build_managed.bat` / `build_collab.bat` +
  rustup stable)、「主要機能」に Source Control の段落、「CLI」に `collab_verify.bat` /
  `collab_fixture.ps1`。
- `CLAUDE.md` — ビルド節に `build_collab.bat`、検証表に `cargo test` と `collab_verify.bat` の 2 行 +
  「43 スイート」→ 44 + check_rules 行に規則 12、決定論の契約「規則 1-11」→「1-12」、
  横断チェックリストに「Source Control の op を足す」、UI 文字列節に themeColor 固定テストの 1 行、
  環境の罠に 5 件 (cargo の PATH / edition 2021 / linker_messages / DLL 上書き不可 /
  期待 NDJSON の禁則と bare origin / PNG 破損 / 未知拡張子の `.meta`)。
- `plans\quiet-merging-harbor.md` — 冒頭に「実装形態の正本は spec §4.0」、進捗表 10 行を
  「計画外の事実だけ」で記入。
- `.github\workflows\ci.yml` — rust-toolchain ステップに `name:`。**他は 1 文字も触っていない**。

sub-10 宛 6 項目の消込: (1) ci.yml の `name:` → 実施 / (2) README に初回認証 → 主要機能の段落 /
(3) `SoundGenWindow` の `.wav` にヒント無し → §14.6 / (4) バッジ引きコスト未実測 → §14.6 /
(5) themeColor の固定テスト → CLAUDE.md UI 文字列節 / (6) acoustic は触らない → ci.yml の
差分は `name:` 1 行のみ・CLAUDE.md の「わざと立てていない」は無変更 (`git diff` で確認)。

仕様との差分:

- [追加] §9 の Asset Browser 行に git バッジの 1 句 (M66i で見た目が変わり表が実態と食い違うため)。
- [追加] §13 の ADR-015 参照を節番号へ (他の ADR 行と体裁を揃えた)。
- [逸脱] §14 は英語で書いた (`engine_spec.md` の本文が英語。README / CLAUDE.md / 計画は日本語)。
- [未実装] README の既存の陳腐化 (「6 シーン被覆」「スクショ 15 枚」= 現在は 7 / 19) は M66 の
  範囲外として触っていない。

検証 (すべて緑): `cargo test` 65 passed / 0 failed → `collab_verify.bat` 9 シナリオ PASS →
`--selftest` Debug・Release とも exit 0 → `replay_verify.bat` 10 ジョブ PASS (128.3 s) →
`shot_verify.bat` 19 枚 PASS (**acoustic 2 枚は初回で緑**、再実行不要) →
`check_rules.ps1` 0 error / 0 warning。

## フィードバック履歴

## planner 追記 (sub-05〜07 の判定から)

- コードの衛生 (DocumentDirty 改名 / Scm_ComingSoon 削除 / merge_in_progress 分類) は **sub-08 へ移した**。本サブは引き続きドキュメント専用。
- engine_spec §14 に書く内容は spec §4.0 (DLL / C ABI / FreeLibrary を呼ばない理由) と §4.1 の確定版 (段階表 / 対の規則 / commit 周り / ブランチ周り / リモート周り / 競合周り / ゲート 13 要因) を正とする。元計画 `plans/quiet-merging-harbor.md` の記述ではなく spec を写すこと。
- README の「初回認証はターミナルで一度 `git push`」(GCM の GUI は背景 fetch から出ない仕様) を忘れずに。
- **acoustic golden のフレーク (sub-08 で発見、spec §2 S14 / §7)**: ユーザー判断 = **c. 何もしない** (2026-09-03)。本サブでは `ci.yml` も CLAUDE.md の「わざと立てていない」の記述も**触らない**。全検証を回すとき acoustic 2 枚が赤くなったら `shot_verify.bat` を再実行して通す (受け入れ条件 2 は「再実行で緑」を許容)。根治は M66 の外。
- [申し送り、sub-09 から] `SoundGenWindow` の `.wav` 書き出しには保存ヒントが無い (監視で 300 ms 後に反映)。数百タイル時のバッジ引きコスト (~1 ms/frame 見込み) は未実測。どちらも v1 では許容 — engine_spec §14 の「既知の制約」に 1 行ずつ書く。
- [申し送り、sub-09 から] themeColor の新しい割り当てを足すときは (d3) と同型の固定テストを添える (配色ルール違反は画面では「なんとなく読みにくい」としか出ない)。CLAUDE.md の UI 文字列の節の隣に 1 行あってよい。
- round 1: VERDICT OK (planner、2026-09-04)。裏取り: engine_spec.md:2066 `## 14. Source Control (M66)` + 14.1〜14.6、Appendix A の前 / :1622-1623 に規則 11・12 / :914 §9 の Source Control 行 / 件数は実コードと一致 — `ops.rs::dispatch` 23 op、`protocol.rs` の `&str` 定数 22 = error.code 18 + event 4、`GateBlocker` 13 + Count / CLAUDE.md:37 「44 スイート」、:28 build_collab + rustup 前提、:42 collab_verify 行、:96 規則 1-12、:51 「わざと立てていない」は不変 (ユーザー判断 c どおり) / README.md:25 rustup、:76 初回認証の案内 / `git diff HEAD -- ci.yml` = `name: rust toolchain (stable)` の追加のみ / quiet-merging-harbor.md:323-332 進捗表 10 行にハッシュと計画外の事実。差分 5 件 (§9 Asset Browser の 1 句 / §13 の参照体裁 / §14 は英語 / README の陳腐化は範囲外 / ci.yml コメントは実態と一致) はすべて採用。
