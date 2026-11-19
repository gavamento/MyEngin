# sub-06: AI 変更追跡との紐付け

- 依存: sub-02 (track 側 Rust 変更は sub-03〜05 と並列可。AI Changes 窓の表示は sub-04 OK 後に入れる)
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 2・4.2。
- track スキーマ v4: `/home/user/MyEngin/tools/track/src/schema/v4.sql` で `sessions.host_terminal TEXT` を追加、`/home/user/MyEngin/tools/track/src/schema/mod.rs:15` の Migration 配列に登録。
- IPC `session_begin` に任意 `hostTerminal` を受け付けて保存 (欠落 = NULL)。
- フック `/home/user/MyEngin/tools/track/src/hook.rs:422-430` の begin_session で環境変数 `MYE_TERMINAL_ID` があれば `hostTerminal` に載せる。`source` は "terminal" のまま。
- エディタ側: AI Changes 窓 (`/home/user/MyEngin/src/Editor/Windows/Project/AiChangesWindow.h` 一式) でセッション行に「統合ターミナル」表示 (host_terminal が非NULL)。DB 読み取り経路を確認して列を追加。ローカライズ両言語。
- 文書: `/home/user/MyEngin/docs/ai-change-tracking/` の ipc-contract.md (:111 付近)、db-schema.md (:47 付近)、hooks-setup.md (:128 付近)、spec-v1.1.md (:131, :229) と engine_spec §9.2 を更新。

## やらないこと (このサブでは)
- source 値の変更、過去セッションの遡及付与。

## 受け入れ条件 (このサブ)
1. spec 5-9。
2. Linux: `cargo test` (tools/track) 成功。新テスト: v3 DB → v4 移行で既存行の host_terminal が NULL、hostTerminal 付き/無し session_begin の保存、フックが環境変数を載せる。
3. 文書の列・フィールド名がコードと一致 (grep)。
4. Windows (要確認): 統合ターミナルで claude を起動しファイル編集 → AI Changes 窓で区別表示 (スクショ)。

## 検証コマンド
- Linux: `cd /home/user/MyEngin/tools/track && cargo test` (依存クレート取得可否を最初に確認、不可なら理由を報告)。
- Windows: エディタビルド、手動。

## 実装メモ

