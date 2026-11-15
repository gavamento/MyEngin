# AI Change Tracking: 復元の安全性仕様

- 状態: 設計のみ (2026-11-13)
- 全体像は [architecture.md](architecture.md)。記録の形は [db-schema.md](db-schema.md) の `restore_operations`

復元は、この機能の中で唯一、作業ツリーとライブのシーンを書き換える操作。仕様書は安全性の要件を P0 としている。
この文書は「どの状態なら適用してよいか」「何と照合するか」「途中で止まったらどうなるか」を決める。

## 1. 原則

1. **適用するのは Editor。** サービスは履歴を記録するだけで、作業ツリーを書かない。ゲート、一括適用、Undo がすべて Editor にあるため。
2. **Git Index に触らない。** stage も unstage も `git apply` も使わない。git のコマンドを 1 つも起動しない。
3. **照合してから書く。** 今の内容が、記録した After と一致するところだけを戻す。一致しなければ自動では何もしない。
4. **書く前に、今の内容を残す。** 戻す前の内容を `recovery\` へ保存してから適用を始める。
5. **MCP からは呼べない。** 復元と承認は Editor の画面からの明示的な操作だけ (仕様書 §12.6)。
6. **完全な原子性は約束しない。** 複数のファイルとメモリ上のシーンをまたぐ操作を 1 つの原子的な操作にはできない。約束するのは「止まっても、整合した状態へ戻せる」こと (仕様書 §11.5)。

## 2. ゲート

既存の `GitTransaction` のゲート (`src\Editor\SourceControl\GitTransaction.h`) を共有する。
`ComputeBlockers(const GateInputs&)` は bool だけを受ける純関数なので、そのまま呼べる。入力の組み立て (`EditorApp::BuildGateInputs`、private) を、`RestoreManager` からも使える形にする。

復元の対象によって、効かせる阻害要因が違う。

| 阻害要因 (`GateBlocker`) | ファイル・Hunk の復元 | ライブのシーンの復元 (Entity / Component / Field / Override) |
|---|---|---|
| `Playing` | 止める | 止める |
| `NetActive` | 止める | 止める |
| `BuildRunning` / `ScriptBuildRunning` | 止める | 止めない (ファイルを書かない) |
| `OpInFlight` (git の書き込み中) | 止める | 止めない |
| `MergeInProgress` | 止める | 止めない |
| `SceneDirty` | 対象が開いているシーンのファイルなら止める。それ以外は止めない | 止めない (編集の 1 つとして Undo に積まれる) |
| `ActorEdit` | 対象が編集中のアセットなら止める | 対象が編集中の文書でなければ止める |
| 各窓の未保存 (`AnimationDirty` など) | 対象がその窓の文書なら止める | — |
| `ServiceUnavailable` (collab) | 止めない (git を使わない) | 止めない |

加えて、追跡に固有の条件が 3 つある。

- 追跡サービスに接続できていない → 止める (Before の内容を取れない)。
- 進行中、または回復待ちの復元がある → 止める (§6)。
- 対象の Blob が欠けている (`blob_missing`) → 止める。

`GitTransaction` と同じく、立っている理由は**全部**列挙して見せる。1 件目で止めると「保存したのにまだ押せない」を繰り返す。
実行の直前にもう一度判定する (`GitTransaction::Hooks::freshBlockers` と同じ考え方)。確認の画面を出している間に状態が変わりうる。

Play 中とネットセッション中は、どの種類の復元も適用しない。Play 中の World を書き換えると sim の状態が変わり、Stop で巻き戻るので復元も消える。

## 3. 競合の検証

### 3.1 ファイル全体

| 今の内容 | 判定 | ふるまい |
|---|---|---|
| 記録した After と同じ | 一致 | Before へ戻せる |
| 記録した Before と同じ | 復元済み | 何もしない。状態を `restored` に直す |
| どちらとも違う | 競合 | 自動では戻さない。今の内容・Before・After の 3 つを見せる |

- 削除された (After が無い) ファイルの復元は「今そのパスに何も無い」ことが条件。何かあれば競合。
- 追加された (Before が無い) ファイルの復元は削除になる。今の内容が After と一致するときだけ。削除は確認の文面で明示する。
- リネームの復元は「新しい名前の内容が After と一致」かつ「古い名前に何も無い」が条件。

### 3.2 Hunk

Hunk は (前の文脈, After 側の本文, 後ろの文脈) の並びとして今のファイルの中で探す。

| 探した結果 | 判定 |
|---|---|
| 記録した行番号の位置で一致 | 一致 |
| 別の位置でちょうど 1 か所だけ一致 | 一致 (位置がずれたことを確認の画面に出す) |
| 2 か所以上で一致 | 競合 (どれを戻すか決められない) |
| 一致なし | 競合 |

- 比べるのはバイト列。空白や改行の違いを無視して合わせない。
- 文脈が 0 行の Hunk (ファイルの先頭・末尾、または全体の置き換え) は、位置がずれた一致を認めない。
- 同じ Revision の Hunk を複数まとめて戻すときは、後ろの Hunk から順に当てる (前の Hunk の行番号がずれないように)。1 つでも競合なら、そのファイルは 1 つも当てない。
- 戻した結果のファイルは、改行の種類を元のまま保つ (Before と After の本文は改行込みで持っている)。

### 3.3 シーン (Entity / Component / Field / Override)

Entity / Component / Field / Override の復元は、**今エディタで開いているシーンだけ**が対象で、ライブの `Scene` に対して照合する (M104q)。
開いていないシーンの文書は、§3.1 / §3.2 のファイル全体・Hunk の経路で戻す。ディスク上の JSON の一部を字句を保って書き換える処理は新しい大きな部品になり、Hunk の復元で代わりが利くので作らない。
AI Changes 窓は、開いていないシーンの構造の変更の行に「このシーンを開くと項目ごとに戻せます (開かずに戻すなら、ファイルまたは Hunk の復元)」と出して、「シーンの変更を復元...」を押せなくする。

| 単位 | 照合するもの |
|---|---|
| Field | `documentId` が開いている文書と一致、`fileId` のエンティティがいる、そのコンポーネントがある、今の値が記録した After の値と一致 |
| Component (追加を取り消す) | コンポーネントがあり、全フィールドが After と一致 |
| Component (削除を取り消す) | コンポーネントが無い |
| Entity (追加を取り消す) | エンティティがいて、サブツリーが After と一致 |
| Entity (削除を取り消す) | その `fileId` が使われていない。親の `fileId` がいる (いなければ依存の欠落として止める) |
| Prefab Override | `overrides` の該当キーの有無と値が After と一致 |

- **今の値が記録した After と同じか**は、字句では比べない。記録の値は字句のまま (`1.0`、AI が書いた書き方) で、ライブの値はエンジンの型 (`float` の `1`) なので、字句どうしは合わない。
  記録の値を**そのフィールドの型で読み** (`FieldFromJson` を作業用の領域へ) `FieldToJson` で書き直した**正規形**と、ライブの値の正規形を比べる。型が合わない記録の値 (`float` に文字列など) は読めないので競合。
  キーが無い値は、ロードが使う既定値 (コンポーネントを `construct` した値) として比べる。コンポーネントとエンティティは、フィールドごとの正規形の集合で比べる (エンティティは `name` / `parent` / `tagMask` / コンポーネントの集合。兄弟の並び `childIndex` と `overrides` の印は付随するので比べない)。
  C# コンポーネントは型の表が無いので、managed の `Serialize` の出力 (10 種類の型の数値・数値の配列・真偽値) どうしを JSON として比べる (`1` と `1.0` は同じ数)。
  戻す値は、記録の Before の字句を同じ規則で読んで書く。
- `EntityRef` のフィールドは `fileId` の整数で比べる。戻す値が指す `fileId` がシーンにいなければ、依存の欠落として止める (同じ変更で消されて、同じ復元で作り直すエンティティは、いるものとして扱う)。
- `documentId` は必ず照合する。別のシーンの同じ `fileId` に当てない。
- プレハブの伝播 (`PrefabPropagation`) は、それ自体を戻す対象にしない。戻すなら原因のプレハブの変更を戻す。画面では原因の変更へ案内する。

### 3.4 依存の確認

- 削除されたエンティティを戻すとき、その親と、`EntityRef` で指している先がいること。
- 追加されたファイルを取り消す (削除する) とき、他のファイルから参照されていないこと。確かめられるのは `.meta` の guid で引ける参照までで、コードからの参照は分からない。分からないことを確認の画面に書く。
- 1 つの操作 (`operation`) をまとめて戻すときは、その中の全対象が一致していること。一部だけ競合なら、戻せるものと戻せないものを分けて見せ、利用者が選ぶ。

## 4. 流れ

仕様書 §11.2 の 14 段を、この設計の部品に対応させる。

| 段 | すること | 担当 |
|---|---|---|
| 1 | 復元の要求 (AI Changes 窓から) | Editor |
| 2 | 対象の Revision・Hunk・シーン変更を取る (`describe`、`hunks`、`scene_diff`、`blob_get`) | Editor ← サービス |
| 3, 4 | ゲートの判定 (§2) | Editor |
| 5 | 競合の検証 (§3) | Editor |
| 6 | 依存の確認 (§3.4) | Editor |
| 7 | 今の内容を `recovery\<restoreId>\` へ保存 (§5) | Editor |
| 8 | プレビュー: 戻した後の内容と、今との差を見せる | Editor |
| 9 | 利用者の確認。直前にゲートを判定し直す | Editor |
| 10 | ジャーナルを `applying` にする。`restore_record` と `restore_expect` を送る | Editor → サービス |
| 11 | 適用 (§4.1、§4.2) | Editor |
| 12 | 整合性の検証: 書いたファイルを読み直してハッシュを確かめる。シーンは値を読み直す | Editor |
| 13 | ジャーナルを `committed` にする。失敗していたら巻き戻す (§6) | Editor |
| 14 | `restore_record` で履歴を更新。対象の状態を `restored` / `partially_restored` にする | Editor → サービス |

### 4.1 ファイルの適用

- 適用の前後を `ReloadHub::BeginBatch` と `EndBatch(changes)` で囲む。この口は git 専用ではなく「外の何かが assets をまとめて書き換えた」ための汎用の口として作られている (`src\Engine\Engine\HotReload\ReloadHub.h`)。囲まないと、半分だけ戻った状態でホットリロードが走る。
- エラーで抜ける経路でも必ず `EndBatch({})` を通す (通さないとホットリロードが止まったままになる)。
- 書き込みは既存の `WriteFileReplacing` (一時ファイルへ書いてから rename)。
- どこまで作り直すかの判断は `StageClassifier` の分類 (段階 A / B / C) を使う。C++ / C# のファイルを戻したら段階 B (再コンパイル)、スキーマや guid の変わった `.meta` なら段階 C (再起動の確認)。
- 追跡サービスは、自分が予告を受けた内容 (`restore_expect`) と一致する変更を `actor = restore` として記録する。復元が「正体不明の変更」として一覧に現れない。

### 4.2 シーンの適用

- `UndoStack::Record(label, scene, selection, fids, structural, edit)` の中で適用する。これで Ctrl+Z で戻せる。
  - Entity: `SceneSerializer::ApplyPartial(scene, json::array({item}))`。
  - Field: `FieldFromJson(comp, field, value)` (`src\Engine\Core\Util\JsonUtil.h`)。`EntityRef` は `fileId` から解決する。
  - Component の追加・削除を戻すときは `StructuralChanges::Apply` を指定する。指定しないと After に写らず、Undo しても何も戻らないエントリになる。
- `UndoStack::CaptureAfter` がプレハブの override 記録を取り直す。この経路を通さずに値を書くと override の記録が取りこぼされ、**静かにデータが失われる**。復元は必ず `Record` を通す。
- シーンの復元はディスクを書かない。シーンは dirty になり、保存は利用者がする。
- 復元の `restore_operations` には、Undo で戻されたかどうかは記録しない (Undo スタックは Editor の再起動で消える)。対象の状態は、次に追跡が観測した内容で決まる。

**確認済み** (試作 4、2026-11-14。コードを読んで確定。[prototype-results.md](prototype-results.md)。M104q で実走して確かめた結果は §8.3): C# コンポーネントのフィールドは `FieldFromJson` では**書けない**。ECS の中身は managed のハンドル (int32) だけで、`FieldDesc` を持たない。`FieldFromJson` は `FieldDesc` の `offset` へ書くので、呼べばハンドルを壊す。
Inspector の `DrawManagedComponentFields` も別経路で (`ManagedHost::SetFieldValue`)、Undo を記録しない。

**決定**: C# コンポーネントは次のとおり扱う。Entity / Component の復元は、そのまま `SceneSerializer::ApplyPartial` が `DeserializeComponent` を通る。
Field の復元は `FieldFromJson` を使わず、C# コンポーネントだけ別の分岐にして `ManagedHost::DeserializeComponent` へ**戻したいフィールドだけの JSON** を渡す (managed の `ApplyJson` は JSON にあるキーだけを書く)。
C# のフィールドは 10 種類 (float / int / uint / ulong・long / bool / Vec2 / Vec3 / Vec4 / Quat / Color) だけなので、Field 差分の値もこの範囲に収まる。除外は要らない。

**実走の結果** (M104q): 上の 3 点は成り立った (§8.3)。C# コンポーネントも Field 単位で戻せる。

## 5. 復元ジャーナル

`<project>\.mye\ai_changes\recovery\<restoreId>\` に置く。Editor が所有する。

```
manifest.json      状態、対象の一覧、各ファイルの適用前後のハッシュ、進んだところ
pre\0001.bin ...   適用前の内容 (ファイルごと)
```

- `pre\` は自分で内容を持つ。追跡サービスが居なくても、DB が壊れていても、ジャーナルだけで巻き戻せる。
- `manifest.json` の更新は `WriteFileReplacing` で行う (半分書かれた状態を作らない)。
- 適用前の内容を**全部**保存し終えてから、状態を `applying` にする。
- `committed` になったジャーナルは、一定期間 (既定 7 日) 残してから消す。消す前に確認はしない (中身は履歴の Blob にもある)。
- ライブのシーンの復元はジャーナルに内容を持たない (Undo が戻す手段)。`manifest.json` には対象と状態だけを書く。

## 6. 状態と回復

```
prepared ──► applying ──► committed
                │
                ├──► rolling_back ──► rolled_back
                │           │
                └───────────┴──► recovery_required
