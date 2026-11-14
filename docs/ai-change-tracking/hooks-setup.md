# Claude Code のフックの設定 (M104i)

`mye_track.exe hook` を Claude Code のフックに登録すると、Claude Code が `Edit` / `Write` で編集する直前と直後に追跡サービスへ知らせが届き、**宣言と一致した編集が Verified (確定) になる**。設定は任意。設定しなくても追跡は動く (その場合 AI の変更は、申告があれば Claimed、無ければ Inferred / Unknown)。

規則の正本は [attribution-rules.md](attribution-rules.md) §6.1、pipe の取り決めは [ipc-contract.md](ipc-contract.md)。

## 1. 準備

- `mye_track.exe` の場所: 開発機は `tools\build_track.bat Release` が `bin\x64\Release\mye_track.exe` (と `bin\x64\Debug\`) へ置く。配布物では Editor の隣 (配布物への同梱は M104u で足す)。
- 設定は利用者が `.claude\settings.json` か `.claude\settings.local.json` に書く。**このリポジトリの作業 (AI を含む) は、これらのファイルを書き換えない。**

## 2. 設定の例

`hooks.PreToolUse` と `hooks.PostToolUse` に、`matcher` を `Edit|Write`、`type` を `command` にして登録する。`--project` には**プロジェクトのルート** (サービスが追跡するディレクトリ) を指定する。`timeout` は秒で、既定の 600 秒に頼らず明示する。

```json
{
  "hooks": {
    "PreToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          {
            "type": "command",
            "command": "<MyEngine のルート>/bin/x64/Release/mye_track.exe hook pre --project <プロジェクトのルート>",
            "timeout": 10
          }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          {
            "type": "command",
            "command": "<MyEngine のルート>/bin/x64/Release/mye_track.exe hook post --project <プロジェクトのルート>",
            "timeout": 10
          }
        ]
      }
    ]
  }
}
```

任意で、セッションの開始と終了も知らせられる (`pre` が最初の編集でセッションを自動で作るので、無くても動く)。上の `hooks` に次を足す。

```json
{
  "hooks": {
    "SessionStart": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "<MyEngine のルート>/bin/x64/Release/mye_track.exe hook session-start --project <プロジェクトのルート>",
            "timeout": 10
          }
        ]
      }
    ],
    "SessionEnd": [
      {
        "hooks": [
          {
            "type": "command",
            "command": "<MyEngine のルート>/bin/x64/Release/mye_track.exe hook session-end --project <プロジェクトのルート>",
            "timeout": 10
          }
        ]
      }
    ]
  }
}
```

`<MyEngine のルート>` と `<プロジェクトのルート>` は自分の環境の絶対パスに置き換える。パスは `/` 区切りで書く (コマンドをシェルが解釈するときに `\` が落ちるのを避けるため。Windows は `/` を受け付ける)。

## 3. 動き

| サブコマンド | 標準入力 (Claude Code の JSON) | サービスへ送るもの |
|---|---|---|
| `pre` | `session_id`、`cwd`、`tool_name`、`tool_input`、`tool_use_id` | `session_begin` (`clientSessionId` = `session_id`、`provider = claude`、`source = terminal`。既にあればそれが返る) → `op_begin` (`clientOpId` = `tool_use_id`、`declaredEdit`) |
| `post` | 同上 + `tool_response` | `op_end` (`clientSessionId` + `clientOpId`、`result`) |
| `session-start` | `session_id`、`cwd` | `session_begin` |
| `session-end` | `session_id` | `session_end` |

- 対応する道具は `Write` (`file_path`、`content`) と `Edit` (`file_path`、`old_string`、`new_string`、`replace_all`)。他の道具 (Bash、MultiEdit、NotebookEdit など) は何もせず終わる。`matcher` を絞っていなくても静かに動く。
- `declaredEdit` が編集の宣言。サービスが `op_begin` で対象を読み、宣言を Before に当てた結果のハッシュと、`op_end` の後に観測した After のハッシュが一致すれば Verified になる。`pre` と `post` の間に人が同じファイルを変えた、`old_string` が実際の内容に無い、といったときは一致せず Claimed 止まり。
- 1 行が 1 MiB を超える要求 (巨大な `content`) は、`declaredEdit` を付けずに記録する (Claimed 止まり。記録は欠落しない)。
- 失敗の説明は標準エラーへ 1 行だけ。

### 守っていること

- **終了コードは常に 0。** 終了コード 2 は PreToolUse で道具を止めるため、サービスが居ない・入力が読めない・時間切れ・内部の失敗 (panic を含む) のどれでも 0 で返る。追跡に欠落が残るだけで、AI の編集は止まらない。
- **標準出力へは何も出さない。** 標準出力の JSON は Claude Code が解釈する。
- **全体で 5 秒を超えない。** 超えそうなら途中でやめて 0 で返る。
- **対応しない入力は無視する。** 知らない項目は読み飛ばす (公式の入力は項目が増えうる。pipe の `bad_args` とは方針が違う)。

## 4. どのプロジェクトのサービスへ繋ぐか

1. `--project <root>` があればそれ。サービスが居なければ、`pre` と `session-start` のときだけ起動する (`post` と `session-end` は、終える相手の記録が無いので起動しない)。
2. 無ければ、対象のファイル (`tool_input.file_path`。無ければ `cwd`) から親へたどり、`.mye\ai_changes\service.json` がある最初のディレクトリ (= サービスが動いたことのあるプロジェクト)。そのサービスが動いていれば繋ぎ、動いていなければ何もしない。**この場合サービスは起動しない。**
3. どちらも無ければ何もしない。

`cwd` はプロジェクトのルートとは限らない (サブディレクトリで作業していることがある) ので、`cwd` をそのままルートにはしない。サブディレクトリを別のプロジェクトとして追跡してしまうため。設定には `--project` を付ける形 (上の例) を勧める。

`file_path` がプロジェクトのルートの外なら、何も送らない。

サービスを起動するときの作法 (切り離し、標準入出力を渡さない、`MYE_TRACK_EXE`) は [ipc-contract.md](ipc-contract.md) §9.5。`MYE_TRACK_EXE` を設定した場合はそのファイルだけを使う (無ければ起動に失敗して、何もせず終わる)。設定しなければ `mye_track.exe` 自身が起動する。

## 5. 本物の入力の記録の取り方 (未確認の項目を確かめるために)

環境変数 `MYE_TRACK_HOOK_DUMP=<ファイル>` を設定したときだけ、フックが標準入力を 1 行にして、そのファイルへ**追記**する。

- **プロジェクトの外へも書く唯一の例外**。既定では無効で、設定したときだけ働く。追記に失敗しても終了コードは 0。
- Claude Code を起動するシェルで設定しておく (フックは Claude Code の子プロセスなので環境を引き継ぐ)。例 (PowerShell): `$env:MYE_TRACK_HOOK_DUMP = "$env:TEMP\mye_hook_dump.jsonl"; claude`
- 入力が壊れていても、そのまま記録する。
- 取れた記録は、`tests\track\fixture\` に固定入力として置いて回帰検証にできる (置く前に、パスや秘密を含む内容を確かめる)。

## 6. 確認済みと未確認

| 項目 | 状態 |
|---|---|
| 入力の項目名 (`session_id` / `cwd` / `tool_name` / `tool_input` / `tool_use_id` / `tool_response`、Write と Edit の `tool_input`) | **確認済み** (公式文書 <https://code.claude.com/docs/en/hooks> を 2026-11-14 に取得。試作 5) |
| 合成した入力でのフックの動作 (宣言との一致、失敗時の終了コード 0、標準出力が空、5 秒以内、サービスの起動、標準入出力を掴ませないこと) | **確認済み** (`cargo test --manifest-path tools\track\Cargo.toml` の `tests\hook.rs` と `tools\track_verify.bat` の `# hook`) |
| **本物の Claude Code でフックを 1 回動かした入力** | **未確認** (ユーザーの手作業。手順は `docs\test_checklists.md` の M104i) |
| `tool_response` の形 (失敗をどう表すか) | **未確認**。D14 では Write の例が `{"filePath":..,"type":"create"}`。`success: false` / `is_error: true` / 空でない `error` の印があれば `failed`、判定できないものは `ok` にしている |
| Edit の CRLF や引用符の扱い (Claude Code が `old_string` / `new_string` を実際にどう当てるか) | **未確認**。一致しなければ Verified にならず、Claimed 止まりに落ちる (安全側) |
| `old_string` が複数あって `replace_all` が偽のときに Claude Code が実際に拒否するか | **未確認** |
| MultiEdit / NotebookEdit | **未確認**。対応しない (何もしない) |
| コマンドを解釈するシェルと、`/` 区切りのパス | **未確認** (Windows の Claude Code が使うシェルでの実走をしていない) |
| Codex CLI / Gemini CLI のフック | **未確認**。後回し (`change_report` で Claimed 止まり) |
