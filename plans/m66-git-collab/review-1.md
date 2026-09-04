# review-1 — M66 エンジン内 Git 連携 v1

- 日付: 2026-09-04
- 対象コミット範囲: `02bf3c924a6b5036255f5e31a694b4bd8c054055..4ab132247d8e8143ed63ae4d26f92b42af24c230` (M66a〜M66j の 10 コミット)
- 仕様: `plans/m66-git-collab/spec.md` (確定 2026-09-02) / sub-01〜sub-10 / harness.md

REVIEW: FAIL
round: 1
軸 (1-5):
  製品の深度: 3 — 条件の外側を 8 通り突いて、7 通りは正しく耐えた (非 ASCII + 空白入りパスの status/stage/commit/log 往復 = `cache/rev_utf8`、300 パスの一括 stage = `MAX_PATHS_PER_CALL` の分割が効いて 936 ms で全 300 件、リポジトリのサブディレクトリをプロジェクトにした場合の `ToplevelMismatch` 縮退 = `cache/rev_ui_06_win2.png`、`MyeCollab.dll` を退避した起動 = `cache/rev_perf_d_win.png` で `NoService` + 他機能無傷、外部 git で作った競合を抱えたままの起動 = `cache/rev_ui_03.png`、未出生ブランチ / detached HEAD / リモート無しの分岐、service_error → ゲート閉鎖)。ただしエラー経路で 2 件の実害が残る (指摘 1・2)。どちらも「保存が失敗した」「まだ 1 度も保存していない」という**正常系の外側**で、v1 の目的 (エディタを閉じずに安全に git を通す) を直接損なう
  機能性: 3 — 受け入れ条件 17 件のうち機械検証できる 1〜6・13・16 は全部緑 (下記「検証した手段」)。実機 7・8(一部)・9(一部)・12・14 は目視で確認。指摘 2 は受け入れ条件 9 の**裏返し** (未保存でないのにゲートが閉じる) で、条件そのものには載っていないが機能を止める
  ビジュアルデザイン: 4 — 窓・バッジ・競合モード・利用不可表示のいずれも意図どおり。バッジ色は spec §4.3 の確定表と一致し、選択行 (Accent の面) の上でも読める (`cache/rev_badges.png`、`cache/rev_cb_02.png`)。競合中の赤い見出しと `!` バッジも判別できる (`cache/rev_ui_03.png`)。**既定ドック幅 (287 px) で stage のヒント文が文中で切れる**のが 1 点 (`cache/rev_ui_05_win.png`、指摘 3)
  コード品質: 4 — コメントが「なぜ / 踏んだ罠」を一貫して書いており、実測値と出典 URL まで添えてある。規則 9 (proto 版) と規則 12 (Editor 層封じ込め) は**故意に壊して赤を実測**した (下記)。仮実装の残骸は無い。コメント腐り 1 件と未使用アクセサ 1 件のみ (指摘 6)

