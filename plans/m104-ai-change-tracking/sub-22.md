# sub-22: 監視の取りこぼしを減らす (M104g2)

- 依存: sub-06
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 順序: sub-06 → **sub-22** → sub-07 (番号は末尾。sub-06 の VERDICT round 2 で足した。spec §2 D34)
- 質問の運用: 推奨で進め、Notion に後回し。

## 背景 (確認済みの事実)

- `notify` 8.2.2 の Windows 版は、読み取りバッファが 16 KiB 固定 (`windows.rs` の `BUF_SIZE = 16384`) で、あふれを報告しない。読む側が CPU を取れないと、2500 ファイルの作成でも黙って失う (sub-06 round 2 の調査。sub-05 の版でも再現)。
- 失った分は検証の走査が `gap` として拾う (sub-21)。記録は欠けないが、`gap` は時刻が分からないので帰属が弱くなる (sub-06 の受け入れ条件 21: 内容による根拠だけ)。フックの無いクライアントの変更は Unknown に落ちる。
- 今は `RecursiveMode::Recursive` でプロジェクトルートを 1 本だけ監視し、無視するパス (`.git` / `.mye` / `cache` / `dist` / `target` / `bin` / `obj` と `.gitignore`) は**届いた後で**捨てている。つまり、ビルドの出力 (`bin` / `obj` / `target`) や `.git` の書き込みの洪水が、追跡対象のファイルと同じ 16 KiB のバッファを取り合う。AI がビルドを回しながら編集する、という普通の使い方で取りこぼしが起きうる (**推測。このサブの最初に実測で確かめる**)。

## やること

**先に測る。** 手を入れる前に、今の実装で次を測って記録する (`C:\HAL\MyEngin\docs\ai-change-tracking\performance.md` に節を足す)。

