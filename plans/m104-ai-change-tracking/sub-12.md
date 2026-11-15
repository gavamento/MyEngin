# sub-12: シーンの変更の集約、一覧、Hierarchy のバッジ、選択の同期 (M104m)

- 依存: sub-11
- 状態: OK (commit は台帳 harness.md を参照)
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

```
SELF_EVAL: sub-12 (round 1)
実装:
  - tools\track\src\scene_overlay.rs (新規) — `fold` (純粋な関数。古い Revision から順に畳む。追加→変更=追加、追加→削除=消える、変更→削除=削除、削除→追加=変更 (scene_diff::diff で中身の差を合成)、同じ (fileId, component, field) の最初の Before と最後の After、同じ字句 (書き方だけ違う JSON を含む) に戻ったら消える、unparsed で incomplete)。`scene_overlay` (引き継ぎの鎖を逆にたどって文書を集め、actor が ai / unknown、state = active、afterRevisionId より後の行だけを読む。fileId 昇順のページ送り、値 8 KiB 超は oversize、読む行は 40 万行まで)、`document_for_path` (document.rs と同じ規則。記録が無ければその場で決めて known:false)。単体 8 件
  - tools\track\src\protocol.rs / server.rs / lib.rs — 2 op を IMPLEMENTED_OPS・role_allows (editor だけ)・ディスパッチへ
  - tools\track\tests\pipe.rs — 結合 2 件 (別シーンの同じ fileId が混ざらない・元へ戻ると消える・afterRevisionId・.meta の無いシーンの仮 ID が document_for_path と scene_overlay で一致・mcp/hook は forbidden・bad_args・ページ送り・oversize・文書の変更は最初のページだけ)
  - tests\track\26_scene_overlay.ndjson / .expected.ndjson (新規) — track_verify のシナリオ 26
  - src\Editor\ChangeTracking\SceneChangeOverlay.h/.cpp (新規) — 開いている文書 1 つ分の表。document_for_path → scene_overlay (全ページを集めてから表を入れ替える)。取り直しの契機 (パスの変更・change_recorded の documentId 一致・attribution_updated・resync・.meta の変更・接続)。500 ms の間引き。世代で古い応答を捨てる。「ここまで確認済みにする」(afterRevisionId で送る。EditorSettings へ保存)。RebuildAncestors (取り直しのあと 1 回だけ、ライブの親子を読む)。SelectEntityByFileId
  - ChangeTrackingSession.h/.cpp — overlay_ を持ち、通知・接続・Poll を配線。NoteSaved を直す (E): 知らされた時点のファイルの大きさと更新時刻を控え、ハッシュの直前と直後に同じでなければ editor_saved を送らない
  - ChangeModel.h/.cpp — scene_diff を全ページ取る (describe に sceneSummary があるとき)。ParseSceneChanges
  - ChangeFormat.h/.cpp — SceneKindLabel (11 種 + unparsed。プレハブの 3 種は文面で区別)、ClipValue (UTF-8 の文字境界)、FormatSceneChange、UnparsedReason、OverlayBadgeTooltip
  - AiChangesWindow.h/.cpp — 帯 (強調の切り替え・件数・ここまで確認済み・全部もう一度表示・incomplete / truncated の注記)、詳細の「シーンの変更」の一覧 (見えている行だけ組み立てる)、ActivateSceneChange (行のクリック → Selection。別シーン・削除済み・文書全体は選ばず理由を出す)
  - HierarchyWindow.h/.cpp — DrawEntityNode と DrawFiltered の両方でバッジ (追加=緑・変更=青・子孫だけ=薄い青)。ツールチップ。描いた数を数える口
  - EditorApp.cpp — 文書パスの伝達、RebuildAncestors の呼び出し、ack の保存先、host.selectEntity
  - EditorSettings.h/.cpp — aiChangeAcks (documentId → Revision)
  - TrackProtocol.h / TrackClient.cpp — op 名と 30 秒の時間切れ
  - LocalizationTable.inl — 47 行の追加 (英日。変換指定子なし)
  - ChangeTrackingFakeIo.h (新規) — 偽の I/O を ChangeTrackingSelfTest.cpp から切り出して共有。SceneChangeOverlaySelfTest.cpp (新規) — 新しい試験 (changetracking スイートの末尾から呼ぶ)
  - ChangeTrackingSelfTest.cpp — 偽の I/O の切り出し、E の試験 4 件、実サービスのプローブにシーンの外からの書き換え → 表
  - docs: attribution-rules.md §3 の限界、ipc-contract.md (2 op・時間切れ)、architecture.md §7、engine_spec.md §9.2、test_checklists.md、plans\m104-ai-change-tracking.md (M104m を実装済み、M104m2 の行)。build\Editor.vcxproj(.filters) は gen_project_files.ps1 の出力
仕様との差分:
  - [追加] scene_overlay の結果に `entityFields` (name / tagMask など、コンポーネントに属さないフィールド)、`hierarchy` (parent / childIndex の差)、フィールドごとの `changeType`、`unparsedRevisionIds`、`assetChangedRevisionId`、`truncated` を足した — 仕様の `components` / `hierarchyChanged` だけでは名前の変更や親子の変更の内容を返せない。`documentChanges` は最初のページだけ
  - [追加] 強調の切り替え (show) は設定に保存しない (既定は表示)。「ここまで確認済み」だけを EditorSettings へ — 仕様は切り替えの保存を求めていない
  - [未実装] 「シーンを開く導線」(任意と書かれていた)。別のシーンの行は選ばず「このシーンは開いていません」と出すだけ
  - [逸脱] 子孫の薄い印は、表を取り直したとき (と、シーンを開いたとき) の親子で作る。Hierarchy で親子を付け替えた直後は、次に表が取り直されるまで古い — 仕様どおり「毎フレーム木を走査しない」を優先した結果
  - [追加] 試験 9 (Play 中の切り替え) は、実際の TickRunner ではなく、決定的な「1 Tick」(位置を Tick 数から決める) を同じ回数回した 2 つのシーン (切り替え + Hierarchy の描画あり / なし) の HashWorld を比べる。強調の経路は sim に入口が無く、World を書かないことを確かめる形にした
  - [追加] ChangeTrackingSelfTest.cpp の偽の I/O を ChangeTrackingFakeIo.h へ移した (新しい試験が同じ道具を使うため)。振る舞いは変えていない
検証:
  - cargo test --manifest-path tools\track\Cargo.toml → 246 件通過 (前 236 + 単体 8 + 結合 2)。コンパイルの警告は既存の 1 件 (pipe.rs の unused_mut) だけ
  - tools\build_track.bat Release → 成功。tools\track_verify.bat → 全 26 シナリオ PASS (26_scene_overlay を新規に撮り、期待値は読んで確認: 別シーンが混ざらない・元へ戻ると消える・afterRevisionId・mcp/hook forbidden)
  - MyEngine.sln を Debug|x64 と Release|x64 で /p:MyeWarnAsError=true ビルド → 成功 (警告 0)。プローブを外したあとにもう一度
  - bin\x64\Debug\Editor.exe --selftest → 112 スイート 0 失敗 (changetracking PASS)。bin\x64\Release\Editor.exe --selftest → 112 スイート 0 失敗。どちらも他のビルドと並走させていない。--selftest-full、replay_verify は未実行 (この sub の検証コマンドに無く、ECS・シミュレーションを触っていない)
  - MYE_TRACK_PROBE=1 + MYE_SELFTEST_ONLY=changetracking (Release) → 実サービスでシーンを外から書き換え → scene_overlay に出る → 表に入る、が PASS。終わりに mye_track.exe が残っていない
  - tools\check_rules.ps1 → 0 error (65 warning は既存。新規は規則 7 の「順序非依存を確認」が SceneChangeOverlay.cpp の 2 か所 = 件数・集合への挿入だけで順序に依存しない)。下位の層の差分は LocalizationTable.inl だけ (git diff --stat -- src\Engine)
  - スクショ (一時のプローブ + Release の Editor.exe --screenshot。プローブは外し、git diff に残っていない): C:\HAL\MyEngin\cache\m104m_shots\1_tree_ja.png (Hierarchy の通常表示。Main Camera に緑、Sun に青、折りたたまれた Spinner に薄い青)、2_search_ja.png (検索表示 "sun" で Sun に青 + AI Changes 窓の帯と一覧、日本語)、3_english.png (英語)、4_window_ja.png (帯・「シーンの変更」の一覧、日本語)。撮れた・文字が切れていない (窓が画面より広いのは既存の大きさ)・化けていない・ImGui のエラー表示が出ていない、まで確認。**見た目の合否は目視 (未確認)**
自己採点 (1-5):
  仕様適合: 4 — 条件 1〜10・12・13 を機械で検証。シーンを開く導線は未実装 (任意)、子孫の薄い印は取り直しの時点の親子で作る (逸脱の欄)
  正しさ: 4 — まとめ方 8 件 + 結合 3 件 + 試験 70 件超で確認。見落としうるのは、実 Editor での操作 (本物のクリック・Ctrl+S 後の見え方)、Play 中の実 TickRunner での確認 (World を書く経路が無いことの確認のみ)
  コード品質: 4 — 純粋な関数 (サービス) と表示 (Editor) を分け、World に触る箇所は RebuildAncestors と SelectEntityByFileId だけ。偽の I/O を共有化。overlay のクラスは表・取り直し・ack・子孫の 4 つを持つので少し大きい
  テスト: 4 — 取り直しの契機・間引き・世代・ack の保存・Hierarchy 2 経路・World 不変・選択の同期・E の 4 通りを自動で確認。実機の人手の操作と見た目は未確認
不安・質問:
  - 強調の切り替えを EditorSettings へ保存するか (今は保存しない)。sub-24 の色の設定と一緒に決めるのが自然
  - 子孫の薄い印の鮮度 (親子を付け替えた直後に古い)。Hierarchy の変更の通知口が無いので、sub-24 以降で必要なら表の再計算の合図を足す
  - ChangeTrackingSession.NoteSaved の保存直後の stat は 1 回 (std::filesystem)。ネットワークドライブなど更新時刻の粒度が粗い場所では、同じ大きさの書き換えを見逃しうる (限界は attribution-rules.md §3 に書いた)
触ったファイル:
  tools\track\src\scene_overlay.rs, tools\track\src\lib.rs, tools\track\src\protocol.rs, tools\track\src\server.rs, tools\track\tests\pipe.rs, tests\track\26_scene_overlay.ndjson, tests\track\26_scene_overlay.expected.ndjson,
  src\Editor\ChangeTracking\SceneChangeOverlay.h, SceneChangeOverlay.cpp, SceneChangeOverlaySelfTest.cpp, ChangeTrackingFakeIo.h, ChangeTrackingSession.h, ChangeTrackingSession.cpp, ChangeTrackingSelfTest.h, ChangeTrackingSelfTest.cpp, ChangeModel.h, ChangeModel.cpp, ChangeFormat.h, ChangeFormat.cpp, TrackProtocol.h, TrackClient.cpp,
  src\Editor\Windows\Project\AiChangesWindow.h, AiChangesWindow.cpp, src\Editor\Windows\Scene\HierarchyWindow.h, HierarchyWindow.cpp, src\Editor\App\EditorApp.cpp, EditorSettings.h, EditorSettings.cpp,
  src\Engine\Core\Localization\LocalizationTable.inl, build\Editor.vcxproj, build\Editor.vcxproj.filters,
  docs\ai-change-tracking\attribution-rules.md, ipc-contract.md, architecture.md, docs\test_checklists.md, engine_spec.md, plans\m104-ai-change-tracking.md, plans\m104-ai-change-tracking\sub-12.md
  (生成物でコミットしないもの: cache\m104m_shots\ のスクショと run ログ)
申し送り:
  - 作った一時物は、自分が作った /tmp の 1 ファイルと %TEMP%\m104m_*.txt 系 (selftest の出力) を消した。止めたプロセスは、自分が起動した Editor.exe (pid 36096、47828、42340。スクショの run が --frames 無しで終了しなかったため) だけ。既存の %TEMP%\mye_track_*、%TEMP%\mye_m104b\、cache\m104k_shots\、cache\m104k2_shots\ は触っていない。cache\m104m_shots\ の古い (プローブの初版の) ファイルは、名前が同じものを上書きし、残りの .log / .err はそのまま残る (cache の削除は許可の確認で止まったため)
  - sub-24 へ: Inspector の Field 差分は `SceneChangeOverlay::Find(fileId)` の `components[].fields[]` (before / after / lastRevisionId / overrideField) をそのまま使える。ワイヤ箱の色は SceneChangeOverlay.h の kOverlayAddedColor / kOverlayModifiedColor / kOverlayFaintAlpha を設定へ置き換える
  - sub-13 へ: AiChangesHost に selectEntity を足した。未保存シーンの確認の経路 (ReloadHub) は触っていない
  - 既存の気づき: tools\track\tests\pipe.rs の scene_diff の試験の `let mut service` が unused_mut の警告 (既存、直していない)
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、下位の層の差分は `LocalizationTable.inl` (47 行) だけ、`schema\` / `tools\mcp` / `.claude` / `.mcp.json` に差分なし、このサブの `src` の差分に `EnsureFileId` / `GuidForPath` / `getenv` の追加なし (プローブが残っていない)、`Editor.exe` は最後のソースの変更より新しい。planner の実走: Release の `--selftest` 全体が 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`tools\check_rules.ps1` 0 error、`cargo test` (track) 246 件通過、`tools\track_verify.bat` 全シナリオ PASS (26_scene_overlay を含む)。スクショ `1_tree_ja.png` を開いて見た: Hierarchy の行の右に緑・青・薄い青の印が出ていて、文字の切れ・化けなし。**見た目の合否はユーザーの目視 (未確認)。**
  - 追加・逸脱の判定: `scene_overlay` の結果の項目の追加 (`entityFields`、`hierarchy`、`changeType` など)、強調の切り替えを保存しない、子孫の薄い印を取り直しの時点の親子で作る、条件 9 を「World を書かないこと」の確認にした、偽の I/O の切り出し、は全部採る。「シーンを開く導線」の未実装は可 (任意だった)。
  - 質問への回答: (1) 強調の切り替えの保存は sub-24 で、色と一緒に `EditorSettings` に入れる。(2) 子孫の薄い印の鮮度はそのままでよい (親子を付け替えた直後に一時的に古いだけで、次の取り直しで直る)。(3) `NoteSaved` の確認の粒度の限界は文書にあるとおりで足りる。
  - nit: `cache\m104m_shots\` にプローブの初版のログが残っている (削除は許可の確認で止まった、との報告)。コミットしない場所で、消すかどうかはユーザーの判断。`tools\track\tests\pipe.rs` の `unused_mut` の警告 1 件は、次に `tools\track` を触るサブで直す。