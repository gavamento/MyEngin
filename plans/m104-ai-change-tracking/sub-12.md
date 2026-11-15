# sub-12: シーンの変更の集約、一覧、Hierarchy のバッジ、選択の同期 (M104m)

- 依存: sub-11
- 状態: 未着手
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-15、sub-11 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。
- 分割: Inspector の Field 差分、Scene View のワイヤ箱、色の設定は **sub-24 (M104m2)** に分けた。順序は sub-12 → sub-24 → sub-13 → sub-14。着手前の確認だった「色を変えられる範囲」(計画の未決事項 5) は sub-24 に移した (裁定は推奨どおり 4 色)。

arch §6、§7、要求 §8。**World を変えないことが存在条件** (ECS のコンポーネント・イベントを足さない。表示のために `Scene::EnsureFileId` を呼ばない)。

## 確認済みの事実 (2026-11-15 に planner が調べた。行番号はずれうる)

- `C:\HAL\MyEngin\src\Editor\Windows\Scene\HierarchyWindow.cpp`: 検索表示は `DrawFiltered` (259 行目)、通常表示は `DrawEntityNode` (292 行目)。**別の経路なので両方に足す。** バッジの前例は `ScmBadgeColor` / `DrawScmTileBadge`。
- fileId を読むだけの手段: `FileIdComponent` を `GetComponent` で読む (`Scene::FindByFileId(fileId)` は `C:\HAL\MyEngin\src\Engine\Engine\Scene\Scene.h` 57 行目。fileId → エンティティ)。`EnsureFileId` は未採番なら採番する (アーキタイプが動く) ので使わない。
- 開いているシーンのパスは `EditorApp::scenePath_` (`EditorApp.h` 257 行目)。
- `AssetDatabase::GuidForPath(path, createIfMissing = true)` (`C:\HAL\MyEngin\src\Engine\Engine\Asset\AssetDatabase.h` 73 行目) は、既定で `.meta` を作る。**表示のために `.meta` を作らない** (下の A の 2)。
- サービス (sub-11): `scene_diff` (Revision ごとの構造の変更)、`describe.sceneSummary`、`change_recorded.documentId`、`resolvedDocumentId` (引き継ぎをたどった ID)。`unparsed` は `afterValue` が理由。値は JSON の断片 (字句のまま)。
- 選択: `Selection` (`C:\HAL\MyEngin\src\Editor\Scene\Selection.h`) は fileId で持ち、毎フレーム `FindByFileId` で解決する。

## やること

### A. サービス側 (`tools\track`)

1. **op `scene_overlay`** (`role = editor` だけ。読み取り専用)。引数 `documentId`、`afterRevisionId?` (これより後の Revision だけ)、`cursor?`。開いている文書に「今かかっている」構造の変更を、エンティティごとにまとめて返す。Editor が Revision を 1 つずつ取って自分でまとめないため (まとめ方を 1 か所に置く)。
   - 対象の Revision: その文書 (引き継ぎをたどった同じ文書) の Revision のうち、行為者が `ai` か `unknown` のもの (`editor_user` と `restore` は除く。Editor の利用者が自分でした編集を強調しない)。`scene_changes.state = 'active'` の行だけ。`afterRevisionId` があればそれより後。
   - まとめ方 (古い Revision から順に畳む。純粋な関数にして単体で試験する):
     - エンティティ: 追加 → その後の変更は「追加」のまま。追加 → 削除は消える (何も出さない)。変更 → 削除は「削除」。削除 → 同じ fileId の追加は「変更」(中身は最初の Before と最後の After)。
     - フィールド / コンポーネント / ヒエラルキー / override: 同じ (fileId, component, field) について、最初の Before と最後の After。**両者が同じ字句になったら消える** (元へ戻された)。
     - `unparsed` の Revision が途中にある場合、それより前の畳み込みは信用できない。結果に `incomplete: true` と、該当の Revision を付ける (Editor は「一部の変更は解析できなかった」と出す)。
   - 結果: `entities` (`fileId`、`status` = added / modified / removed、`name?`、`hierarchyChanged`、`components` = [`name`、`status`、`fields` = [`name`、`before`、`after`、`lastRevisionId`、`override`]]、`revisionIds`)、`documentChanges` (ルート直下)、`incomplete`、`latestRevisionId`、`nextCursor`。1 行の上限に収まるようページ送り。値が単独で大きいものは `oversize`。
   - 件数の上限を置く (エンティティ数万でも応答が返る)。超えたら `truncated: true`。