1. 実測 A (洪水と編集の重なり): 一時プロジェクトで、無視されるディレクトリ (`bin\`) へ別スレッドが大量に書き続けている (例: 16 スレッドで計 64000 ファイル) 間に、追跡対象のファイルを 200 回、間隔を空けて 1 回ずつ保存する。追跡対象の 200 件のうち、通常の Revision になった数と、`gap` になった数。CPU 負荷あり / なしの両方。
2. 実測 B: sub-06 で再現した条件 (CPU 負荷 + 追跡対象 2500 ファイルの作成) での `gap` の数。

次に、安い手から入れる。

3. **監視を分ける (第 1 の手)**: プロジェクトルートを再帰で 1 本監視する代わりに、
   - ルート自身は非再帰 (`RecursiveMode::NonRecursive`) で監視する (ルート直下のファイルと、最上位のディレクトリの作成・削除・名前変更を見る)。
   - 最上位のディレクトリのうち、**無視しないもの**を 1 つずつ再帰で監視する (`notify` は `watch()` ごとに別のハンドルとバッファを持つ。着手時にソースで確かめる)。無視するもの (固定の名前と、ルートの `.gitignore` がディレクトリごと無視しているもの) は監視しない。
   - 最上位のディレクトリが増えた・消えた・名前が変わったら、監視を足す・外す。足す前に中で起きた変更は、ディレクトリ単位の突き合わせ (sub-03) で拾う。
   - 入れ子の無視ディレクトリ (このリポジトリなら `tools\track\target`、`tools\mcp\target`、`tools\collab\target`) は、親の再帰の監視に含まれてしまう。**実測 A を入れ子の場所でも測り**、取りこぼしが出るなら、無視ディレクトリを含む親を「非再帰 + 無視しない子を再帰」に分ける (深さの上限を決める)。分ける本数が増えすぎないよう、上限 (例: 監視 256 本) を置き、超えたら再帰 1 本へ戻して `status` に出す。
4. 入れた後に実測 A / B を取り直し、前後を並べる。
5. **自前の `ReadDirectoryChangesW` (第 2 の手)**: 第 1 の手の後も、実測 A で追跡対象の `gap` が出る、または実測 B が実用上問題になる数のままなら、着手する。大きいバッファ (ローカルなら 64 KiB を超えてよい) と、あふれの検出 (完了したのに 0 バイト = 取りこぼし → 即 `watcher_overflow` + 検証の走査)。coder の sub-06 round 2 の試作は「バースト直後の最初の数件が届かない」欠陥が未解決だった。**着手するなら、まずその欠陥の原因を最小の試作 (リポジトリの外) で突き止める。** 突き止められなければ実装せず、分かったことを「不安・質問」で返す (第 1 の手だけでこのサブを閉じてよい)。
6. どちらの手でも、検証の走査 (sub-21) は残す (最後の守り)。あふれを検出できるようになったら、検出した時点ですぐ走査を走らせる。

## やらないこと (このサブでは)

- `notify` の fork や、依存の差し替え (第 2 の手は `windows-sys` で自前に書く。依存を足さない)。
- 帰属の規則の変更 (sub-06)。`gap` の意味の変更。
- 作業ツリーへの書き込み。`src\` の変更。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\watch.rs`、`collector.rs`、`ignore.rs`、`bench.rs` (実測 A / B を取り直せる口にする)、`tests\watch.rs` / `tests\reconcile.rs`
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §4、`performance.md`
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表に M104g2

## 受け入れ条件 (このサブ)

1. 実測 A / B の手入れ前の値が、取り直せるコマンドと一緒に文書にある → 文書とコマンドの実行。
2. 手入れ後の実測 A で、無視されるディレクトリへの洪水の最中に保存した追跡対象 200 件が、全部通常の Revision になる (`gap` 0 件。CPU 負荷あり・なしの両方、各 3 回)。0 件にならなければ、残った数と理由を報告する (成功扱いにしない) → 実測 + `cargo test` (件数を減らした版を試験にする。時間がかかるなら `#[ignore]`)。
3. 無視するディレクトリの中の変更が、今までどおり記録されない。無視しない最上位のディレクトリを後から作る・名前を変える・消す、で中のファイルの変更が全部記録される (監視の付け替えの間の変更も、突き合わせで拾われる) → `cargo test`。
4. ルート直下のファイルの作成・変更・削除・リネームが記録される → `cargo test`。
5. `.gitignore` を書き換えて、無視していたディレクトリを無視しなくした / その逆、の後の変更が正しく扱われる (監視の付け替え) → `cargo test`。対応しないなら、その制限を文書に書き、次の起動で直ることを確かめる。
6. 監視の本数、取りこぼしの検出の数 (第 2 の手を入れた場合) が `status` で分かる → `tools\track_verify.bat` か `cargo test`。
7. 第 2 の手を入れた場合: あふれを人為的に起こす (小さいバッファの設定) と、検出されて `watcher_overflow` が残り、すぐ検証の走査が走る。バースト直後の最初のイベントが届く (試作の欠陥が無い) ことを、作成 → 即確認の試験 1000 回で確かめる → `cargo test`。入れなかった場合は対象外と書き、判断の根拠 (実測の値) を書く。
8. 既存の試験とシナリオが全部通る。全体の `cargo test` を 10 回、`--test reconcile` を 20 回、`tools\track_verify.bat` を 3 回続けて通す。sub-03 の受け入れ条件 10 (書き手が失敗しない) と 7 (作業ツリーを書かない) が成り立つ。
9. `tools\check_rules.ps1` が 0 error。`git diff --stat ca6a3991 -- src` が空 (空なら selftest / replay_verify は回さない。回していないことを書く)。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml` (10 回)、`--test reconcile` (20 回)
- `cargo test --manifest-path tools\track\Cargo.toml --release -- --ignored`
- `tools\build_track.bat Release`、`tools\track_verify.bat` (3 回)
- 実測のコマンド (文書に書いたもの)
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の要約)

- 実測 A / B は先に取った (旧版の exe を保存して比較)。A: 洪水が無視ディレクトリにあるだけで、CPU 負荷のもとでは追跡対象 200 件のうち 8〜21 件が `gap` (負荷なしは 0)。B: 計測の道具では 36 回とも 0 件で、sub-06 の取りこぼしを再現できなかった。値と手順は `docs\ai-change-tracking\performance.md` §8。
- 第 1 の手を実装: `tools\track\src\watch_plan.rs` (無視するディレクトリを含む所は非再帰、含まない子は再帰。上限 256 本、超えたらルート再帰 1 本)、`watch.rs` (付け替え `apply_watch_plan` / `replan_if_due` / `may_change_watch_plan`、`TrackerStatus.watch_count` / `watch_fallback`)、`server.rs` (`status.watches`)。分けた後の実測 A は 12 回とも `gap` 0 (入れ子 `src\sub\target` を含む)。
- 第 2 の手 (自前の `ReadDirectoryChangesW`) は入れていない: 実測 A は 0 件、実測 B は再現できず根拠が無く、sub-06 の試作の欠陥の原因も未解明。
- 試験: `tests\watch.rs` に 5 本 (`watches_are_split_around_ignored_directories`、`root_level_files_are_recorded_by_the_non_recursive_root_watch`、`top_level_directories_created_renamed_and_removed_keep_being_watched`、`gitignore_changes_move_the_watches`、`saves_during_a_flood_into_an_ignored_directory_are_never_gaps`)、`watch_plan` の単体 5 本、`tests\pipe.rs` に `status.watches` の確認、`tests\track\05_status.expected.ndjson` に `watches` を追記。
- 検証: `cargo test` 全体 10 回・`--test reconcile` 20 回・`--release -- --ignored` 1 回・`tools\track_verify.bat` 3 回・`check_rules.ps1` (0 error) すべて通過。selftest / replay_verify は `src\` を触らないので回していない。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `cargo` / `rustc` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、試験の前後で `git status` が同じ (作業ツリーは動いていない)、`Cargo.toml` / `Cargo.lock` / `schema\` に差分なし。planner が全体の `cargo test` を 2 回 (毎回 168 件、失敗なし)、`tools\build_track.bat Release` → `tools\track_verify.bat` を 1 回 (全シナリオ PASS) 実走。`mye_track bench plan --project .` はこのリポジトリで監視 76 本・fallback なし・計画 42 ms (監視は張らない)。
  - 実測 A の前後 (負荷ありで `gap` 8〜21 件 → 12 回すべて 0 件) は coder の実測と performance.md §8 による。planner は再実行していない。
  - 第 2 の手 (自前の `ReadDirectoryChangesW`) を入れなかった判断は採る。sub-22.md が「突き止められなければ第 1 の手だけで閉じてよい」としており、実測 B は 36 回 0 件で、入れる根拠になる取りこぼしを再現できていない。受け入れ条件 7 は対象外。
  - 追加 (`bench load` / `bench plan`、付け替えの間隔の下限、付け直しの後の検証の走査、`05_status` の期待値) は採る。
  - 質問への回答: (1) 第 2 の手は今は入れない。1 つのディレクトリへの一括作成で取りこぼしを再現できる条件が見つかった時点で再検討する (検証の走査と D35 が最後の守り)。(2) 200 ms / 8 倍 / 256 本は初期値でよい。数万ディレクトリでの計画づくりの時間は sub-19 の計測の項目に足す。(3) そのとおり。このサブが解いたのは洪水との取り合いで、一括作成の取りこぼしではない。
  - nit: `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` 25 行目 (M104g の行) の `tools\track` がタブに化けている (sub-06 の時の書き込み)。sub-07 が進捗表を更新するときに直す。