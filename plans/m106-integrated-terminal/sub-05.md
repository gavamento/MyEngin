# sub-05: VS 開発者環境とエンジン環境変数

- 依存: sub-04
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 2・4.2。
- `/home/user/MyEngin/src/Editor/Tools/VisualStudioLauncher.cpp:238-290` の匿名 namespace の vswhere 探索を名前付き関数に昇格 (既存呼び出しの挙動は不変)。
- VS 開発者環境: `cmd /s /c ""<VsDevCmd.bat>" -arch=x64 -no_logo && set"` を非表示で実行し出力を解析、親環境との差分をキャッシュ (エディタ起動中1回。読み込み完了まで窓に「読み込み中」を表示して待ち、完了後にシェルを起動 [2026-11-19 ユーザー確定]。タイムアウト 30 秒で素の環境にフォールバック、警告表示)。出力解析は Win32 非依存の純関数にして g++ テスト。
- エンジン環境変数: MYE_PROJECT_ROOT / MYE_TERMINAL_ID / TERM_PROGRAM / COLORTERM / PATH 前置 `<root>\tools` を sub-02 の環境組立に渡す。
- 設定 terminalLoadVsDevEnv=false で読み込まない。
- (sub-04 round 1 から移管) ツールバーに VS 環境の状態 (読み込み中 / 読み込み済み / 失敗・未導入 / 無効) を Tr() 両言語で表示。TerminalWindow::StartSession (/home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.cpp) の TerminalLaunchSpec.environment に環境組立の結果を渡す。MYE_TERMINAL_ID の採番もここ。
- (sub-04 round 1 で仕様変更) スクロールバック追従: 履歴表示中は新規出力で最下部へ戻さず、キー入力・貼付で戻す (spec 4.1)。判定は TerminalLayout 側の純関数にして terminalwindow スイートへ回帰を追加。
- (sub-04 round 1) ADR を /home/user/MyEngin/docs/adr/ に追加: 統合ターミナルの方式 (ConPTY + libvterm + ImGui 自前描画、却下案: 外部ターミナル起動・VT 自作・Windows Terminal 流用・libtsm)。既存 ADR の番号と書式に合わせる。

## やらないこと (このサブでは)
- AI 追跡側の読み取り。

## 受け入れ条件 (このサブ)
1. spec 5-8。spec 5-7 の色 (COLORTERM) の確認もこのサブの後に Windows で行う。
5. Linux: 追従判定の純関数テストが terminalwindow に入り成功。ADR が存在し engine_spec §9.4 から参照される。
2. Linux: `set` 出力解析 (= を含む値、空値、`=C:` 系の隠し変数、Unicode) と差分計算の g++ テスト成功。
3. VisualStudioLauncher の既存挙動不変 (差分レビューで関数移動のみであること)。
4. Windows (要確認): `where cl`、`msbuild -version`、`$env:MYE_TERMINAL_ID` が窓内で通る。VS 未導入擬似 (vswhere パスを壊す) で警告+起動。

## 検証コマンド
- Linux: g++。Windows: ビルド、手動。

## 実装メモ (coder が追記)
SELF_EVAL の写し。round ごとに積む。

