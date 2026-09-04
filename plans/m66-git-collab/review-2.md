# review-2 — M66 エンジン内 Git 連携 v1 (修正ラウンド)

- 日付: 2026-09-04
- 対象コミット範囲: `4ab132247d8e8143ed63ae4d26f92b42af24c230..81aa33d` (= `7d99a85` M66k + `81aa33d` M66l)
- 全体の対象は引き続き `02bf3c9..HEAD` (M66a〜M66l の 12 コミット)
- 前回: `plans/m66-git-collab/review-1.md`

REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — round 1 の major 2 件は**根から**直っている。#1 は 3 手を `SaveThenCommit` という純関数へ切り出し「1 手目が空パスなら以降を実行しない」を仕様 (§4.1) とテストの両方で固定、#2 は「不在時の比較相手」を呼び手が渡す 3 引数版を足して `InputActions{}` と比べる形にした (`PhysicsLayerNames::DiffersFromDisk` と同じ考え方に揃った)。修正で新たに現れた穴も自分で見つけて塞いでいる (probe の失敗コミット検査が未追跡ファイル 1 個で空振りしていた件、`DrawRemoteBar` の 5 か所も同型で切れていた件)。減点は新規 minor 1 件 (指摘 1) — 分岐した (ahead>0 かつ behind>0) 状態でブランチ名が長いと ahead がどこにも表示されなくなる。壊れはしないが、`pull --ff-only` が失敗する理由が画面から読めない
  機能性: 4 — 受け入れ条件 22 件のうち機械検証できるもの (1〜6, 13, 16, 18, 19, 20, 22) は全部緑。追加された 18/19/20 は新しいセルフテスト 16 本が実走し、22 は `MYE_COLLAB_PROBE` の PASS 行を**未追跡だけ / staged あり / サイドカーだけ staged**の 3 通りで数えて仕様どおり (14 / 12 / 12) であることと、走らせてもリポジトリのコミットが増えないことを確認した。21 は自分で撮った ja 5 枚 + en 1 枚で確認し、1 件だけ抜けを見つけた (指摘 1)
  ビジュアルデザイン: 4 — 既定ドック幅 287 px の ja で、round 1 で切れていたヒント文と `DrawRemoteBar` の 5 種がすべて折り返して読めるようになった (`cache/r2_ja_noremote_win.png` / `r2_ja_uptodate_win.png` / `r2_ja_ahead_win.png` / `r2_ja_diverged_win.png` / `r2_ja_conflict_win.png`、en は `cache/r2_en_conflict_win.png`)。残る 1 点はヘッダ行の 先行/遅れ (指摘 1)
  コード品質: 5 — 直しがすべて「純関数 + セルフテストで固定」の形に落ちていて、UI を描かずに壊れを検出できる。却下した代案 (EndBatch で 1 デバウンス分無視する / 常時の回復案内 / 4 窓への砦拡張) が理由つきで残っている。`TextDiffersFromDisk` の 2 引数 / 3 引数併存も、2 引数版が完全に旧挙動へ委譲していて既存 3 窓の挙動が 1 バイトも変わらないことを差分で確認した。コメント腐り 2 件 (`BuildSettingsWindow.h` / `AssetOps.cpp`) も実態に合わせ直っている。新たな品質上の指摘は無し

指摘:
  1. [minor] 宛先: coder — **ブランチ名が長いと、ヘッダ行の「先行 %d / 遅れ %d」が画面から丸ごと消える。分岐 (ahead>0 かつ behind>0) のときは ahead がどこにも出なくなる。** `src/Editor/Windows/SourceControlWindow.cpp:234-241` はブランチ名 → upstream 名 → 先行/遅れ を `SameLine()` で 1 行に並べる。既定ドック幅 (287 px) では `main` / `origin/main` なら 3 つとも収まる (`cache/r2_ja_uptodate_win.png` に「main origin/main 先行 0 / 遅れ 0」が写っている) が、`feature/inventory-rework` のような普通の長さのブランチ名だと upstream 名の途中で右端に達し、先行/遅れは**描かれた形跡ごと消える** (`cache/r2_ja_ahead_win.png` / `cache/r2_ja_diverged_win.png` / en も同じ `cache/r2_en_conflict_win.png`)。窓に横スクロールバーは無い (`ImGui::Begin(name, &open)` にフラグ無し) ので、スクロールして読むこともできない。
     ふだんは実害が無い — `DrawRemoteBar` が behind なら「新しいコミットが N 件」、ahead なら「送信していないコミットが N 件」を出すので、片方だけなら帯で分かる。抜けるのは**分岐しているとき**で、`同:526-556` は `if (behind > 0) {…} else if (ahead > 0) {…}` と排他なので behind の帯だけが出て ahead は帯にも出ない。再現手順: `pwsh -File tools\collab_fixture.ps1 cache\x` → bare origin を足して `feature/inventory-rework` を push → 自分で 2 コミット、相手が 1 コミット push → `git fetch` → `Editor.exe --project cache\x --lang ja`。画面には「新しいコミットが 1 件」しか出ず、自分の未送信 2 件はどこにも無い。これは `pull --ff-only` が `non_fast_forward` で失敗する状態そのもので、**なぜ普通に取り込めないのかが押す前に読めない**。
     spec §4.3 の幅の規則では「途中で切れても意味が壊れないもの」だけが折り返さない対象 (= 語) なので、意味ごと消えるこの表示は規則の側に立っても直す対象に入る。撮った画面に写っている実測なので「推測で全箇所を触らない」にも触れない — 期待: 先行/遅れ を独立した行にする (ブランチ名 / upstream 名は語なので今のまま切れてよい)、または分岐時に `DrawRemoteBar` の帯へ ahead も併記する。どちらでも「分岐している」が 1 か所で読めればよい