```

| 状態 | 意味 | 次に起動したとき |
|---|---|---|
| `prepared` | 適用前の内容を保存した。まだ何も書いていない | ジャーナルを捨てるだけ |
| `applying` | 書き込みの途中 | 回復を出す (下) |
| `committed` | 全部書いて確かめた | 何もしない |
| `rolling_back` | 失敗して、保存した内容へ戻している途中 | 回復を出す |
| `rolled_back` | 元の内容へ戻し終えた | 何もしない |
| `recovery_required` | 自動では整合した状態へ戻せなかった | 回復を出す |

- 適用の途中で 1 つでも失敗したら、既に書いたファイルを `pre\` の内容へ戻す (`rolling_back`)。戻す前に、今の内容が「自分が書いた内容」と一致することを確かめる。一致しないファイル (途中で誰かが書いた) は戻さず、`recovery_required` にする。
- Editor の起動時に `recovery\` を見て、`applying` / `rolling_back` / `recovery_required` があれば回復の画面を出す。出すのは、ファイルごとの 3 つの内容 (適用前、書こうとした内容、今の内容) と、選択肢 (適用前へ戻す / 今のままにする)。**勝手には戻さない。**
- 回復待ちのジャーナルがある間は、新しい復元を始められない (§2)。
- 回復の画面を閉じても Editor は普通に使える。

## 7. Git との共存

- 復元は Index を読みも書きもしない。Index の中身 (stage 済みの内容) は復元の前後で変わらない。
- 復元の対象が stage 済みのファイルと重なる場合、作業ツリーだけが変わり、Index には AI の変更が残る。確認の画面に「このファイルは stage 済み。Index の内容は変わらない」と出す。重なっているかどうかは、既存の `SourceControlSession` の status (読み取り) で分かる。collab が使えないときは、分からないことを書く。
- マージやリベースの途中は、ファイルの復元を止める (§2 の `MergeInProgress`)。
- 復元は git の未コミットの変更を 1 つ増やすだけで、コミットも stash も作らない。

## 8. パスの検査

`tools\mcp\src\notes.rs` が notes の書き先でしている検査と同じ考え方を、復元の書き先に当てる。

- 履歴にあるパスはルートからの相対。絶対パスへ直した後、正規化してプロジェクトルートの下にあることを確かめる。
- `..`、ドライブ名、UNC、予約デバイス名 (`CON`、`NUL` など) を含むパスを拒否する。
- 書き先と、その親ディレクトリのどれかがシンボリックリンクやジャンクションなら拒否する。
- この検査を、プレビューのときと、書く直前の 2 回行う (仕様書 §14.2「復元直前にパスを再検証」)。
- `.git`、`.mye` の下には書かない。

守れる範囲の限界: pipe の DACL とトークンは、同じユーザーの別プロセスによる接続を防げない。同じユーザーのプロセスは、そもそも作業ツリーも DB も直接書ける。
追跡は「何が起きたかを見えるようにする」仕組みで、同じユーザーの中の権限の境界にはならない。

## 8.1 判定とプレビューで決めた細部 (M104p)

判定とプレビューまでを先に作った (`src\Editor\ChangeTracking\RestorePlan.*`、`HunkRestore.*`、`RestoreGate.*`、`PathGuard.*`、`RestorePreview.*`)。**作業ツリーを書く部分 (適用・ジャーナル・巻き戻し・回復) は M104p2。** このサブのコードは `RestoreFileIo` (属性と読み取りだけ) と `blob_get` 以外の I/O を持たない。

- **対象にする Revision**: 行為者が `ai` か `unknown`。`editor_user` と `restore` は対象にしない (利用者自身の編集は Undo と git で戻す。復元の復元は、元の Revision を選び直す)。`superseded` の変更は止めず、今の内容との照合で決まる (置き換わった変更の After と今の内容が合うことはまれで、合えば戻せる)。
- **戻す先が無いもの**: Before のスナップショットが無い (`no_before`、追加以外) と、Before の Blob が欠けている・除外・回収済みは「戻せない」(理由を出す)。画面を出すときに `blob_get` で Before を 1 バイトだけ引き、`describe` の印が古くても Blob の欠けを見つける。After は比べるためのハッシュがあればよい (Blob が欠けていても比べられる)。After が除外 (ハッシュ無し) なら比べられないので戻せない。
- **ファイル全体の照合**は 3 つの内容 (今・After・Before) の「ある / ない」とハッシュだけで決める。追加の取り消し (削除) は今が After と一致のとき、削除の取り消し (作成) は今何も無いとき、リネームは新しい名前が After と一致かつ古い名前に何も無いとき。今が Before と同じなら「復元済み」(追加で今も無い、を含む)。リネームで内容も変わっていれば「名前を戻す + 内容を Before で書く」の 2 操作。
- **Hunk 単位で戻せるのは `modified` で `diff_mode = lines` のときだけ**。`whole` / `binary` / `excluded` / `unavailable`、本文が `oversize` で届かない Hunk、追加・削除・リネームはファイル全体の復元だけ。
- **Hunk の探し方**: (前の文脈 + After の本文 + 後ろの文脈) を行の並びとして探す。記録した位置で一致すればそれ、無ければ全体を探して 1 か所なら一致 (何行ずれたかを出す)、2 か所以上は競合、無ければ競合。文脈が前後とも 0 行の Hunk は記録した位置でしか認めない。After の並びが無く、Before の並びが 1 か所にあれば「復元済み」。複数の Hunk は**全部を今のファイルの元の内容の中で探してから**一度に組み立てる (後ろの Hunk から当てても、前から当てても同じバイト列になり、前の Hunk の行数の増減が後ろの探索に影響しない)。1 つでも競合か範囲の重なりがあれば、1 つも当てない。戻す本文は Before のバイト列そのもの (改行の種類を含む)。
- **Rust と同じ結果であることの保証**: `tests\track\fixture\hunk_apply_cases.json` を Rust (`tests\diff.rs`) と C++ (`changetracking` スイート) の両方が読む。`strict` の列は記録した位置でだけ当てる規則 (サービスの `unapply_hunks`) で、Rust の結果と全行が一致する。`tolerant` の列は Editor の判定 (位置ずれ・復元済み) で、C++ だけが読む。表の Hunk は `diff_lines` の実際の出力と一致することも Rust 側で確かめている。
- **ゲート**: `GateInputs` を `EditorApp::BuildGateInputs` から受け (`AiChangesHost::gateInputs`)、`ComputeBlockers` をそのまま呼ぶ。止める条件は §2 の表のとおりで、窓ごとの未保存は「どの窓がどの文書を編集中か」(`EditedDocuments`) を受け取る。**`EditorApp` が教えられるのは今開いているシーン (ミニシーン編集中はそのアセット) と、ミニシーン編集中のアセットだけ**。Animation / Animator / Audio Mixer / Behavior Tree / Project Settings / Sprite / Tile Palette の各窓は編集中の文書のパスを外へ出していないので、その窓の未保存が立っていれば対象に関係なく止める (安全側)。進行中・回復待ちの復元を問う口 (`restoreJournalPending`) は、ジャーナルが無いうちは常に「無い」。
- **パスの検査** (`PathGuard`): 綴りの検査 (`..`・絶対パス・UNC・ドライブ名・予約デバイス名・末尾の空白とピリオド・`:`・`.git` / `.mye`・制御文字と `<>"|?*`・バックスラッシュと空の要素) と、ルートの下の各要素が再解析ポイント (シンボリックリンク・ジャンクション) でないことの検査 (存在しない要素から先はこれから作る場所として調べない)。**8.3 の短い名前の形 (`GIT~1` など、`~` + 数字) は保護領域の疑いとして拒否する** (`a~1.txt` のような実在の名前も拒否される。保守側)。プレビューのときと、書く直前の 2 回使う口は `CheckRestorePath`。
- **注記**: 削除になるときは「参照しているコードやアセットがあるかは確かめていません」を必ず出す。stage 済みかは `SourceControlSession` の status モデルの読み取りだけで答え (Index を読みも書きもしない、git を起動しない)、使えなければ「分からない」と出す。再コンパイル・再読み込み・再起動の見込みは `StageClassifier` の分類 (A / B / C)。`.meta` を書くときは guid が変わるかを確かめていないと出す。
- **読み取りの上限**: 今のファイルは 8 MiB まで (行 diff と同じ)。1 フレームに 1 MiB ずつ読み、読んでいる間は「準備中」。上限を超えるファイルは判定しない (ハッシュだけで足りるバイナリも同じ。M104p2 で見直す)。