2. **op `document_for_path`** (`role = editor` だけ)。引数 `path` (プロジェクトのルートからの相対、または絶対)。そのパスの今の文書の `documentId` (引き継ぎをたどったもの)、`idSource`、`kind` を返す。記録が無ければ、サービスが sub-11 と同じ規則 (`.meta` の guid、無ければパスの仮 ID) でその場で決めた値と `known: false`。**Editor は guid の規則を自分で再実装しない** (規則が 2 か所に分かれると、`.meta` の無いシーンで食い違う)。
3. `protocol.rs` (`IMPLEMENTED_OPS`、`role_allows`)、ipc、試験 (`tests\scene_diff.rs` か新しいファイル、`tests\track\` のシナリオ)。スキーマは変えない (読むだけ)。

### B. Editor: `SceneChangeOverlay` (`src\Editor\ChangeTracking\SceneChangeOverlay.h/.cpp`、新規)

- 持つもの: 開いている文書の `documentId` (`document_for_path` の結果)、`scene_overlay` の結果を (fileId → 状態) の表にしたもの、表示の有無 (切り替え)、`afterRevisionId` (「ここまで確認済みにする」の印。下)。
- 更新の契機: シーンを開いた・切り替えた (`scenePath_` が変わった)、`change_recorded` の通知で `documentId` が一致した、`attribution_updated`、`resync`、追跡の状態が `Ready` になった。**毎フレームは取り直さない** (契機があったら「古い」印を付け、間引いて取り直す。sub-09 のモデルと同じ作り)。
- 引き方: 各窓は毎フレーム、エンティティの `FileIdComponent` を読んで表を引く (fileId の無いエンティティは対象外)。表の引き当ては O(1)。削除されたエンティティ (ライブのシーンに居ない fileId) は、このサブでは一覧 (C) に出すだけ (ゴーストは sub-17)。
- ライブのシーンと食い違う場合: オーバーレイは「ディスク上のファイルの変更」を表す。未保存の編集や、まだ読み込んでいない外部の変更があると、ライブのシーンと合わないことがある。表にある fileId がライブに居なければ黙って飛ばす。
- 「ここまで確認済みにする」: 今の `latestRevisionId` を `afterRevisionId` として覚え、以後はそれより後の変更だけを強調する。文書ごとに持ち、`<project>\.mye\editor_settings.json` に保存する (キーは documentId)。「全部もう一度表示」で消せる。レビュー (sub-14) とは別物で、**記録にもサービスにも何も書かない、表示だけの印**であることを文面に書く (sub-14 で、承認済みの変更を強調から外す扱いに置き換えるかを見直す)。
- 追跡が無効・未接続・非対話起動のときは、表は空で、各窓の追加の仕事は「空の表を 1 回引く」だけ。

### C. AI Changes 窓 (sub-10 の `AiChangesWindow`) への追加

- 詳細の列に「シーンの変更」の一覧: 選んだ Revision の `scene_diff` (種別、エンティティの名前と fileId、コンポーネント、フィールド、Before → After の値。値は長ければ切って表示)。`unparsed` は理由を出す。プレハブの 3 種の区別が分かる文面。
- 行をクリックすると、その文書が今開いているシーンなら `Selection` を対象の fileId にする (Hierarchy と Inspector が追従する)。開いていない文書なら選択せず、「このシーンは開いていません」と出す (シーンを開く導線は付けてよいが、未保存のシーンを黙って閉じない。既存の「シーンを開く」の確認の経路を通す)。削除されたエンティティは選択できない旨を出す。
- 窓の上部に、開いているシーンについての帯: 強調の表示の切り替え、件数 (追加 / 変更 / 削除)、「ここまで確認済みにする」、`incomplete` の注記。
- 整形 (種別の文面 11 種 + `unparsed`、値の切り方) は `ChangeFormat` に置き、スイートから呼べるようにする。英日。

### D. Hierarchy のバッジ

- `DrawEntityNode` と `DrawFiltered` の**両方**で、行の右 (既存のバッジやアイコンと重ならない位置) に小さい印を描く: 追加 / 変更。色は、このサブでは `ChangeTracking\` の中の定数 (緑 / 青。sub-24 が設定に置き換える)。
- 子孫に変更があるが自分には無いノード (折りたたまれていると見えない) には、薄い印を出す。子孫の集計は、表を作るとき (取り直したとき) に 1 回だけ計算する。毎フレーム木を走査しない (親をたどる集計には World の親子関係を読む必要があるので、表の更新のときに 1 回読む。読むだけ)。
- ツールチップに、何が変わったかの要約 (コンポーネント名とフィールド名を数個)。
- 強調の表示が切のときは何も描かない。

### E. sub-23 から持ち越した直し

- `ChangeTrackingSession::NoteSaved`: 保存の直後 (呼ばれた時点) にファイルの大きさと最終更新時刻を控え、ハッシュを取る直前と直後に同じであることを確かめ、違えば送らない (sub-23 の VERDICT の should。外の変更を「Editor の利用者」として確定しないため)。試験: 控えた後にファイルを書き換えると `editor_saved` が送られない。`C:\HAL\MyEngin\docs\ai-change-tracking\attribution-rules.md` §3 の限界の記述を直す。

### F. 文書

- arch §7、ipc (`scene_overlay`、`document_for_path`)、`C:\HAL\MyEngin\engine_spec.md` §9.2、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表。M104m2 の行を足す)。

## やらないこと (このサブでは)

- Inspector の Field 差分、Scene View のワイヤ箱、色の設定 (sub-24)。ゴースト、Before / After (sub-17、18)。復元・レビューの操作 (sub-14〜16)。
- ECS のコンポーネント・イベントの追加。Engine 層の変更 (下位の層で変わってよいのは `LocalizationTable.inl` だけ)。
- `.meta` の作成 (`GuidForPath` を `createIfMissing = true` で呼ばない)。`EnsureFileId` の呼び出し。

## 受け入れ条件 (このサブ)

1. `scene_overlay` のまとめ方: 追加 → 変更、追加 → 削除 (消える)、変更 → 削除、削除 → 追加、フィールドが元へ戻った (消える)、2 つの Revision にまたがる同じフィールド (最初の Before と最後の After)、`editor_user` / `restore` の Revision は含まれない、`afterRevisionId` より前は含まれない、途中の `unparsed` で `incomplete`、のそれぞれ → `cargo test --manifest-path tools\track\Cargo.toml` (純粋な関数の単体 + サービスを通した結合)。
2. `scene_overlay` と `document_for_path` が `mcp` / `hook` で `forbidden`。ページ送り。別のシーンの同じ fileId が混ざらない。`.meta` の無いシーンで `document_for_path` が仮の ID を返し、`scene_overlay` と同じ ID になる → `cargo test` + `tools\track_verify.bat`。
3. **表示の有無で World が変わらない**: オーバーレイに追加・変更を入れて Hierarchy を描いた状態と、空の状態で、`HashWorld`、エンティティ数、アーキタイプ数が同じ。fileId の無いエンティティに `FileIdComponent` が付かない → `changetracking` スイート (ヘッドレスの ImGui フレームで実際に描く。sub-10 の `WithHeadlessImGuiFrame` を使う) (要求 §16.5 の 5 番目)。
4. 開いているシーンと違う `documentId` の変更は、同じ fileId でもバッジが出ない → スイート (要求 §16.3 の 2 番目)。
5. バッジが、通常表示と検索表示の両方の経路で出る (描いたバッジの数を数える口で確かめる)。子孫だけに変更があるノードに薄い印が出る。強調が切なら 0 → スイート。
6. 更新の契機: `change_recorded` (documentId が一致) で 1 回だけ取り直す。一致しない通知では取り直さない。連続の通知が間引かれる。シーンを切り替えると表が入れ替わる。追跡が無効・非対話起動のとき I/O の口が呼ばれない → スイート。
7. 一覧のクリックで `Selection` が対象の fileId になる。開いていない文書・削除されたエンティティでは `Selection` が変わらない → スイート。
8. 「ここまで確認済みにする」で、それ以前の変更のバッジが消え、新しい変更のバッジは出る。設定に保存され、読み直しても同じ。サービスへ記録の op が送られない → スイート。
9. Play 中に強調を切り替えても、`HashWorld` が同じ Tick 数の後で変わらない → スイート。
10. E: 控えた後に書き換えたファイルの `editor_saved` が送られない → スイート。
11. スクショ (一時のプローブ。コミットしない): (1) Hierarchy のバッジ (通常表示) (2) 検索表示 (3) AI Changes 窓の「シーンの変更」の一覧と帯 (4) 英語。Release で撮る。プローブが `git diff` に残らない。**見た目の合否はユーザーの目視。**
12. `Editor.exe --selftest` (Debug / Release) が全部通り、実行中と後に `mye_track.exe` が無い。`tools\check_rules.ps1` が 0 error (規則 10、19)。`cargo test` と `tools\track_verify.bat` が通る。下位の層の差分が `LocalizationTable.inl` だけ。
13. 実サービスでの確認 (`MYE_TRACK_PROBE=1` の結合に足す): 一時プロジェクトのシーンのファイルを外から書き換える → `scene_overlay` に出る → オーバーレイの表に入る。実 Editor を人が操作する確認は未確認として残す (手順を `docs\test_checklists.md` に)。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `--selftest` を Debug / Release (他のビルドと並走させない。sub-23 で、並走すると時間に依存する試験が落ちた)
- `cargo test --manifest-path tools\track\Cargo.toml`、`tools\build_track.bat Release`、`tools\track_verify.bat`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- exe の実行は PowerShell から。`tools\mcp\target\release\` の exe に触らない。このリポジトリのルートで追跡を有効にしない。

## 実装メモ (coder が追記)

## フィードバック履歴
