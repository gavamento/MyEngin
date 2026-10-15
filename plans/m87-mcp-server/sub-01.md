# sub-01: `--dump-index` と索引生成スクリプト、規則 15-a/b/f

- 依存: なし
- 状態: OK (commit 54f22f1)
- 往復: 0

## やること

spec 4.1.1 / 4.1.2 / 4.4 (規則 15-a・15-b・15-f) を実装する。

1. `Editor.exe --dump-index <out.json>`: ヘッドレスで組み込みコンポーネント・フィールド・エンジン共通 CLI 表・showcase 表を決定的な JSON で書いて終了する。
2. `EngineIndexDumpSelfTest` (`*SelfTest` パターン、Editor 層): ダンプの JSON 文字列を作り、component 件数が組み込み登録数と一致すること、既知の 1 型 (例: `LocalTransform` の `position` が `Float3`) が入っていること、2 回作って同一であることを見る。`--selftest` から呼ぶ。
3. `tools\gen_engine_index.ps1`: ダンプ + 静的パース (abi / cliEditor / adr / spec / golden / l10n / mentions / sources) → `cache\engine_index.json`。下限チェック、`-CheckOnly`、ルート差し替え引数。
4. `check_rules.ps1` に規則 15 の見出しコメント (15-a〜15-f の全体像を先に書く — 計画書 §6「後から足すと必ず漏れる」) と、15-a / 15-b / 15-f の実装。
5. §8-1 の実測 (セクション別・全体のバイト数) を実装メモに書く。

## やらないこと (このサブでは)

- `tools\mcp\` の crate (sub-02)。規則 15-c / 15-d / 15-e の実装 (見出しコメントで予告だけする)
- CLI フラグのパース挙動・表の中身の変更
- 既知の check_rules エラー 3 件の修正 (EditorMain.cpp の `#ifdef _DEBUG` を含む)

## 触る場所 (planner の見立て)

