# sub-03: ConPTY ホストとセッション

- 依存: sub-02
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 4.1・4.4。
- ConPTY ホスト (Win32): kernel32 から CreatePseudoConsole / ResizePseudoConsole / ClosePseudoConsole を GetProcAddress で解決 (失敗 = 非対応状態)。パイプ作成、STARTUPINFOEX + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE、CREATE_SUSPENDED → Job (KILL_ON_JOB_CLOSE) 割当 → ResumeThread (手本 `/home/user/MyEngin/src/Engine/Engine/Online/HostServerProcess.cpp:85-124`)、CREATE_UNICODE_ENVIRONMENT で sub-02 の環境ブロックを渡す。
- 読み取りスレッド: 出力パイプ → ロック付きキュー。書き込みは UI スレッドから同期 (小さい) で可。
- TerminalSession: 画面モデル (sub-02) とホストを束ね、Pump(フレームごとの投入上限付き)、SendInput、Resize、Restart、終了検出 (終了コード)、libvterm 応答バイトの返送、破棄順序 (ClosePseudoConsole → join → ハンドル解放)。所有者は窓 (sub-04)、エディタ終了時に確実に破棄。
- SelfTest (Full): `cmd.exe /c echo <マーカー>` を ConPTY で起動しマーカーが画面に出て終了コード 0、Resize 呼び出し成功、ConPTY 解決失敗の擬似 (関数ポインタ null 注入) でエラー状態になる。

## やらないこと (このサブでは)
- UI、VS 環境。

## 触る場所 (planner の見立て)
- `/home/user/MyEngin/src/Editor/Tools/Terminal/` に TerminalHost (Win32), TerminalSession, TerminalSessionSelfTest。QuoteCommandLineArg `/home/user/MyEngin/src/Editor/ChangeTracking/TrackLaunch.h:21`、WideToUtf8/Utf8ToWide `/home/user/MyEngin/src/Engine/Platform/PathUtil.h:31-32` を再利用。

## 受け入れ条件 (このサブ)
1. Win32 依存はホストの .cpp に閉じ、TerminalSession のヘッダは Win32 型を露出しない (grep)。
2. Linux: Win32 部を除くセッションロジック (投入上限・応答返送・状態遷移) を偽ホストで g++ テストして成功。
3. vcxproj 整合。
4. Windows (要確認): SelfTest Full の新スイート成功、Debug/Release ビルド、エディタ終了後にタスクマネージャで子 (conhost/pwsh) が残らない。

## 検証コマンド
- Linux: g++ 偽ホストテスト、grep、XML 検査。
- Windows: `MYE_SELFTEST_ONLY=<名>` `Editor.exe --selftest-full`。

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-03 (round 1)
実装:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalByteQueue.h/.cpp:TerminalByteQueue — 読み取りスレッド→UI のロック付きバイトキュー。上限 (Bounded) で Push が待つ背圧、Unbounded / Discard へのモード切替で待ちを解く、PopUpTo で最大 n バイトを取り出す
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalHost.h:TerminalHost / TerminalLaunchSpec / CreateConPtyTerminalHost / FindTerminalExecutable — 子プロセスの口 (純粋仮想)。Win32 型を含まない
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalConPtyHost.cpp:ConPtyTerminalHost — ConPTY 実装。kernel32 から 3 関数を GetProcAddress、パイプ2本、STARTUPINFOEX + PSEUDOCONSOLE 属性、CREATE_SUSPENDED → Job(KILL_ON_JOB_CLOSE) → ResumeThread、CREATE_UNICODE_ENVIRONMENT、読み取りスレッド、Close の順序。FindTerminalExecutable (SearchPathW)
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSession.h/.cpp:TerminalSession — 画面 (sub-02) とホストを束ねる。Start / Restart / Pump(既定 256KB) / SendKey・SendChar・SendPaste / Resize / 状態 (NotStarted・Running・Exited・Failed・Unsupported) / 終了コード / 破棄時 Close
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSessionSelfTest.h/.cpp — Fast "terminalsession" (キュー + 偽ホストのセッション、49 項目) と Full "terminalconpty" (実 ConPTY)
  - /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp — 2 スイート登録
  - /home/user/MyEngin/build/Editor.vcxproj, Editor.vcxproj.filters — BEGIN/END FILES 区画と Tools\Terminal フィルタへ 8 ファイル追加 (gen_project_files.ps1 の並び = Rel の昇順)