検証した手段:

**前提の再確認**
- `MSBuild MyEngine.sln` Debug / Release とも exit 0 (警告なし)。Rust は 2 コミットとも無変更なのを `git diff --stat` で確認済みだが、念のため `cargo test` (24 + 35 + 6 = 65 本) と `tools\collab_verify.bat` (9 シナリオ) も回して緑

**受け入れ条件 (spec §5、22 件)**
| # | 結果 | 実行したもの |
|---|---|---|
| 1 | ○ | `cargo test` → 65 passed / 0 failed |
| 2 | ○ | `tools\collab_verify.bat` → `all scenarios passed`、exit 0 |
| 3 | ○ | `pwsh -File tools\check_rules.ps1` → 0 error / 0 warning (規則 9・12 の「赤になること」は round 1 で worktree を使って実測済み。今回は対象コードが変わっていないので再実測はしていない) |
| 4 | ○ | `--selftest` Debug / Release とも exit 0。PASS 行 3171 (round 1 は 3155 = 新規 16 本ぶん増えている)、`Source control self test PASSED` |
| 5 | ○ | `tools\replay_verify.bat` → `[PASS] replay consistency (…7 scenes…)`、exit 0 = **sim 無変更を維持** |
| 6 | ○ | `tools\shot_verify.bat` → `[PASS] screenshot regression (19 shots…)`、exit 0。acoustic 2 枚も 1 回目で緑 |
| 7〜17 | ○/△ | round 1 で確認したものは、2 コミットが触っていない範囲 (Rust / ci.yml / パッケージ / ReloadHub の挙動 / バッジ) なので再実行していない。実機の窓は今回も 6 枚撮って壊れていないことを確認 |
| **18** | ○ | セルフテスト新規 6 本が実走: `save+commit: a failed save stages nothing and commits nothing` / `…a successful save stages that document, then commits` / `…with no save hook wired nothing is staged or committed` / `commit message: a failed commit keeps what the user wrote` / `…a successful commit clears the box` / `…text typed while the commit was in flight is not thrown away`。加えて「送れなかった commit も必ず 1 回応答する」2 本 (`kServiceDead` / `kBadRequest`)。呼び出し側の配線 (`SourceControlWindow.cpp:1029-1031` が `SaveThenCommit(host.saveDocument, …)` を通し、`:1063-1071` の `SubmitCommit` が応答で消す) は差分で確認 |
| **19** | ○ | セルフテスト 4 本: `a project without actions.json is not dirty` / `an action added but not saved is dirty (file still absent)` / `saving clears the dirty state` / `without an assets root there is nothing to compare`。実ファイル (`%TEMP%\mye_scm_actions`) を使う本物の往復。窓経由の実機確認だけは未了 (下記) |
| **20** | ○ | セルフテスト 3 本: 境界 (0 / 0.2 / 14.9 / **15.0 ちょうど**) は出さない、15.1 と 600 で出す、文言がローカライズ済み。実装は `GitTransaction.cpp:853-861` が `ImGui::GetTime() - runningSince_` で判定 (`runningSince_` は `:385` の `BeginOp` で毎回セット。git は DLL の worker が回すのでフレームは進み続ける) |
| **21** | △ | 既定ドック幅 287 px で **ja 5 枚 + en 1 枚**を自分で撮った。round 1 で切れていた `Scm_SelectToStage` は 3 行に折り返して全文が読める。`DrawRemoteBar` の 5 種も確認: リモート無し (`r2_ja_noremote_win.png`)、up-to-date (`r2_ja_uptodate_win.png`)、ahead (`r2_ja_ahead_win.png`)、behind + 分岐 (`r2_ja_diverged_win.png`)、競合モード ja/en (`r2_ja_conflict_win.png` / `r2_en_conflict_win.png`)。**文はどれも切れていない**。ただしヘッダ行の 先行/遅れ が消える (指摘 1) ので △ |
| **22** | ○ | `MYE_COLLAB_PROBE` の PASS 行を 4 通りで実測: (a) 素の fixture = **14**、(b) 未追跡ファイル 1 個を足した fixture = **14** + skip ログ 0 件 (= 空振りしていない)、(c) `git add` で 1 件 staged = **12** + `skipped the failing-commit check` 1 件、(d) **サイドカーだけ staged** (`assets/textures/test.png.meta` のみ) = **12** + skip 1 件。(d) は spec が §5 の 22 に書き足した保守側の分岐で、実際に飛ぶことを確認した。4 通りとも実行後の `git log --oneline` は `fixture: initial` の 1 行のまま = **プローブがリポジトリを変えていない** |

