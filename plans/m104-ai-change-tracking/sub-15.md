# sub-15: 復元の判定 — ゲート、競合の検証、パスの検査、プレビュー (M104p)

- 依存: sub-26 (順序は sub-14 → sub-26 → sub-15 → sub-27 → sub-16)
- 状態: 未着手
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

## フィードバック履歴
