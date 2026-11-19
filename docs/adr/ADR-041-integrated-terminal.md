# ADR-041: 統合ターミナルは ConPTY + libvterm + ImGui 自前描画の埋め込みパネルにする

- 状態: 実装済み (2026-11-19、M106a〜e。Windows での実機確認は未実施の項目あり: [docs/test_checklists.md](../test_checklists.md) の M106)
- 出所: [plans\m106-integrated-terminal\spec.md](../../plans/m106-integrated-terminal/spec.md) (2. が疑った点と結論、4. が仕様、7. が未決とリスク)

## 背景

エディタで作業しながら、PowerShell・git・ビルド・Claude Code CLI などの対話型 CLI を、エディタの外へ切り替えずに使いたい。
さらに (1) 統合ターミナル内の AI CLI の変更を AI 変更追跡 ([ADR-039](ADR-039-ai-change-tracking.md)) で区別でき、
(2) Visual Studio 開発者環境 (`cl`・`msbuild`) とエンジン用の環境変数・`tools\` の PATH が最初から通っていることが、外部ターミナルより優れる点になる。
エンジンにあった子プロセス起動 (`ChildProcess`) はログファイルへ出力するだけで、対話はできない。

## 決定

1. **子プロセスは ConPTY (疑似コンソール) で動かし、出力の VT シーケンスを libvterm 0.3.3 (MIT、`external/libvterm/` に同梱) で解釈し、
   セルを ImGui の描画リストで自前に描く。** 窓は Editor 層のドック窓 (*Window > Terminal*) で、1 パネル 1 シェル。
   `CreatePseudoConsole` 系は kernel32 から実行時に解決し、無い OS (Windows 10 1809 未満) では窓に「非対応」を出して残りは動かす。
2. **Win32 非依存のコアと Win32 の口を分ける。** 画面モデル (`TerminalScreen`)・キー→VT (`TerminalInput`)・シェル解決・環境ブロック・窓の純関数 (`TerminalLayout`)・
   VS 環境の解析と差分 (`TerminalVsEnv`) は Linux の g++ で試験できる。`Windows.h` を include するのは `TerminalConPtyHost.cpp` と `TerminalVsEnvWin32.cpp` だけ。
   libvterm と画面モデルは UI スレッドだけが触る。読み取りスレッドはバイトをキューへ積むだけ。
3. **Editor 層に閉じ込める。** `src\Editor\Tools\Terminal\` を Engine / Runtime / GameLogic / Shared / Server から include させない (`check_rules` 規則 21)。
   シミュレーション・リプレイ・SimSnapshot・ABI・シーン形式には触れない (決定性の契約外)。
4. **VS 開発者環境は一度だけワーカースレッドで読み、完了を待ってからシェルを起動する (2026-11-19 ユーザー確定)。**
   `cmd /s /c "chcp 65001>nul && "<VsDevCmd.bat>" -arch=x64 -no_logo && echo <目印> && set"` を非表示で実行し、目印より後の `set` 出力を解析して、
   親環境 (エディタ自身) との差分だけをキャッシュする。環境全体ではなく差分を持つのは、エディタ起動後に親環境が変わっても (別窓で変えた変数)、子に古い値を押し付けないため。
   読み込み中は窓に「読み込み中」を出し、ツールバーに状態 (読み込み中 / 読み込み済み / 利用できません / 無効) を出す。
   30 秒で時間切れにして素の環境で起動し、警告行を出す。未導入・失敗・時間切れの結果はやり直さない (起動済みのシェルと表示が食い違うのを避ける。再試行はエディタの再起動)。
   設定 `terminalLoadVsDevEnv=false` なら読み込まない。vswhere の探索は `VisualStudioLauncher` の匿名 namespace から名前付き関数 (`RunVswhere` / `FindLatestVsInstallPath`) へ昇格し、既存の `FindLatestDevenv` は挙動を変えない。
5. **エンジンの環境変数。** `MYE_PROJECT_ROOT`・`MYE_TERMINAL_ID` (`<エディタの PID>-<エディタの起動時刻 (UNIX 秒)>-<シェル起動の連番>`)・`TERM_PROGRAM=MyEngine`・
   `COLORTERM=truecolor` (親に無いときだけ)、PATH の先頭に `<root>\tools`。`MYE_*` と `TERM_PROGRAM` は親の値があっても自分の値に置き換える
   (別のターミナルや VS Code からエディタを起動した場合に、親の値が子に残って「どのターミナルの子か」を誤らせるため)。
6. **スクロールバックは「最下部を見ているときだけ新規出力に追従」する (Windows Terminal と同じ)。** 履歴を見ている間は、出力でスクロールバックが増えた分だけ
   表示位置を過去へずらして同じ内容を見続け、キー入力・貼付で最下部へ戻す。判定は `FollowTerminalViewOnOutput` (純関数、Fast スイート `terminalwindow` で回帰)。
   当初の仕様「新規出力で最下部へ戻す」は、Claude Code CLI のスピナー再描画のたびに履歴の閲覧が中断されるので改めた。
7. **貼付本文は常に消毒する。** bracketed paste (DECSET 2004) の本文に `ESC[201~` があると貼付が途中で閉じ、後ろがキー入力として実行されるので、有効・無効によらず `SanitizeTerminalPaste` (純関数) が HT と CR 以外の C0 (ESC を含む)・DEL・UTF-8 の C1 (U+0080-U+009F) を取り除く (置換文字にはしない)。改行は CR へ揃え、日本語・絵文字は残す。

## 却下した案

- **外部ターミナル (`wt.exe` など) の起動。** ドッキング・AI 追跡の紐付け・環境注入が効かない。
- **VT 解釈の自作。** Claude Code CLI (Ink 製) はカーソル移動・消去・truecolor・bracketed paste・代替画面を使う。網羅と保守のコストが大きい。
- **Windows Terminal の TerminalCore の流用。** 依存 (C++/WinRT) が巨大で、同梱方針に合わない。
- **libtsm。** LGPL 系が混ざり、Windows での実績が薄い。libvterm は MIT で C99 単体、キー→VT 変換 (`vterm_keyboard_*`) と貼付の包みまで持つ。
- **VS 環境を非同期に後付けする (シェルを先に起動し、読み込み完了後に差し込む)。** 起動済みのシェルの環境は外から変えられない。
  `cl` が通るかどうかが起動のタイミングで揺れるより、完了を待つ方が予測しやすい (ユーザー確定)。
- **VS 環境の全体をキャッシュする。** 親環境が後で変わったときに古い値を子へ押し付ける (決定 4)。

## 既知の制限

- DECSET 2026 (同期出力) は libvterm 0.3.3 が無視する。読み取りキューを 1 フレーム分まとめて投入してから描くことで、ちらつきを抑える。
- マウスレポーティング・OSC 8 のクリック・OSC 133・インライン IME 変換 (確定文字だけを送る)・vim などの全画面エディタの保証は対象外。
- スクロールバックが上限に達して最古の行が捨てられている間は、履歴の閲覧位置・選択が 1 行ずつ上へずれる (増分が 0 に見えるため。`FollowTerminalViewOnOutput` の注記)。
- VS 環境の時間切れ後に取得が遅れて成功しても採用しない。ワーカースレッドが固まったままエディタを終了するときは、2 秒待ってスレッドを切り離す。
- `chcp 65001` でパイプへの `set` 出力が UTF-8 になる前提は Windows で未確認。`MYE_TERMINAL_VSENV_BEGIN` の目印があるので、前置きの出力は読まれない。