SELF_EVAL: sub-05 (round 1)
実装:
  - /home/user/MyEngin/src/Editor/Tools/VisualStudioLauncher.cpp / .h:RunVswhere(property)・FindLatestVsInstallPath — 匿名 namespace の vswhere 探索を名前付き関数へ昇格 (property 引数化)。FindLatestDevenv は RunVswhere(L"productPath") を呼ぶだけで、結果・キャッシュ・既存呼び出しは不変
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.h / .cpp:ParseTerminalSetOutput・ComputeTerminalEnvironmentDiff・ApplyTerminalEnvironmentDiff・FormatTerminalId・BuildTerminalLaunchEnvironment・TerminalVsEnvLoader — Win32 非依存の `set` 解析・差分・環境組立・非同期ローダー (状態機械)
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnvWin32.cpp:FetchTerminalVsDevEnvironment・GetTerminalParentEnvironment・GetTerminalEditorIdentity — vswhere → VsDevCmd.bat を非表示 cmd で実行 (Job で孫ごと cancel 可能) → 差分。親環境の列挙、エディタの PID・起動時刻
  - /home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.h / .cpp:UpdateVsEnvGate・VsEnvShownState・StartSession・DrawToolbar・DrawStatusLine・Pump・Shutdown — VS 読み込み待ちの起動保留・ツールバーの状態表示・警告行・TerminalLaunchSpec.environment の組立 (MYE_TERMINAL_ID 採番はここ)・Pump のスクロール追従
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayout.h / .cpp:FollowTerminalViewOnOutput — スクロール追従の純関数
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayoutSelfTest.cpp / .h:TestFollowOutput — 追従の回帰 (terminalwindow)。途中成果で TestScroll の末尾の 1 check が TestFollowOutput へ紛れていたのを元へ戻した
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnvSelfTest.cpp / .h・/home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp — 新スイート terminalvsenv (Fast)
  - /home/user/MyEngin/src/Engine/Core/Localization/LocalizationTable.inl — Term_VsEnv* 7 件追加 (両言語)、Term_SettingsApplyOnRestart の文言に VS 環境を追記
  - /home/user/MyEngin/build/Editor.vcxproj・Editor.vcxproj.filters — 新規 5 ファイルを追記 (BEGIN/END FILES 区画を gen_project_files.ps1 の規則で手更新)
  - /home/user/MyEngin/docs/adr/ADR-041-integrated-terminal.md — 新規 ADR (方式・却下案・VS 環境・環境変数・追従規則・制限)
  - /home/user/MyEngin/engine_spec.md §9.4 (ADR-041 参照・環境の節・追従規則・設定・検証フック)、/home/user/MyEngin/docs/test_checklists.md (M106e の Windows 確認項目を追加、追従の記述を更新)