SELF_EVAL: sub-06 (round 1)
実装:
  - /home/user/MyEngin/tools/track/src/schema/v4.sql (新規), /home/user/MyEngin/tools/track/src/schema/mod.rs — 版 4 `ALTER TABLE sessions ADD COLUMN host_terminal TEXT` を MIGRATIONS 末尾へ登録 (v1→v2→v3 と同じ前進のみの方式。番号 4 は既存の最新が 3 であることを mod.rs で確認)
  - /home/user/MyEngin/tools/track/src/ops_record.rs:host_terminal_arg / session_begin — 任意引数 `hostTerminal` を受け、欠落・null・空は None、128 バイト超 (`MAX_HOST_TERMINAL_BYTES`) と非文字列は bad_args。イベント payload に `host_terminal` を載せる
  - /home/user/MyEngin/tools/track/src/session.rs:session_begin — INSERT に host_terminal を追加。payload に無い (版 3 までのイベント) は NULL なので、ジャーナルからの作り直しでも同じ列になる
  - /home/user/MyEngin/tools/track/src/ops.rs:sessions — SELECT に host_terminal を足し、応答の各項目に `hostTerminal` (無ければ null)
  - /home/user/MyEngin/tools/track/src/hook.rs:host_terminal_from_env / session_begin_args / begin_session — 環境変数 `MYE_TERMINAL_ID` を読み、有効なら session_begin の `hostTerminal` に載せる (source は "terminal" のまま)。無効値 (空・128 バイト超・制御文字・非 UTF-8) は送らず記録を続ける
  - /home/user/MyEngin/src/Editor/ChangeTracking/ChangeModel.h/.cpp — TrackSessionRow.hostTerminal、ParseSessions が `hostTerminal` を読む (null・欠落は空)
  - /home/user/MyEngin/src/Editor/ChangeTracking/ChangeFormat.h/.cpp:FormatSessionRow — 第 2 引数 fromThisEditor (既定 false) と SessionRowText.origin。hostTerminal が非空なら「統合ターミナル」、このエディタのものなら「統合ターミナル (このエディタ)」
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.h/.cpp:TerminalIdBelongsToEditor — ID が `<PID>-<起動UNIX秒>-<数字のみの連番>` で、PID・起動時刻が一致するか (純関数)
  - /home/user/MyEngin/src/Editor/Windows/Project/AiChangesWindow.cpp:DrawSessions — エディタの PID・起動時刻を static で 1 回だけ取り、各行の fromThisEditor を決め、行の 1 行目 (`claude  [running]`) の後ろに origin を足す
  - /home/user/MyEngin/src/Engine/Core/Localization/LocalizationTable.inl — AiChg_SessionHostTerminal / AiChg_SessionHostTerminalHere (両言語。### なし)
  - 試験: tools/track/src/hook.rs (単体 2 件)、tests/session.rs (2 件)、tests/storage.rs (1 件)、tests/hook.rs (Windows 結合 1 件)、ChangeViewSelfTest.cpp / ReviewSelfTest.cpp / TerminalVsEnvSelfTest.cpp (各 1〜3 チェック)
  - 文書: docs/ai-change-tracking/ipc-contract.md (§4.2 の session_begin 引数と注記、§9.4 sessions の項目)、db-schema.md (sessions 表・session_begin の payload・§8 版 4)、hooks-setup.md (§3 の pre)、spec-v1.1.md (:131 と :229)、/home/user/MyEngin/engine_spec.md (§9.2 に 1 項、§9.4 に 1 文)、docs/test_checklists.md (M106f の Windows 確認項目)、docs/adr/ADR-039-ai-change-tracking.md (スキーマ版の 1 文)
技術解説: (ユーザー向け)
  - 技術: 前進のみのスキーマ移行 (SQLite `ALTER TABLE ADD COLUMN`) — 既存の `MIGRATIONS` 配列へ `Migration { version: 4, sql: include_str!("v4.sql") }` を足すだけ。起動時 (db.rs) が旧版の DB を `tracking.db.bak-v3` へコピーしてから 1 トランザクションで適用し、失敗なら元のまま db_degraded になる (db-schema.md §8)。ADD COLUMN は既存行を NULL にするので「遡って付与しない」がそのまま満たされ、表の作り直しも要らない。却下: `source` に新値 "editor_terminal" を足す案 (既存の `terminal` 判定が壊れる。ユーザー承認済みで host_terminal 列に決定済み)。
  - 技術: イベントソーシングとの整合 — tracking.db の sessions は `events` (ジャーナル) から作り直せる派生表なので、列を足すだけでなく `session_begin` イベントの payload に `host_terminal` を載せ、`session::session_begin` (apply_event) が payload から INSERT する。DB を壊して作り直しても同じ値になる (tests/session.rs `a_host_terminal_id_survives_a_rebuild_from_the_journal`)。版 3 までのイベントには項目が無く `p["host_terminal"].as_str()` が None → NULL。
  - 技術: 任意引数の後方互換 — IPC は `Args` が未知の引数を `finish()` で bad_args にする作りなので、サーバ側で `hostTerminal` を `take` するだけで「欠落 = 旧フック」を受理できる。値の検証は長さ 128 バイトまで (MYE_TERMINAL_ID は数十バイト) と型だけ。ID の中身は解釈しない。
  - 技術: AI Changes 窓の「このエディタ」判定 — ID の書式 `<エディタPID>-<起動UNIX秒>-<連番>` の先頭 2 要素だけを、窓から見えるエディタ自身の PID・起動時刻 (`GetTerminalEditorIdentity`、sub-05 が MYE_TERMINAL_ID の材料に使うのと同じ関数) と照合する。PID は再利用されるが、起動時刻が一致するかを併せて見るので別起動のエディタを誤認しない。却下: 現在の TerminalWindow の ID と完全一致で比べる案 — 窓の再起動で連番が変わるたびに同じエディタのセッションが「他」になる上、AiChangesWindow が TerminalWindow を知る結合が要る。
  - コード: hook.rs `host_terminal_from_env(Option<OsString>) -> Option<String>` — 入力は環境変数の値、処理は UTF-8 化→空・128 超・制御文字なら None、出力は載せる文字列。`session_begin_args(session_id, root, host_terminal) -> Value` が JSON を組む。呼び元は `Link::begin_session` (pre / session-start のたびに呼ばれるので、既存セッションの再開でも毎回同じ値を送る。サーバは再開時に保存済みの値を変えない)。
      ```rust
      let mut args = json!({ "clientSessionId": session_id, "provider": "claude", "source": "terminal", "workspace": root.to_string_lossy() });
      if let Some(id) = host_terminal { args["hostTerminal"] = json!(id); }
      ```
  - コード: ops_record.rs `host_terminal_arg(&mut Args) -> Result<Option<String>, OpError>` → `session_begin` が payload の `"host_terminal"` に入れ、writer スレッド上の `record_and_apply` がジャーナルへ追記してから `session.rs` の INSERT (`?11` = `p["host_terminal"].as_str()`) で DB へ入れる。`sessions` op (ops.rs) は読み取り接続で `SELECT ... host_terminal` し `hostTerminal` として返す (列は `Option<String>` なので NULL は JSON null)。
  - コード: ChangeFormat.cpp `FormatSessionRow(row, fromThisEditor)` — 入力は 1 行のモデルと真偽値、出力は表示文字列の束。`row.hostTerminal` が空なら origin は空 (従来どおりの行)。呼び元は AiChangesWindow::DrawSessions (描画のたび。`TerminalIdBelongsToEditor` は短い文字列比較だけなので毎フレームでも軽い)。Tr の結果は `std::string` へ代入するだけで書式文字列には使わない。
      ```cpp
      if (!row.hostTerminal.empty()) {
          out.origin = Tr(fromThisEditor ? StrId::AiChg_SessionHostTerminalHere : StrId::AiChg_SessionHostTerminal);
      }
      ```
  - 前提・落とし穴: (1) 古い (版 3 の) サービスが動いたまま新しいフックが `hostTerminal` を送ると、ipc-contract.md §7 の方針どおり未知の引数として bad_args になり、その session_begin は失敗する (フックは静かに終わり、その編集は記録されない)。新旧の食い違いは「サービスの再起動」で解消するので ipc-contract.md の注記に書いた。(2) 統合ターミナルの外から同じ clientSessionId で後から再開しても host_terminal は上書きしない (最初の値が残る)。(3) `source` は常に "terminal"。統合ターミナルかどうかは host_terminal の有無だけで判る。(4) ID は信頼境界の外の環境変数由来 (ユーザーが手で設定できる) なので表示専用で、帰属 (attribution) の根拠には使っていない。(5) tests/hook.rs の結合テストは Windows の名前付きパイプが要るので、この環境では型検査までしか確認できていない。
仕様との差分:
  - [逸脱] 「現在の窓のターミナル ID と一致するなら『このターミナル』表示」→ 窓の ID との完全一致ではなく、エディタの PID・起動時刻の接頭辞一致で「統合ターミナル (このエディタ)」と表示 — シェル再起動で連番が変わっても同じエディタ内として扱える・AiChangesWindow が TerminalWindow に依存しないため。文言が「このターミナル」でなく「このエディタ」なのもその意味に合わせた
  - [追加] `hostTerminal` の検証 (128 バイト上限、空は NULL、非文字列は bad_args) と、フック側で無効値を送らない処理 — 仕様に長さの規定が無く、拒否でセッション全体が失敗するのを避けるため。上限値は後から変更可能 (定数 MAX_HOST_TERMINAL_BYTES、hook.rs が同じ定数を参照)
  - [追加] /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.h/.cpp に TerminalIdBelongsToEditor (sub-05 の成果物への追記) — ID の書式知識を 1 か所に置くため
  - [追加] docs/test_checklists.md の M106f 項目と docs/adr/ADR-039 の 1 文、engine_spec §9.4 の 1 文 — 台帳の指示は engine_spec §9.2 までだが、手動確認項目と版の記述を揃えるため
  - [未実装] Windows 上の実操作確認 (統合ターミナルで claude を起動し AI Changes 窓のスクリーンショット) — Linux コンテナで実行不能。test_checklists に項目だけ追加
検証:
  - `cd /home/user/MyEngin/tools/track && cargo test` (実クレート) → 依存クレートの取得・コンパイルは成功したが、クレート自体が Windows 専用 API (`std::os::windows`、`share_mode`、`creation_flags`、`from_raw_handle` 等) を使っており `could not compile mye_track (lib) due to 15 previous errors` で Linux ではビルド不能 (変更前から同じ。Cargo.lock は変化なし)
  - 代替: Windows API を stub した検証用コピー (/tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub06/shim、製品コードには含めない。windows 型の trait stub + Win32 シンボルの空実装 + 乱数だけ /dev/urandom) で `cargo test --lib` / `--test storage` / `--test session` を実行 → 新規テスト 5 件 (hook.rs 単体 2、session.rs 2、storage.rs 1) は全て PASS。変更前 (git HEAD のコピーに同じ stub) の失敗集合と比較: lib 2 件 (content の Windows ロック)、storage 1 件 (`cmd` 起動)、session 5 件 (名前付きパイプ・PID・share_mode) が前後で同一。review / scene_diff / diff / project_id も前後で同一の失敗集合 (Windows パス・固定表依存)。reconcile / revision / attribution / propagation は全て PASS
  - 既存の作り直しテスト (`a_broken_db_is_rebuilt_*` 系 session.rs 3 件) は `SELECT * FROM sessions` を比較しており、列追加後も PASS
  - `tests/hook.rs` の新規結合テスト (`the_terminal_id_variable_becomes_the_host_terminal_of_the_session_and_its_absence_leaves_it_null`) → 型検査のみ (shim で `cargo test --test hook --no-run` が通る)。実行は未実行 (名前付きパイプと実 exe の起動が要る)
  - `python3 .../sub04/check_loc.py src/Engine/Core/Localization/LocalizationTable.inl` → entries 2784 errors 0
  - C++: `g++ -std=c++20 -fsyntax-only -Wall -Wextra` で ChangeModel.cpp / ChangeViewSelfTest.cpp / TerminalVsEnv.cpp は警告・エラーなし、ChangeFormat.cpp は MinGW で通過。ReviewSelfTest.cpp / AiChangesWindow.cpp は MinGW の DirectXMath が既存ヘッダ (Scene/GameObject.h、ScenePreviewSession.h) と合わず、それ以外の自分の行に起因するエラーは無し (MSVC ビルドは未実行)
  - ヘッドレス駆動 (shim の g++ ビルド): ChangeFormat.cpp + ChangeModel.cpp + Localization.cpp で FormatSessionRow の origin (en / ja・outside / other / this editor) と ParseSessions の hostTerminal (文字列 / null / 欠落) → 全て PASS。TerminalVsEnvSelfTest (sub05 の駆動を再ビルド) の `id:` 3 件 (自分のもの・別 PID/起動時刻・空/連番なし/非数字/前後に数字が付く) PASS
  - 文書: `grep` で列名・フィールド名 (`host_terminal` / `hostTerminal` / `MYE_TERMINAL_ID`) が db-schema.md・ipc-contract.md・hooks-setup.md・spec-v1.1.md・engine_spec.md とコードで一致することを確認
  - `Editor.exe --selftest` / `--selftest-full`、`tools\check_rules.ps1`、`tools\replay_verify.bat`、MSVC ビルド、実画面確認 → 未実行 (Windows 専用)
自己採点 (1-5):
  仕様適合: 4 — 列・IPC・フック・窓表示・文書が揃っている。「このターミナル」を接頭辞一致の「このエディタ」にした逸脱と、Windows 実操作 (受け入れ条件 9 の後半・4) が未確認
  正しさ: 4 — Linux stub 環境で新規テストと既存の移行・作り直しテストが通り、変更前との失敗集合が同一。ただし Windows ネイティブの cargo test・フック結合テスト・MSVC ビルドは未実行
  コード品質: 4 — 既存の移行方式・Args・イベントソーシングに沿った最小の追加。FormatSessionRow の第 2 引数で書式知識を Tools/Terminal に 1 か所に保った
  テスト: 4 — 移行 (v3→v4)・保存/欠落/空/不正値・再開・作り直し・hook の値選別と引数・窓の表示と解析・ID 判定を網羅。フックの実プロセス結合と窓の描画は Windows でしか動かせない
不安・質問:
  - 古い版のサービスが残ったまま新しいフックが hostTerminal を送ると bad_args で session_begin が失敗する (ipc-contract.md §7 の方針どおり)。フックが hostTerminal 付きで bad_args を受けたら、付けずに 1 回だけ再送して記録を続ける方が「障害の局所化」に合うが、方針 (拒否は見える失敗として許容) との兼ね合いなので入れていない。入れるか判断してほしい
  - 「このターミナル」を「このエディタの統合ターミナル」に変えた判断 (上記 [逸脱]) の可否
触ったファイル:
  /home/user/MyEngin/tools/track/src/schema/v4.sql (新規), /home/user/MyEngin/tools/track/src/schema/mod.rs, /home/user/MyEngin/tools/track/src/ops_record.rs, /home/user/MyEngin/tools/track/src/session.rs, /home/user/MyEngin/tools/track/src/ops.rs, /home/user/MyEngin/tools/track/src/hook.rs, /home/user/MyEngin/tools/track/tests/session.rs, /home/user/MyEngin/tools/track/tests/storage.rs, /home/user/MyEngin/tools/track/tests/hook.rs, /home/user/MyEngin/src/Editor/ChangeTracking/ChangeModel.h, /home/user/MyEngin/src/Editor/ChangeTracking/ChangeModel.cpp, /home/user/MyEngin/src/Editor/ChangeTracking/ChangeFormat.h, /home/user/MyEngin/src/Editor/ChangeTracking/ChangeFormat.cpp, /home/user/MyEngin/src/Editor/ChangeTracking/ChangeViewSelfTest.cpp, /home/user/MyEngin/src/Editor/ChangeTracking/ReviewSelfTest.cpp, /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.h, /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.cpp, /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnvSelfTest.cpp, /home/user/MyEngin/src/Editor/Windows/Project/AiChangesWindow.cpp, /home/user/MyEngin/src/Engine/Core/Localization/LocalizationTable.inl, /home/user/MyEngin/docs/ai-change-tracking/ipc-contract.md, /home/user/MyEngin/docs/ai-change-tracking/db-schema.md, /home/user/MyEngin/docs/ai-change-tracking/hooks-setup.md, /home/user/MyEngin/docs/ai-change-tracking/spec-v1.1.md, /home/user/MyEngin/engine_spec.md, /home/user/MyEngin/docs/test_checklists.md, /home/user/MyEngin/docs/adr/ADR-039-ai-change-tracking.md, /home/user/MyEngin/plans/m106-integrated-terminal/sub-06.md (実装メモ)
申し送り:
  - 新規ファイルは v4.sql のみで、gen_project_files.ps1 の生成区画 (C++ ソース) には関係しない (Rust / SQL は vcxproj 管理外)。他に新しい C++ ファイルは無い
  - Windows で必ず確認: `cd tools\track && cargo test` 全件、`tools\check_rules.ps1`、`Editor.exe --selftest` (changetracking・terminalvsenv)、docs/test_checklists.md の「AI 変更追跡との紐付け」4 項目
  - 検証用 stub コピー (scratchpad/sub06/shim, base) は製品に含まれない。Cargo.lock は変化なし
  - 版 4 を入れた後は mye_track のサービスを再起動すること (古いサービスは hostTerminal を拒否する。ipc-contract.md §4.2 に書いた)
- round 1: VERDICT OK (planner)。実装メモは scratchpad 原文と一致 (diff 空)。git status は触ったファイル一覧と一致。v4.sql (ADD COLUMN)・MIGRATIONS 登録・ops_record の host_terminal_arg (finish 前に take)・session.rs の payload からの INSERT を差分で確認。逸脱 (このエディタ = PID・起動時刻の接頭辞一致) は仕様側の改善として追認し spec を修正。追加 3 件 (128B 検証とフックの無効値除外・TerminalIdBelongsToEditor・checklist/ADR-039/§9.4) は追認。未実装 (Windows 実操作・スクショ) は環境制約として容認。旧サービスへの再送案は入れない (spec 7 に記録)。nit: 正しさ 4 は stub 実走 + 変更前後の失敗集合比較で根拠はあるが、実クレートの Windows cargo test・tests/hook.rs 実行・MSVC が未実行なので 3 が妥当。Windows で最初に `cargo test` 全件を回すこと。
