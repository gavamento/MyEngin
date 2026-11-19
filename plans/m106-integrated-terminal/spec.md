# MyEngine 統合ターミナル — 仕様書

- slug: m106-integrated-terminal
- 状態: 確定 (2026-11-19)
- 依頼原文: このエンジンでターミナルが開けるようにしたい
  - 追加指示: 「ConPTY+ImGui描画 は？」「vimやCLIなどにも対応させたい」「vimは対応対象外でいいけどclaudecodeCLIなどは対象に入れて」「git・ビルド・Claude Code CLIなども使えるMyEngin統合ターミナルにするなら」

## 1. 目的 (なぜ作るか)
エディタ利用者が、エディタを離れずに PowerShell・git・ビルド・Claude Code CLI 等の対話型 CLI を使える状態にする。
さらに「このエディタの中で動いた AI CLI の変更」を AI 変更追跡で区別でき、VS 開発者環境とエンジン用の環境変数・`tools\` の PATH が最初から通っていることで、外部ターミナルより手数が少なく、追跡可能 (AGENTS.md 3.2 観測可能性) にする。

## 2. 疑った点と結論
| 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|
| 二重実装か | ターミナル/ConPTY/VT パーサは未実装。`/home/user/MyEngin/src/Editor/App/ChildProcess.h` はログファイル出力のみで対話不可 | — | 新規実装。ChildProcess は流用しない (責務が違う) |
| 外部ターミナル起動 (wt.exe) で十分では | 外部窓ではドッキング・AI追跡の紐付け・環境注入が効かない | ConPTY + ImGui 自前描画 (合意済み) | 埋め込み |
| VT 解釈を自作するか | Claude Code CLI は Ink 製で、カーソル移動・消去・SGR truecolor・bracketed paste (DECSET 2004)・代替画面等を使う。自作は巨大 | 司会推奨 libvterm | **libvterm 0.3.3 (MIT, C99) を `/home/user/MyEngin/external/libvterm/` に同梱**。この環境で取得・gcc -std=c99 -Wall 警告 0 でコンパイル・スモーク (1049/2004/truecolor/全角幅2/OSC8 無害/貼付で `ESC[200~` 出力) を確認済み (下記7) |
| 既定レイアウトへの配置 | 気づきやすさと既存レイアウトの変化のトレードオフ | 2026-11-19: 下段 Console ドックにタブで入れる | 4.3 のとおり |
| 日本語 IME | インライン表示は Win32 IME 処理が窓へ入る | 2026-11-19: 確定文字のみ送る | 4.1 のとおり |
| 却下: 自作 VT パーサ | 保守・網羅性が劣る | — | 却下 |
| 却下: Windows Terminal の TerminalCore 流用 | 依存が巨大 (C++/WinRT)、同梱方針に合わない | — | 却下 |
| 却下: libtsm | LGPL 系混在・Windows 実績薄 | — | 却下 |
| DECSET 2026 (同期出力) 未対応 | libvterm 0.3.3 の state.c に 2026 の case 無し (未知モードは無視) | — | 許容。描画は「読み取りキューを1フレーム分まとめて投入してから描く」ことでちらつきを抑える。対応は後回し |
| ConPTY を静的リンクできるか | WindowsTargetPlatformVersion=10.0、_WIN32_WINNT 未定義、対応 OS は Windows 10/11 | — | `CreatePseudoConsole` / `ResizePseudoConsole` / `ClosePseudoConsole` は kernel32 から GetProcAddress で実行時解決。無ければ窓に「非対応」表示 (障害の局所化) |
| フォント | 基本は Segoe UI (プロポーショナル) `/home/user/MyEngin/src/Engine/Renderer/ImGui/ImGuiTheme.cpp:175` | — | 等幅フォント (Cascadia Mono → Consolas の順で `C:\Windows\Fonts` から実行時読込、無ければ既定フォントで固定セル幅描画) を追加。日本語は既存の日本語グリフ統合方式に合わせて merge (merge 方式は coder が ImGuiTheme を確認して決める) |
| 決定性への影響 | ターミナルは Editor 層のみ、シミュレーションに触れない | — | 決定性契約外。ただし Engine/Runtime/GameLogic/Shared/Server から include 禁止を check_rules 新規則で強制 (規則20と同型) |
| AI 追跡の識別方法 | フックは常に `source="terminal"` で session_begin (`/home/user/MyEngin/tools/track/src/hook.rs:422-430`)。sessions に識別列なし (`/home/user/MyEngin/tools/track/src/schema/v1.sql:10-23`)。移行は `/home/user/MyEngin/tools/track/src/schema/mod.rs:15` の Migration 配列 (v3 まで) | 「AI変更追跡との紐付け」(合意済み)。2026-11-19 裁定を承認: host_terminal 列 (v4)、source は terminal のまま | 統合ターミナルが子プロセスに `MYE_TERMINAL_ID` (エディタ起動ごとの一意ID + パネル番号) を渡す。フックが読めれば session_begin に `hostTerminal` を載せ、**スキーマ v4 で `sessions.host_terminal TEXT NULL` を追加**。`source` の値は変えない (既存クエリ互換)。却下案: source に新値 "editor_terminal" — 既存の terminal 判定が壊れる |
| VS 開発者環境の読み込み方法 | vswhere は `/home/user/MyEngin/src/Editor/Tools/VisualStudioLauncher.cpp:238-290` の匿名 namespace | 合意済み。2026-11-19: 読み込みを待ってから起動、30 秒タイムアウトで素の環境 | vswhere 探索を名前付き関数に昇格して共用。`VsDevCmd.bat -arch=x64 && set` を一度だけ実行して環境差分を取得・キャッシュ (エディタ起動中のみ)。失敗・未導入時は警告表示して素の環境で起動 |
| エンジン環境変数 | — | 合意済み | `MYE_PROJECT_ROOT` (リポジトリ/プロジェクトルート)、`MYE_TERMINAL_ID`、PATH 先頭に `<root>\tools` を追加。既存変数は上書きしない (PATH のみ前置)。値の一覧は 4.2 |
| vim 等全画面エディタ | — | 対象外 (合意済み) | 代替画面自体は libvterm が持つので動く可能性はあるが、受け入れ条件にしない |
| タブ | — | 1パネル1シェル (合意済み) | セッションを `TerminalSession` オブジェクトに閉じ、窓が1個所有。複数化を妨げない |
| SCM 窓更新 | — | 対象外 (合意済み) | やらない |

## 3. スコープ
- やる: libvterm 同梱 / Win32 非依存のターミナルコア (画面モデル・スクロールバック・キー→VT 変換・シェル解決・環境ブロック組立) / ConPTY ホスト / ドッキング可能な Terminal 窓 (描画・入力・IME 確定文字・選択コピー・貼付・リサイズ・再起動) / 設定 (シェル上書き、フォントサイズ、スクロールバック行数、VS 環境の有無) / VS 開発者環境 / エンジン環境変数と tools PATH / AI 変更追跡の紐付け (v4 スキーマ、フック、AI Changes 窓で「統合ターミナル」表示) / ローカライズ / SelfTest / check_rules 規則 / engine_spec §9.4
- やらない: vim 等全画面エディタの保証、タブ・分割、SCM 窓更新、マウスレポーティング (DECSET 1000 系、Claude Code CLI の必須ではない)、OSC 8 リンクのクリック、シェル統合 (OSC 133)
- 後回し: DECSET 2026、複数セッション、ハイパーリンク、検索

## 4. 仕様
### 4.1 振る舞い
- Window メニュー「ターミナル」で窓を開く。初回表示でシェルを起動 (遅延起動)。窓を閉じてもセッションは維持、エディタ終了 (OnShutdown) で Job を閉じて子孫ごと終了。
- シェル解決: 設定 `terminal.shell` が空なら pwsh.exe → powershell.exe → cmd.exe の順に PATH/既定位置で探す。設定値が見つからなければエラー表示し自動検出へ落とさない (明示指定を尊重)。引数は空白・`"` を含むものだけ QuoteCommandLineArg (`/home/user/MyEngin/src/Editor/ChangeTracking/TrackLaunch.h:21`) で引用し、それ以外はそのまま渡す (cmd.exe は引用されたスイッチ "/c" を解釈しないため。sub-03 round 1 で変更)。
- 作業ディレクトリ: プロジェクトルート。
- 出力: ConPTY 出力パイプを専用読み取りスレッドが読み、ロック付きバイトキューへ。UI スレッドが毎フレーム全量を libvterm へ投入 (libvterm は UI スレッドのみが触る)。
- 入力: 窓フォーカス中のキー/文字を VT バイト列へ変換 (libvterm の `vterm_keyboard_*` を使用) して入力パイプへ書く。Ctrl+C はシェルへ送る (選択中ならコピー)。Ctrl+V / 右クリックは貼付 (bracketed paste 有効時は 200~/201~ で包む)。フォーカス中は `WantTextInput` 相当を立ててエディタショートカット (`/home/user/MyEngin/src/Editor/App/EditorApp.cpp:1970-2011`) を抑止。
- リサイズ: 窓サイズ ÷ セル寸法で行列を算出し、変化時のみ `vterm_set_size` と `ResizePseudoConsole`。最小 20x5。
- スクロールバック: libvterm の sb_pushline で行を保持 (既定 5000 行、設定可)。ホイールでスクロール。最下部表示中は新規出力に追従し、履歴表示中は新規出力で戻さず、キー入力・貼付で最下部へ戻る (Windows Terminal と同じ。sub-04 round 1 で変更)。
- 子終了: 終了コードを1行表示し「再起動」ボタン。ConPTY 非対応 OS / シェル無し / 起動失敗はパネル内にエラー表示し、エディタは継続。
- 選択: ドラッグで矩形でなく行方向選択、Ctrl+C / コンテキストメニューでコピー (UTF-8)。

### 4.2 データ・保存形式・互換性
- 設定は `/home/user/MyEngin/src/Editor/App/EditorGlobalSettings.h` (`%LOCALAPPDATA%\MyEngine\editor_global.json`) に追加: `terminalShell` (文字列、空=自動)、`terminalFontSize` (既定 14)、`terminalScrollback` (既定 5000、範囲 100〜100000)、`terminalLoadVsDevEnv` (既定 true)。キー欠落時は既定値 (旧ファイル互換)。
- 子プロセス環境: 親環境 (+ VS 開発者環境差分) に `MYE_PROJECT_ROOT`、`MYE_TERMINAL_ID=<エディタPID>-<起動時刻>-<連番>`、`TERM_PROGRAM=MyEngine`、`COLORTERM=truecolor`、PATH 先頭に `<root>\tools`。Windows の環境ブロック規則 (大文字小文字無視でソート済み、二重 NUL 終端) を守る。
- AI 追跡: track スキーマ v4 `ALTER TABLE sessions ADD COLUMN host_terminal TEXT`。IPC `session_begin` に任意フィールド `hostTerminal` (欠落=NULL、旧フック互換)。`/home/user/MyEngin/docs/ai-change-tracking/` の ipc-contract.md / db-schema.md / hooks-setup.md を同時更新。
- ABI (src/Shared) と Interop.cs には触れない。

### 4.3 UI / ビジュアル
- ドッキング窓「ターミナル###Terminal」。既定レイアウトでは下段 (Console と同じドック) にタブとして配置 (2026-11-19 ユーザー確定) (`SetupDockLayout` `/home/user/MyEngin/src/Editor/App/EditorApp.cpp:2315-2374`)。
- 背景は濃色、16色 + 256 + truecolor、太字・下線・反転・カーソル (ブロック点滅なし)、全角は2セル。上部ツールバー: シェル名、再起動、クリア、VS 環境読み込み状態。
- 文字列はすべて `Tr()` + `LocalizationTable.inl` 両言語。

### 4.4 非機能
- Editor 層のみ。Engine/Runtime/GameLogic/Shared/Server から `Editor/Tools/Terminal/` を include 禁止 (check_rules 新規則)。生 D3D 型は使わない (ImGui DrawList のみ)。
- 性能: 80x24 で 1MB の出力を流しても UI が固まらない (1フレームの投入量上限 例 256KB、残りは次フレーム)。
- スレッド: libvterm と画面モデルは UI スレッドのみ。読み取りスレッドはバイトをキューへ積むだけ。終了順序: ClosePseudoConsole → 読み取りスレッド join → ハンドル解放 (ClosePseudoConsole が読み取り側のパイプ排出を待つ既知の仕様に注意。先に読み取りを止めない)。
- `/fp:precise` 等の構成は既存のまま。libvterm の vcxproj は手書き (`/home/user/MyEngin/tools/gen_project_files.ps1:100` の external 扱い)。

## 5. 受け入れ条件
1. libvterm 0.3.3 が `/home/user/MyEngin/external/libvterm/` に LICENSE ごと同梱され、VERSIONS.md に版・出所・ライセンスが記載。— Linux: gcc でコンパイル+スモーク / Windows: ソリューションビルド
2. 画面モデルが UTF-8・全角幅・SGR (16/256/truecolor)・代替画面・スクロールバック・リサイズを正しく反映する。— Linux g++ テスト + SelfTest(Fast)
3. キー→VT 変換が Enter/BS/Tab/矢印/Home/End/PgUp/PgDn/Del/F1-12/Ctrl+文字/Alt+文字/bracketed paste を正しいバイト列にする。— 同上
4. シェル解決が設定優先・自動検出順を守り、環境ブロックが規則どおり (ソート・二重NUL・PATH 前置・既存変数を上書きしない)。— 同上
5. ConPTY でシェルが起動・入出力・リサイズ・終了・再起動でき、エディタ終了で子孫が残らない。ConPTY 解決失敗時はエラー表示で継続。— Windows 手動 + SelfTest(Full) で `cmd /c echo` 往復
6. Terminal 窓が Window メニューから開け、既定レイアウトに入り、等幅描画・選択コピー・貼付・ショートカット抑止が効く。— Windows 手動 (スクショ)
7. Claude Code CLI が窓内で起動し、入力欄・スピナー・色・複数行貼付が崩れずに使える。git log/status、MSBuild 出力が表示できる。— Windows 手動 (スクショ)
8. VS 開発者環境読み込みで `cl` / `msbuild` が通る。未導入時は警告して素の環境で起動。`MYE_PROJECT_ROOT`・`MYE_TERMINAL_ID`・tools PATH が見える。— Windows 手動 + 環境組立テスト
9. 統合ターミナル内の Claude Code セッションが `sessions.host_terminal` に ID を持ち、AI Changes 窓で「統合ターミナル」と区別表示。旧 DB は v4 へ移行、旧フック (フィールド無し) も受理。— Linux: `cargo test` (tools/track) / Windows: 実操作
10. 文字列が両言語で揃い、check_rules (規則10・新規則) が通る。engine_spec §9.4 と docs/test_checklists.md が更新。— Windows: `tools\check_rules.ps1` / Linux: grep による同等確認
11. Debug|x64・Release|x64 ビルド成功、`Editor.exe --selftest` (Fast 60 秒以内) と `--selftest-full` 成功。— Windows

## 6. サブ分割
| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | libvterm 同梱と VT 機能充足の確認 | なし | 1 | M106a: libvterm 0.3.3 を external に同梱する |
| sub-02 | Win32 非依存のターミナルコア (画面モデル・キー変換・シェル解決・環境組立) | sub-01 | 2,3,4 | M106b: ターミナルの画面モデルと入力変換を Win32 から分けて作る |
| sub-03 | ConPTY ホストとセッション | sub-02 | 5 | M106c: ConPTY でシェルを起動するターミナルセッション |
| sub-04 | Terminal 窓 (描画・入力・設定・配線・ローカライズ・規則) | sub-03 | 6,7,10,11 | M106d: ドッキング可能なターミナル窓を追加する |
| sub-05 | VS 開発者環境とエンジン環境変数 | sub-04 | 8 | M106e: ターミナルに VS 開発者環境とエンジン環境変数を通す |
| sub-06 | AI 変更追跡との紐付け | sub-02 (track 側は並列可、窓表示は sub-04 後) | 9 | M106f: 統合ターミナル由来の AI セッションを区別する (track スキーマ v4) |

## 7. 未決事項・リスク
- libvterm の取得経路: GitHub / launchpad はこの環境のプロキシで 403。**Ubuntu ミラー `http://archive.ubuntu.com/ubuntu/pool/universe/libv/libvterm/libvterm_0.3.3.orig.tar.gz` から取得できた** (sha256 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0、展開先 `/tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/libvterm-probe/libvterm-0.3.3/`)。上流 (launchpad 0.3.3) と同一物のはずだが、Windows 側で上流の sha256 と照合すること。
- MSVC での C99 コンパイル: neovim が MSVC で libvterm をビルドしている実績 (記憶による、未確認)。この環境では検証不可。
- ConPTY は Windows 10 1809 以降。対応 OS 表記「Windows 10/11」の古いビルドでは非対応表示になる。
- Claude Code CLI の VT 要件は公式に列挙されていない (記憶による)。sub-01 で代表的シーケンスのスモークを取り、実機確認は sub-04 で行う。
- 日本語 IME: 確定文字のみ送る。未確定文字のインライン表示はしない (2026-11-19 ユーザー確定)。
- 貼付サイズ: 上限・分割書込なし (同期 WriteFile)。巨大貼付で UI が待ちうる。実機で問題が出たら別サブで分割書込を入れる (sub-04 round 1 裁定)。
- track の Rust テスト: cargo はこの環境にある (`/root/.cargo/bin/cargo`)。依存クレート取得可否は sub-06 で確認。

## 8. 変更履歴
- 2026-11-19 sub-03 round 1 (coder SELF_EVAL): 4.1 の引数引用を「空白・" を含むものだけ引用」に変更。理由は cmd.exe が引用されたスイッチを解釈しないため。
- 2026-11-19 sub-04 round 1 (coder 質問): 4.1 スクロールバックの追従条件を変更 (出力では戻さず入力で戻す)。理由はスピナーを出し続ける CLI 実行中に履歴が読めないため。実装は sub-05 に追加。ADR 作成と VS 環境状態表示も sub-05 に割当。