技術解説:
  - 技術: ConPTY (Pseudo Console API) — CreatePseudoConsole(size, 入力読み端, 出力書き端) で conhost を疑似端末として立て、PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE を付けた STARTUPINFOEXW で子を起動すると、子の標準入出力がその疑似端末に繋がり、こちらは VT バイト列を読み書きするだけになる (Microsoft Docs「Creating a Pseudoconsole session」の手順)。3 関数は GetProcAddress で実行時解決: 静的リンクだと 1809 未満の Windows で Editor 自体が起動しなくなるため (spec 2「障害の局所化」)。HPCON と属性値はヘッダの NTDDI ガードに依存しないよう自前定義 (void* と 0x00020016)。MinGW ヘッダで既定 _WIN32_WINNT ではマクロが未定義になること、NTDDI を上げれば PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE==0x20016 になることを static_assert で確認済み。
  - 技術: Job Object (JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) + CREATE_SUSPENDED — 手本 HostServerProcess.cpp:85-124 と同型。停止状態で作ってジョブに入れてから ResumeThread するので、子が先に孫 (git, claude, node) を作ってジョブの外へ逃がすことがない。ジョブのハンドルを閉じると (Close 時、または Editor が落ちて OS が閉じたとき) 子孫ごと終わる。
  - 技術: 背圧付き有界キュー (mutex + condition_variable) — 読み取りスレッドは 64KB ずつ ReadFile して Push、UI は Pump で PopUpTo(256KB)。キュー上限 4MB を超えると Push が待ち、出力パイプが詰まり、子の書き込みが待つ = メモリ無制限に溜めない。読み済み位置 m_head で持ち、半分を超えたときだけ前を詰める (毎回 erase すると O(n^2))。
  - コード: TerminalConPtyHost.cpp:Close — 停止順序の要。ClosePseudoConsole は conhost が出力を吐き切るまで返らないことがあるので、先にキューを「待たない」モードにして読み取りスレッドを走らせ続ける:
    (抜粋)
    m_queue.SetMode(keepPendingOutput ? TerminalByteQueue::Mode::Unbounded : TerminalByteQueue::Mode::Discard);
    if (m_console != nullptr) {
        m_api.close(m_console);
        m_console = nullptr;
    }
    // conhost が終わると出力パイプの書き込み端が全部閉じ、ReadFile が失敗して ReadLoop が抜ける
    if (m_reader.joinable()) {
        m_reader.join();
    }
    ReleaseHandles();   // inputWrite, outputRead, process, job (ここで子孫を KILL)

    Start では CreatePseudoConsole 直後にこちら側の inputRead / outputWrite を閉じる。outputWrite を持ち続けると conhost が終わっても EOF にならず join が永久に待つ。
  - コード: TerminalSession::Pump — 入力: maxBytes。処理: ① host->ReadOutput で最大 maxBytes を m_feedScratch へ → screen->Feed ② screen->TakeOutput (DA 応答など) を Running なら host->Write ③ Running かつ host->PollExit なら終了コードを保存し Exited、host->Close(true) で最後の出力を残す (cmd /c echo の出力は子の終了後に conhost から届き切ることがあるため)。出力: 投入バイト数。Exited / Failed でも ①は続けるので、残りは次フレーム以降に画面へ出る。呼び出し元は sub-04 の窓で、毎フレーム (窓が隠れていても) 呼ぶ前提。
    (抜粋)
    m_feedScratch.clear();
    const size_t fed = m_host->ReadOutput(m_feedScratch, maxBytes);
    m_screen->Feed(m_feedScratch);
    FlushScreenOutput();
    uint32_t code = 0;
    if (m_state == TerminalSessionState::Running && m_host->PollExit(code)) {
        m_exitCode = code;
        m_state = TerminalSessionState::Exited;
        m_host->Close(true);
    }

  - コード: TerminalSession::Restart — host->Close(false) (旧出力は捨てる) → 同じ大きさで TerminalScreen を作り直す → 保存した TerminalLaunchSpec で Start。Unsupported と未起動では false。Resize は screen->Resize が変化を返したときだけ、Running なら host->Resize (最小 20x5 へ切り上げた値)。状態はすべて TerminalSession のメンバ (UI スレッド専用)、子側の状態は host 内部。
  - コード: ConPtyTerminalHost::LaunchChild — コマンドラインは exe を QuoteCommandLineArg、引数は QuoteArgIfNeeded (空白・タブ・'"' を含むか空のときだけ QuoteCommandLineArg)。環境は FormatTerminalEnvironmentBlock → Utf8ToWide (長さ指定なので途中の NUL を保つ) → CREATE_UNICODE_ENVIRONMENT。environment が空なら nullptr (親を継ぐ)。bInheritHandles = FALSE。
  - 選ばなかった案: (a) 読み取りスレッドから直接 TerminalScreen::Feed — 申し送りどおり画面はスレッド安全でないので不可。(b) 無制限キュー — UI が止まっている間に大量出力でメモリが膨らむ。(c) overlapped I/O で UI スレッドからポーリング — 読み取りスレッド 1 本の方が単純で、spec 4.1 が専用スレッドを指定。(d) ホストの抽象を作らず Session に Win32 を直書き — Linux で状態遷移をテストできず、受け入れ条件 1 (ヘッダに Win32 型を出さない) も満たしにくい。抽象は TrackIo と同じ「試験で偽物に差し替える口」の目的に限定。(e) 終了検出時に Close せず待つ — ConPTY は子の終了後も conhost が生きていて出力パイプが EOF にならないため、閉じないと最後の出力が届かない場合がある。
  - 前提・落とし穴: スレッド — TerminalSession / TerminalScreen は UI スレッド専用。読み取りスレッドが触るのは m_outputRead と m_queue だけ。Close は UI スレッドで ClosePseudoConsole + join を同期実行するので数十 ms 程度止まりうる (エディタ終了・再起動・子の終了時のみ)。Write は同期 WriteFile: 子が入力を読まない状態で巨大な貼付をすると UI が待つ (sub-04 で貼付サイズの扱いを決めるなら注意)。寿命 — 破棄順は ~TerminalSession → host->Close(false) → join → ハンドル解放 → (メンバ破棄で) 画面。所有者 (sub-04 の窓) はエディタ終了 (OnShutdown) で必ず破棄すること。Job を作れない・割当に失敗した場合は警告ログのみで続行 (Editor 異常終了時に子孫が残りうる)。cmd.exe は引用されたスイッチ ("/c") を解釈しないため、引数を常に引用する方式に戻さないこと。決定性 — Editor 層のみでシミュレーションに触れない。
