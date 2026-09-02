# sub-01: M66a: Rust cdylib + CLI + fixture + collab_verify + CI + 規則 12 + DLL 往復の縦切り

- 依存: なし
- 状態: OK (commit: M66a — ハッシュは harness.md のサブ進捗表)
- 往復: 1
- 前提 (ユーザー側): rustup (stable) がインストール済みで `cargo --version` が通ること。**coder は環境を変えない**。
  通らなければ着手せず「不安・質問」で司会へ返す。

## やること

spec §4.0 / §4.4 / §5 の 1, 2, 3, 4(a)(b), 16。**このサブで最大の未知 = 「Rust cdylib を Editor.exe が
LoadLibrary して JSON が往復する」を潰す**。Source Control 窓はまだ作らない。

1. **crate `tools\collab\`** (`Cargo.toml`: package `mye_collab`、`[lib] crate-type = ["cdylib", "rlib"]`、
   `[[bin]] name = "mye_collab_cli"`、deps は serde / serde_json のみ。`profile.release` に `panic = "unwind"` を明示)。
   - `src\protocol.rs`: `pub const PROTO_VERSION: u32 = 1;`、`Request{id,op,args}` / `Response{id,ok,result|error}` /
     `Event{event,...}` / `ErrorBody{code,detail,paths?}` の serde 型。
   - `src\git.rs`: git の所在 (`PATH` の `git` → 無ければ `git_missing`)、共通引数 `-c core.quotepath=false -c color.ui=false --no-pager`、
     env `LC_ALL=C` `GIT_TERMINAL_PROMPT=0`、`CREATE_NO_WINDOW`、stdin null、stdout/stderr をバイト列で回収、`git --version` の解析 (`git_too_old` < 2.11)。
   - `src\porcelain.rs`: `status --porcelain=v2 -z --branch` の解析 (`#` ヘッダの branch.head / upstream / ab、`1` / `2` (rename、-z では次トークンが旧パス) / `u` / `?` / `!`)。
   - `src\ops.rs`: `hello{fetchIntervalMin,autoFetch}` → `{gitVersion}` / `repo_check` → `{toplevel, isRepo, head, mergeInProgress, rebaseInProgress}` / `status` → `{branch, upstream, ahead, behind, entries[{path, oldPath?, index, worktree, conflict}]}`。
   - `src\worker.rs`: 要求 FIFO を 1 本の worker スレッドで直列実行、応答・通知を出力キューへ。`catch_unwind` で panic → `service_error{code:"internal_panic"}` + dead 化。
   - `src\lib.rs`: C ABI 6 関数 (spec §4.0 の署名どおり。`#[no_mangle] extern "C"`、文字列は `CString` を `into_raw` / `mye_collab_free` で戻す)。
   - `src\main.rs` (bin): stdin の NDJSON を 1 行ずつ `request`、`poll` の出力を stdout へ、stdin EOF 後は未応答の id が無くなるまで poll して終了。`--root <dir>` 引数。
   - `tests\`: porcelain fixture 6 種 (M / A / D / R / ? / u) + ヘッダ ab、要求→応答の往復、`hello` の proto。
2. **`tools\build_collab.bat <Config>`** (CRLF): `where cargo` 不在 → メッセージ + `exit /b 1`。
   `cargo build --release --manifest-path tools\collab\Cargo.toml` → `target\release\mye_collab.dll` を
   `bin\x64\Debug\MyeCollab.dll` と `bin\x64\Release\MyeCollab.dll` へ、`mye_collab_cli.exe` を `MyeCollabCli.exe` へ (両構成、ディレクトリが無ければ作る)。
   終了コードは `if !ERRORLEVEL! NEQ 0` で見る。
3. **`tools\collab_fixture.ps1 <dir>`**: 一時プロジェクト (`project.mye.json` = name / engineVersion / bootScene、`assets\scenes\main.scene.json` はエンジン `assets\scenes\` の最小シーンをコピー、`assets\textures\` に png 1 枚、`.mye\`、`.gitignore` はテンプレ 3 行) を作り、`git init -b main` + spec §4.4 の隔離 (`GIT_CONFIG_GLOBAL` 空ファイル / `GIT_CONFIG_NOSYSTEM=1` / `-c user.name=mye -c user.email=mye@example.com` / `core.autocrlf=false`) で初回コミット。既存ディレクトリは拒否。
4. **`tools\collab_verify.bat`** (CRLF、中身は `pwsh -File tools\collab_verify.ps1` 呼び出し): `cache\collab_verify\` に fixture → シナリオ `tests\collab\01_status.ndjson` (hello → repo_check → status 清浄 → ファイル変更 → status M → 追加 → status ? → 削除 → status D) を `MyeCollabCli.exe --root` へ流し、出力を正規化 (`<sha>` `<time>` `<author>`、fixture の絶対パスは `<root>`) して `tests\collab\01_status.expected.ndjson` と比較。差分は全文表示して exit 1。
5. **`.gitignore`** に `tools/collab/target/`。
6. **`ci.yml`**: env に `MYE_COLLAB_REQUIRED: 1`。managed host (Release) の後・replay の前に 1 ステップ: `dtolnay/rust-toolchain@stable` → `cargo test --manifest-path tools\collab\Cargo.toml` → `tools\build_collab.bat Release` → `tools\collab_verify.bat` (`shell: cmd`)。package contents ステップに `MyeCollab.dll` / `MyeCollabCli.exe` / `.git` が**無い**ことの検査を足す。
7. **`tools\check_rules.ps1`**: 規則 12 = `src\Engine` `src\Runtime` `src\GameLogic` `src\Shared` の `#include` に `SourceControl/` (または `SourceControl\`) を含む行があれば error。`$constGroups` に `kCollabProtoVersion / PROTO_VERSION` (`src\Editor\SourceControl\CollabProtocol.h` ⇄ `tools\collab\src\protocol.rs`)。メッセージ文言「across C++ and HLSL」はそのままでよい (nit)。
8. **C++ 最小クライアント** `src\Editor\SourceControl\CollabProtocol.h` (`kCollabProtoVersion = 1`、op 名の定数) / `CollabClient.h/.cpp`: `Load(exeDir)` → `LoadLibraryW` + `GetProcAddress` ×6 + 版照合 → 状態 `Unavailable::{NoService, ProtoMismatch}` or Ready、`Create(rootUtf8)`、`Request(json, std::function<void(const nlohmann::json&)>)` (id 採番)、`Poll()` (NULL まで drain、id で callback へ、`event` は購読者へ)、`Shutdown()` (`destroy` + `FreeLibrary`)。C++ 側にスレッドを作らない。
9. **`SourceControlSelfTest.h/.cpp`** + `EditorMain.cpp` 連鎖の**末尾**に append: (a) 偽の応答行 / 通知行を `CollabClient` の dispatch へ流して callback と購読が呼ばれる (b) `<exeDir>\MyeCollab.dll` があれば `Load → Create(一時ディレクトリ) → hello → Poll ループ (最大 5 s) → gitVersion が非空 → Shutdown`。無ければ `[selftest] SourceControl: MyeCollab.dll not found — SKIP` を出して true、ただし env `MYE_COLLAB_REQUIRED=1` なら false。
10. `pwsh -File tools\gen_project_files.ps1` (新ディレクトリ `src\Editor\SourceControl\`)。
11. **ADR-015** `docs\adr\ADR-015-in-process-rust-collab.md`: 元計画の決定 1〜3 を移し、決定 1 の反転 (別プロセス → in-process cdylib) と根拠 (ユーザー判断 2026-09-02、失うもの = メモリ隔離、残るもの = JSON 形 / cargo test / CLI) を書く。`engine_spec.md` §13 の ADR 一覧に 1 行 (本文の §14 節は sub-10)。
12. 冒頭確認: `--porcelain=v2` の下限 git 版を公式リリースノートで確認し、`git.rs` の定数コメントに出典を書く。

## やらないこと (このサブでは)

- notify 監視 / 定期 fetch / status の対束ね / 窓 / ローカライズ文字列 (sub-02 以降)。
- `hello` 以外の op を C++ から呼ぶ UI。

## 触る場所 (planner の見立て)

- 新規: `tools\collab\**`、`tools\build_collab.bat`、`tools\collab_fixture.ps1`、`tools\collab_verify.bat` / `.ps1`、`tests\collab\*.ndjson`、`src\Editor\SourceControl\{CollabProtocol.h, CollabClient.h, CollabClient.cpp, SourceControlSelfTest.h, SourceControlSelfTest.cpp}`、`docs\adr\ADR-015-*.md`。
- 変更: `.gitignore`、`.github\workflows\ci.yml`、`tools\check_rules.ps1`、`src\Editor\EditorMain.cpp` (連鎖末尾)、`build\Editor.vcxproj` (生成)、`engine_spec.md` §13 の一覧。
- 前例: `src\Engine\Engine\Script\DllReloader.h` (LoadLibrary の流儀)、`BuildSettingsWindow.cpp:38-82` (子プロセス旗)、`tools\build_managed.bat` (sln 外ビルドの bat の型)、`ci.yml` の既存ステップ順。

## 受け入れ条件 (このサブ)

1. `cargo test` 緑 (porcelain 6 fixture + ab ヘッダ + 往復 + proto)。
2. `tools\build_collab.bat Debug` → `bin\x64\Debug\MyeCollab.dll` / `MyeCollabCli.exe` と Release 側の 4 ファイルが揃う。`PATH` から cargo を外した cmd で実行 → メッセージ + exit 1 (1 回観測)。
3. `tools\collab_verify.bat` 緑 (01_status)。
4. `bin\x64\Debug\Editor.exe --selftest` (cmd /c 経由) 緑で、ログに hello の gitVersion が出る。DLL をリネームして再実行 → SKIP 行 + 緑。`set MYE_COLLAB_REQUIRED=1` + DLL 不在 → 赤 (1 回観測して戻す)。
5. `pwsh -File tools\check_rules.ps1` 緑。`kCollabProtoVersion` を 2 にして赤を 1 回観測して戻す。`src\Engine` のどこかに `#include "Editor/SourceControl/CollabClient.h"` を仮置きして規則 12 の赤を 1 回観測して戻す。
6. `bin\x64\Release\Editor.exe --package cache\dist_probe` の出力に `MyeCollab*` と `.git` が無い。
7. `tools\replay_verify.bat` 無変更緑 (Editor 層とツールしか触っていない証明。割れたら記録中に窓を触っていないかを先に疑う)。

## 検証コマンド

```
cd tools\collab && cargo test
tools\build_collab.bat Debug
tools\collab_verify.bat
cmd /c bin\x64\Debug\Editor.exe --selftest
pwsh -File tools\check_rules.ps1
cmd /c bin\x64\Release\Editor.exe --package cache\dist_probe
tools\replay_verify.bat
```

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL の写し)

