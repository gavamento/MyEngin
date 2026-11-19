# sub-03: ConPTY ホストとセッション

- 依存: sub-02
- 状態: 未着手
- 往復: 0

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