**新しい修正が壊していないかの確認**
- `DrawRemoteBar` / `DrawChanges` の `PushStyleColor` ⇄ `PopStyleColor` の対応を 3 経路 (リモート無し / upstream 無し / それ以外) と 3 分岐 (behind / ahead / 同じ) で読み直し、すべて釣り合っていることを確認。実画面 6 枚でも色が崩れていない
- `TextDiffersFromDisk` の 2 引数版は `TextDiffersFromDisk(path, inMemory, "")` に委譲し、不在時は `Normalize(inMemory) != Normalize("")` = 旧来の `!Normalize(inMemory).empty()` と同値。**既存 3 窓 (Animation / Animator / Mixer) の挙動は 1 ビットも変わらない**
- **司会の質問「他に 3 引数版へ移すべき窓が無いか」への答え = 無い。** 残り 3 窓は「ライブラリに実在するアセット」を相手にしていて (`AnimationWindow.cpp:405-408` は `anims_->Get(hash)` が非 null の clip だけ、`AnimatorControllerWindow.cpp:325-329` も同様、`AudioMixerWindow.cpp:453-458` は `ActiveHash()` のアセットが無ければ即 false)、そこで「ファイルが無い」は本当に「メモリにある資産がディスクに無い = 今上書きされたら失われる」を意味する。`InputActions` だけが「常に存在してしまうグローバル」で性質が違う。`PhysicsLayerNames` / `PartTagNames` は元から読み直し比較なので対象外
- `Commit()` のシグネチャ変更 (`WriteDoneFn done = {}`) の呼び出し元は `SourceControlWindow.cpp:1067` の 1 か所だけ (grep)。`GitTransaction` は commit を通さないので一括モードには無関係
- 応答コールバックが `this` を掴む件: `CollabClient::Shutdown()` は `pending_.clear()` するだけでコールバックを呼ばない / commit は Write 扱いで `timeoutMs = 0` なので `ExpireTimedOut` からも呼ばれない → 窓が畳まれた後に呼ばれる経路は無い
- `ReloadHub.cpp` の差分はコメントのみ (コード 0 行) を差分で確認。`replay_verify` / `shot_verify` が緑なのと整合