仕様との差分:
  - [追加] TerminalByteQueue を独立したクラスにした — Win32 非依存にして g++/TSan でテストするため。上限 4MB の背圧と Mode (Bounded/Unbounded/Discard) は sub に無い仕様。ClosePseudoConsole が排出待ちで固まる問題 (spec 4.4) を避けるのに必要
  - [追加] Close(keepPendingOutput) の引数 — 子が自分で終わったときに最後の出力を残すため。spec の終了順序 (ClosePseudoConsole → join → 解放) は守っている
  - [追加] FindTerminalExecutable (SearchPathW 実装の TerminalExecutableFinder) — SelfTest で cmd.exe を探すため。sub-04 で ResolveTerminalShell の finder に使える想定
  - [追加] SelfTest を 2 スイートに分けた: Fast "terminalsession" (偽ホスト、Win32 不要) と Full "terminalconpty" (sub の指定どおり) — 状態遷移の回帰を Fast で毎回回すため
  - [逸脱] 引数の引用: spec 4.1 は「引数は QuoteCommandLineArg で組む」だが、空白・'"' を含まない引数は引用しない (含むときは QuoteCommandLineArg)。cmd.exe が "/c" "/d" のような引用付きスイッチを解釈しない (記憶による、未確認) ため。CRT の規則で読む子には結果は同じ
  - [追加] 「関数ポインタ null 注入」は TerminalConPtyApiSource::Missing を CreateConPtyTerminalHost に渡す形にした (関数ポインタの型を公開ヘッダに出さないため)