```
SELF_EVAL: sub-01 (round 1)
実装:
  - tools\collab\Cargo.toml — package mye_collab / [lib] cdylib+rlib / [[bin]] mye_collab_cli /
    deps serde+serde_json のみ / profile.release+dev に panic="unwind" を明示。edition は 2021
  - tools\collab\src\protocol.rs — PROTO_VERSION=1、Request/Response/ErrorBody の serde 型、
    error.code 18 種と event 名 4 種の定数、event_line() / service_error_line()
  - tools\collab\src\git.rs — git 起動口 (共通引数 -c core.quotepath=false -c color.ui=false
    --no-pager / env LC_ALL=C GIT_TERMINAL_PROMPT=0 / CREATE_NO_WINDOW / stdin NUL /
    stdout+stderr をバイト回収)、parse_version、version() の下限 2.11 判定、classify_error()
  - tools\collab\src\porcelain.rs — status --porcelain=v2 -z --branch の解析
    (# ヘッダ 4 種 / 1 / 2 (旧パスは次レコード) / u / ? / !)
  - tools\collab\src\ops.rs — hello{fetchIntervalMin,autoFetch}->{gitVersion} /
    repo_check->{isRepo,toplevel,head,mergeInProgress,rebaseInProgress} /
    status->{branch,upstream,ahead,behind,entries[]}
  - tools\collab\src\worker.rs — worker 1 本の FIFO 直列実行、catch_unwind で panic ->
    service_error{internal_panic} + dead 化、以後は service_dead、Drop で join
  - tools\collab\src\lib.rs — C ABI 6 関数 (全て catch_unwind で包む)
  - tools\collab\src\main.rs — NDJSON CLI (--root)
  - tools\collab\tests\{porcelain,service}.rs — 14 本
  - tools\build_collab.bat — cargo 解決 (PATH -> %USERPROFILE%\.cargo\bin) -> release ビルド ->
    bin\x64\Debug\ と Release\ の両方へ MyeCollab.dll / MyeCollabCli.exe
  - tools\collab_fixture.ps1 — git 初期化済みの一時プロジェクト (project.mye.json /
    assets\scenes\main.scene.json / assets\textures\test.png / .mye\ / .gitignore 3 行)
  - tools\collab_verify.bat / .ps1 + tests\collab\01_status.ndjson(.expected) —
    fixture -> シナリオ -> 正規化 (<root>/<sha>/<gitversion>/<time>/<author>) -> 期待と比較
  - tools\check_rules.ps1 — 規則 12 (Editor 層封じ込め) と $constGroups の
    kCollabProtoVersion/PROTO_VERSION
  - .github\workflows\ci.yml — env MYE_COLLAB_REQUIRED=1、rust-toolchain + collab 1 ステップ
    (managed host の後・replay の前)、package contents に否定検査
  - src\Editor\SourceControl\{CollabProtocol.h,CollabClient.h/.cpp} — LoadLibraryW +
    GetProcAddress x6 + 版照合、Request/Poll/DispatchLine/Shutdown。C++ 側にスレッドは無い
  - src\Editor\SourceControl\SourceControlSelfTest.{h,cpp} + EditorMain.cpp 連鎖末尾
  - docs\adr\ADR-015-in-process-rust-collab.md + engine_spec.md §13 の ADR 一覧 1 行
  - .gitignore に tools/collab/target/
仕様との差分:
  - [追加] build_collab.bat の cargo 解決に %USERPROFILE%\.cargo\bin フォールバック
    (司会申し送りの罠 1)。**不在時の終了コードは sub どおり exit 1** (申し送りの
    「WARN + exit 0 の縮退」は採らなかった。受け入れ条件 2 と CI の要求に従った)
  - [追加] collab_verify のシナリオに '#' ディレクティブ (write / delete) と --update。
    sub は「ファイル変更 -> status M」を要求しているが変更手段が未定義だったため、
    ドライバ側の最小構文として定義した (ディレクティブ位置で CLI を起動し直す)
  - [逸脱] CLI の終了条件: sub は「EOF 後に未応答 id が無くなるまで poll」。実装は
    「1 要求ごとに応答を待って次を送る」+ EOF 後 200 ms の drain。出力順を決定的にするため
  - [逸脱] protocol.rs の Event は serde struct ではなく event_line() ビルダ (ワイヤ形は同じ)
  - [逸脱] CollabClient::Request の署名は (op, args, callback)。sub は Request(json, cb) だが
    id 採番をクライアント内に閉じたいので JSON はこちらで組む
  - [追加] CollabClient::AddPendingForTest (selftest (a) を DLL 無しで回すため)
  - [追加] git.rs で cwd 非存在を bad_request に分類 (放置すると git_missing に化けて
    「git を入れてください」という見当違いの案内になる)
  - [追加] status は --untracked-files=all (M66i のバッジがファイル単位のため)
  - [追加] porcelain は '!' (ignored) も解析。--ignored を渡す op を足しても壊れないように
  - [追加] edition 2021 を選択 (2024 ではない)。2024 は #[unsafe(no_mangle)] を要求し
    rustc 1.85 未満で通らなくなるため
  - [逸脱] 新規 C++ ファイルに japanese-comment-style の 5 行ヘッダを付けていない。
    MyEngine の既存ファイルに前例が 0 件で、CLAUDE.md の「既存コードに合わせる」を優先した
検証:
  - cd tools\collab && cargo test → 14 passed (porcelain 6 / service 8)。0 failed
  - tools\build_collab.bat Debug / Release → bin\x64\{Debug,Release}\MyeCollab.dll +
    MyeCollabCli.exe の 4 ファイル。PATH と USERPROFILE を潰した cmd → メッセージ + exit 1 を観測
  - tools\collab_verify.bat → PASS 01_status (6 行)。fixture を作り直しても緑 = 正規化が効いている
  - cmd /c bin\x64\Debug\Editor.exe --selftest → exit 0、ログに
    "[collab] git version: 2.48.1.windows.1"。Release も exit 0
  - DLL をリネーム → "SourceControl: MyeCollab.dll not found - SKIP" + exit 0。
    set MYE_COLLAB_REQUIRED=1 + DLL 不在 → FAIL + exit 1 を観測して戻した
  - pwsh -File tools\check_rules.ps1 → 0 error。kCollabProtoVersion=2 → rule 9 赤、
    src\Engine\Engine\Project.cpp に SourceControl の include → rule 12 赤、いずれも観測して戻した
  - cmd /c bin\x64\Release\Editor.exe --package cache\dist_probe_m66a → exit 0、
    出力に MyeCollab* も .git も 0 件
  - tools\replay_verify.bat → [PASS] 10 ジョブ全緑 (7 シーンペア + snapshot + タイムトラベル + 規則)
自己採点 (1-5):
  仕様適合: 4 — sub の 12 項目は全て実装。差分は上記 11 件で全て記載済み (逸脱 4 / 追加 7)
  正しさ: 4 — 受け入れ条件 7 件を全て実測 (成功側だけでなく cargo 不在 / DLL 不在 /
    規則 9・12 の赤という失敗側も観測)。未検証は ci.yml の実走のみ (YAML 構文と
    ステップ順は静的に確認、同じ 3 コマンドはローカルで緑)
  コード品質: 4 — 層の越境なし (規則 12 で機械保証)、C++ 側にスレッド無し、
    Rust の unsafe は FFI 境界の 6 関数だけ。コメントは日本語で「なぜ」を書いた
  テスト: 4 — cargo 14 本 + selftest 2 系統 + collab_verify 1 シナリオ。
    porcelain のリネーム消費・panic 隔離・不正 JSON など壊れ方の側も検査している
不安・質問: (SELF_EVAL 本文参照)
```

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-09-02)。受け入れ条件 7 件は SELF_EVAL の主張を現物で裏取り: bat 2 本は CRLF (52/52, 27/27)、lib.rs の export は spec §4.0 の 6 関数と一致し全て catch_unwind、規則 12 の正規表現と $constGroups の追加を diff で確認、ci.yml のステップ順 (managed x2 -> rust-toolchain -> collab -> replay) と否定検査 3 件を確認、EditorMain の連鎖末尾に append、cache/replay_m66a.log 3223 行目に [PASS]。差分 11 件は全件「仕様の欠落 or 妥当な追加」で spec §8 に取り込んだ。should 1 件 (porcelain の非 ASCII パス fixture が無い) と nit 2 件 (AddPendingForTest のテスト専用公開 API、rust-toolchain ステップに name が無い) は sub-02 へ申し送り。