- `src\Editor\App\EditorMain.cpp`: argv ループ (230〜360 行付近)。連鎖は MSVC の入れ子上限 (C1061) に達しているので、`--anim-preview` 等と同じく**連鎖の手前で拾って continue** する。実行は `--fracture-bench` / `--perf-bench` と同じ早期 return 群 (395〜420 行付近、D3D 初期化より前) に置く。stdout ではなくファイルへ書く (GUI サブシステムなので stdout は信用しない)。
- 新規 `src\Editor\Tools\EngineIndexDump.{h,cpp}` (+ `EngineIndexDumpSelfTest.{h,cpp}`)。登録は `RegisterBuiltinComponents()` (`src\Engine\Core\Ecs\Components.cpp:42`。`World` の ctor が呼ぶ。`Register` は同名なら既存 TypeId を返すので重ねて呼んでも安全 — coder は確認すること) を直接呼び、`ComponentRegistry::Get()` を列挙する。World は作らない (規則 15-b の許可リストに World.h を入れない)。
- `src\Engine\Engine\App\EngineCli.{h,cpp}`: `kEngineCliFlags[]` (40 行) は無名名前空間。名前と `CliValue` を列挙する読み取り専用関数を足す。
- `src\Engine\Engine\Demo\ShowcaseScenes.{h,cpp}`: `kShowcases[]` (24 行) を列挙する関数 (既に `FindShowcase` があるので同じ流儀で)。
- JSON の文字列エスケープ (`"` `\` 制御文字、UTF-8 はそのまま)。既存のエスケープ関数があれば使う (coder が探す)。
- 新規 `tools\gen_engine_index.ps1`。`$apiVersionSlots` は check_rules.ps1:743 にある — 二重定義にせず check_rules 側から読むか、同じ値を取り出す方法を coder が決める (二重定義するなら check_rules 側と食い違ったら失敗する照合を入れる)。日本語を含むファイルは `[IO.File]::ReadAllText` / `ReadLines` で UTF-8 として読む (check_rules.ps1:660 の注意と同じ)。
- `tools\shot_verify.bat` の `call :shot` 行 (117 行〜)。`set SHOT=` の切り替えがあるので、引数は行に書かれたものだけ (SHOT の展開はしない) を記録すればよい。
- `tools\check_rules.ps1`: 規則 14 の後 (`=== result` の前)。15-f は `pwsh -NoProfile -File tools\gen_engine_index.ps1 -CheckOnly` を呼び、非 0 で ERROR。
- `tools\gen_project_files.ps1` を新規 .cpp 追加後に実行 (AGENTS.md §7)。

## 受け入れ条件 (このサブ)

spec 5. の 1〜7。

1. `bin\x64\Debug\Editor.exe --dump-index <tmp>\a.json` が exit 0、D3D デバイス作成のログ行なし、components 件数 = 組み込み登録数 (script 型を除く)。
2. 同じ exe で 2 回 / Debug と Release で、出力がバイト同一。
3. `gen_engine_index.ps1` が全セクション入りの `cache\engine_index.json` を書く。abi = version 30 / 178 スロット。セクション別・全体のサイズを実装メモに記録 (全体 2 MB 超なら不安・質問に書く)。
4. `-CheckOnly` は Editor.exe を起動せず exit 0。違反フィクスチャ (golden 行の削除、またはスロットの削除) で exit 1。フィクスチャは scratch に作り、作業ツリーに残さない。
5. 規則 15-a / 15-b / 15-f が入り、check_rules OK (既知 3 件以外の ERROR なし)。15-a / 15-b は違反を仕込んだ一時コピーで ERROR を 1 回示す。
6. `Editor.exe --selftest` (Debug) PASS、`EngineIndexDumpSelfTest` を含む。Release ビルド成功。
7. `EngineCliSelfTest` PASS。

## 検証コマンド

- MSBuild で Debug|x64 と Release|x64 をビルド
- `bin\x64\Debug\Editor.exe --dump-index ...` (PowerShell から呼ぶなら `Start-Process -Wait -PassThru` か `cmd /c` — 直接 `&` だと待たない)
- `bin\x64\Debug\Editor.exe --selftest`
- `pwsh -NoProfile -File tools\gen_engine_index.ps1` / `-CheckOnly`
- `pwsh -NoProfile -File tools\check_rules.ps1`
- `replay_verify.bat` は不要 (sim の経路を変えない。CLI のパースは EngineCliSelfTest が見る)。sim に触れる変更が要ると分かったら不安・質問に書いて止まる

## 実装メモ (coder が追記)

SELF_EVAL: sub-01 (round 1) — 本文は司会への返信と同じ。要点:

- 実測サイズ (§8-1、`cache\engine_index.json`): 全体 351,942 B。components 106,747 (81 型 / 932 フィールド) / l10n 181,822 (1639) / adr 25,690 (31) / abi 8,230 (178) / spec 7,003 (105 見出し) / mentions 10,073 / cliEngine 4,646 (112) / sources 3,206 / golden 2,397 (30) / cliShowcase 970 (22) / cliEditor 991 (41)。2 MB には遠い。`--dump-index` の生出力は 113,647 B。
- 下限 (gen_engine_index.ps1): adr 27 / spec 94 / golden 27 / l10n 1475 / cliEditor 40 (-CheckOnly)、components 72 / fields 838 / cliEngine 100 / cliShowcase 19 (通常モード)。abi は check_rules の `$apiVersionSlots[30]` = 178 と一致。
- Debug / Release の `--dump-index` 出力は SHA256 A56D43A8...95C1 でバイト同一 (113,647 B)。

## フィードバック履歴
- round 1: VERDICT REWORK (planner)。must 1 件: 新規 5 ファイル (EngineIndexDump.h/.cpp、EngineIndexDumpSelfTest.h/.cpp、gen_engine_index.ps1) の作業ツリーが CRLF。.gitattributes は `* text=auto eol=lf` なので、コミット後に check_rules 規則 14-c が 5 件の ERROR を出す (今は未追跡なので見えていない)。LF に直し、`git add -N` 等で追跡に載せた状態で check_rules を確かめること。他の条件 1〜7 は planner が再実行して確認済み (check_rules 既知 3 件のみ、-CheckOnly exit 0、Release の dump の SHA256 が SELF_EVAL と一致)。15-b の厳密な許可リストは承認
- round 2: VERDICT OK (planner)。訂正: round 1 の must (CRLF) は planner の測り違いだった。Git Bash の `grep -c $''` が全行を数えていた。バイト単位で数え直すと 5 本とも CR は 0 で、round 1 の時点から LF だった。round 2 の `git add -N` + check_rules で、既知 3 件以外に ERROR が無いことを確認した。コミット件名: "M87a: エンジン索引のダンプ (--dump-index) と gen_engine_index.ps1、規則 15 を追加する"