## 8.2 適用・ジャーナル・巻き戻し・回復で決めた細部 (M104p2)

作業ツリーを書くのはここだけ (`src\Editor\ChangeTracking\RestoreRunner.*`、`RestoreJournal.*`、`RestoreWriteIo.*`)。**書く・消す・名前を変える・ディレクトリを作る API は `RestoreWriteIo.cpp` の 1 か所**で、ほかのファイルは持たない (試験が grep で確かめる)。読み取りは `RestoreFileIo` (別の口)。

- **書くのは計画 (`RestorePlan.ops`) にあるものだけ**。実行の直前に、プレビューと同じ入力 (describe・Hunk・選んだ Hunk) から計画を作り直す (パスの検査 (2 回目)・今のファイルの読み直し・Before の Blob の有無を含む) と、書く内容のハッシュを含めて**プレビューの計画と同じ**ことを確かめる。違えば書かずに止める (`PlanChanged`)。ゲートも同じ入力で判定し直し (`Gate`)、書く直前にもう一度 (ジャーナルへ保存し終えたあと) 判定する。パスの検査は操作ごとにも行う。
- **流れ**: 判定し直し → Before の内容を `blob_get` (512 KiB ずつ) で取る → ジャーナル (`prepared`) へ適用前の内容を全部保存 → ゲートとパスと内容の最後の確認 → `applying` へ → `restore_expect` と `restore_record` を送る → `ReloadHub::BeginBatch` → 操作を 1 フレームに 1 つずつ → 書いたものを読み直してハッシュを確かめる → `committed` → `EndBatch` → `restore_record`。どこで止まっても (例外を含む) `BeginBatch` の対になる `EndBatch` を 1 回通す。Editor の終了時も `RestoreRunner::Shutdown` が通す。
- **取った内容のハッシュを Editor でも確かめてから書く**: `blob_get` で取った内容の SHA-256 が Revision の Before のハッシュと違えば書かない (`BlobMismatch`)。取っている間に切断・時間切れ・欠けがあれば、何も書かずにやめる (ジャーナルは捨てる)。
- **軌跡 (`JournalFile.states`)**: ジャーナルは、触るパスごとに「適用前 → 各操作のあと」の内容のハッシュ (空 = ファイルが無い) を持つ。**巻き戻しと起動時の回復は、今の内容が軌跡のどれかに合うファイルだけを適用前へ戻し、どれにも合わない (他者が書いた) ファイルは触らない** (`recovery_required`)。書いたあとの読み直しが合わないときも同じで、他者の内容を上書きしない。ディレクトリを作る操作 (削除の取り消し・名前の変更の取り消し) の巻き戻しは、ファイルだけを戻す (作ったディレクトリは残す)。
- **ジャーナル**: `manifest.json` (状態・操作・軌跡・進んだ操作の数。更新は `WriteFileReplacing`)、`pre\0001.bin ...` (適用前の内容)、大きい内容の取得の途中は `new\0001.bin`。`applying` にするのは、適用前の内容を全部保存して読み直したあと。状態は §6 の 6 つに `recovered` を足した: 起動時の回復の画面で利用者が選んだ結果 (`resolution` = `restored_pre` / `kept_current` / `dismissed`)。`prepared` は起動時に捨て、`committed` / `rolled_back` / `recovered` は 7 日後に自分で消す。`manifest.json` が壊れた・無いジャーナルは、落ちずに回復待ちとして扱い、「無視する」では中身を消さずに印 (`dismissed.txt`) だけを付ける。
- **起動時の回復** (対話起動のときだけ。追跡が無効でも見る): `applying` / `rolling_back` / `recovery_required` のジャーナルと壊れたジャーナルがあれば、回復の画面 (`RestoreRunWindow`) を出す。ファイルごとに「復元の前」「書こうとした内容」「今」のハッシュと、今がどれに当たるかを出し、選択は 2 つ: 適用前へ戻す (軌跡に合うファイルだけ。実行の直前にゲートを判定し直す。サービスに繋がっていなくてもよい)、今のままにする。**選ぶまで何も書かない**。回復待ちの間と復元の実行中は新しい復元を始められない (`restoreJournalPending` は回復待ちの有無で、実行中は `RestoreRunner::Busy`)。画面を閉じても Editor は普通に使える。
- **ほかの操作との排他**: `BeginBatch` 〜 `EndBatch` の間 (`RestoreRunner::WriteSectionActive`) は、`EditorApp::BuildGateInputs` の `opInFlight` に入れて git の書き込みと別の復元を止め、保存・Undo のショートカットとファイルのドロップを受けない。実行の画面は閉じられないモーダルで、取り消せるのは最初のファイルを書くまで。
- **大きいファイル (8 MiB 超)**: ファイル全体の復元だけ判定・適用できる。今のファイルは別のスレッドで `Sha256FileHex` (ストリーム)、`pre\` への保存は `CopyFileW` (別のスレッド)、Before の内容は `new\` へ塊ごとに積んでハッシュを確かめ、所定の位置へは名前の付け替えで置く。メモリには載せず、フレームを止めない。Hunk 単位は対象外のまま。
- **サービス側の記録**: `restore_expect` (書く前) と `restore_record` (`applying` / `committed` / `rolling_back` / `rolled_back` / `recovery_required` / `recovered`)。送れなかった `restore_record` は残して送り直し、結果の画面に「履歴をまだ送れていない」と出す。`restore_expect` が届いていない書き込みは「行為者が分からない変更」になる (制限)。
- **削除の取り消し・追加の取り消し**: `EndBatch` の変更集合に `Added` / `Deleted` を載せる。`ReloadHub` は `Deleted` を扱わない (既存の仕様) ので、削除したアセットの登録は残る。段階 B / C (スクリプトの再コンパイル・再起動) の自動の作り直しはしない: プレビューの注記が出した見込みのとおり、利用者がする。
- **未検証**: 実 Editor の GUI 操作 (押す・取り消す・回復の選択・見た目)、`ReloadHub` が実際に開いているシーンへ差分を当てる経路 (`EndBatch` が `Modified` を流すところまで)、stage 済みの判定の実走、複数ファイルの計画 (計画の作り手が 2 ファイルまでなので、3 ファイルはジャーナルの手組みで巻き戻しだけを確かめた)。

## 8.3 ライブのシーンの復元で決めた細部 (M104q)

World を書き換える唯一の復元 (`src\Editor\ChangeTracking\SceneRestorePlan.*` (照合。World を書かない純関数)、`SceneRestoreRunner.*` (Undo に積んで書き、読み直す)、`SceneRestoreFormat.*` (文面)、`src\Editor\Windows\Project\SceneRestoreWindow.*` (プレビューと結果の画面))。**ディスクは書かない**: ジャーナルも `restore_expect` も使わない。シーンは dirty になり (`UndoStack::StateSerial` が変わる)、保存は利用者がする (保存すると `editor_saved` で Editor の利用者の変更として記録される)。

- **対象**: 今エディタで開いているシーン (§3.3)。範囲は 4 つ (フィールド / コンポーネント / エンティティ (その変更の中のそのエンティティの全部) / 変更全体)。レビューの範囲 (`SceneReviewOptions`) と同じ並びで、「シーンの変更を復元...」は選んだ行とその範囲で開く。範囲の行は 4096 件まで (`restore_record` の上限)。
- **行の種類と戻し方**:

  | 行 | 戻し方 | 照合 |
  |---|---|---|
  | `FieldModified` / 値のある `PrefabOverrideChanged` (コンポーネントとフィールドがある) | フィールドを書く (通常は `FieldFromJson`、`EntityRef` は `fileId` から解決、C# は `DeserializeComponent` へ 1 キーの JSON) | 今の正規形 = After の正規形。今が Before なら復元済み |
  | `EntityModified` (`name` / `tagMask`)、`HierarchyChanged` (`parent` / `childIndex`) | エンティティの JSON を直して `ApplyPartial` (Undo の復元と同じ経路) | 同上。親の復元は、親がいて輪にならないこと |
  | `ComponentAdded` | コンポーネントを外す (`ApplyPartial`) | 全フィールドが After と同じ。必須の `LocalTransform` と隠しコンポーネントは戻せない |
  | `ComponentRemoved` | Before の JSON でコンポーネントを付ける | エンティティがいて、同じコンポーネントが無い。`EntityRef` の先がいる |
  | `EntityAdded` | 根のサブツリーを消す (`DestroyEntity`) | 全員が After のままで、After に無い子孫がいない |
  | `EntityRemoved` | Before の JSON で作る (同じ変更で消えた子孫と一緒に、根ごと。`PeekNextFileId` を戻した fileId の先へ進める) | その fileId が使われていない。親と `EntityRef` の先がいる (同じ復元で作るものはいる) |
  | `PrefabOverrideChanged` の印だけの行 (`overrides` の要素) | 値の行に連動して変わるので戻さない (`derived`)。印だけが残る行は「戻せない」 | — |
  | `PrefabInstanceChanged` / `PrefabAssetChanged` / `PrefabPropagation` / 文書全体の値 / `unparsed` | 戻せない (理由を出す)。プレハブのアセットはファイル・Hunk の経路 | — |

- **1 つでも戻せない・競合なら、何も当てない** (ファイルの復元と同じ)。実行の直前にゲートと計画をもう一度作り、プレビューのときの `ops` と違えば書かない (`PlanChanged`)。
- **必ず `UndoStack::Record` の中で書く**。`StructuralChanges::Apply`、`fids` は操作が触るエンティティ (作る・消すものは根)。`Record` の `CaptureAfter` がプレハブの上書きの記録を取り直すので、上書き済みフィールドを戻すと、記録から該当キーが消える (取りこぼしも、余分な残りも無い。保存して読み直しても同じ)。ラベルは「AI の変更を復元」(英日)。Undo のエントリは 1 件で、前後で何も変わらなければ積まれない (`UndoStack::EndRecord` の既存の規則)。
- **書いた後に読み直す**: 同じ行から計画をもう一度作り、全部が「復元済み」になることを確かめる。合わなければ、`Record` の中で自分が書いた分 (作ったエンティティを消し、書く前のサブツリーを `ApplyPartial` で当て直す) を戻して抜ける。前後が同じになるので Undo には何も積まれず、`rolled_back` を記録する。
- **選択**: 消したエンティティは選択から外す (`Record` の `EndRecord` が書く選択へ反映される)。
- **ゲート** (§2 の右の列): Play 中・ネットセッション中・ミニシーンの編集中を止める。ビルド中・git の書き込み中・マージ中・シーンが未保存・各窓の未保存は止めない (ファイルを書かない)。追跡サービスの未接続、進行中・回復待ちの復元、外部の変更の確認の保留中 (ライブのシーンとディスクが食い違い、照合が合わないため。先に読み込むか決める) も止める。ミニシーンの編集中は、対象がそのアセットでも止める (ミニシーンの `fileId` と今のシーンの対応は保証しない。シーンの文書だけを対象にする)。理由は全部並べ、実行の直前にもう一度判定する。
- **サービスへの記録**: 書いて確かめたあとの `committed` (対象 = `{ revisionId, scope: scene, sceneChangeIds, paths: [シーンのパス] }`)、読み直しが合わなかったときの `rolled_back` (`error`)。`restore_record` は `RestoreRunner::QueueRecord` で順番待ちに積み (切断・時間切れなら送り直す)、受け取られたら `ChangeTrackingSession` が一覧と強調の表を取り直す (記録は通知を出さないため)。サービスは `scene_changes.state` を `restored` にし、`scene_overlay` は `active` の行だけを返すので、戻した項目は強調から消える。Undo で戻した場合は何も記録しない (§4.2)。
- **C# コンポーネント** (試作 4 の残り。M104q の実走で確かめた): (a) `DeserializeComponent` へ渡した 1 キーの JSON は、そのフィールドだけを変え、他は変えない。(b) `Record` 経由の復元は Undo で戻り、Redo でもう一度当たる (スナップショットに managed の値が入っている)。(c) プレハブのインスタンスでは、C# のフィールドは `FieldDesc` を持たないので、エンジンの上書きのライブ diff が記録しない (既存の仕様。M104q 前から同じ)。復元しても上書きの記録は壊れず、保存して読み直しても同じ。以上が成り立ったので、C# も Field 単位で戻せる。.NET ランタイムか `MyeScripting.dll` が無い環境では、この試験は SKIP と明記して成功とは数えない。
- **正規形の限界**: 比べるのは `FieldDesc` のあるフィールドだけ。ファイルにあってエンジンが知らないキー (古いフィールドなど) は、ロードで捨てられるので比べない。キーが無い側は既定値として比べるので、AI が既定値のキーを消しただけの変更は「同じ値」とみなされて戻す操作が空になる (復元済みと表示)。エンティティの照合は `childIndex` と `overrides` の印を見ない。
- **削除を含むファイルの復元のあとのアセットの登録** (sub-27 からの持ち越し): `ReloadHub::EndBatch` は削除を扱わず、新しいファイルも登録しない。Editor の復元のフック (`EditorApp`) が `EndBatch` のあとに `SyncAssetRegistry` (`AssetRegistrySync.*`) を呼び、削除は `AssetDatabase::MoveAsset(path, path)` (Asset Browser の削除と同じ経路で旧キーを外す)、追加は `GuidForPath(path, true)` (git の `RegisterAdded` と同じ)、名前の変更は `MoveAsset(旧, 新)` を行う。`assets\` の外は触らない。Asset Browser の一覧は、ファイルシステムを見る画面なので別 (必要なら更新する)。段階 B / C (再コンパイル・再起動) は自動にしない (プレビューの注記のまま)。
- **限界**: 開いていないシーンは項目ごとには戻せない (ファイル・Hunk の経路)。プレハブのアセットそのものと、`PrefabPropagation` は戻さない (後者は sub-19 で表示される)。Entity の `fileId` の無い (採番されていない) 子孫は「After に無い子孫」として扱う。C# のフィールドが追加・削除された変更は戻せない (既定値が分からない)。

## 9. 受け入れ条件との対応

| 仕様書 §16.4 / §16.5 の項目 | この文書の規則 |
|---|---|
| Hunk 単位で復元できる | §3.2、§4.1 |
| Field 単位で復元できる | §3.3、§4.2 |
| 再編集された対象は競合として検出する | §3 |
| Rejected によって変更が自動削除されない | 復元は明示的な操作だけ (§1)。レビューと復元は別の状態 |
| 復元操作を履歴として記録できる | §4 の段 10、14 |
| 複数ファイル復元の途中失敗から回復できる | §6 |
| 復元中の強制終了を検出できる | §6 (起動時の検査) |
| Git Index を勝手に変更しない | §7 |
| Play 中の危険な変更適用を停止する | §2 |
