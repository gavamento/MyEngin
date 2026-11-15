# sub-15: 復元の判定 — ゲート、競合の検証、パスの検査、プレビュー (M104p)

- 依存: sub-26 (順序は sub-14 → sub-26 → sub-15 → sub-27 → sub-16)
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-26 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。
- 分割: 元の M104p (ファイルと Hunk の復元) を 2 本に割った。**このサブは「戻してよいか・何を書くことになるか」を決めて見せるところまでで、作業ツリーを 1 バイトも書かない。** 実際に書く・ジャーナル・巻き戻し・回復は **sub-27 (M104p2)**。

`C:\HAL\MyEngin\docs\ai-change-tracking\restore-safety.md` §1〜§3、§7、§8 (restore)。要求 §11。安全性は P0。

## なぜ先に判定だけを作るか

復元は、この機能で唯一、利用者のファイルを書き換える。書く前の判定 (ゲート、競合、パス) が誤っていると、書く部分がどれだけ丁寧でも作業を壊す。判定を「書かない」サブとして先に閉じ、純粋な関数と固定の入出力で固めてから、書く部分 (sub-27) を載せる。

## 使えるもの (Phase 4 まで)

- サービス: `describe` (Before / After のハッシュとサイズ、`diff_mode`、`reviewable`、行為者)、`hunks` (本文つき、`state`)、`blob_get` (512 KiB ずつ、原本とバイト一致。欠ければ `blob_missing`)、`scene_diff`。
- Editor: `TrackClient::Request`、`ChangeModel`、`DiffViewer` / `DiffRows` (`C:\HAL\MyEngin\src\Editor\Widgets\DiffView.h`。Hunk の選択つき。追跡に依存しない)、`ReviewManager` (sub-14)、`Sha256Hex` / `Sha256FileHex`。
- ゲート: `C:\HAL\MyEngin\src\Editor\SourceControl\GitTransaction.h` の `GateBlocker` (SceneDirty / ActorEdit / AnimationDirty / ControllerDirty / MixerDirty / BehaviorTreeDirty / ProjectSettingsDirty / SpriteEditorDirty / TilePaletteDirty / Playing / NetActive / BuildRunning / ScriptBuildRunning / OpInFlight / MergeInProgress / ServiceUnavailable)、`GateInputs` (同じ名前の bool)、`ComputeBlockers(const GateInputs&)` (純粋な関数)。入力の組み立ては `EditorApp::BuildGateInputs` (private、`EditorApp.cpp` 2029 行目付近)。
- 最初に `tools\build_track.bat Release` を回す (sub-26 が `tools\track\src` を変えたが、`bin\x64\` の `mye_track.exe` は置き直されていない。`MYE_TRACK_PROBE` の結合はこの exe を使う。サービスが動いていないことを確かめてから)。

## やること

### A. 復元の対象と計画 (`src\Editor\ChangeTracking\RestorePlan.h/.cpp`、新規。ImGui も I/O も呼ばない)

1. 対象の単位 (このサブと sub-27 が扱うのはファイルの側だけ): **ファイル全体** (1 つの Revision を Before へ戻す) と **Hunk** (1 つの Revision の中の、選んだ Hunk だけを戻す)。シーンの Entity / Component / Field は sub-16。
2. 対象にできる Revision: 行為者が `ai` か `unknown` (`reviewable`)。`editor_user` と `restore` は対象にしない (Editor の利用者自身の編集は Undo と git で戻す。復元の復元は、元の Revision を選び直す)。`changes.state` が `superseded` の変更、`diff_mode` が `binary` / `excluded` / `unavailable` の扱いは下の B。
3. 計画 = 「どのパスに、どんなバイト列を書く (または消す・作る・名前を変える) ことになるか」と「そう判断した根拠 (照合の結果)」の一覧。計画を作る関数は純粋: 入力は (Revision の情報、Hunk の列、Before / After の内容、**今のファイルの内容**、今そのパスに何があるか)、出力は計画か、競合の理由。今の内容の読み取りは呼び手が行って渡す。

### B. 競合の検証 (restore §3.1、§3.2。純粋な関数)

**ファイル全体**

| 今の内容 | 判定 | 計画 |
|---|---|---|
| 記録した After と同じ (ハッシュ) | 一致 | Before の内容を書く |
| 記録した Before と同じ | 復元済み | 何も書かない (「既に戻っている」と出す) |
| どちらとも違う | 競合 | 書かない。今・Before・After を見せる |

- 追加された変更 (Before が無い) の復元は**削除**。今の内容が After と一致するときだけ。確認の文面で削除と明示する。
- 削除された変更 (After が無い) の復元は**作成**。今そのパスに何も無いときだけ。何かあれば競合。
- リネームの復元は、新しい名前の内容が After と一致し、**かつ**古い名前に何も無いときだけ。
- `no_before` (変更前の内容を見ていない Revision) と、Before の Blob が欠けている (`blob_missing` / 除外) Revision は、戻す先の内容が無いので復元できない (理由を出す)。
- バイナリは、ファイル全体の復元だけ (ハッシュの一致で判定)。Hunk は無い。
- 比べるのはバイト列 (ハッシュ)。改行や空白の違いを無視して合わせない。

**Hunk** (restore §3.2)

- Hunk は (前の文脈, After 側の本文, 後ろの文脈) の並びとして、今のファイルの中で探す。

| 探した結果 | 判定 |
|---|---|
| 記録した行番号の位置で一致 | 一致 |
| 別の位置でちょうど 1 か所だけ一致 | 一致 (位置がずれたことを確認の画面に出す) |
| 2 か所以上で一致 | 競合 |
| 一致なし | 競合 |

- 文脈が 0 行の Hunk (ファイルの先頭・末尾、全体の置き換え) は、位置がずれた一致を認めない。
- 複数の Hunk をまとめて戻すときは、後ろの Hunk から順に当てる。**1 つでも競合なら、そのファイルは 1 つも当てない。**
- 戻した結果の改行の種類は、Before の本文のまま (Hunk の本文は改行込み)。`eolOnly` の Hunk も同じ規則。
- `diff_mode = whole` (全体を 1 Hunk に落とした大きいファイル) と、本文が `oversize` で届かない Hunk は、Hunk 単位では戻せない (ファイル全体の復元だけ)。
- 既に戻っている Hunk (今のファイルに Before 側の本文がその位置にある) は「復元済み」。

**Rust と同じ結果になること**: サービス側の `apply_hunks` / `unapply_hunks` (`tools\track\src\diff.rs`、sub-04) と、Editor 側の Hunk の逆適用が、同じ入力で同じバイト列を出す。**固定の入出力の表を 1 つのファイル (`C:\HAL\MyEngin\tests\track\fixture\hunk_apply_cases.json` など) に置き、Rust の試験と C++ のスイートの両方が読む** (projectId の固定表と同じやり方)。表には、通常・位置ずれ・競合 (2 か所、一致なし)・文脈 0 行・CRLF と LF の混在・末尾改行なし・日本語・複数 Hunk の後ろからの適用、を入れる。

### C. ゲート (restore §2)

1. `EditorApp::BuildGateInputs` を、`RestorePlan` の判定からも使える形にする: `EditorApp` が `GateInputs` を返す関数を `ChangeTracking` 側へ渡す (`AiChangesHost` に足す。`ChangeTracking\` から `EditorApp` を include しない)。`ComputeBlockers` はそのまま呼ぶ。`SourceControl\` のヘッダ (`GitTransaction.h`) を `ChangeTracking\` から include するのは、ゲートの型と純粋な関数のためだけ (書き込み系の `GitTransaction` 自体は使わない)。
2. 復元の種類ごとに、どの `GateBlocker` で止めるかの表 (restore §2 の「ファイル・Hunk の復元」の列) を、純粋な関数にする: 入力は (立っている `GateBlocker` の集合、対象のパスの集合、今開いているシーンのパス、各窓が編集中の文書のパス)。出力は「止める理由の一覧」。
   - `Playing` / `NetActive` / `BuildRunning` / `ScriptBuildRunning` / `OpInFlight` / `MergeInProgress`: 止める。
   - `SceneDirty`: 対象に、開いているシーンのファイルが含まれるときだけ止める。
   - `ActorEdit` と各窓の未保存 (`AnimationDirty` など): 対象がその文書のときだけ止める。**どの窓がどの文書を編集中か、を Editor から受け取る口が要る。取れない窓は、安全側 (その Blocker が立っていれば止める) に倒す。**
   - `ServiceUnavailable` (collab): 止めない (git を使わない)。
3. 追跡に固有の条件: 追跡サービスに接続できていない (Before を取れない)、進行中・回復待ちの復元がある (sub-27 が作るジャーナルを見る。このサブでは「ジャーナルのディレクトリに未完了があるか」を問う口だけを用意し、常に「無い」を返す実装でよい)、対象の Blob が欠けている。
4. **理由は全部並べる** (1 件目で止めない)。確認の画面を出している間に状態が変わりうるので、実行の直前に判定し直す口を持つ (呼ぶのは sub-27)。

### D. パスの検査 (restore §8。`PathGuard`。純粋な部分と、ファイルシステムを見る部分を分ける)

- 履歴のパスは、プロジェクトのルートからの相対・`/` 区切り。絶対パスへ直し、正規化して、ルートの下にあることを確かめる。
- 拒否: `..` を含む、ドライブ名や UNC で始まる、予約デバイス名 (`CON`、`PRN`、`AUX`、`NUL`、`COM1`〜`9`、`LPT1`〜`9`。拡張子つきも)、末尾が空白やピリオド、代替データストリーム (`:` を含む)、`.git` と `.mye` の下。
- 書き先と、その親ディレクトリのどれかがシンボリックリンクやジャンクション (再解析ポイント) なら拒否。
- 前例は `C:\HAL\MyEngin\tools\mcp\src\notes.rs` (notes の書き先の検査) と、そのシナリオ (`tests\mcp\notes_junction_*.ndjson`、`notes_traversal.ndjson`)。同じ攻め方を試験に入れる。
- プレビューのときに 1 回。書く直前にもう 1 回呼べる口にする (呼ぶのは sub-27)。

### E. 依存と注記 (restore §3.4、§7)

- 追加されたファイルを取り消す (削除になる) とき、他から参照されているかは `.meta` の guid で引ける範囲までしか分からない。**分からないことを確認の画面に書く** (「このファイルを参照しているコードがあるかは確かめていません」)。
- stage 済みのファイルと重なる場合の注記: 既存の `SourceControlSession` の status (読み取り) で分かれば「このファイルは stage 済み。Index の内容は変わりません」と出す。collab が使えないときは「分からない」と出す。**Index を読みも書きもしない。git を起動しない。**
- 再コンパイルや再起動が要るか (C++ / C# のファイル、スキーマや guid の変わった `.meta`) の見込みを、`StageClassifier` (`C:\HAL\MyEngin\src\Editor\SourceControl\StageClassifier.h`) の分類を読んで出す。

### F. 復元のプレビュー (AI Changes 窓)

- 詳細の列に「復元…」(ファイル全体) と、Hunk の選択に対する「選んだ Hunk を復元…」。押すと、必要な内容 (`hunks`、`blob_get` で Before / After、今のファイル) を集めて計画を作り、プレビューの画面を出す。
  - 止める理由 (ゲート) の一覧。全部。
  - 競合の結果: 一致 / 位置がずれて一致 (何行ずれたか) / 復元済み / 競合 (理由)。競合のときは、今・Before・After のどこが違うかを見られる (`DiffViewer` で、記録した After と今の内容の差を出すのが分かりやすい。Editor 側に行 diff の実装は無いので、**今の内容と After の差の計算は、Hunk の照合が失敗した位置を示すまで**でよい。全面的な 3 者の比較は作らない)。
  - 書くことになる内容: 対象のパス、操作 (書き換え / 削除 / 作成 / 名前を戻す)、戻す Hunk を逆向きにした差分 (`DiffViewer`)。
  - 注記 (E)。
  - **このサブでは、実行のボタンは押せない** (「適用は次の版で入ります」の 1 行。sub-27 が押せるようにする)。プレビューを閉じても何も変わらない。
- 今のファイルの読み取りは、大きさの上限 (行 diff と同じ 8 MiB) つき。フレームを止めない (大きいファイルは分けて読むか、読み取りを数フレームに分ける。読んでいる間は「準備中」)。
- 文字列は英日。

### G. sub-10 / sub-14 から持ち越した小さい追加 (窓を触るついでに)

1. `diff_mode = whole` の大きいファイル (Hunk が `oversize`) の詳細に、Before / After の先頭 (定数の大きさまで) を `blob_get` で取って見せる (sub-10 の残り)。
2. 詳細の列に「この操作の変更をまとめてレビュー」(`describe` の `operationReview` と `ReviewManager::PrepareRevisions`。件数つきの確認 1 回) と、見出しに変更 (ファイル) 単位の集約 (`changeReview`) の状態と件数 (sub-14 の残り。要求 §10.2)。
3. `MYE_TRACK_PROBE=1` の結合に、レビューの 1 往復 (承認 → `describe` に出る) を足す (sub-14 の nit)。

### H. 文書

- restore (このサブで決めた細部: 対象にする Revision、`whole` / `oversize` / `no_before` の扱い、Hunk の「復元済み」)、arch §7、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表。M104p2 の行を足す)。

## やらないこと (このサブでは)

- **作業ツリーへの書き込み** (ファイルの書き換え・削除・作成・名前の変更)。復元ジャーナル。巻き戻し。起動時の回復。`restore_record` / `restore_expect` (全部 sub-27)。
- ライブのシーンの復元 (sub-16)。
- git の起動、Index の読み書き。MCP からの復元。3 者のマージ。
- 下位の層の変更 (変わってよいのは `LocalizationTable.inl` だけ)。`tools\mcp\src` の変更。スキーマの変更 (このサブはサービスへ記録を書かない)。

## 受け入れ条件 (このサブ)

1. ファイル全体の判定: 表の 3 行、追加の取り消し (削除になる。After と一致のときだけ)、削除の取り消し (何も無いときだけ)、リネーム (2 条件)、`no_before`、Blob の欠け、バイナリ、のそれぞれ → `changetracking` スイート (純粋な関数)。
2. Hunk の判定: 表の 4 行、文脈 0 行で位置ずれを認めない、複数 Hunk を後ろから当てる、1 つ競合なら全部やめる、改行の種類の保持、`eolOnly`、`whole` / `oversize` は Hunk 単位で戻せない、復元済み → スイート。
3. **1 つの Hunk だけを戻した結果が、「After からその Hunk だけ Before に戻した内容」とバイト一致する。全部の Hunk を戻した結果が Before とバイト一致する** → スイート (要求 §16.4 の 1 番目の土台)。
4. **Rust と C++ が同じ結果**: 固定の表 (1 つのファイル) を、Rust の試験と C++ のスイートの両方が読み、全行が通る。表に、位置ずれ・競合・文脈 0 行・CRLF / LF・末尾改行なし・日本語・複数 Hunk が入っている → `cargo test --manifest-path tools\track\Cargo.toml` + スイート。
5. After の後で再編集された対象は競合になる (要求 §16.4 の 3 番目)。競合のとき、計画に「書く」操作が 1 つも入らない → スイート。
6. ゲート: 表の全行。`SceneDirty` は対象に開いているシーンが含まれるときだけ、`ServiceUnavailable` では止めない、追跡サービス未接続・Blob の欠けで止める。**理由が 2 つ以上あるとき全部並ぶ** → スイート (要求 §16.5 の 2 番目の土台)。
7. パスの検査: `..`、絶対パス、UNC、ドライブ名、`CON` / `NUL.txt` / `COM1`、末尾のピリオドと空白、`:` を含む名前、`.git` の下、`.mye` の下、ジャンクション越し (一時ディレクトリに実際にジャンクションを作る)、シンボリックリンクの親、のそれぞれが拒否される。正常なパス (日本語、深い階層、空白を含む) は通る → スイート。
8. **このサブのコードは作業ツリーを書かない**: プレビューを作る・閉じる、の前後で、一時プロジェクトの全ファイルのパスとハッシュが同じ。`src\Editor\ChangeTracking\` の、このサブで足したファイルに、ファイルを書く・消す・名前を変える API (`WriteFileReplacing`、`std::ofstream`、`fopen` の書き込み、`DeleteFileW`、`MoveFileW`、`std::filesystem::remove` / `rename` / `copy`) の呼び出しが無い (試験のコードが一時ディレクトリを作る分を除く) → スイート + grep。
9. git を起動しない・Index に触らない: このサブで足したコードに、プロセスの起動と `.git` の下の読み書きが無い → grep。stage 済みの注記は `SourceControlSession` の既存の status を読むだけ。
10. プレビューの画面: 止める理由の一覧、競合の結果、書くことになる内容、注記が出る。実行は押せない。追跡が無効・未接続のとき I/O の口が呼ばれない → スイート (ヘッドレスの ImGui フレーム)。
11. 整形 (競合の理由、ゲートの理由、操作の種類、注記) が英日で期待どおり → スイート。
12. G: `whole` の大きいファイルの先頭の表示、操作全体のまとめてレビュー (件数の確認つき)、変更単位の集約の表示、`MYE_TRACK_PROBE` のレビューの 1 往復 → スイート + `MYE_TRACK_PROBE=1` (Debug か Release)。
13. スクショ (一時のプローブ。コミットしない): (1) 復元のプレビュー (一致) (2) 競合 (3) ゲートで止まっている (理由が複数) (4) 削除になる復元の確認文 (5) 英語。Release。**見た目の合否はユーザーの目視。**
14. `Editor.exe --selftest` (Debug / Release。負荷の下でも `changetracking` が通ること: sub-25) が全部通り、実行中と後に `mye_track.exe` が無い。`tools\check_rules.ps1` が 0 error (規則 10、19。規則 12 は `SourceControl\` を Engine 側から include させない規則で、Editor の中の `ChangeTracking\` からの include は対象外であることを確かめる)。`cargo test` (track)、`tools\track_verify.bat` が通る。下位の層の差分が `LocalizationTable.inl` だけ。

## 検証コマンド

- `tools\build_track.bat Release` (最初に)
- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `--selftest` を Debug / Release
- `MYE_TRACK_PROBE=1` + `MYE_SELFTEST_ONLY=changetracking`
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- exe の実行は PowerShell から。**試験は必ず一時プロジェクトで行い、このリポジトリの作業ツリーで復元を試さない。** `tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。

## 実装メモ (coder が追記)

SELF_EVAL: sub-15 (round 1)
実装:
  - tools\track\src\ops.rs:list — [追加] `operationId` 絞り込み (操作全体のまとめてレビューの対象集め。読み取りだけ、スキーマ・記録は不変)。tests\review.rs に 1 件の断言、ipc-contract.md の list の行に追記
  - tests\track\fixture\hunk_apply_cases.json (新規) + tools\track\tests\diff.rs:the_shared_hunk_table_matches_unapply_hunks — Hunk の逆適用の固定表 27 行 (通常・複数 Hunk の全部 / 片方・行数が変わる Hunk・位置ずれ ±・2 か所・一致なし・文脈変化・復元済み・一部復元済み・1 つ競合・文脈 0 行・CRLF/LF 混在・改行だけ・末尾改行なし・日本語・行の削除)。`strict` の列を Rust の `unapply_hunks` の結果と全行突き合わせ、表の Hunk が `diff_lines` の実際の出力と一致することも確かめる。`tolerant` の列は C++ だけが読む
  - src\Editor\ChangeTracking\HunkRestore.h/.cpp (新規) — Hunk の逆適用 (純関数)。(前の文脈 + After の本文 + 後ろの文脈) を行の並びとして探し、記録した位置 → 唯一の別位置 (ずれ) → 2 か所 (競合) → 無し (競合)、Before の並びなら復元済み。文脈 0 行は位置ずれ不可。全 Hunk を元の内容の中で探してから一度に組み立てる (1 つでも競合・重なりなら何も当てない)。`exactContextLength` / `allowShift=false` / `detectRestored=false` で、サービスの unapply_hunks と同じ規則の「strict」にできる
  - src\Editor\ChangeTracking\RestorePlan.h/.cpp (新規) — `BuildRestorePlan` (純関数): ファイル全体 (今 / After / Before の「ある・ない」とハッシュだけで 一致 / 復元済み / 競合。追加=削除、削除=作成、リネーム=2 条件、no_before・Blob 欠け・After 不明・バイナリ・フォルダ/リンク/大きすぎ/読めない・行為者対象外・書き先拒否) と Hunk (modified かつ lines のみ。whole / oversize / 追加などは全体だけ)。競合・復元済み・戻せないときは ops が空。`ReverseHunkView` / `ConflictHunkView` (差分の入力)、`BuildRestoreNotes` (削除・参照未確認・stage 済み・StageClassifier の A/B/C・.meta・ずれ)
  - src\Editor\ChangeTracking\RestoreGate.h/.cpp (新規) — `EvaluateRestoreGate` (純関数): GateInputs → ComputeBlockers をそのまま呼び、restore-safety §2 の表で絞る (常に止める 6 種 / SceneDirty は対象が開いているシーンのときだけ / ActorEdit と各窓の未保存は EditedDocuments、教えられない窓は安全側 / ServiceUnavailable は止めない) + 追跡に固有の 3 条件。理由は全部並べる
  - src\Editor\ChangeTracking\PathGuard.h/.cpp (新規) — `CheckRelativePath` (綴り) と `CheckPathOnDisk` (各要素が再解析ポイントでないか。RestoreFileIo 経由)、`CheckRestorePath` (両方。書く直前にも呼べる口)、`RelativeToProject`、`RestorePathKey`
  - src\Editor\ChangeTracking\RestoreFileIo.h/.cpp (新規) — ファイル属性と読み取りだけの口 (書く・消す・名前を変える関数なし)。本物は Win32 の読み取り専用 API だけ、試験は偽物で呼び出しを数える
  - src\Editor\ChangeTracking\RestorePreview.h/.cpp (新規) — プレビューの準備: Open で書き先の検査 → Before の Blob を blob_get 1 バイトで確認 → 今のファイル (と旧パス) を 1 Tick 1 MiB ずつ読む (8 MiB 超は判定しない) → BuildRestorePlan。追跡に接続できなければ I/O を 1 回も呼ばず計画なし。古い応答は世代で捨てる
  - src\Editor\ChangeTracking\RestoreFormat.h/.cpp (新規) — 文面 (英日) と、画面に出す内容 `RestoreScreen` (ImGui を呼ばずに作る)、`ReviewGroupText`、先頭表示の文面
  - src\Editor\ChangeTracking\BlobHeadView.h/.cpp (新規) — whole + oversize の詳細に Before / After の先頭 8 KiB を blob_get で取る (base64 の復号つき)。同じ Revision は取り直さない
  - src\Editor\ChangeTracking\ReviewManager.h/.cpp — `PrepareOperation` (list の operationId で全ページ集める。CollectPage を key 引数で共通化)
  - src\Editor\Windows\Project\AiChangesWindow.h/.cpp — 詳細に「復元...」「選んだ Hunk を復元...」(行為者が対象外なら灰色 + 理由)、モーダルのプレビュー (止める理由 / 照合 / 書くことになる操作 / 注記 / 差分 / 競合の位置。「復元する」は常に押せず「適用は次の版で入ります」)、変更 (ファイル) 単位・操作単位の集約と「この操作の変更をまとめてレビュー」(件数つきの確認 1 回)、大きいファイルの先頭。ゲートと注記は毎フレーム判定し直す。AiChangesHost に gateInputs / editedDocuments / restoreJournalPending / stagedState
  - src\Editor\App\EditorApp.cpp — AiChangesHost の 4 口を結ぶ (BuildGateInputs、開いているシーン + ミニシーンのアセット、ジャーナルは常に無し、stage 済みは scm_ の status モデルの読み取りだけ)
  - src\Engine\Core\Localization\LocalizationTable.inl — 89 件 (英日)。下位の層の差分はこのファイルだけ
  - src\Editor\ChangeTracking\RestoreSelfTest.cpp (新規) + ChangeTrackingSelfTest.h/.cpp — `changetracking` スイートに RunRestoreSelfTests を追加、MYE_TRACK_PROBE にレビューの 1 往復と、実サービス + 本物の読み取り口での復元プレビュー (全体 / Hunk、ファイルが変わらないこと) を追加
  - docs\ai-change-tracking\restore-safety.md §8.1 / architecture.md §7 / ipc-contract.md / docs\test_checklists.md (M104p) / plans\m104-ai-change-tracking.md (M104p を判定待ち、M104p2 の行を追加)
仕様との差分:
  - [追加] サービスの `list` に `operationId` 絞り込み。「操作全体のまとめてレビュー」の対象を全ページ集めるのに要る (list には操作の絞り込みが無く、窓が持つ一覧は 50 件ずつで操作の全体を含まない)。読み取りだけで記録・スキーマは変えていない。サブ仕様は tools\track\src を触るとも触らないとも書いていないので足したが、planner の判断を仰ぎたい (不安・質問 1)
  - [追加] `ReviewManager::PrepareOperation`、`BlobHeadView`、`RestoreFileIo`、`RestoreFormat` (`RestoreScreen`)、`ReviewGroupText`: 仕様の「触る場所」に無い新規の部品。いずれも ImGui・書き込みを持たない (画面の内容を文字列で作るのは、ヘッドレスの試験が画面に出るものを確かめられるようにするため)
  - [逸脱] 窓ごとの未保存の「どの文書か」を EditorApp が教えられるのは、開いているシーン (ミニシーン編集中はそのアセット) とミニシーン編集中のアセットだけ。Animation / Animator / Audio Mixer / Behavior Tree / Project Settings / Sprite / Tile Palette の各窓は文書のパスを外へ出しておらず、窓のクラスを触るのは範囲外と判断した。仕様の「取れない窓は安全側」のとおり、その窓の未保存が立っていれば対象に関係なく復元を止める
  - [追加] パスの検査は 8.3 の短い名前の形 (`GIT~1`、`a~1.txt` のような `~` + 数字) を保護領域の疑いとして拒否する (`.git` の別名で書けてしまうのを防ぐ。実在の `a~1.txt` も拒否される保守側)
  - [追加] 今のファイルが 8 MiB を超えるときは判定しない (バイナリの全体復元も。ハッシュだけで足りるが BCrypt の増分ハッシュが無く、フレームを止めない読み取りと両立しないため。M104p2 で見直し)
  - [未実装] 復元ジャーナルの実体 (常に「無い」を返す口だけ、仕様どおり)。Hunk の複数選択 (DiffViewer が単一選択なので、選んだ 1 つの Hunk を戻す。モデル・計画は複数 Hunk を扱える)
  - [追加] `superseded` の変更は止めず、今の内容との照合で決める (仕様に扱いが無かった。restore-safety §8.1 に書いた)。Hunk 単位は modified だけ (追加・削除・リネームは全体だけ)
検証:
  - tools\build_track.bat Release (最初に。サービス不在を確認済み。ops.rs の変更を含む exe を bin\x64\Debug|Release へ置いた。使用中の exe なし) → 成功
  - cargo test --manifest-path tools\track\Cargo.toml → 全 OK (71 秒、失敗 0、警告 0)。新規: tests\diff.rs の固定表 (27 行、表の Hunk が diff_lines の出力と一致)、tests\review.rs の operationId 絞り込み
  - tools\track_verify.bat → 27 シナリオ全部 PASS。実行後 mye_track.exe なし
  - MyEngine.sln を Release|x64 / Debug|x64 で /p:MyeWarnAsError=true → 成功 (警告 0)。スクショ用プローブ (RestoreProbe.h と EditorApp の 2 行) を外したあとにもう一度ビルド。外したことを grep で確認
  - MYE_SELFTEST_ONLY=changetracking + MYE_TRACK_PROBE=1 の Debug / Release → ALL PASS、FAIL 0、739 PASS (新規の復元の試験 約 140 件 + プローブ 3 件 (実サービス: 全体のプレビューが Ready・Hunk が "one" を組み直す・ファイルが変わらない) + レビューの 1 往復 3 件)。実行後 mye_track.exe なし
  - Release --selftest-full → 125 スイート 0 失敗 (224 秒)。Debug --selftest (fast) → 112 スイート 0 失敗 (72 秒)。Debug は --selftest-list のスイート 124 個 (physics だけ除く。D38) を MYE_SELFTEST_ONLY に並べて → 124 スイート 0 失敗 (657 秒)。(注: --selftest-list にはエイリアス twod / m94 / m95 が混じり、m94 / m95 は physics を含むので、名前を並べるときは `fast` / `full` の付く行だけを使うこと。最初に混ぜて Debug の physics がスタックオーバーフローした。コードの不具合ではない)
  - 最後の変更 (試験のプローブ追加) のあとに Release / Debug の --selftest (fast) をもう一度 → 112 スイート 0 失敗 (Release 22 秒、Debug 72 秒)
  - pwsh tools\check_rules.ps1 → 0 error (警告 66 は既存のみ)。規則 12 (SourceControl の逆依存) は Engine 側からの include だけを見るので、ChangeTracking\ から GitTransaction.h / StageClassifier.h を include しても出ない
  - git diff -- src\Shared src\Runtime src\GameLogic src\Server src\Scripting tools\mcp tools\track\src\schema → 空。src\Engine の差分は LocalizationTable.inl だけ
  - 条件 8 / 9 の grep: 新規の 8 組 (HunkRestore / PathGuard / RestoreFileIo / RestorePlan / RestoreGate / RestorePreview / RestoreFormat / BlobHeadView) に対して `WriteFileReplacing|ofstream|fopen|DeleteFile|MoveFile|CopyFile|filesystem::remove|rename|copy|GENERIC_WRITE|WriteFile|CreateDirectory` → 該当なし。`CreateProcess|ShellExecute|system(|popen|.git|SourceControlSession|GitTransaction` → コメントと PathGuard の保護名の文字列だけ (読み書きなし)。書き込みの試験 (RestoreSelfTest) が一時ディレクトリを作る分は除く
  - 作業ツリーを書かない: 一時プロジェクトの全ファイルとフォルダの (パス, ハッシュ) をプレビューの作成・駆動・閉じる前後で比べて同一 (RestoreSelfTest の no-write)。実サービス + 本物の読み取り口のプローブでも probe.txt が変わらず recovery\ も作られない
  - スクショ (一時のプローブ + Release の `Editor.exe --frames 40 --shot-frame 36 --screenshot`。プローブは外した): C:\HAL\MyEngin\cache\m104p_shots\ の 1_ready_ja.png (一致)、2_conflict_ja.png (Hunk の競合 + 3 つのハッシュ + 合わなくなった位置の差)、3_gate_ja.png (ゲートで止まる 4 つの理由)、4_delete_ja.png (削除になる復元)、5_ready_en.png / 6_conflict_en.png / 7_gate_en.png (英語)。画面は偽のサービスの応答で描いたもの。撮れた・文字が切れていない・化けていないは確認、**見た目の合否はユーザーの目視 (未確認)**
  - 未実施: tools\replay_verify.bat (sim・下位の層を触っていないため。サブ仕様の検証コマンドにも無い)、実 Editor の対話操作 (復元ボタンのクリック、本物の Play 中・未保存シーンでの止まり方)、Rust 1.89 での実ビルド
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 14 件を試験・スクショ・grep で対応。サービスへの operationId の追加と、窓ごとの編集文書を取れない窓 (安全側) は差分欄のとおり。planner の確認が要る
  正しさ: 4 — 判定は純関数 + 固定表 (Rust と C++ が同じ 27 行) + 実サービスのプローブで確認。位置ずれ・競合・復元済み・重なりの全部で「競合なら書く操作が空」を断言。未検証: 実 Editor の GUI 操作、stage 済みの判定の実走 (偽の stagedState のみ)、8 MiB 超のバイナリ
  コード品質: 4 — 純関数と I/O の口を分け、読み取りだけの RestoreFileIo に閉じた。画面の内容を文字列で組む RestoreScreen で試験可能にした。AiChangesWindow.cpp が約 1600 行に伸びた (復元の画面は別ファイルに出す余地あり)
  テスト: 4 — 約 140 件の新規 + 固定表の Rust 側 + 実サービスのプローブ。「両言語で描ける」の一部は落ちないことだけの断言。見た目の合否と実操作は未確認
不安・質問:
  1. サービスの `list` へ `operationId` を足したのは、サブ仕様が想定していない tools\track\src の変更。planner が追加を認めるか、(代わりに) 窓が持つ一覧の行だけで操作の Revision を集める (50 件ずつのページに操作の全体が入らないことがある) 案にするか決めてほしい
  2. 窓ごとの未保存の「どの文書か」: Animation / Animator / Audio Mixer / BT / Project Settings / Sprite / Tile Palette の 7 窓はパスを外へ出していないので、いずれかの未保存が立っている間は、無関係なファイルの復元も止まる (安全側)。sub-27 の前に窓へ「編集中のパス」の口を足すか、今のまま (安全側) で行くか
  3. `a~1.txt` のような `~` + 数字の名前を拒否する保守側の検査 (8.3 の短い名前 = `.git` の別名の防止) でよいか。実在のファイルが戻せなくなる (まれ)
  4. 今のファイルが 8 MiB を超えるときは判定しない (バイナリの全体復元も)。BCrypt の増分ハッシュを足して分けて計算するのは sub-27 でよいか
  5. 画面の確認のボタンの「復元する」は常に灰色。ユーザーが「止める理由が全部出る」ことを目視してから sub-27 で押せるようにする運びでよいか
触ったファイル:
  - tools\track\src\ops.rs、tools\track\tests\diff.rs、tools\track\tests\review.rs、tests\track\fixture\hunk_apply_cases.json
  - src\Editor\ChangeTracking\ の新規: BlobHeadView.h/.cpp、HunkRestore.h/.cpp、PathGuard.h/.cpp、RestoreFileIo.h/.cpp、RestoreFormat.h/.cpp、RestoreGate.h/.cpp、RestorePlan.h/.cpp、RestorePreview.h/.cpp、RestoreSelfTest.cpp
  - src\Editor\ChangeTracking\ の変更: ReviewManager.h/.cpp、ChangeTrackingSelfTest.h/.cpp
  - src\Editor\Windows\Project\AiChangesWindow.h/.cpp、src\Editor\App\EditorApp.cpp、src\Engine\Core\Localization\LocalizationTable.inl
  - docs\ai-change-tracking\restore-safety.md、architecture.md、ipc-contract.md、docs\test_checklists.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-15.md (実装メモ)
  - (生成物だが追跡対象: build\Editor.vcxproj と build\Editor.vcxproj.filters は tools\gen_project_files.ps1 で更新済み。ファイルを足したのでステージに要る)
消した・止めたもの:
  - 止めたプロセスなし (自分が起動した Editor.exe / mye_track.exe は全部自然に終了。実行後の mye_track.exe は 0。ユーザーの mye_mcp.exe (pid 15352) には触れていない)
  - 消したのは自分が作った物だけ: src\Editor\ChangeTracking\RestoreProbe.h (スクショ用の一時プローブ。EditorApp の 2 行も外した)、試験が作った %TEMP% の一時ディレクトリ (試験が自分で消す。残りなし)。既存の %TEMP%\mye_track_*、%TEMP%\mye_m104b\、cache\m104*_shots の既存分、tools\mcp\target\release\ には触っていない。cache\m104p_shots\ と %TEMP%\mye_p15_*.log は残した (コミットしない生成物)。C:\HAL\MyEngin\.mye は作っていない。bin\x64\Debug|Release の mye_track.exe は build_track.bat が置き直した (自分の変更を含む新しい版)
申し送り:
  - sub-27 へ: 書く直前に `CheckRestorePath` (PathGuard) と `AiChangesWindow::RestoreBlocks` (または同じ `EvaluateRestoreGate`) を呼び直す。計画 `RestorePlan.ops` が「書くこと」の全部 (Overwrite は BeforeBlob なら hash の Blob を blob_get で取る、Bytes ならそのまま。Delete / Create / Rename)。`RestoreFileIo` は読み取り専用なので、書き込みは別の口を足す。`restoreJournalPending` は EditorApp が常に false を返している (ジャーナルを実装したら差し替え)。`AiChangesWindow::DrawRestorePopup` の「復元する」は `RestoreScreen::canApply` が常に偽
  - 8.3 の短い名前の形の拒否、8 MiB 超の判定なし、窓ごとの文書が取れない件は上の不安・質問のとおり
  - テスト: `--selftest-list` のエイリアス (twod / m94 / m95) を MYE_SELFTEST_ONLY に並べると physics が走る (Debug で落ちる)。スイート名だけ (`fast` / `full` の付く行) を使うこと
  - 固定表 hunk_apply_cases.json は tests\track\fixture\ にあり、Rust (tests\diff.rs) と C++ (RestoreSelfTest.cpp) が CARGO_MANIFEST_DIR / exe の親を辿って読む。表を足すときは `fromDiff` の行が diff_lines と一致することを cargo test が確かめる
  - ADR-039 / engine_spec.md は更新していない (ABI・SimSnapshot の変更なし。restore-safety / architecture / test_checklists / 計画を更新した)

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、下位の層の差分は `LocalizationTable.inl` (110 行) だけ、`tools\mcp` とスキーマに差分なし、スクショ用のプローブ (`RestoreProbe.h`) の残りなし、Debug / Release の `Editor.exe` は最後のソースの変更より新しい。**このサブで足した 8 組のファイル (`BlobHeadView` / `HunkRestore` / `PathGuard` / `RestoreFileIo` / `RestoreFormat` / `RestoreGate` / `RestorePlan` / `RestorePreview`) を、書き込み・削除・名前の変更・プロセスの起動の API で grep して、当たったのは `RestoreFileIo.cpp` 26 行目の `CreateFileW(GENERIC_READ, …, OPEN_EXISTING)` (読み取り専用) だけ。** 「復元する」のボタンは `AiChangesWindow.cpp` 1619 行目で `canApply` が偽の間は押せない。planner の実走: Debug の `MYE_SELFTEST_ONLY=changetracking` が終了コード 0 (復元・固定表・パスの検査・ゲートに関する PASS が 77 行、FAIL 0)、Release の `--selftest` 全体が 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`cargo test` (track) 278 件通過、`tools\check_rules.ps1` 0 error。固定表 `tests\track\fixture\hunk_apply_cases.json` は 27 行で、`tools\track\tests\diff.rs` と `src\Editor\ChangeTracking\RestoreSelfTest.cpp` の両方が読んでいる。スクショ `3_gate_ja.png` を開いて見た: 止める理由が 4 つ並び、照合の結果、戻すと行う操作、stage 済みの注記、戻すと変わる内容が出ている。**見た目の合否と、実 Editor での操作はユーザーの確認 (未確認)。** `--selftest-full`、Debug の physics を除いた全スイート、`MYE_TRACK_PROBE=1`、`track_verify` は coder の結果による。
  - 質問への回答: (1) `list` の `operationId` の絞り込みは認める (読み取りだけ。操作全体が 1 ページに収まらないことがあるので要る)。(2) 7 つの窓が編集中の文書のパスを出さない件は、安全側のまま (その窓に未保存があれば、関係の無いファイルの復元も止まる)。窓へ口を足すのは M104 の外。止まったときの文面で、どの窓の未保存かが分かること。(3) `~` + 数字の名前を拒否するのはそのままでよい。(4) 8 MiB を超えるファイルは、sub-27 で `Sha256FileHex` (ストリーム) を使って、ファイル全体の復元だけ判定できるようにする。(5) 「復元する」は sub-27 で押せるようにする。ユーザーの目視を待たずに進める (運用どおり)。
  - nit: `AiChangesWindow.cpp` が約 1600 行。sub-27 で復元の実行と回復の画面を足すときは、復元の画面を別のファイルに分ける。