指摘:
  1. [major] 宛先: coder — **「保存してコミット」は保存に失敗してもコミットまで進む。** `EditorApp` 側のフックは「保存に失敗すると空を返す = 呼び出し側は stage も commit もしない」と契約を書いているのに、呼び出し側が `scm.Commit()` を `if (!saved.empty())` の**外**で呼んでいる — 根拠: `src/Editor/Windows/SourceControlWindow.cpp:1006-1011` (`saved` が空でも 1010 行の `Commit` は走る) と契約側 `src/Editor/EditorApp.cpp:808-814` (「空を返す = 呼び出し側は stage も commit もしない」)。保存が失敗する経路は実在する (`src/Editor/EditorApp.cpp:1333` の競合ガード、`:1345` の `SceneSerializer::SaveToFile` が false = 読み取り専用 / ロック / 書き込み不能)。既に別のファイルを stage してあれば commit は**成功し**、「保存してコミット」を押した人の意図と違う中身が共有履歴に残る (このコメント自身が 1002-1005 行で警告している事故そのもの)。さらに 1011 行が本文を無条件に消すので、失敗しても入力欄は空になり気付きにくい — 期待: `saved` が空なら stage も commit もせず、保存が失敗したことだけを伝えて本文を残す (コミット本文の保持は指摘 4 と同じ直し方でよい)
  2. [major] 宛先: coder — **`assets/input/actions.json` を持たないプロジェクトでは、Project Settings 窓を 1 度開くだけで書き込み系ゲートが恒久的に閉じる。** 根拠 (コード経路。UI を触れないレビュアーの制約上、画面での再現は取っていない): (a) `src/Editor/Windows/ProjectSettingsWindow.cpp:32-34` が窓を開いた最初のフレームで `inputActions_` / `assetsRoot_` を控え、閉じても戻さない。(b) `同:427-437` の `HasUnsavedChanges()` が `TextDiffersFromDisk(assetsRoot + "\input\actions.json", inputActions_->ToJsonText())` を返す。(c) `src/Editor/DiskCompare.cpp:34-37` はファイルが無いとき「メモリ側が非空なら未保存」と判定する。(d) `src/Engine/Platform/InputActions.cpp:325` の `ToJsonText()` は定義が 0 件でも `{"actions": [], "axes": []}\n` を返す = 必ず非空。(e) `src/Engine/Platform/InputActions.cpp:112-114` は不在なら空マップで返るだけで、`actions.json` を書く経路は `ProjectSettingsWindow.cpp:420` の Save ボタンしか無く、`src/Editor/ProjectTemplates.cpp` の `CreateProject` も `assets/input/` を作らない (grep で 0 件)。つまり**新規作成したプロジェクトは全部この状態**で、`cache/review_fx` の fixture も同じ。結果、`GateBlocker::ProjectSettingsDirty` が立ちっぱなしになり revert / checkout / pull / merge_abort / continue が全部「Project Settings の編集が未保存」で塞がる (`src/Editor/EditorApp.cpp:1568`)。同じ罠を `PhysicsLayerNames::DiffersFromDisk` は「ディスクを読み直した表と比べる」ことで意図的に避けている (`src/Editor/PhysicsLayerNames.h:20-25` のコメントが理由を明記) — 期待: InputActions も同じ方式にする (不在時は「読み直した空の InputActions」と比べる、あるいはファイルが無くかつ定義が 0 件なら差分無しとみなす)。合わせて「窓を一度も開いていなければ評価しない」だけでは足りない (開いた後も false のままである必要がある) ことをテストで固定してほしい
  3. [minor] 宛先: coder — **既定のドック幅で stage のヒント文が文中で切れる。** `src/Editor/Windows/SourceControlWindow.cpp:567-572` が `Stage` / `Unstage` ボタンの後ろに `SameLine()` + `TextDisabled("%s", Tr(StrId::Scm_SelectToStage))` を置いており、spec §4.3 が確定した既定ドック幅 (左列 ≒ 287 px) では "Select a file to stage it. The" で切れて残りが読めない (根拠: `cache/rev_ui_05_win.png` — 窓を 287x814 にして撮ったもの)。すぐ下の 576-581 行のコメントが「既定のドック幅 285px では 4 個目のボタンのラベルが実際に切れた」と同じ問題を扱っているので、幅の想定自体は共有されている — 期待: 折り返す (`TextWrapped`) か、ボタン行の**次の行**に出す。文字列は `src/Engine/Core/LocalizationTable.inl:1046`
  4. [minor] 宛先: coder — **コミット本文が応答を待たずに消える。** `src/Editor/Windows/SourceControlWindow.cpp:996-997` と `:1010-1011` が `Commit()` を投げた直後に `commitMessage_[0] = '\0'` する。`nothing_to_commit` / `identity_missing` / hook 失敗など commit が失敗する経路は `tools/collab/src/ops.rs:405-427` に実在し、その場合ユーザーが書いた本文は復元できない — 期待: 成功応答 (`ApplyWriteResult` が ok) を受けてから消す
  5. [minor] 宛先: planner — **競合中の保存ガードがシーン / アクター編集にしか無い。** spec §4.1「競合周り」は `SaveCurrentScene` 先頭の `IsConflictedPath` を「唯一の砦」と書いているが、M66d が dirty 追跡を足した残り 3 窓 (Animation / AnimatorController / AudioMixer) には同じガードが無い。根拠: `IsConflictedPath` の呼び出し元は `src/Editor/EditorApp.cpp:1333` の 1 箇所だけ (grep、SelfTest を除く)。競合した `.anim.json` はライブラリのパースが失敗して**pull 前の版がメモリに残る**ので、その窓の Save を押すと競合マーカーを ours で黙って上書きできる (index は未マージのままなので `resolve` からは復旧できる = データ喪失には至らないが、spec がシーンについて禁じた操作と同型) — 期待: 仕様として (a) 3 窓にも同じガードを要求する / (b) 「working tree だけの上書きは index が残るので許容」と明記して §14.6 の既知の制約に落とす、のどちらかを決める
  6. [minor] 宛先: coder — **M66e で塞いだ穴を「まだ空いている」と書いたコメントが残っている + 未使用アクセサ。** `src/Editor/Windows/BuildSettingsWindow.h:33-39` が「Asset Browser の [Rebuild Scripts] は `AssetOps::RebuildGameLogic` = ShellExecuteW の fire-and-forget でハンドルを持たないため、そちらが走っているかはエディタから観測できない」と書いているが、`RebuildGameLogic` は M66e で削除済み (`src/Editor/AssetOps.cpp:1428-1433` の削除跡コメント) で、`src/Editor/EditorApp.cpp:1555` が `scriptBuildProc_` を OR して観測できるようになっている。次に読む人が「まだ穴がある」と誤読する。同 `:39` の `IsRunning()` は全ソースで参照 0 件 (grep) — 期待: コメントの更新と、使わない `IsRunning()` の削除
  7. [minor] 宛先: coder — **`EndBatch` の「溜まりを捨てる」がデバウンス残りを取りこぼす。** `src/Engine/Engine/HotReload/ReloadHub.cpp:163-177` は `DiscardPendingChanges()` (= `DrainChanges`) で watcher の溜まりを捨ててから一括適用するが、`src/Engine/Core/FileWatcher.cpp:13,124-127` の `kDebounceMs = 150` により「最後の書き込みから 150 ms 経っていないパス」は `pending_` に残って drain されない。git が書き終えてから `EndBatch` までは数十 ms なので、変更したファイルの一部は **EndBatch の直後に通常経路でもう一度 `HandleChange` される**。実害は「同じファイルをもう一度読み直す」だけ (シーンは同内容の `ApplyDiff`、prefab は同 base の `PropagateBaseChange` で no-op) だが、コメントが「捨てるので 2 度読まない」と言い切っているのと食い違う — 期待: コメントを実態に合わせるか、`EndBatch` で適用したパスを 1 デバウンス分だけ無視する
  8. [minor] 宛先: planner — **書き込み系に中断手段が無く、返らない git でエディタが操作不能になる。** spec §4.4 は書き込み系を「無期限 (キャンセル無し)」と決め、engine_spec §14.2 もその理由を「打ち切っても git は走り続ける」と書いている。ただし `src/Editor/SourceControl/GitTransaction.cpp:842-843` の `Phase::Running` は「実行中」と出すだけでボタンが 1 つも無いモーダルなので、git が返らない限りエディタ全体が入力を受け付けない。stdin は NUL なので端末プロンプトでは止まらないが、チームリポでよくある `pre-commit` / `pre-push` の hook が待つ経路は塞げていない (hook は git が同期実行する)。ゲートが全文書の保存を保証しているのでプロセスを殺せば失うのは undo 履歴だけ、という前提は成立している — 期待: 「返らない git は想定内 / エディタを終了して回復する」ことを engine_spec §14.6 の既知の制約に 1 行足すか、Running に「エディタを終了して回復してください」の案内を出すかを決める
  9. [minor] 宛先: coder — **`MYE_COLLAB_PROBE` が恒久の検証フックなのに、どの文書にも書かれていない。** `src/Editor/SourceControl/SourceControlSelfTest.cpp:1218-1325` は env が立っているときだけ実 DLL 経由で status / hint / バッジ / revert / branches / checkout / ServiceDied を実走する良いフックで、実際に走らせて全 10 件 PASS した (下記)。しかし engine_spec §14.5 の検証表にも CLAUDE.md にも載っておらず、隣の `MYE_COLLAB_REQUIRED` だけが文書化されている — 期待: engine_spec §14.5 か CLAUDE.md の検証表に 1 行 (`MYE_COLLAB_PROBE=<repo> Editor.exe --selftest` で窓のボタンと同じ配線を UI 抜きで実走できる)