**前回指摘の消込**
| # | 判定 | 根拠 |
|---|---|---|
| 1 (major) | **解消** | `SourceControlWindow.cpp:1029-1031` が `SaveThenCommit` を通し、`SaveThenCommit` (`:1039-1054`) は `saved.empty()` で `stage` も `commit` も呼ばずに false を返す。契約側 (`EditorApp.cpp:808-814`) の「空を返す = 何もしない」と一致した。セルフテスト 3 本が固定 (フックが空を返す / 成功する / フック自体が無い) |
| 2 (major) | **解消** | `ProjectSettingsWindow.cpp:427-438` が `InputActionsDifferFromDisk` を呼び、そこ (`:427-437`) が `InputActions fresh;` の `ToJsonText()` を「不在時の比較相手」として渡す。`%TEMP%` の空ディレクトリを使うセルフテスト 4 本が「actions.json 不在では dirty にならない / 1 本足せば dirty / 保存で消える」を実走。spec §4.1 の元の一文 (「ファイルが無ければメモリに何かあれば dirty」) も撤回された。**窓を実際に開いてゲートが開いたままであることの目視だけは未了** (レビュアーはクリックできない。判定ロジックは実ファイルで実走済み) |
| 3 (minor) | **解消 + 拡大** | `DrawChanges` のヒント 3 種が `TextWrapped` になり、ja で 3 行に折り返して全文が読める (`cache/r2_ja_noremote_win.png`)。加えて coder が `DrawRemoteBar` の 5 か所も同型で切れているのを見つけて直しており、ja のリモート無し / up-to-date / ahead / behind をすべて撮って確認した |
| 4 (minor) | **解消** | `SubmitCommit` (`:1063-1071`) が投げた本文を控え、応答で `ShouldClearCommitMessage(ok, sent, current)` が true のときだけ消す。セルフテスト 3 本 + `MYE_COLLAB_PROBE` の実 DLL 経路 2 本 (`a commit with an empty index answers with a failure` / `a failed commit leaves the message in the box`) が固定。送れなかった commit も必ず 1 回応答する形になった (`SourceControlState.cpp:815-826`) |
| 5 (minor) | **却下 (ユーザー確定) — 妥当と判断** | 却下の根拠 3 点を実コードで再確認した: (a) 競合中は `DrawChanges` が `DrawConflicts` へ抜けて stage ボタンが存在しない (実画面 `cache/r2_ja_conflict_win.png` / `r2_en_conflict_win.png` に stage 行が無い)、(b) `resolve` は `checkout --ours|--theirs` → `add` で working tree を index から上書きする (`ops.rs`)、(c) `continue` は index を `commit --no-edit` する。したがってエディタの UI だけを使う限り、窓の Save が書いた内容が共有履歴へ到達する経路は無い。残存リスク 2 つ (外部ターミナルの `git add` / 競合文書を触っていた窓がゲートを閉じる) は `engine_spec.md` §14.6 に明記された。**蒸し返さない** |
| 6 (minor) | **解消** | `BuildSettingsWindow.h:33-39` が「呼び出し元は 2 経路 (この窓と `EditorApp::PollScriptBuild`)、観測できないビルド経路はもう無い」に書き換わり、`IsRunning()` は削除。coder が `AssetOps.cpp:1348-1353` にも同型の腐り (`RebuildGameLogic = fire-and-forget`) を見つけて一緒に直した |
| 7 (minor) | **解消 (コメントのみ)** | `ReloadHub.cpp:168-182` がデバウンス残りで二度読みが起きることと、その実害 (同じ内容の再適用で結果は変わらない)、そして「1 デバウンス分無視する」を採らない理由 (EndBatch 直後の本物の外部編集まで飲み込む) を明記。コード差分ゼロなのは妥当 — 私の指摘も「コメントが実態と食い違う」だった |
| 8 (minor) | **採用 (一部) — 妥当と判断** | キャンセルは作らず、`Phase::Running` が 15 s を超えたときだけ `Scm_OpStuckHint` を出す (`GitTransaction.cpp:853-861`)。しきい値は純関数 `ShouldShowStuckHint` でセルフテストが境界込みで固定。`engine_spec.md` §14.6 にも「返らない書き込みはエディタを終了して回復する / 失うのは undo 履歴だけ」が入った。**画面での発火は未確認** (15 s 以上返らない git を作るには hook が要り、実行にはクリックが要る) |
| 9 (minor) | **解消** | `engine_spec.md` §14.5 に `MYE_COLLAB_PROBE=<repo> Editor.exe --selftest` の行が入り、何を検査するか / いつ飛ばすか / 「未追跡は staged ではない」まで書かれている |

**再現できなかったこと (round 1 と同じ制約)**: マウス押下を要する経路。今回は特に (a) Project Settings 窓を開いた後に破棄ボタンが押せること (受け入れ条件 19 の実機側)、(b) 15 s の回復案内が実際に画面へ出ること (受け入れ条件 20 の目視側)、(c) Branches / History タブの選択、(d) 「保存してコミット」の実押下。いずれも判定ロジックはセルフテストか実 DLL プローブで実走しており、残るのは 2〜3 行の配線。
**根拠が無いので指摘にしなかった観察**: `AudioMixerWindow::HasUnsavedChanges` は `AudioSystem::CurrentMixer()` (ライブのバス構成) を `MixerLibrary::ToJson` に通してディスクと比べる (`AudioMixerWindow.cpp:461-465`)。この往復が 1 ビットでも非可逆なら、ミキサーを持つプロジェクトで Audio Mixer 窓を 1 度開いた時点で #2 と同型の恒久的なゲート閉鎖になる。fixture にミキサーが無く窓も開けないため**再現も反証もできていない**ので指摘にはしない。M66 の外で気にする価値はある。