技術解説:
  - 技術: マーカー付き `set` 出力の解析 + 差分キャッシュ — cmd /s /c "chcp 65001>nul && "<VsDevCmd.bat>" -arch=x64 -no_logo && echo MYE_TERMINAL_VSENV_BEGIN && set" の標準出力を読み、目印の行より後だけを NAME=VALUE として解析する (バナー・chcp の出力・警告は目印より前なので混ざらない。VsDevCmd が失敗すれば && で目印が出ず、解析が false になり Failed)。名前が '=' で始まる隠し変数 ("=C:") は 2 文字目以降の最初の '=' で区切る。親環境 (GetEnvironmentStringsW) と大文字小文字無視で比べ、親に無い・値が違うものだけを差分として持つ。環境全体でなく差分にしたのは、エディタ起動後に親環境が変わっても古い値を子へ押し付けないため。却下: VS 環境全体のキャッシュ、シェル起動後の後付け (起動済みの環境は外から変えられない)
  - 技術: 状態機械 + ワーカースレッド + shared_ptr 共有状態 (TerminalVsEnvLoader) — Idle → Loading → Ready/NotInstalled/Failed/TimedOut。UI スレッドの Poll が期限 (steady_clock) を見て、完了していれば完了を優先して取り込み、未完了で期限なら cancel(atomic<bool>) を立てて TimedOut にする。時間切れ後に遅れて成功しても採用しない (起動済みシェルと表示が食い違うのを防ぐ)。スレッドが触るのは shared_ptr<Shared> だけなので、デストラクタが shutdownWait (2 秒) 待っても戻らないスレッドを detach しても安全。実処理は fetch (std::function) として差し込むので Linux で偽 fetch を使い TSan で試験できる。却下: std::async/future (cancel と切り離しを表現しにくい)、UI スレッドでの同期実行 (vswhere + VsDevCmd は数秒かかり UI が止まる)
  - 技術: Job Object (KILL_ON_JOB_CLOSE) + PeekNamedPipe ポーリング — ReadFile のブロッキングでは cancel を見られないので、PeekNamedPipe で読めるだけ読み、無ければ WaitForSingleObject(process, 15ms) で終了と cancel を交互に見る。VsDevCmd が起こす孫 (powershell・git) まで確実に止めるため、CREATE_SUSPENDED で作って Job へ入れてから ResumeThread する (入れる前に孫が生まれる競合を避ける)。出力は 8MB で打ち切る安全弁付き
  - 技術: 最下部追従オフセット補正 (FollowTerminalViewOnOutput) — viewOffset_ は「スクロールバックを何行戻って見ているか」。出力でスクロールバックが after-before 行増えると、同じ内容は画面上で同じだけ上へずれるので offset にその増分を足せば同じ内容を見続けられる。offset==0 (最下部) はそのまま 0 = 追従。キー入力と貼付は既存の HandleKeyboard / PasteClipboard が 0 に戻す。Windows Terminal と同じ挙動 (spec 4.1)。スクロールバックが上限で古い行が捨てられる間は増分が 0 に見え、内容が 1 行ずつずれる (選択のずれと同じ既知の制約。ADR-041・関数コメントに明記)
  - コード: TerminalVsEnv.cpp:BuildTerminalLaunchEnvironment — 入力 (親環境・VS 差分・projectRoot・terminalId) → 出力 (環境ブロック順に整列済みの vector<TerminalEnvVar>)。ApplyTerminalEnvironmentDiff で親に差分を重ね (同名は親の綴りのまま値を差し替え)、MYE_PROJECT_ROOT / MYE_TERMINAL_ID / TERM_PROGRAM を先に除去してから BuildTerminalEnvironment (sub-02) に additions として渡す (BuildTerminalEnvironment は親にある同名を上書きしない仕様なので、エンジンが決める 3 変数だけ先に外す)。COLORTERM は外さないので親にあればそのまま。PATH は pathPrefix = "<root>\tools" を前置 (root に ';' があれば前置しない、"C:\" は区切りを残す)。呼び出し元は TerminalWindow::StartSession のみ。
    spec.environment = BuildTerminalLaunchEnvironment(GetTerminalParentEnvironment(), vsDiff, host.projectRoot,
                                                      FormatTerminalId(editorPid, editorStartUnixSeconds, shellSerial_));
  - コード: TerminalWindow.cpp:UpdateVsEnvGate / DrawSurface — OnImGui が表示されたフレームの先頭で UpdateVsEnvGate を呼ぶ。未起動 (session_ なし・startAttempted_ なし) で設定が有効なときだけ、初回に loader を作り Start、毎フレーム Poll して Loading の間 vsEnvPending_ = true。DrawSurface は `!session_ && !startAttempted_ && !vsEnvPending_` のときだけ StartSession を呼ぶ。状態行が保留中の文言を出す。決まった結果は loader が持ち続けるので、再起動 (RequestRestart → startAttempted_=false) でも待たずに StartSession へ進む (読み込みは再実行しない)。StartSession は起動時点の状態を sessionVsEnv_ (Disabled/Loaded/Unavailable) に固定し、ツールバーと警告行はそれを見る (後から loader が変わっても表示を動かさない)。呼び出し順: Pump (毎フレーム) → OnImGui → UpdateVsEnvGate → DrawToolbar → DrawStatusLine → DrawSurface。
  - コード: TerminalWindow.cpp:Pump — 追従の配線。
    const int scrollbackBefore = session_->Screen().ScrollbackCount();
    session_->Pump();
    const TerminalScreen& screen = session_->Screen();
    viewOffset_ = FollowTerminalViewOnOutput(viewOffset_, scrollbackBefore, screen.ScrollbackCount(), screen.IsAltScreen());
    以前は Pump が 1 バイトでも進めたら viewOffset_ = 0 にしていた (履歴を見ていても新規出力で最下部へ戻る)。窓が閉じていても Pump は呼ばれ、viewOffset_ は窓の状態として保たれる。
  - コード: TerminalVsEnvWin32.cpp:FetchTerminalVsDevEnvironment — vswhere で installationPath (FindLatestVsInstallPath) → <install>\Common7\Tools\VsDevCmd.bat の存在確認 (無ければ NotInstalled) → パイプ + Job + CREATE_SUSPENDED|CREATE_NO_WINDOW で cmd を起動 → ポーリングで出力を集める → ParseTerminalSetOutput (目印なしは Failed) → ComputeTerminalEnvironmentDiff(GetTerminalParentEnvironment(), vsEnv)。ワーカースレッドで走る。UI スレッドの Poll が TimedOut にして cancel を立てると、ここのループが TerminateJobObject して戻る。
  - 前提・落とし穴: (1) Windows 実機で未実行の経路: FetchTerminalVsDevEnvironment (vswhere → VsDevCmd.bat → set)、chcp 65001 でパイプへの `set` が UTF-8 になるという前提 (日本語ユーザー名の化けは未確認。化けても環境変数値が mojibake になるだけで起動は妨げない)、Job への割当 (入れ子 Job は Windows 8 以降で可)。(2) 親環境との差分は fetch 完了時点の親環境で取るが、子へ渡す親環境は StartSession 時点で読み直す (その間に親が変わっても差分は値の上書きだけなので整合する)。(3) 時間切れ・失敗・未導入の結果は、設定の切替や再起動ではやり直さない (エディタ再起動で再試行)。(4) loader の Poll/Start/State/Diff は UI スレッド専用、スレッドと共有するのは shared_ptr<Shared> だけ。デストラクタ (= TerminalWindow::Shutdown の vsLoader_.reset()) は最悪 2 秒待つ。(5) ローダーは窓が最初に表示されたときに始まる (エディタ起動時ではない)。窓を一度も開かなければ VS 環境は読まれない。(6) MYE_* / TERM_PROGRAM を親の値から置き換えるのは spec の「既存変数は上書きしない」からの意図的な逸脱 (下)。(7) RunVswhere は同期で ReadFile が EOF まで戻らない (最大でも vswhere の実行時間)。UI スレッドから呼ばない。
仕様との差分:
  - [逸脱] spec 4.2/2 の「既存変数は上書きしない」に対し、MYE_PROJECT_ROOT / MYE_TERMINAL_ID / TERM_PROGRAM は親の値があっても自分の値で置き換える (COLORTERM は親にあれば残す = 仕様どおり) — エディタを別の MyEngine ターミナルや VS Code から起動すると親の TERM_PROGRAM=vscode や古い MYE_TERMINAL_ID が子に残り、「どのターミナルの子か」(sub-06 の AI 追跡の識別) を誤らせるため。前の coder の判断を踏襲。planner の追認が必要
  - [追加] VS 環境の読み込みは「エディタ起動時」でなく「ターミナル窓が最初に表示されたとき」に始める — 窓は遅延起動 (spec 4.1) で、使わない人に cmd + VsDevCmd のコストを払わせないため。spec 2 の「エディタ起動中 1 回」は満たす
  - [追加] cmd に `chcp 65001>nul &&` を前置、`echo <目印> &&` を挟む、Job Object で孫ごと終了 — パイプへの `set` の文字コードを揃える・バナーを除く・cancel で孫が残らないため。Windows 未確認
  - [追加] RunVswhere を匿名 namespace の外へ出し property を引数にした (受け入れ条件 3 の「関数移動のみ」より少し多い: 引数追加と FindLatestVsInstallPath 追加。FindLatestDevenv の挙動は不変)
  - [追加] Tr 文言 7 件 (VS 環境ラベル・4 状態・保留・警告) と Term_SettingsApplyOnRestart の文言変更 (VS 環境の設定が再起動で反映されると明記)
  - [追加] engine_spec §9.4 に環境の節を追加、test_checklists.md の M106e 項目、ADR は番号 041 (既存の最新が 040)
  - [未実装] 受け入れ条件 4 の Windows 手動 (`where cl` / `msbuild -version` / `$env:MYE_TERMINAL_ID`、VS 未導入擬似) と spec 5-7 の COLORTERM の色確認 — Linux コンテナでは実行不可 (test_checklists.md に項目化)
検証:
  - g++ -std=c++20 -Wall -Wextra -fsanitize=address,undefined で terminalvsenv + terminalwindow のドライバ実走 → TerminalVsEnv self test PASS (0 failure)、TerminalLayout self test PASS (0 failure)。警告なし
  - 同ドライバを -fsanitize=thread で実走 → 両スイート PASS、TSan 報告なし (ローダーのスレッド・時間切れ・切り離しを含む)
  - 変異試験: FollowTerminalViewOnOutput の補正を `return 0` に壊したコピーで実走 → follow 系 6 件が FAIL (回帰テストが実際に効く)
  - 既存スイート (terminalcore + terminalsession + terminalwindow) を ASan/UBSan で再実走 → 全 PASS (回帰なし)
  - 実 ImGui (バックエンド無し) + 偽ホスト + 偽 fetch で TerminalWindow をヘッドレス実走 (scratchpad/sub05/win/win_driver.cpp、ASan/UBSan と TSan の両方) → 16 項目 PASS: 読み込み中は起動保留で fetch は 1 回だけ、完了で起動、環境 = 親 + VS 差分 + MYE_* / TERM_PROGRAM / COLORTERM / Path 前置、再起動で連番 2 かつ再読込なし、無効設定は即起動で loader 無し、未導入は素の環境 + Unavailable、読み込み中の Shutdown が 500ms 未満、履歴閲覧中は新規出力でも動かず同じ内容を見続ける・スピナー再描画で動かない・キー入力と貼付で最下部へ戻る
  - x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only (Windows.h を shim) で TerminalVsEnvWin32.cpp と VisualStudioLauncher.cpp (変更後・変更前) → いずれも出力なし (構文・型は通る。MSVC では未確認)
  - LocalizationTable.inl を規則 10 相当の python (check_loc.py) で検査 → entries 2782 errors 0
  - tools\check_rules.ps1 / MSVC ビルド / Editor.exe --selftest / --selftest-full / replay_verify.bat / VS・ConPTY・実画面の確認 → 未実行 (Linux コンテナで不可)。規則 10 は目視で `"%s", Tr(x)` のみ使用、規則 21 は Editor 層内の include のみ
自己採点 (1-5):
  仕様適合: 4 — sub の「やること」(昇格・差分取得・環境変数・設定・ツールバー・追従・ADR) は全て実装。MYE_* / TERM_PROGRAM の置換という spec からの逸脱と、Windows 手動項目 (受入 1・4) が未実行のため満点にしない
  正しさ: 3 — 純関数・状態機械・窓配線はサニタイザ付きで実走し変異試験で効きを確認したが、実際に VS を呼ぶ Win32 経路 (vswhere → VsDevCmd → パイプ → Job) は構文チェックのみで一度も走っていない。chcp 65001 の前提も未確認
  コード品質: 4 — 責務を純関数 / ローダー / Win32 に分け、既存の Win32 非依存コアの規則と命名・コメント形式に合わせた。VS 起動待ちの状態が窓に 4 つのフラグ (vsEnvPending_・sessionVsEnv_・vsLoader_・shellSerial_) として載る点はやや重い
  テスト: 4 — terminalvsenv (解析・差分・環境・ローダー) と追従の回帰を追加し、窓のヘッドレス駆動 (一時物、コミットしない) で配線を確認。ただし窓の配線の自動回帰は SelfTest スイートに入っておらず (ImGui 窓は Editor.exe 内でしか走らせられない)、Win32 経路のテストは無い
不安・質問:
  - 逸脱 1 (MYE_* / TERM_PROGRAM の置換) を planner が追認するか。追認なら spec 4.2 の「既存変数は上書きしない」に「ただし MYE_* と TERM_PROGRAM は置換」を足してほしい
  - `chcp 65001` の前置は、Windows の cmd がパイプ出力を UTF-8 にする挙動 (既知) に依存するが未実機確認。化けた場合の次善策 (GetOEMCP で変換) を sub-06 以降へ回すか
  - 読み込みの開始を窓の初回表示にしたこと (差分の追加) の可否
触ったファイル:
  /home/user/MyEngin/src/Editor/Tools/VisualStudioLauncher.h
  /home/user/MyEngin/src/Editor/Tools/VisualStudioLauncher.cpp
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.h
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnv.cpp
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnvWin32.cpp
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnvSelfTest.h
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalVsEnvSelfTest.cpp
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayout.h
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayout.cpp
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayoutSelfTest.h
  /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayoutSelfTest.cpp
  /home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.h
  /home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.cpp
  /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp
  /home/user/MyEngin/src/Engine/Core/Localization/LocalizationTable.inl
  /home/user/MyEngin/build/Editor.vcxproj
  /home/user/MyEngin/build/Editor.vcxproj.filters
  /home/user/MyEngin/docs/adr/ADR-041-integrated-terminal.md
  /home/user/MyEngin/docs/test_checklists.md
  /home/user/MyEngin/engine_spec.md
  /home/user/MyEngin/plans/m106-integrated-terminal/sub-05.md (実装メモのみ)
申し送り:
  - sub-06: MYE_TERMINAL_ID は "<エディタ PID>-<起動時刻 UNIX 秒>-<シェル起動連番>"。TerminalWindow::StartSession で採番し環境へ入れる。フック側は環境変数 MYE_TERMINAL_ID を読んで hostTerminal に渡す
  - Windows で最初に確認する順: (1) ビルド (TerminalVsEnvWin32.cpp は MSVC 未確認)、(2) terminalvsenv スイート、(3) test_checklists.md の M106e (特に chcp 65001 と時間切れ・未導入)
  - TerminalWindow の VS 環境ゲートはヘッドレス駆動 (scratchpad/sub05/win/win_driver.cpp) でのみ確認。SelfTest 化するなら ImGui コンテキストと偽ホストが要る
  - 窓の既存の viewOffset_ 補正は、スクロールバック上限で古い行が捨てられている間は 1 行ずつずれる (ADR-041 既知の制限)
- round 1: VERDICT OK (planner)。SELF_EVAL は scratchpad 原文と一致。git status の触ったファイルと一致。planner が build.sh を ASan/UBSan で再実走し terminalvsenv / terminallayout とも PASS。VisualStudioLauncher の差分は匿名 namespace 外への移動 + property 引数化 + FindLatestVsInstallPath 追加のみで FindLatestDevenv は不変 (受入 3 は追加の範囲で満たすと判定)。逸脱 (MYE_* / TERM_PROGRAM 置換) は仕様側の誤りとして追認し spec 4.2 を修正。追加 5 件はすべて追認 (読み込み開始を窓初回表示に・chcp/目印/Job・RunVswhere 公開・Tr 7 件・engine_spec/checklist/ADR-041)。未実装 (受入 4 と 5-7 の Windows 手動) は環境制約として容認、Windows 確認待ち。chcp 65001 化け時の GetOEMCP 変換は後回し (Windows 確認で化けたら別コミット)。nit: 採点「正しさ 3」は妥当、Win32 経路は MSVC 未ビルド。