検証した手段:

**ビルド (全部 exit 0)**
- `MSBuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64` / 同 `Release` — 警告なく 4 プロジェクト
- `tools\build_collab.bat Debug` (PATH に `~/.cargo/bin` を足して) — cargo は最新、DLL/CLI を両構成へコピー

**受け入れ条件 (spec §5)**
| # | 結果 | 実行したもの |
|---|---|---|
| 1 | ○ | `cd tools\collab && cargo test` → 35 + 6 + doc 0、**0 failed** |
| 2 | ○ | `tools\collab_verify.bat` → 9 シナリオ全 PASS (01_status〜09_merge_abort)、exit 0 |
| 3 | ○ | `pwsh -File tools\check_rules.ps1` → 0 error。**赤も実測**: `git worktree` に HEAD を出して (a) `kCollabProtoVersion` を 2 にすると `ERROR [rule 9] ... must match across C++ and HLSL`、(b) `src/Engine/Engine/Project.cpp` に `#include "Editor/SourceControl/CollabProtocol.h"` を足すと `ERROR [rule 12] ... source control must stay in the Editor layer`。worktree は撤去済み、作業ツリーは未変更 |
| 4 | ○ | `bin\x64\Debug\Editor.exe --selftest` / `bin\x64\Release\Editor.exe --selftest` とも exit 0 (PASS 行 3155)。`Source control self test PASSED`、DLL 往復は SKIP ではなく実ロード (`MyeCollab.dll loaded (proto 1)` → `hello gets a response within 5s`)。(a)〜(k) の節が spec の (a)〜(i) を包含 |
| 5 | ○ | `tools\replay_verify.bat` → 並列 10 ジョブ 84.9 s、`[PASS] replay consistency (Debug/Release, 7 scenes ...)`、exit 0 = **sim 無変更** |
| 6 | ○ | `tools\shot_verify.bat` → `[PASS] screenshot regression (19 shots ...)`、exit 0。acoustic 2 枚も 1 回目で緑 (再実行不要) |
| 7 | ○ | `pwsh -File tools\collab_fixture.ps1 cache\review_fx` → `Editor.exe --project cache\review_fx` で status が出る (`cache/rev_ui_02.png`)。`MyeCollab.dll` を退避して起動 → `MyeCollab.dll was not found. Build it with tools\build_collab.bat (needs rustup stable).` + 他機能無傷 (`cache/rev_perf_d_win.png`)。裸起動は窓を開かない (`sourceControl_.open = !ctx.projectRoot.empty()`) |
| 8 | △ | commit の往復は CLI で実走 (下記「サービス直叩き」)。「保存してコミット」誘導は画面に出ている (`cache/rev_ui_02.png` の Commit 欄) が、**押下は再現できていない** (指摘 1 はコードで判定)。identity 未設定の案内はコード確認のみ |
| 9 | △ | ゲートが開いているとき「Discard all」が押せる状態は目視 (`cache/rev_ui_02.png`)。競合中は他の書き込み系が塞がることを目視 (`cache/rev_ui_03.png`: Finish が無効、Abort のみ有効)。revert → 実 DLL 経由の往復は `MYE_COLLAB_PROBE` で実走 (未追跡ファイルを置いて revert → ディスクから消える) |
| 10 | △ | checkout の実 DLL 往復は probe で実走 (現在のブランチへの切替 = names 空)。A/B/C 3 種のブランチ切替はマウス押下が要るため未再現 |
| 11 | ○ | 起動時 fetch のログ (`[collab] background fetch: 1 behind, 1 ahead (origin/main)`)、bare origin + peer で push → 相手 behind の往復は `07_remote` シナリオと自作の origin/peer で成立 |
| 12 | ○ | 外部 git で作った競合を抱えたまま起動 → 自動で競合モード、7 種別の `both changed it`、ours/theirs ボタン、`1 file(s) still conflicting` (`cache/rev_ui_03.png`)。`merge in progress - resolve or abort before anything else` が見出しに出る |
| 13 | ○ | `--selftest` の ParticleSelfTest 緑 + `shot_verify.bat` (内部で `--particle-backend gpu` を使う) の後も `assets/project_settings.json` は `git status` で無変更 |
| 14 | ○ | Content Browser のツリーとタイルの両方にバッジ (`cache/rev_cb_02.png`: フォルダタイル左上の M、ツリーの M / ?)。競合時は `!` (`cache/rev_ui_03.png`)。保存ヒントの往復は probe で 156 ms (`[probe] hint_changed round trip: 156 ms` < 監視デバウンス 300 ms) |
| 15 | ○ | `engine_spec.md` §14 (14.1〜14.6) / §11.2 の規則 11・12 / README の Source Control 節と検証コマンド / CLAUDE.md (43→44 スイート、build_collab、collab_verify、cargo test、op の足し方) / `docs/adr/ADR-015` を通読。件数はコードと一致 (op 23 = `ops.rs::dispatch` の match 23 本、event 4、error.code 18 + 合成 `timeout` 1、Unavailable 8、GateBlocker 13) |
| 16 | ○ | `bin\x64\Release\Editor.exe --package cache\rev_dist2` → `Runtime.exe` / `GameLogic.dll` / `assets\shaders` / `assets\scenes` あり、`MyeCollab.dll` / `MyeCollabCli.exe` / `.git` **なし** |
| 17 | △ | エンジンリポの既存ホットリロードは `replay_verify` / `shot_verify` が緑であることと `ReloadHub::Update` の batch 分岐 (batching_ = false のとき従来経路そのまま) から回帰なしと判断。hlsl / png / 外部編集 scene の手動 3 種は未再現 |