検証:
  - g++ 13 -std=c++20 -Wall -Wextra -fsyntax-only (TerminalByteQueue / TerminalSession / TerminalSessionSelfTest) → 警告 0
  - g++ で偽ホストテスト + sub-02 の terminalcore を libvterm と一緒にリンクして実行 (Log.h はスタブ、CreateConPtyTerminalHost は未使用スタブ) → terminalsession PASS 49 / FAIL 0、terminalcore も PASS (計 168 PASS)、終了コード 0
  - 同上を -fsanitize=address,undefined → 168 PASS、ASan/UBSan 報告なし
  - 同上を -fsanitize=thread → 168 PASS、ThreadSanitizer 報告なし (キューの背圧待ち・Discard での解放を含む)
  - x86_64-w64-mingw32-g++ 13 (posix) -std=c++20 -Wall -Wextra -fsyntax-only -DNOMINMAX -DWIN32_LEAN_AND_MEAN で TerminalConPtyHost.cpp / TerminalSession.cpp / TerminalSessionSelfTest.cpp / TerminalByteQueue.cpp → 警告 0 (Windows.h は小文字 windows.h への中継ヘッダで代用。リンク・実行はしていない)
  - MinGW で PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE == 0x20016 を static_assert (NTDDI_VERSION=RS5 指定時) → 成立。既定設定ではマクロ未定義 = 自前定義にした理由の確認
  - grep "windows.h|HANDLE|HRESULT|DWORD" src/Editor/Tools/Terminal/*.h → 該当なし。Windows.h を include するのは TerminalConPtyHost.cpp のみ (受け入れ条件 1)
  - python xml.dom.minidom で Editor.vcxproj / .filters を解析 → 整形式。git diff は追加行のみ (vcxproj +8、filters +24)
  - 未実行 (Windows 環境なし): MSVC Debug/Release ビルド、`MYE_SELFTEST_ONLY=terminalsession,terminalconpty Editor.exe --selftest-full`、エディタ終了後に子 (conhost/cmd/pwsh) が残らないことのタスクマネージャ確認、tools\gen_project_files.ps1 の再生成との差分照合、check_rules.ps1
自己採点 (1-5):
  仕様適合: 4 — sub のやること (GetProcAddress 解決・STARTUPINFOEX・SUSPENDED→Job→Resume・UNICODE 環境・読み取りスレッド・Pump 上限・応答返送・終了コード・再起動・破棄順序・Full SelfTest) を全て実装。引数引用の逸脱と追加分は差分欄に記載
  正しさ: 2 — Win32 非依存部 (キュー・状態遷移・上限・応答返送) は g++/ASan/TSan で検証済みだが、ConPTY 部は MinGW の構文チェックのみで一度も実行していない。cmd の引用付きスイッチの挙動、ClosePseudoConsole の待ち方も未確認
  コード品質: 4 — Win32 は 1 ファイルに閉じ、ホストの口は試験目的に限定。コメントは理由中心の日本語。MSVC で警告が出るかは未確認
  テスト: 3 — Fast スイートは状態遷移・境界 (空・上限ちょうど・残り・明示上限・最小サイズ・二重 Start・終了後の入力) を網羅し実行済み。Full スイートは書いたが未実行
不安・質問:
  - [planner] 引数引用の逸脱 (空白等を含まない引数は引用しない) を許容するか。spec 4.1 の文言を「必要時 QuoteCommandLineArg」に直すか、あるいは常に引用して cmd 用の扱いを別にするか
  - Windows で terminalconpty を回したとき、Close(true) (子の終了時) が UI を目立って止めないか、ClosePseudoConsole が古い Win10 ビルドで返らない既知不具合に当たらないかは実機で要確認
触ったファイル:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalByteQueue.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalByteQueue.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalHost.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalConPtyHost.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSession.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSession.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSessionSelfTest.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSessionSelfTest.cpp
  - /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp
  - /home/user/MyEngin/build/Editor.vcxproj
  - /home/user/MyEngin/build/Editor.vcxproj.filters
  - /home/user/MyEngin/plans/m106-integrated-terminal/sub-03.md (実装メモ節のみ)
申し送り:
  - sub-04: TerminalSession の所有者は窓。毎フレーム Pump() を窓の表示状態に関係なく呼ぶ (呼ばないと読み取りキュー 4MB で子が止まる)。OnShutdown で session を確実に破棄。シェル解決の finder には FindTerminalExecutable を渡せる。表示用の状態は State() / ExitCode() / ErrorMessage() (英語、Tr で包むのは窓側)
  - sub-04: SendPaste は同期 WriteFile。巨大な貼付で UI が待ちうる
  - Windows 確認時: `MYE_SELFTEST_ONLY=terminalsession,terminalconpty` で `bin\x64\Debug\Editor.exe --selftest-full`。終了後にタスクマネージャで cmd/conhost が残らないこと
  - 検証用スクリプト・ログ: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub03/ (driver.cpp, stub_host.cpp, stub/, run_*.log)
```
- round 1: VERDICT OK (planner)。実装メモ節と原文 selfeval.md は書式以外一致。差分6件を承認 (引数引用の逸脱は spec 4.1 を修正して取り込み)。ConPTY 実経路・MSVC・Full SelfTest・子孫残りの確認は Windows 未検証として申し送り (sub-04 の test_checklists に項目化)。
