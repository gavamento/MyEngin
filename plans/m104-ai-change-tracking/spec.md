# M104 AI Change Tracking (M104b〜M104t の実装) — 仕様書

- slug: m104-ai-change-tracking
- 状態: 確定 (2026-11-14。確定確認は司会が AskUserQuestion で取得。§7.1)
- 依頼原文: この間計画作成した差分表示のやつ実装したい
- 基点コミット: `ca6a3991ac6ea6bd217d3a3290aab24291b73a4a` (M104a 完了時点)

この仕様書は要求と設計を書き直さない。正本は次のとおりで、ここに書くのは「正本を疑って裏取りした結果」「受け入れ条件」「サブ分割」だけ。

| 役目 | 正本 (絶対パス) |
|---|---|
| 要求 | `C:\HAL\MyEngin\docs\ai-change-tracking\spec-v1.1.md` (以下「要求 §N」) |
| 設計 | `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` / `ipc-contract.md` / `db-schema.md` / `attribution-rules.md` / `restore-safety.md` (以下 arch / ipc / db / attr / restore) |
| 決定と却下案 | `C:\HAL\MyEngin\docs\adr\ADR-039-ai-change-tracking.md` |
| 進捗表・予約番号 | `C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (以下「計画」) |

食い違ったときの優先順: この spec.md の §2 の結論 > 設計文書 > 要求。設計文書を変える必要が出たら、そのサブの中で設計文書と ADR-039 も直す (AGENTS.md §8)。

## 1. 目的 (なぜ作るか)

AI クライアントがコード・シーン・アセットを書き換えたとき、開発者が「どこを・誰が・どう変えたか」を git の差分に頼らずに確かめ、範囲ごとに承認し、一部だけ安全に戻せる状態にする。
git の差分では AI 以前の未コミット変更が混ざり、シーンは JSON の行でしか見えず、部分復元の手段が無い (ADR-039 背景)。
達成したい状態は「見える」だけではなく「誤って AI のせいにしない」「戻す操作で作業を壊さない」「sim の決定性に 1 バイトも触らない」まで含む。

## 2. 疑った点と結論

2026-11-14 に planner がコードと公式文書で裏取りした。ユーザーの判断欄が「未確認」のものは planner の裁定 (AskUserQuestion が使えなかった) で、§7.1 に `[ユーザーに聞ける]` として並べてある。

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| D1 | 計画の進捗表は M104b を「試作 3 件」と書くが、表は 5 件ある | 計画 19 行目と 53〜63 行目 | — (事実の誤記) | 5 件が正。sub-01 で計画の表記を直す |
| D2 | 試作コードをコミットするか。残すと「将来使うかもしれないコード」になり、消すなら削除の事前承認が要る (AGENTS.md §1、§5) | 試作は Rust の小 crate と C++ の小 exe になる。`tools\track` の実装は M104c から | 裁定どおり (2026-11-14) | **試作コードはリポジトリの外 (`%TEMP%\mye_m104b\`) に作り、コミットしない。** sub-01 のコミットは文書だけ。再現できるよう、手順・コマンド・観測を `docs\ai-change-tracking\prototype-results.md` に残す。却下: リポジトリ内 `tools\track\prototypes\` に残す案 (本実装と二重になり、消すときに承認が要る) |
| D3 | 試作 2 の「CI のランナーでビルドできる」は push しないと確かめられない | CI は `.github\workflows\ci.yml` の `windows-2022` + `dtolnay/rust-toolchain@stable`。collab は既に `cargo test` を回している (141 行目) | 未確認 | sub-01 では開発機だけで確かめ、CI は「未検証」のまま残す。CI での確認は sub-05 (M104f、未決事項 2 の確認と同時)。却下: 試作のために branch を push する案 (ユーザーの push 判断を planner が先取りする) |
| D4 | サービスの exe の置き場所と見つけ方が設計に無い | `.mcp.json` は `tools/mcp/target/release/mye_mcp.exe` を直接指す。collab は `tools\build_collab.bat` が `bin\x64\Debug` と `Release` の両方へコピーする。arch §3 は「`mye_track.exe serve` を起動」とだけ書く | 未確認 | **仮置き**: `tools\build_track.bat` (build_collab.bat と同じ形) が `bin\x64\Debug\` と `bin\x64\Release\` へ `mye_track.exe` を置く。探す順は (1) 環境変数 `MYE_TRACK_EXE` (2) 自分の exe の隣 (3) MCP だけ: 自分の exe から見た `..\..\..\track\target\release\mye_track.exe`。sub-05 で確定し ipc に書く |
| D5 | エンジン配布物 (Releases) にサービスを入れるかが未定義 | `tools\make_dist.ps1` 72〜73 行目の `$required` に `MyeCollab.dll` はあるが `mye_mcp.exe` は無い | **配布物に入れる作業を M104 に含める** (2026-11-14。planner の裁定「後回し」は採られなかった) | sub-20 (M104u) を sub-19 の後に足し、`make_dist.ps1` と `release.yml` で `mye_track.exe` をエンジン配布物へ入れる。前提は D4 の置き場所 (`bin\x64\Release\`、Editor は自分の exe の隣を探す)。却下された planner の案: 後回し (配布物には入れず、入っていない Editor は「追跡停止中」で通常動作)。なお**ゲームの配布物** (`--package`) には入れない (要求 §14.2。`ci.yml` 196 行目の `$forbidden` に足す) |
| D6 | `.gitignore` に `tools/track/target/` が無い。計画の「実装時に直す既存のもの」から漏れている | `.gitignore` は `tools/collab/target/` と `tools/mcp/target/` を個別に書いている | — | sub-02 で `.gitignore` に足す (target は数百 MB) |
| D7 | 規則 9 に版定数の組を「M104h、M104j」で足す計画だが、規則 9 は site のファイルが無いと error になる | `tools\check_rules.ps1` 846〜849 行目 `ERROR [rule 9] missing file` | — | 段階的に足す: sub-05 で Rust の `PROTO_VERSION` を作る (規則 9 にはまだ入れない)。sub-07 で `tools\track\src\protocol.rs` と `tools\mcp\src\track_client.rs` の 2 site の組を足す。sub-09 で `src\Editor\ChangeTracking\TrackProtocol.h` を同じ組へ足す |
| D8 | 規則 19 の番号と対象 | `check_rules.ps1` の末尾は規則 18 (1447 行目)。規則 12 の対象は Engine / Runtime / GameLogic / Shared の 4 つで Server を含まない (1077 行目)。規則 15-a は Server を含む (1238 行目) | — | 2026-11-14 時点で 19 は空き。sub-09 着手時に確かめ直す。対象は ADR-039 決定 9 のとおり Server を含む 5 つ (規則 15-a の形)。`tools\check_rules_selftest.ps1` に違反の一時コピーの検査を足す |
| D9 | 規則 15-d: MCP から pipe を開くのに `OpenOptions` を使うと規則 15-d に当たる | `check_rules.ps1` 1270〜1283 行目。許可は「ファイル x API」の単位 | — | sub-07 で `$mcpSpawnAllowed` に `track_client.rs`、`$mcpWriteAllowed` に `'track_client.rs' = @('OpenOptions')` だけを足す。`create_dir_all` や `write` は許可しない (MCP は `.mye\ai_changes\` を書かない。arch §2.1) |
| D10 | 「親と運命を共にしない」(arch §3) は、MCP クライアントが `mye_mcp.exe` を kill-on-close の Job Object に入れていると成り立たない (子プロセスは同じ Job を継ぐ) | Win32 の Job は既定で子へ継承される。設計文書に検証項目が無い | 未確認 | **試作 6 として sub-01 に足す**: MCP 経由と Editor 経由で起動したサービスが、親の終了後に生き残るかを実測する。生き残らない場合の縮退は「親と一緒に終わり、次の起動時に `service_down` の欠落を記録する」(要求 §3.4 の範囲内)。責務の分け方は変えない |
| D11 | Editor が自動テスト中にサービスを起動すると、CI にプロセスが残り、テストが環境に依存する | arch §3 は「pipe が無ければ起動」。`Editor.exe --selftest` / `--screenshot` / `replay_verify.bat` の扱いが書かれていない | 裁定どおり (2026-11-14) | **`--selftest*` / `--screenshot` / リプレイ検証 / `--dump-index` などの非対話起動では、設定にかかわらずサービスへ接続も起動もしない。** sub-09 の受け入れ条件にする。`changetracking` スイートは偽の応答行だけで動く (計画「検証」) |
| D12 | 要求 §1.4 は「完全な 3D Side by Side 比較」を初期対象外と書くが、要求 §15 Phase 6 と計画 M104s は含めている | 要求 §1.4 と §15 が食い違う。2026-11-13 のユーザー判断は「設計の範囲は Phase 0〜6 の全部」で、実装の範囲ではない | 裁定どおり (2026-11-14) | 進捗表どおり sub-17〜19 に含める。ただし **sub-17 の着手前に「Phase 6 へ進むか」をユーザーへ確認する** (P2。ここで止めても要求 §18 の機能面 1〜8 は Phase 5 までで満たせる)。却下: 今 Phase 6 を外す案 (司会の指示は進捗表の踏襲) |
| D13 | 「M104n は独立しているので前倒しできる」(計画) と「Phase をまたいで先へ進まない」(司会の指示) | 計画 31 行目と 39 行目 | 司会の指示 (2026-11-14): Phase をまたがない | sub-13 は Phase 3 の中で行う。依存は sub-08 (Phase 2 の完了)。sub-09〜12 とコード上の依存は無い |
| D14 | フックの入力の項目名 (試作 5 の Claude Code 分) | 公式: https://code.claude.com/docs/en/hooks を 2026-11-14 に planner が取得。共通 `session_id` / `transcript_path` / `cwd` / `hook_event_name`、道具イベント `tool_name` / `tool_input` / `tool_use_id`、PostToolUse は加えて `tool_response`。Write は `file_path` / `content`、Edit は `file_path` / `old_string` / `new_string` / `replace_all`。設定は `.claude\settings.json` などの `hooks.PreToolUse[].matcher` + `hooks[].type="command"`。終了コード 2 は PreToolUse で**道具を止める**、他の非 0 は止めない。既定の時間切れは 600 秒 | — | 設計の前提 (attr §6.1) は成り立つ。追加の制約 3 つ: (a) フックは**終了コード 2 を絶対に返さない** (b) 設定例に `timeout` を明示する (既定 600 秒に頼らない) (c) pre と post の対は `tool_use_id` で結ぶ。`replace_all` は declared edit の適用で扱う。MultiEdit / NotebookEdit の入力は公式ページで確認できなかった → 未確認。対応しない道具は Claimed 止まりでよい |
| D15 | 試作 5 の Codex CLI / Gemini CLI 分 | 公式ページの存在だけ確認 (中身は未取得): Codex https://developers.openai.com/codex/hooks (PreToolUse / PostToolUse、`config.toml` か `hooks.json`。第三者の記事は「対象は Bash だけ」と書くが未確認)、Gemini https://geminicli.com/docs/hooks/reference/ (`BeforeTool` / `AfterTool`、`settings.json`、stdin に JSON) | — | **未確認として扱う。** sub-01 で coder が公式ページを読めれば結果を書き、読めなければ「未確認」のまま残す。v1 のフック CLI が対応を約束するのは Claude Code だけ。他は `change_report` (Claimed 止まり) で正しい (attr §6.1) |
| D16 | 依存の版。collab は「古い stable でも通る側」に倒している | `tools\collab\Cargo.toml`: edition 2021、`notify = { version = "8", default-features = false }`、`panic = "unwind"` | — | `tools\track` も edition 2021、notify は 8 系 `default-features = false`、単独 crate (workspace にしない)。`rusqlite` の版は sub-01 の試作で、開発機の rustc 1.98 でビルドできた版に固定する |
| D17 | 「追跡が作業ツリーを書かない」(arch §2.1) は主張だけで、機械的な検査が無い | 規則 15-d に相当するものが `tools\track` には無い | 未確認 | サービスのテストで担保する: 監視・記録の一連のシナリオの前後で、作業ツリー (`.mye` を除く) の全ファイルのハッシュが変わらないことを検査する (sub-03 の受け入れ条件)。静的規則は足さない (書く API は `.mye\ai_changes\` に必要なので、パターンでは区別できない) |
| D18 | UI の見た目の検証範囲 | ユーザーの既存の方針: UI の見た目の変更は Release ビルドのスクショまで、目視はユーザー | 既存の合意 | UI のサブ (sub-10、12、13、14〜18) は、偽データを流した `--screenshot` を撮って添える。目視の合否はユーザー。スクショが撮れたことと、見た目が正しいことを区別して報告する |
| D19 | 監査で見つかった別件 (AGENTS.md §4.1 の「規則15」誤記、`verify_job` の「22 ジョブ」、Prefab の Apply が Undo に載らない、`freshBlockers` の `tilePaletteDirty`) | 計画「申し送り」 | — | このマイルストーンでは直さない (AGENTS.md §1: 依頼と無関係な改善へ広げない)。ただし sub-07 で `docs\mcp-server.md` と ADR-032 を触るとき、「道具は 5 つ → 10」以外の行は変えない |
| D20 | ABI / SimSnapshot / TypeId を変えない、の確かめ方 | 現在値: ABI v35 = 202 スロット、SimSnapshot v82、TypeId 100 (arch §9、2026-11-11 時点の記録) | — | 各 Phase の最後のサブで `git diff ca6a3991 -- src\Shared src\Scripting\Interop.cs` に版・スロットの変更が無いことと、`src\Engine\Core\Ecs` に新しいコンポーネント登録が無いことを確かめる。Engine 層の差分は `ReloadHub` の口 (sub-13) だけ |

| D21 | sub-01 が arch §4 に書いた「flush は `FILE_SHARE_READ` だけで開いて読む」は、追跡が開いている間、他のプロセスの書き込み open・rename・delete を共有違反で失敗させる | Win32 の共有モードの仕様。試作 3 の `sharecheck` は「書き手がいると追跡が開けない」側だけを確かめ、「追跡が開いていると書き手が失敗する」側を確かめていない | 未確認 (技術的な裁定) | 探りは開いてすぐ閉じる。内容は全共有で読み、前後のサイズと更新時刻で安定を確かめる。書き手の失敗が 0 件であることを sub-03 の受け入れ条件にし、0 にならなければ探りをやめる。arch §4 は sub-03 で直す。却下: sub-01 を差し戻して文書だけ直す (実測が要るので sub-03 が適切) |
| D22 | `rusqlite` 0.42 / `sha2` 0.11 は最低の Rust が 1.89。collab の「古い stable でも通る側に倒す」方針と違う | prototype-results 試作 2。開発機は 1.98、CI は `dtolnay/rust-toolchain@stable` | 未確認 | `rust-version = "1.89"` を明示して採る。サービスは配布物にビルド済みで入る (D5) ので、ソースからビルドするのは開発者だけ。却下: 最低版の低い古い `rusqlite` へ固定する (bundled の SQLite が古くなり、版の組を試作し直すことになる) |
| D23 | 試作 6(b) (本物の MCP クライアントの Job の中での生存) を coder が確かめられない | 実行すると coder を動かしている `claude.exe` ごと終わる。本物の `mye_mcp.exe` は Job の中にいた (性質は読めず) | 未確認 | sub-07 でユーザーの手動確認にする (手順を `docs\test_checklists.md` に書く)。どちらの結果でも設計は成り立つ (生き残らなければ `service_down` の欠落) |
| D24 | Editor からのサービス起動に `StartChildProcess` を使う (arch §3) と、ハンドルが継承される | `C:\HAL\MyEngin\src\Editor\App\ChildProcess.cpp` 30 行目 `bInheritHandles = TRUE` | — | sub-09 で、ハンドルを継がせないサービス用の起動口を `src\Editor\ChangeTracking\` に置く。`StartChildProcess` は変えない |

| D25 | projectId を「`NormalizePathKey` と同じ結果」のハッシュにすると、末尾の区切りの有無で値が変わる。projectId は mutex と pipe の名前なので、同じ DB に書き手が 2 つできる | sub-02 round 1 の固定表: `C:\Proj\MyGame` → `64489cc1…`、`C:\Proj\MyGame\` → `33ee953a…`。`C:\HAL\MyEngin\src\Engine\Platform\PathUtil.cpp` の `NormalizePathKey` は末尾を落とさない | — (planner の仕様の穴) | projectId は末尾の区切りを落としてからハッシュする (ドライブのルートは除く)。それでも別名 (`subst`、ジャンクション、短い名前) は残るので、sub-05 で `.mye\ai_changes\service.lock` を共有なしで持ち続ける二重の守りを足す。却下: パスを `GetFinalPathNameByHandle` で実体へ解決する案 (C++ の `NormalizePathKey` と結果が離れ、Editor・MCP・フックの 3 か所で同じ解決を実装することになる) |
| D26 | DB が壊れて作り直したとき、ジャーナルから戻るのは `events` と `coverage_gaps` だけ (sub-02 の実装で判明) | db §6 の 1 は「`journal\` から入れ直す」としか書いていない | — | 観測のイベントに派生の表を導ける情報を全部入れ (sub-03)、作り直しの関数と往復のテストを sub-04 に置く |

| D27 | `notify` 8.2.2 は Windows で監視バッファのあふれを報告せず、黙って取りこぼす | sub-03 の実測: 16 スレッド × 4000 ファイル = 64000 件の作成で、届いた `Create` は 57064 件、`Err` 0 件、再走査のフラグ 0 件。ソース (`windows.rs` の `handle_event`) が転送バイト数を見ていない | 未確認 | 「黙って取りこぼさない」(A3、要求 §5.3) は、イベントの報告に頼らず**検証の走査**で満たす: 大量のイベントの後と、静かな時間に、ベースラインと実際の中身を突き合わせ、ずれを `gap` の変更と `coverage_gaps` に残す (sub-21)。却下: (a) このまま制限として文書化するだけ (取りこぼしが次の起動まで見えず、その間の変更の Before も狂う)、(c) `ReadDirectoryChangesW` を自前で呼んで `notify` を外す (確実だが監視の実装をまるごと持つことになる。(b) で足りないと実測で分かったときの次の手) |
| D28 | ベースラインは追跡対象の全ファイルの**内容**を Blob に複製する。記録の速さは 32000 ファイルで十数分 (約 40 件/秒、debug、負荷あり。原因は未計測) | `tools\track\src\observe.rs` の `record_baseline` が `record_snapshot` を呼ぶ。`blob.rs` は 1 件ごとに `sync_all` と読み直し。このリポジトリは git 管理下の 8 MiB 以下のファイルが 2949 本・391 MiB (2026-11-14 に planner が計測) | 未確認 | 内容の複製は続ける (無いと、フックの無い AI が最初に触ったファイルの Before が無く、差分を出せない)。sub-21 で、このリポジトリを対象にベースラインの所要時間と Blob の総量を実測し、遅い箇所を計測で特定してから軽くする (AGENTS.md §3.5)。却下: ベースラインはハッシュだけにする (各ファイルの最初の変更が `no_before` になり、差分表示という目的の中心が欠ける) |
| D29 | 1 回だけの保存が、負荷の下で `coalesced` と記録される | planner の実走で `tests\watch.rs:412` が 5 回中 1 回落ちた。判定が「イベントが 5 ms 超で 2 つ以上の塊」という実時間に依存している | — | sub-03 の受け入れ条件 15 (負荷の下で 1 回の保存は `complete`) を足して差し戻した。`coalesced` は多めに付ける側に倒してよいが、普通の保存に付いてはいけない |

| D30 | attr §4 は「`session_begin` でその時点の全ファイルの内容ハッシュを基準として覚える」と書くが、サービスは常時記録していて、同じ情報を既に持っている | sub-03 / sub-21 の実装: 全ファイルの最後の状態と、変更ごとの `observed_at` がある。このリポジトリで約 2950 ファイル | 未確認 | セッションの基準は `session_begin` の時刻とジャーナルの位置にする。`preexisting` は「開始より前の Revision」と「`gap` の Revision」。意味は attr §4 と同じで、写しを持たない。却下: セッションごとに全ファイルのハッシュの写しを持つ (同じ情報の二重持ちで、セッションの開始が重くなる) |
| D31 | 管理された操作 (フック) の Before / After を監視のデバウンスに任せると、`op_end` の方が先に届き、`coalesced` や窓の外れが起きる | sub-21 の計測: 1 件の変更が記録されるまで 422〜474 ms (デバウンス 300 ms + 書き手の待ち 100 ms)。フックの pre / post は道具の実行の直前・直後 | 未確認 | `op_begin` / `op_end` / `report` で、サービスが対象パスをすぐ flush する (上限 2 秒、超えたら `pending`)。要求 §5.2 の「Before 保存 → 編集 → After 保存」の順になる。却下: 監視だけに任せて窓の猶予で合わせる (実時間に依存し、負荷で外れる。sub-03 の `coalesced` と同じ型の不安定さ) |
| D32 | ipc §4.2 の `op_end` は `operationId` を取るが、フックは pre と post が別のプロセスで、pre が受け取った `operationId` を post へ渡す場所が無い | 公式文書 (D14): pre / post の両方に `session_id` と `tool_use_id` が入る | — | `op_begin` が `clientOpId` を受け、`op_end` は `clientSessionId` + `clientOpId` でも引ける。`session_begin` は同じ `clientSessionId` なら既存を返す。却下: フックが一時ファイルに `operationId` を書いて渡す (フックがファイルを書くことになり、後始末と競合が増える) |
| D33 | attr §6.2 の `worktree` は「その `workspace` を登録しているセッションが 1 つだけ」で Verified にする。`workspace` がプロジェクトのルート (普通の使い方) だと、人の編集まで Verified になる | 要求 §4.3「申告だけで Verified にしない」「隔離された作業領域を照合できたら Verified」 | 未確認 | 4 条件を全部満たすときだけ `worktree` を付ける: `isolated` の申告、プロジェクトルートが git のリンクされた作業ツリー (`.git` がファイル)、その `workspace` の running のセッションが 1 つ、Editor の接続が無い。却下: (a) attr §6.2 のまま (誤って確定する) (b) `worktree` を作らず Claimed を上限にする (要求 §4.3 の「隔離された作業領域」を満たす手段が無くなる) |

| D34 | `notify` の読み取りバッファは 16 KiB 固定で、2500 ファイルの作成でも、読む側が CPU を取れないとあふれて黙って失う | sub-06 round 2 の調査: `notify-8.2.2\src\windows.rs` の `BUF_SIZE = 16384`。CPU 負荷の下で、sub-05 の版でも再現。落ちた回は連番の 54〜130 件が届かず、処理待ちは 0 | 未確認 | (1) sub-06 の受け入れ条件 19 は、試験の前提が誤りだったので書き直す (取りこぼしが起きても起きなくても通る形 + 決定的な別の試験)。(2) 取りこぼしを減らす手は新しい sub-22 で行う。先に安い手 = **無視するディレクトリ (`bin` / `obj` / `target` / `cache` / `.git` など) を監視の対象から外し、ビルドの出力の洪水が追跡対象と同じバッファを取り合わないようにする** (最上位と、既知の入れ子)。それで足りなければ自前の `ReadDirectoryChangesW` (大きいバッファ + あふれの検出)。却下: (a) 今すぐ自前の監視へ置き換える (coder の試作 3 版が別の欠陥で未完。先に安い手を測る) (b) 何もしない (フックの無いクライアントの変更が、ビルドと重なると `gap` / Unknown に落ち続ける) |
| D35 | `gap` の Revision を一律に `preexisting` → `unknown` にすると、監視の取りこぼしを検証の走査が先に拾った場合、宣言と一致した AI の変更まで Unknown になる | sub-06 round 2 の報告。attr §4 の「止まっていた間は見ていない」は、サービスが止まっていた区間の話 | 未確認 | 起動時の `gap` は今までどおり `unknown`。動いている間の取りこぼし (`verify_gap`) は、**時刻による根拠を作らず、内容による根拠 (`before_match` + `declared_edit_match`、`report_hash_match`) だけを数える**。内容が宣言と一致していることは、いつ観測したかに依らない。却下: (a) `gap` は全部 `unknown` のまま (正しいが、根拠があるのに捨てる) (b) `verify_gap` を通常の Revision と同じに扱う (`observed_at` が走査の時刻なので、時刻の窓や `time_near` が誤って当たる) |

| D36 | フックの入力の `cwd` をそのままプロジェクトのルートにすると、サブディレクトリで作業しているときに、そこを別のプロジェクトとしてサービスを起動しベースラインを走らせてしまう | 公式文書 (D14): フックの入力に `cwd` はあるが、プロジェクトのルートを指す保証は無い。サービスは `--project` のパスから projectId を作る (D25) | 未確認 | フックは (1) `--project` があればそれ (2) 無ければ対象のファイルから親へたどって `.mye\ai_changes\service.json` がある最初のディレクトリ (3) どちらも無ければ何もしない。**サービスを起動するのは (1) のときだけ。** 設定の手順は (1) の形を勧める。却下: `cwd` をルートにして起動する (誤った場所に `.mye` とサービスができる) |

| D37 | Editor 側の追跡クライアントの試験が、機械が重いと落ちる | 2026-11-15 に planner が再現: CPU 使用率 100% の状態で、Debug の `MYE_SELFTEST_ONLY=changetracking` が 5 回中 5 回失敗 (`poll cap`、`model: describe is stored`、`hunks: …`)。`C:\HAL\MyEngin\src\Editor\ChangeTracking\TrackClient.cpp` 178・199 行目が `Poll()` の時間の上限 (2 ms) を `steady_clock` で直接測っている (他の時刻は差し替えられる `io_->NowMs()`)。Release は同じ状態で通る | 未確認 | sub-24 と sub-13 の間に **sub-25 (M104m3)** を足し、`Poll()` の上限を差し替えられる時計で測り、試験を偽の時計で決定的にする。製品の挙動 (実時間で 2 ms) は変えない。却下: (a) 試験の許容幅を広げる (機械がもっと重ければまた落ちる。CI で落ちうる) (b) sub-24 を差し戻してその中で直す (sub-24 の変更と関係の無い直しが同じコミットに混ざる) (c) 放置して「静かな機械で回す」運用にする (検証の結果が機械の状態に依存する) |

| D38 | Debug の `Editor.exe --selftest-full` が `physics` スイートの開始直後にスタックオーバーフローで落ちる (sub-25 の coder の報告) | 2026-11-15 に planner が `MYE_SELFTEST_ONLY=physics` の Debug で再現 (終了コード -1073741571)。M104 の `src\Engine` の差分は `LocalizationTable.inl` だけで、physics のコードもスタックの設定も変えていない。最後に CI が緑だった push (`a639ed1`) から基点までに、physics / 自己テストのあたりを触ったコミットが M97a〜M103 に 9 本ある。**基点のビルドでの再現は未確認** | 未確認 | M104 より前からの別件と見て、**M104 では直さない**。A20 の横断は「Release は全部、Debug は `physics` を除いた全部」で確かめ、除いたことを明記する (sub-14 に書いた)。却下: (a) M104 の中で原因を調べて直す (追跡と無関係な Engine 層の修正が混ざる) (b) 基点をビルドし直して確定させる (別の作業ツリーでエンジン全体をビルドする手間に対し、結論が変わっても M104 の進め方は同じ) |

| D39 | サービス側 (Rust) の、実時間に依存する試験が、機械が重いと落ちる | 2026-11-15 に planner が観測: `cargo test` (track) の全体を 5 回回して 3 回失敗。落ちた試験は毎回違い (`tests\pipe.rs:811`、`tests\watch.rs:988` の `single_saves_stay_complete_under_cpu_load`、`tests\watch.rs:780` の `waiting_for_a_writer_…`)、どれも sub-14 より前からある。`--test pipe` だけなら 6 回通る。機械はユーザーの別のプログラムで CPU 27〜100% | 未確認 | sub-14 と sub-15 の間に **sub-26 (M104o2)** を足す。原因を (a) 試験の待ち方 (b) 製品の経験則が負荷で外れた (c) 製品の不具合、に分け、製品の時刻の出どころを差し替えられるようにして、経験則の判定を偽の時計で確かめる決定的な試験を受け入れ条件にする。負荷で製品の経験則そのものが変わる性質の試験は、既定の実行から外して明示して回す。却下: (a) 再試行で緑にする (不具合と区別できなくなる) (b) 放置して復元へ進む (安全性 P0 の検証が、機械の状態に依存する) |

## 3. スコープ

- やる: 計画の進捗表 M104b〜M104t (Phase 0 の試作 〜 Phase 6)。順序は要求 §19 (記録 → 帰属 → 可視化 → 承認 → 復元 → 3D 比較)。最後に、エンジン配布物へのサービス同梱 (M104u、D5)。
- やらない (明示的に外したもの):
  - 要求 §1.4 の初期対象外 (AST 解析、AI による自動承認、MCP からの Restore / Approve、実行時 World の状態履歴、推論過程の保存、マシン間の履歴同期)。
  - `Runtime.exe` / `Server.exe` からの追跡。ABI・SimSnapshot・TypeId の変更。ECS コンポーネントの追加。
  - git の起動、Index の読み書き (復元を含む)。
  - 既存の Source Control の Diff 窓の変更。既存の MCP 5 ツールの挙動変更。
  - D19 の別件の修正。
- 後回し:
  - Codex CLI / Gemini CLI のフック対応 (D15)。
  - 性能の目標値 (arch §11。計測できるようにするまでがスコープ。値は sub-19 の計測で置く)。

## 4. 仕様

振る舞い・データ・UI・非機能の本文は設計文書が正本。ここには、設計文書に無い取り決めだけを書く。

### 4.1 振る舞い

- サービスの起動抑止 (D11): 非対話起動の Editor は追跡に一切触れない。
- フック CLI (D14): 終了コードは常に 0。サービス不在・時間切れ・入力が読めない、のどれでも 0 で返し、標準出力へ JSON を出さない (出すと Claude Code が解釈する)。
- exe の探し方 (D4、仮置き)。
- それ以外は arch §3〜§8、attr、restore。

### 4.2 データ・保存形式・互換性

- db と ipc のとおり。スキーマ版 1、プロトコル版 1。**実装時の正本は `tools\track\src\schema\` と `tools\track\src\protocol.rs`** で、設計文書の表はその要約 (ipc §4、db §3)。実装で形を変えたら同じサブで文書を直す。
- 既存データへの影響は無い (新規の `.mye\ai_changes\` だけ)。シーン・プレハブの保存形式は変えない。

### 4.3 UI / ビジュアル

- arch §7。文字列は全部 `LocalizationTable.inl` + `Tr()` (英日)。窓名は `"表示名###固定ID"`。
- 検証は D18。

### 4.4 非機能

- 決定論: arch §9。受け入れ条件 A20。
- 障害の局所化: arch §10。サービス不在・DB 破損・MCP 切断で Editor が通常動作すること。
- 性能: 重い処理はサービス側。Editor のフレームに入るのは `Poll()` と表の参照だけ (arch §11)。

## 5. 受け入れ条件

各条件の「→」の後ろが検証手段。要求 §16 との対応を括弧に書く。

- **A1 (試作)**: 試作 6 件 (計画の 5 件 + D10) のそれぞれについて、手順・コマンド・観測した出力・結論・未確認の残りが `docs\ai-change-tracking\prototype-results.md` にあり、設計文書の該当する「未検証」が結果で置き換わっている。計画の「試作 3 件」が直っている。リポジトリに試作コードが無い → `git status` と差分の目視、文書の相互参照。
- **A2 (保存の土台)**: 空のディレクトリから DB を作れる。移行を 2 回当てても同じ。壊した DB は退避されて作り直される。Blob は同じ内容で 1 つ、書いた後に検証される。ジャーナルは DB より先に書かれ、DB に無い行を起動時に入れ直す → `cargo test --manifest-path tools\track\Cargo.toml`。
- **A3 (検出)**: 作成・変更・削除・リネームが記録される。起動時にベースラインを取る。止まっていた間の変更は `gap` + Unknown の 1 件になる。デバウンス中の複数回の書き込みは `coalesced`。除外パターンとバイナリは内容なし / ファイル単位。**シナリオの前後で作業ツリー (`.mye` を除く) が 1 バイトも変わらない** (D17) → `cargo test` (§16.1 の 4、5 番目)。
- **A4 (行 diff)**: 1 ファイルの複数 Hunk、前後 3 行の文脈、改行だけの変更の区別、「削除 + 同じ内容の作成」のリネームまとめ。**全 Hunk を Before に当てると After とバイト一致する**性質をテストで確かめる (これが sub-15 の Hunk 復元の土台) → `cargo test` (§16.1 の 1、2 番目)。
- **A5 (pipe)**: `hello` の 4 つの検査 (版・トークン・プロジェクト・接続元)、二重起動は既存へ接続、`status` / `list` / `describe`、サービスを再起動しても履歴が取れる (要求 Phase 1 の完了条件) → `tools\track_verify.bat` の NDJSON 照合 + `cargo test`。
- **A6 (帰属)**: attr §3 の表の全行、§4 の `preexisting`、§5 の `overlap` と PID + 開始時刻、中断セッションの `interrupted` が、それぞれ 1 つ以上のテストで確かめられる → `cargo test` (§16.1 の 3 番目、§16.2 の 1、2、4、5 番目)。
- **A7 (MCP)**: 道具が 10 になり、既存 5 つのシナリオの期待値は `tools_list` 以外変わらない。サービス不在で 5 つの新しい道具は `isError`。規則 15-c / 15-d が通り、許可していない API を `track_client.rs` に仕込むと落ちる。Editor を起動せずに MCP の操作が記録される → `tools\mcp_verify.bat`、`tools\check_rules.ps1`、`tools\track_verify.bat` (§16.2 の 3 番目)。
- **A8 (フック)**: pre / post の入力 (D14 の形) から `declared_edit_match` が成り立ち Verified になる。人の編集を挟むと Verified にならない。サービス不在でも終了コード 0 で 5 秒以内に返る。設定の手順が文書にある → `cargo test` + `track_verify` のシナリオ。
- **A9 (Editor の土台)**: `changetracking` スイートがサービスなしで通る。規則 19 が通り、違反の一時コピーで落ちる。規則 9 の組に C++ の定数が入る。非対話起動でサービスを起動しない (D11)。サービス不在・`proto_mismatch`・`db_degraded` で Editor が通常動作する → `MYE_SELFTEST_ONLY=changetracking` の `Editor.exe --selftest`、`tools\check_rules.ps1`、`tools\check_rules_selftest.ps1` (§16.5 の 3、4、7 番目)。
- **A10 (一覧と Diff)**: AI Changes 窓に、セッション別の変更、帰属と根拠、`completeness` が出る。Diff はインラインと左右。Unknown / Inferred は既定で別の節 (attr §8)。文字列は英日そろう → スイート (モデルの整形) + `--screenshot` + 規則 10。
- **A11 (シーン構造 diff)**: `fileId` で対応付ける。キー順だけの違いは変更にしない。数値は字句で比べる。`version` 違いは「解析不可」。別のシーンの同じ `fileId` を混ぜない。プレハブの 3 種 (Asset / Instance / Override) を区別する → `cargo test` + `track_verify` の `scene_diff` (§16.3 の 1、2、4 番目)。
- **A12 (シーン上の表示)**: Hierarchy のバッジ (通常表示と検索表示の両方)、Inspector の Field 差分、Scene View のワイヤ箱、一覧からの選択同期。**表示の有無で `HashWorld` が同じ。表示のために `EnsureFileId` を呼ばない** → スイート + `--screenshot` (§16.5 の 5 番目)。
- **A13 (未保存シーンの保護)**: 口が未設定なら今までどおり (既存の hotreload 系スイートが無変更で通る)。dirty のとき外部変更は保留され確認が出る。「読み込む」「自分の編集を保つ」が設計どおり。dirty でなければ自動適用 → スイート + `--selftest-full` (§16.3 の 6 番目)。
- **A14 (レビュー)**: 7 つの単位で承認・却下・要確認・再開。記録は追記だけ。新しい Revision は Pending。親は子から計算し Partially Approved が出る。却下してもファイルは変わらない。`role = mcp` の `review_set` は `forbidden` → `cargo test` + スイート + `--screenshot` (§16.4 の 4、5 番目)。
- **A15 (ファイルと Hunk の復元)**: restore §3.1 / §3.2 の表の全行。ゲートは理由を全部列挙し、実行直前に判定し直す。ジャーナルの状態遷移。途中失敗の巻き戻し。強制終了後の起動で回復の画面。**復元の前後で `.git\index` のバイト列が同じ**。パスの検査 (restore §8)。復元による変更は `actor = restore` → スイート (一時プロジェクト) + `cargo test` (§16.4 の 1、3、6、7、8 番目、§16.5 の 1、2 番目)。
- **A16 (シーンの復元)**: Field / Component / Entity / Override。Ctrl+Z で戻る。プレハブの override 記録が保たれる。`documentId` が違えば当てない。Play 中は止まる → スイート (§16.4 の 2 番目)。
- **A17 (ゴーストと Overlay)**: 削除されたエンティティが Before の内容で読み取り専用に見える。World は変わらない → スイート (`HashWorld`) + `--screenshot` (§16.3 の 3 番目)。
- **A18 (Before / After と左右比較)**: 一時 Scene で描く。本番の Scene の `HashWorld` が変わらない。アセットを再現できないときはその旨が出る → スイート + `--screenshot`。
- **A19 (伝播と大きいシーン)**: プレハブの伝播が原因の Change と結び付いて直接の編集と区別される。大きいシーンの計測値が文書に残る → `cargo test` + スイート + 計測の記録 (§16.3 の 5 番目)。
- **A20 (横断。各 Phase の最後のサブで確かめる)**: `tools\check_rules.ps1` が 0 error。`Editor.exe --selftest-full` が通る。追跡を有効にした場合と無効にした場合で `tools\replay_verify.bat` が一致。ABI・SimSnapshot・TypeId が変わっていない (D20) (§16.5 の 6 番目)。

- **A21 (配布)**: `tools\make_dist.ps1` の配布物に `mye_track.exe` が入り、欠けていれば `make_dist` が失敗する。`release.yml` がサービスをビルドしてから詰める。配布物を展開した場所の Editor が、隣の `mye_track.exe` を起動して接続できる (リポジトリの `tools\track\target` に頼らない)。ゲームの配布物 (`--package`) には `mye_track.exe` も `.mye\ai_changes\` も入らない → `make_dist.ps1 -SkipBuild` の実行と配布物の中身の確認、展開先での手動の起動確認、`ci.yml` の `$forbidden` (要求 §14.2)。

## 6. サブ分割

harness の sub-NN と計画の M104x は 1 対 1。コミット件名は `M104x: ...` (AGENTS.md §8)。

| サブ | M104x | Phase | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|---|---|
| sub-01 | M104b | 0 | 試作 6 件と設計文書への反映 | なし | A1 | `M104b: AI Change Tracking の試作 6 件の結果を設計文書へ反映する` |
| sub-02 | M104c | 1 | `tools\track` の骨組み、DB、Blob、ジャーナル | sub-01 | A2 | `M104c: 追跡サービスの骨組みと DB・Blob・ジャーナルを作る` |
| sub-03 | M104d | 1 | ファイル監視、ベースライン、欠落区間 | sub-02 | A3 | `M104d: 追跡サービスにファイル監視とベースラインを足す` |
| sub-21 | M104d2 | 1 | 取りこぼしの検出と、ベースライン・記録の計測 | sub-03 | A3 |
| sub-04 | M104e | 1 | 行 diff、Hunk、Revision、リネーム | sub-21 | A4 | `M104e: 追跡サービスに行 diff と Revision の確定を足す` |
| sub-05 | M104f | 1 | pipe サーバ、`hello`、取得の op、`track_verify.bat` | sub-04 | A5、A20 | `M104f: 追跡サービスに名前付きパイプと履歴取得を足す` |
| sub-06 | M104g | 2 | セッション、操作、帰属 | sub-05 | A6 | `M104g: 追跡サービスにセッションと帰属の判定を足す` |
| sub-22 | M104g2 | 2 | 監視の取りこぼしを減らす (無視するディレクトリを監視から外す) | sub-06 | A3 |
| sub-07 | M104h | 2 | MCP の 5 ツール | sub-22 | A7 | `M104h: MCP に変更追跡の道具を 5 つ足す` |
| sub-08 | M104i | 2 | フック CLI | sub-07 | A8、A20 | `M104i: 追跡サービスにフック CLI を足す` |
| sub-09 | M104j | 3 | Editor の pipe クライアント、モデル、規則 19 | sub-08 | A9 | `M104j: エディタに追跡サービスのクライアントと規則 19 を足す` |
| sub-10 | M104k | 3 | AI Changes 窓、Diff 表示部品 | sub-09 | A10 | `M104k: AI Changes 窓と行 diff の表示を足す` |
| sub-23 | M104k2 | 3 | Editor の保存をサービスへ知らせる (`editor_saved`) | sub-10 | A10 |
| sub-11 | M104l | 3 | シーン構造 diff | sub-23 | A11 | `M104l: 追跡サービスにシーンの構造 diff を足す` |
| sub-12 | M104m | 3 | Hierarchy / Inspector / Scene View の表示 | sub-11 | A12 | `M104m: シーン上に AI の変更を表示する` |
| sub-24 | M104m2 | 3 | Inspector の Field 差分、Scene View のワイヤ箱、色の設定 | sub-12 | A12 |
| sub-25 | M104m3 | 3 | 追跡クライアントの試験から実時間への依存を外す | sub-24 | A9 |
| sub-13 | M104n | 3 | 未保存シーンへの外部変更の確認 | sub-25 | A13 | `M104n: 未保存のシーンへ外部の変更を自動で適用しない` |
| sub-14 | M104o | 4 | レビュー | sub-24、sub-13 | A14、A20 | `M104o: AI の変更の承認・却下を足す` |
| sub-26 | M104o2 | 4 | サービス側の時間に依存する試験を、機械の負荷に強くする | sub-14 | A2〜A6 の検証の土台 |
| sub-15 | M104p | 5 | 復元の判定 (ゲート、競合の検証、パスの検査、プレビュー。書かない) | sub-26 | A15 |
| sub-27 | M104p2 | 5 | ファイルと Hunk の適用、復元ジャーナル、巻き戻し、起動時の回復 | sub-15 | A15 | `M104p: ファイルと Hunk の復元と復元ジャーナルを足す` |
| sub-16 | M104q | 5 | ライブのシーンの復元 | sub-27 | A16、A20 | `M104q: シーンの復元を Undo に載せる` |
| sub-17 | M104r | 6 | 削除エンティティのゴースト、Overlay | sub-16 | A17 | `M104r: 削除されたエンティティのゴーストを表示する` |
| sub-18 | M104s | 6 | Before / After のプレビュー、左右の 3D 比較 | sub-17 | A18 | `M104s: Before / After を別シーンで描いて比べる` |
| sub-19 | M104t | 6 | プレハブ伝播の表示、大きいシーンの計測 | sub-18 | A19、A20 | `M104t: プレハブ伝播の表示と大きいシーンの計測を足す` |

| sub-20 | M104u | 配布 | エンジン配布物へのサービス同梱 | sub-19 | A21 | `M104u: エンジン配布物に追跡サービスを入れる` |

### 6.1 サブ仕様の書き込みの深さ (planner の判断)

- **sub-01〜05 (Phase 0〜1) は細部まで書いた。**
- **sub-06〜20 (Phase 2〜6 と配布) は、範囲・受け入れ条件・検証・着手前の確認事項までを書いた骨子。** 触る場所の細部は、sub-01 の結果と前の Phase の実装で変わるので今は確定させない。
- 見直しの時点: planner は **sub-01 の VERDICT で sub-02〜19 を**、**各 Phase の最後のサブ (sub-05、08、14、16) の VERDICT で次の Phase のサブを**読み直し、変わった点を §8 に積んでから OK を返す。司会は、骨子のサブに着手する前にその見直しが済んでいることを確かめる。

### 6.2 着手前にユーザーへ確認するサブ

司会が該当サブの `sub-NN.md` の「着手前にユーザー確認が要る未決事項」を読んで聞く (2026-11-14 のユーザー判断: 計画の未決事項 1〜5 は着手時に確認)。

**2026-11-15 から運用が変わった (ユーザーの指示: 推奨で進めて Notion に後回し)。** 下の表の未着手の確認 (sub-09 / 12 / 13 / 17) は、各 sub の「推奨」を答えとして進める。司会は聞かずに Notion の質問表へ書く。逆の答えが後から来たら、その sub の「逆を選ぶと何が変わるか」に書いた範囲を差し戻す。sub-17 の「Phase 6 へ進むか」の推奨は「進む (進捗表どおり sub-17 → 18 → 19)」とする。ユーザーの手作業が要る確認 (sub-07 の本物のクライアントの終了、sub-08 の本物のフック入力の記録) は「未確認」として残し、それ無しで受け入れ条件が成り立つ形にしてある。

| サブ | 確認すること | 出所 |
|---|---|---|
| sub-02 | Blob の除外パターン・上限・保存期間の既定値 | 計画の未決事項 4 |
| sub-05 | `tools\track` を CI に入れるか / exe の置き場所 (D5 の確定により、配布物へ入れられる置き場所であることが条件) | 計画の未決事項 2、D4 |
| sub-09 | 追跡を既定で有効にするか | 計画の未決事項 3 |
| sub-12 | ハイライトの色を変えられる範囲 | 計画の未決事項 5 |
| sub-13 | Play 中に外部変更が来たときの扱い | 計画の未決事項 1 |
| sub-17 | Phase 6 へ進むか | D12 |

## 7. 未決事項・リスク

### 7.1 ユーザーの回答 (2026-11-14、司会が AskUserQuestion で取得)

1. 確定確認: A1〜A20 と 19 サブで確定して sub-01 へ進める → planner の裁定どおり。
2. D2 試作コードはコミットせず文書だけ残す → 裁定どおり。
3. D12 Phase 6 は含め、sub-17 の着手前に確認する → 裁定どおり。
4. D11 Editor の非対話起動ではサービスへ接続も起動もしない → 裁定どおり。
5. D5 エンジン配布物にサービスを入れるか → **裁定と違う。M104 に含める。** sub-20 (M104u) と A21 を足した (§8)。

D3、D4、D10 は sub-01 / sub-05 の結果で決まる。着手時に聞く項目は §6.2。
### 7.2 リスク

| リスク | 当たったとき | 手当て |
|---|---|---|
| 試作 1 (DACL 付き pipe と C++ の overlapped I/O) が通らない | sub-05 と sub-09 の手段が変わる | sub-01 が最初。責務の分け方は変えない (ADR-039) |
| 試作 2 (`rusqlite` bundled) が開発機か CI でビルドできない | sub-02 の保存形式 | sub-01 で開発機、sub-05 で CI。CI で落ちたら sub-05 で止めてユーザーへ |
| 試作 3 (`notify` の返し方) が想定と違う | sub-03 のデバウンスと sub-04 のリネームまとめ | sub-01 の観測で arch §4、§5 を直してから sub-03 |
| 試作 4 (C# のフィールド) が書けない | sub-16 の範囲 | C# コンポーネントは Entity 単位の復元だけ (restore §4.2) |
| 試作 6 (親の Job) でサービスが道連れになる | 「Editor を閉じても記録が続く」が MCP 起動のときだけ弱まる | 縮退を文書に書く (D10) |
| 19 サブの後半は骨子 | 着手時に仕様の穴が出る | §6.1 の見直しの時点 |
| 後半の UI サブが大きい (sub-10、12、15) | 1 コミットで閉じない | 着手時の見直しで、進捗表の文字を足して割る (例: M104k を M104k / M104k2)。割るときは §8 に積む |

## 8. 変更履歴

確定後の変更のみ。

- 2026-11-15 (planner、sub-15 の VERDICT round 1): sub-27 を着手可にした (書くことの全部は `RestorePlan.ops`、書き込みの口を 1 か所に分ける、取った Blob のハッシュを Editor 側でも確かめる、8 MiB を超えるファイルの全体の復元、進行中のゲート、落ちた場合の試験、切断の扱い)。サービスの `list` に `operationId` の絞り込みが足された (読み取りだけ)。
- 2026-11-15 (planner、sub-26 の VERDICT round 1 と Phase 5 の見直し): 元の M104p を 2 本に割った。**sub-15 (M104p) は「復元の判定とプレビュー」で、作業ツリーを書かない。sub-27 (M104p2) が適用・ジャーナル・巻き戻し・回復。** sub-15 を詳細に書き直し (受け入れ条件 14 件。Rust と C++ の Hunk の逆適用を 1 つの固定表で照合する)、sub-27 を骨子で新設、sub-16 の依存を sub-27 にした。
- 2026-11-15 (planner、sub-14 の VERDICT round 1): D39 を追加。sub-14 と sub-15 の間に **sub-26 (M104o2)** を足した (Rust 側の時間に依存する試験が、機械が重いと落ちる。planner が 5 回中 3 回観測)。sub-15 に Phase 4 までの結果と、sub-14 から持ち越した窓の追加 2 件を書き、依存を sub-26 にした。
- 2026-11-15 (planner、sub-13 の VERDICT round 1): sub-14 を詳細に書き直した (`review_set`、今の状態の決め方 = いちばん新しい行、集約の表、承認済みを強調から外す、Editor の操作、持ち越しの直し 3 件、Phase 3〜4 の横断、受け入れ条件 15 件)。Engine 層の差分は `ReloadHub` の口 1 つ + 保留の操作になった (ADR-039 の文面は sub-14 で直す)。
- 2026-11-15 (planner、sub-25 の VERDICT round 1): D38 を追加 (Debug の `--selftest-full` が `physics` で落ちる件は M104 の外の別件として扱い、A20 の Debug の横断は `physics` を除いて回す)。sub-14 に書いた。sub-13 は変更なし。
- 2026-11-15 (planner、sub-24 の VERDICT round 1): D37 を追加。sub-24 と sub-13 の間に **sub-25 (M104m3)** を足した (Debug の `changetracking` スイートが負荷の下で落ちるのを、planner が 5 回中 5 回再現した)。sub-13 を着手可にし (確認済みの事実と追加の取り決めを書いた)、依存を sub-25 にした。
- 2026-11-15 (planner、sub-12 の VERDICT round 1): sub-24 を着手可にした (sub-12 の結果、強調の入・切の保存を色と一緒に入れること、Scene View の試験の分け方を追記)。
- 2026-11-15 (planner、sub-11 の VERDICT round 1): sub-12 を詳細に書き直した (サービス側に `scene_overlay` と `document_for_path`、`SceneChangeOverlay`、AI Changes 窓の「シーンの変更」の一覧、Hierarchy のバッジ、選択の同期、受け入れ条件 13 件)。大きいので Inspector の Field 差分・Scene View のワイヤ箱・色の設定を **sub-24 (M104m2)** に分け、sub-14 の依存を sub-24 と sub-13 にした。DB のスキーマ版は 3。
- 2026-11-15 (planner、sub-23 の VERDICT round 1): sub-11 を詳細に書き直した (文書の同一性、字句のままの構造比較、解析不可の扱い、`scene_diff`、作り直し、受け入れ条件 14 件)。sub-23 の読み直しの穴を狭める直しを sub-12 に、`ipc-contract.md` の制御文字の直しを sub-11 に足した。DB のスキーマ版は 2 (sub-23 が索引を足した)。
- 2026-11-15 (planner、sub-10 の VERDICT round 1): Diff 表示の部品のクラス名は `DiffViewer` / `DiffRows` (ファイルは `src\Editor\Widgets\DiffView.h`)。既存の `mye::DiffView` (`SourceControlState.h`) と同名を避けた。以後の sub で「DiffView」と書いてあるのはこの部品のこと。oversize の Hunk の表示の残りを sub-15 に、「セッションなし」の取り方の直しを sub-23 に足した。sub-23 を着手可にした。
- 2026-11-15 (planner、sub-09 の VERDICT round 1): sub-10 を詳細に書き直した (`hunks` / `blob_get`、DiffView、AI Changes 窓、受け入れ条件 12 件)。大きいので `editor_saved` を **sub-23 (M104k2)** に分け、sub-11 の依存を sub-23 にした (順序は sub-10 → sub-23 → sub-11)。
- 2026-11-15 (planner、sub-08 の VERDICT round 1 と Phase 3 の見直し): sub-09 を詳細に書き直した (既定は無効、`allowChangeTracking = !automation`、サービス用の起動口、`subscribe` と通知をこのサブでサービス側に足す、`MYE_TRACK_PROBE` の結合試験、規則 19 と規則 9 の 3 site、受け入れ条件 10 件)。`editor_saved` を sub-10 へ移した。sub-10〜13 に Phase 2 までの結果と、着手前の確認の裁定 (推奨どおり) を書き足した。
- 2026-11-15 (planner、sub-07 の VERDICT round 1 と見直し): D36 を追加。sub-08 を詳細に書き直した (プロジェクトの決め方、終了コード 0 の徹底、起動のときのハンドルの継承、設定の手順の文書、持ち越しの直し 4 件、受け入れ条件 13 件)。
- 2026-11-15 (planner、sub-22 の VERDICT round 1): 第 2 の手 (自前の `ReadDirectoryChangesW`) は入れずに sub-22 を閉じた (実測で、入れる根拠になる取りこぼしを再現できなかった)。残りの計測を sub-19 に、進捗表の化けの直しを sub-07 に足した。
- 2026-11-15 (planner、sub-06 の VERDICT round 3 と見直し): sub-07 を詳細に書き直した (プロジェクトの決め方、依存を足さずに SHA-256 を自前で持つ、`MYE_TRACK_EXE` が設定されていればそれだけを使う、サービスを伴うシナリオは `track_verify` 側、受け入れ条件 14 件)。sub-22 は変更なしで着手可。
- 2026-11-15 (planner、sub-06 の VERDICT round 2): D34、D35 を追加。sub-06 の受け入れ条件 19 を書き直し (試験の前提が誤りだった)、21 (`verify_gap` の帰属) を足した。sub-06 と sub-07 の間に **sub-22 (M104g2)** を足し、sub-07 の依存を sub-22 にした。
- 2026-11-15 (planner、sub-05 の VERDICT round 1 と Phase 2 の見直し): D30〜D33 を追加。sub-06 を詳細に書き直した (セッションの基準、管理された操作の即時 flush、`clientOpId`、`worktree` の条件、派生の表の作り直し、受け入れ条件 18 件)。sub-07、sub-08 に Phase 1 の結果と sub-06 の決定を足した。§6.2 に質問の運用の変更 (推奨で進める) を書いた。A1〜A21 は変えていない。
- 2026-11-15 (planner、sub-04 の VERDICT round 1): sub-05 に、`describe` が `diff_mode` を返すこと、sub-05 のコミット以降は `v1.sql` を変えず移行で足すこと (スキーマの凍結)、db §6 の制限の追記を足した。受け入れ条件は変えていない。
- 2026-11-15 (planner、sub-21 の VERDICT round 1): sub-21 の結果 (`verify_gap`、`status` の項目、ベースラインの実測 = このリポジトリで約 488 MB・12〜35 秒) を sub-04、sub-05、sub-09 に書き足した。sub-04 に試験の一時ディレクトリの後始末を足した。受け入れ条件とサブの数は変えていない。
- 2026-11-14 (planner、sub-03 の VERDICT round 1): D27〜D29 を追加。sub-03 と sub-04 の間に **sub-21 (M104d2)** を足した (番号は末尾だが順序は sub-03 → sub-21 → sub-04)。sub-03 に受け入れ条件 15、sub-04 の依存を sub-21 に、sub-05 に標準入力の EOF で終わらない旨を足した。A3 の「取りこぼさない」は sub-03 と sub-21 の 2 本で満たす。
- 2026-11-14 (planner、sub-02 の VERDICT round 1): D25、D26 を追加。sub-02 (projectId の定義と受け入れ条件 8)、sub-03 (イベントのペイロード、`service_start` / `service_stop`、一時 Blob の掃除)、sub-04 (派生の表の作り直し、受け入れ条件 9)、sub-05 (`service.lock`、受け入れ条件 3b)、sub-09 (projectId の定義) を直した。
- 2026-11-14 (planner、sub-01 の VERDICT round 1 と §6.1 の見直し): 試作の結果を受けて D21〜D24 を足した。変えたサブは sub-02 (依存の版、MSRV、進捗表の直し)、sub-03 (デバウンスの確定値、共有モードの直し、ディレクトリ単位の操作、受け入れ条件 10〜14)、sub-04 (移動のまとめ、受け入れ条件 5b)、sub-05 (pipe の作法、試作 6(b) を sub-07 へ)、sub-07 (標準ライブラリだけでの再試行と起動、受け入れ条件 8・9)、sub-08 (本物の入力の記録の口)、sub-09 (サービス用の起動口)、sub-11 (C# の値の形)、sub-16 (C# の Field の分岐)。A1〜A21 とサブの数・依存は変えていない。sub-06、10、12〜15、17〜20 は読み直して変更なし。
- 2026-11-14 (ユーザー、確定確認と同時): D5 を「後回し」から「M104 に含める」へ。スコープの「後回し」から配布物の項を外し、A21、sub-20 (M104u)、§6.2 の sub-05 の条件を足した。sub-01〜19 の範囲と A1〜A20 は変えていない (sub-05 と sub-19 の文言だけ整合させた)。