**条件の外側 (深度)**
- 非 ASCII + 空白入りパス: `cache/rev_utf8` に `assets/普通.mat.json` と `assets/新規 ファイル.txt` を置き、CLI 経由で `status` → `stage` → `commit`(日本語本文) → `log` → `diff` を実走。`core.quotepath=false` のおかげでパスが生 UTF-8 で往復し、日本語の件名も `log` にそのまま載る
- 一括: 300 ファイルを 1 回の `stage` で送信 → ok、`status.entries` 300 件、936 ms (`MAX_PATHS_PER_CALL=64` の分割が効いている)
- 壊れた要求: BOM 付き JSON を投げると `bad_request` + `worker.rs::extract_id` の手書き走査で id を救って返す (待ちコールバックが残らない) ことを偶然実測
- プロジェクトがリポジトリのサブディレクトリ: `cache/rev_sub/game` → `The project root is not the top of the repository.` + repo top の表示で機能ごと停止 (元計画 決定 22 のとおり。指摘にしない)
- DLL 不在 / service_error / ProtoMismatch: 前 2 者は実走、3 つ目はコード確認
- 性能: Source Control が毎フレーム git を叩いていないことを確認 (窓の要求はすべて `remoteRequested_` / `branchesRequested_` / `historyRequested_` / `diffRequestedPath_` の 1 回ガード)。FPS への影響も実測で 0 — DLL を退避した起動と載せた起動で 8.8 / 8.8 FPS (Debug + WARP、`cache/rev_perf_c.png` / `cache/rev_perf_d.png`)

