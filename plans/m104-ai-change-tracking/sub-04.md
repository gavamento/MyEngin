# sub-04: 行 diff、Hunk、Revision、リネーム (M104e)

- 依存: sub-03
- 状態: 未着手
- 往復: 0

## やること

arch §5 を作る。sub-15 の Hunk 復元はここで作る Hunk の形に乗るので、**Hunk が Before と After を正確に再構成できること**がこのサブの中心。

1. Myers 法の行 diff をサービス内に実装する (依存を足さない)。入力は Before / After の Blob、出力は Hunk の列 (前後 3 行の文脈)。
2. 行の区切りは LF / CRLF / 末尾改行なしをそのまま保持する。`before_text` / `after_text` / `context_pre` / `context_post` は改行込みのバイト列を保てる形で持つ (db §3 は TEXT。UTF-8 として読めたファイルだけが行 diff の対象なので TEXT で足りるが、改行を正規化しないこと)。
3. 改行だけが変わった Hunk は `eol_only = 1`。本文の変更と同じ Hunk に混ざる場合の扱いを決めて文書に書く (推奨: 行ごとに判定し、Hunk の全行が改行だけの違いなら `eol_only`)。
4. Diff ワーカー 1 本 (arch §3.1)。結果は Writer へ渡す。Revision の確定は「スナップショット保存 → 差分生成 → Revision 確定 → DB 登録」(要求 §5.2) の順で、Hunk の無い Revision が外から見える瞬間を作らない (1 トランザクション、または「未確定」の状態を外へ出さない)。
5. Hunk ID は (Revision, 通し番号)。Revision が変われば Hunk ID も変わる。
6. Change と Revision: 同じパスへの変更は 1 つの Change にまとめ、観測ごとに Revision を足す (db §3.1)。新しい Revision ができたら、前の Revision の状態の扱い (`changes.state` の `superseded`) を db のとおりにする。
7. リネームのまとめ (sub-01 の試作 3 で確定: 別のディレクトリへの移動は `Remove` + `Create` でしか届かないので、ハッシュによるまとめが**主たる手段**。ディレクトリの名前変更で sub-03 が作った「中のファイルの削除と作成」も同じ規則でまとまる): 監視がリネームとして返したものに加え、同じデバウンス窓の中の「削除 + 同じ内容ハッシュの作成」を 1 件の `renamed` にする。内容が違えば別々の削除と作成のまま (推測でつながない)。同じハッシュの候補が 2 つ以上あるときはつながない。
7b. 派生の表の作り直し (sub-02 の VERDICT で追加): DB が壊れて作り直した後、`events` (ジャーナルから戻ったもの) と残っている Blob から、`changes` / `change_revisions` / `change_hunks` / `snapshots` を作り直す関数を持つ。Hunk は Blob から計算し直せる。Blob が欠けている Revision は行だけ戻し、`blob_state = missing` にする。起動時の検査 (db §6 の 1) の後に呼ぶ。通常の記録の経路と作り直しの経路が同じ関数を通る形にする (2 通りの実装を持たない)。
8. 大きい入力の上限: 行数 / サイズが大きいとき、Myers の最悪計算量で Diff ワーカーが止まらないよう上限を置き、超えたら「ファイル全体を 1 Hunk」に落として、その旨を Revision に残す。上限の値は計測して決め、文書に書く。

## やらないこと (このサブでは)

- pipe 越しの `hunks` op (sub-05 は `describe` の要約まで、本文付きの `hunks` は sub-09 か sub-10 の着手時に、サービス側の op として足す。骨子の見直しで確定する)。
- シーンの構造 diff (sub-11)。JSON も、このサブではただのテキストとして行 diff する。
- Hunk の適用 (復元) の本実装。ただしテスト用に「Hunk の列を Before に当てて After を得る」関数を lib に置く (sub-06 の `declared_edit_match` と sub-15 の検証でも参考になる)。

## 触る場所 (planner の見立て)

- `C:\HAL\MyEngin\tools\track\src\` に `diff.rs` (Myers と Hunk の組み立て)、`revision.rs`、`rename.rs`、`tests\diff.rs`。
- `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §5、`db-schema.md` §3 — 実装で決めた点 (eol の扱い、上限)。
- `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` — 進捗表。

## 受け入れ条件 (このサブ)

1. 1 ファイルの離れた 3 か所を変えると Hunk が 3 つでき、それぞれの `old_start` / `old_count` / `new_start` / `new_count` と文脈 (最大 3 行、ファイルの端では短くなる) が正しい → `cargo test`。
2. **性質テスト**: 固定 seed で生成した多数の (Before, After) の組について、Hunk の列を Before に当てた結果が After とバイト一致し、逆向き (After に逆適用) で Before とバイト一致する。seed はテストに固定で書く → `cargo test`。
3. 境界: 空 → 内容あり、内容あり → 空、1 行、末尾改行の有無だけの違い、CRLF と LF の混在、全行置き換え、先頭・末尾だけの変更、日本語と絵文字を含む行 → `cargo test`。
4. CRLF → LF だけを変えたファイルは `eol_only` の Hunk になり、本文の変更として数えられない → `cargo test`。
5. 同じ窓の中の「削除 + 同じ内容の作成」が 1 件の `renamed` (`old_path` 付き) になる。内容が 1 バイトでも違えば 2 件。同じ内容の候補が 2 つあればつながない → `cargo test`。
5b. 別のディレクトリへの移動 (1 ファイル) と、3 ファイルを含むディレクトリの名前変更が、それぞれ `renamed` 1 件 / 3 件になる。同じ内容のファイルが 2 本あるディレクトリの名前変更は、候補が一意に決まらないので、パスの対応 (ディレクトリ名の前方一致) で決められる場合だけつなぎ、決められなければつながない → `cargo test`。
6. 同じパスを 2 回変えると Change は 1 つ、Revision は 2 つで `seq` が連番。2 つ目の Hunk ID は 1 つ目と違う → `cargo test`。
7. 上限を超える入力で Diff ワーカーが時間内に返り、Revision に「全体を 1 Hunk に落とした」ことが残る → `cargo test` (時間の上限付き)。
8. sub-03 の受け入れ条件 7 (作業ツリーを書かない) がこのサブのシナリオでも成り立つ → `cargo test`。

9. 記録した後に `tracking.db` を壊して起動し直すと、退避 → 作り直しの後で、壊す前と同じ Change / Revision / Hunk (パス、種別、ハッシュ、Hunk の範囲と本文) が得られる。Blob を 1 つ消しておくと、その Revision は行が戻り `missing` になる → `cargo test`。

## 検証コマンド

- `cargo test --manifest-path tools\track\Cargo.toml`
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

## フィードバック履歴