**読んだ範囲**
- コミット範囲の diff 全体 (113 ファイル、+16943/-110) を通読。仕様に無い変更は見つからなかった。`plans/quiet-merging-harbor.md` の更新 (進捗表 + spec への誘導) は sub-10 の宣言どおり
- 精読: `tools/collab/src/{ops,worker,protocol,git}.rs`、`src/Editor/SourceControl/{CollabClient,GitTransaction,PairRule,StageClassifier,SourceControlState(抜粋),SourceControlSelfTest(抜粋)}.cpp`、`src/Editor/Windows/SourceControlWindow.cpp`、`src/Engine/Engine/HotReload/ReloadHub.*`、`src/Engine/Engine/Particles/ParticleSystem.*`、`src/Engine/Engine/Project.*`、4 窓の `HasUnsavedChanges`、`src/Editor/DiskCompare.cpp`、`.github/workflows/ci.yml`
- CI ステップの `cargo test ... && tools\build_collab.bat Release && tools\collab_verify.bat` は、cmd の `&&` 連鎖で bat を `call` 無しに呼んでも後続が走り、途中の失敗がそのまま exit code になることを scratchpad の再現で確認 (B 成功 → C 実行 / B が exit 3 → C 未実行かつ全体 3)。台帳の「未検証」1 件を消し込み

**画像**: `cache/rev_ui_01.png` (既定ドック) / `cache/rev_ui_02.png` + `cache/rev_badges.png` + `cache/rev_cb_02.png` (変更一覧とバッジ) / `cache/rev_ui_03.png` (競合モード) / `cache/rev_ui_05_win.png` (287 px 幅、指摘 3 の根拠) / `cache/rev_ui_06_win2.png` (ToplevelMismatch) / `cache/rev_perf_d_win.png` (NoService)

**再現できなかったこと**: マウス押下を要する経路 (「保存してコミット」「Switch」「Pull」「ours/theirs」の実押下、Branches / History タブの選択、設定ポップアップ)。reviewer はソースに触れないため一時プローブを足せず、`MYE_COLLAB_PROBE` で代替できる範囲までを実走した。指摘 1・2 はこの制約のためコード経路での判定で、画面での再現は取っていない。

前回指摘の消込: なし (round 1)
