# review-1: m105-embedded-browser

- 対象: `da0c13e53041529404238a405ef987ef477ef126..HEAD` (9b373892 M105a / cb0aa48f M105b / acdd0a3f M105c)
- 日付: 2026-11-19
- reviewer の作業場所: 画像・ログ・一時スクリプトはスクラッチ `C:\Users\akita\AppData\Local\Temp\claude\58c0e4c841a0685f\68a3f882-bcde-4363-91f6-662fdfe0ad8f\scratchpad\` (以下 `<scratch>`)。リポジトリには review-1.md 以外書いていない (`git status` は開始時と同じ 2 件のみ)

```
REVIEW: PASS
round: 1
軸 (1-5):
  製品の深度: 4 — 応答なし・親の喪失・Closed/Lost・既に起動中 (Edge/Brave/Vivaldi とも起動 PID ≠ 窓 PID で特定成功)・エディタの正常終了での切り離しを実走で確認。最小化中に WM_CLOSE した場合も、復元後に閉じてからダミーが普通の窓で残った。残りは minor 3 件 (起動待ち中のパネル ×、切り離し後の画面外はみ出し、名前の違うランチャー exe)
  機能性: 4 — 機械検証できる受け入れ条件 (1〜6, 11 の dummy 部分, 12 の selftest, 13〜16) は全部実走で充足。`--selftest-full` をリポジトリルートから 1 回通しで 127 suites / 0 failed。`tools\replay_verify.bat` の通しは環境要因で未取得 (下記)。手動の受け入れ (7〜10、11/12 の実ブラウザ部分、sub-03 の 1/3) は人の手の確認待ちで、FAIL 扱いにしていない
  ビジュアルデザイン: 4 — パネル ja/en (`<scratch>\rv_panel_ja.png`, `<scratch>\rv_panel_en.png`) は全文言が翻訳され崩れなし。埋め込み中のダミー (`<scratch>\cap_rv_dummy3.png`) と Edge (`<scratch>\cap_rv_edge.png`) がツールバーの下の内容矩形にぴったり描かれる。応答なし表示 (`<scratch>\rv_hang_crop.png`) は赤字「Not responding」+「Detach」無効。切り離し後の Edge が作業領域の下へ 320 px はみ出す件を minor に
  コード品質: 4 — 層 (Editor 層のみ、規則 20 + フィクスチャ)、`BrowserWindow.h` に Windows.h なし、純関数と Win32 の分離、Tr() の書式安全 ("%s" 渡し) は規約どおり。古い申し送りコメント 2 件と、事実と逆のコメント 1 件 (GetWindowText) を minor に
指摘:
  1. [minor] 宛先: planner — 起動待ち (Launching) 中にパネルを × で閉じたときの振る舞いが仕様に無く、実装では「窓が見つかった瞬間に一度子窓化 → 次のフレームで切り離し」になる。子窓化の時点で窓はメイン窓のクライアント原点へ動かされる (`SetWindowPos(child, nullptr, 0, 0, ...)`) ので、切り離し後のブラウザ (最大化でないもの) はエディタの左上に出て、1 フレームはエディタのメニューの上に子窓として重なる — 根拠 (コード経路。実操作では未再現): `src/Editor/Windows/Project/BrowserWindow.cpp:462-465` (閉じていて Embedded なら Detach、Launching は止めない) → `src/Editor/Tools/Browser/BrowserHost.cpp:446-447` (Attach で client (0,0) へ) → `BrowserHost.cpp:489-500` (RestoreTopLevel は今の画面位置に置く)。spec 4.1.9 は「パネルの × も Detach」だけで Launching を定めていない — 期待: spec 4.1.9 に「Launching 中のパネル × は CancelLaunch (待つのをやめるだけ。ブラウザは閉じない)」を足す
  2. [minor] 宛先: planner — 切り離し (Detach / × / 終了) で窓を「パネルがあった位置 + 控えた通常時の大きさ」に置くとき、モニタの作業領域へ収めていないので、背の高い窓は画面外へはみ出す。実測: Edge (最大化でない 1094x1241) の切り離し後 `rect_screen=(410,471,1504,1712)`、主モニタの作業領域は `{X=0,Y=0,Width=2560,Height=1392}` = 下端が 320 px 外 — 根拠: `<scratch>\probe_rv_edge.txt` の O5 行、`[System.Windows.Forms.Screen]::AllScreens` の出力、`src/Editor/Tools/Browser/BrowserHost.cpp:491-492` (目標矩形 = 今の左上 + saved のサイズ、クランプなし)。spec 4.1.4 の文言どおりの実装なので仕様の穴 — 期待: spec 4.1.4 に「置き直す矩形は窓の乗るモニタの作業領域 (`MonitorFromRect` + `GetMonitorInfo` の rcWork) に収める (はみ出す分は位置をずらし、それでも入らなければ縮める)」を足す。タイトルバーは見えているので手で戻せる = minor
  3. [minor] 宛先: planner — 「その他 (exe を指定)」で選んだ exe が、別名の実行ファイルに窓を作らせるランチャーだと、自動特定 (15 秒で TimedOut) も「既存の窓を取り込む」も同じ「同名 + 配下」条件で絞るため、その窓を**どの経路でも埋め込めない**。D9 が対象に挙げた「ポータブル版」はランチャー型が多い (PortableApps.com 形式の `FirefoxPortable.exe` → `App\Firefox64\firefox.exe` 等。この構成は記憶による・この PC では未確認) — 根拠: `src/Editor/Tools/Browser/BrowserHost.cpp:163-165` (ファイル名が違えば不一致)、`src/Editor/Windows/Project/BrowserWindow.cpp:357-358` (取り込み一覧も選択中の exe で `EnumerateBrowserWindows`) — 期待: 取り込み一覧に「選んだブラウザの窓 / すべての窓」の切り替え、または「その他」のときはファイル名一致を外して「登録 exe のディレクトリ配下」だけで絞る、のどちらかを spec 4.1.8 で決める
  4. [minor] 宛先: coder — 実装の現状と食い違う一時的な申し送りコメントが残っている (AGENTS.md §5「コメントを人やAIへの一時的な申し送りに使わない」) — 根拠: `src/Editor/App/EditorApp.h:116`「sub-02 で窓に置き換わる薄い配線」(プローブは既に BrowserWindow 経由で、このメンバは残る)、`src/Editor/Tools/Browser/BrowserHost.h:199`「UI の文字列は sub-02」(既に BrowserFailReason + Tr で実装済み) — 期待: 今の役割の説明に書き換える
  5. [minor] 宛先: coder — `WindowTitleNoBlock` のコメント「GetWindowText は他プロセスの窓だと同期メッセージになる」は公式の説明と逆 (他プロセスの窓では WM_GETTEXT を送らず、窓のキャプションを内部から読む: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextw の Remarks)。いまの `SendMessageTimeoutW(WM_GETTEXT, SMTO_ABORTIFHUNG, 100 ms)` はむしろ同期メッセージを投げており、応答なし判定 (5 秒) 前の忙しい窓 1 枚ごとに UI スレッドを最大 100 ms 待たせうる (spec 4.4「ブラウザ窓へ同期メッセージを投げない」の趣旨にも沿わない) — 根拠: `src/Editor/Tools/Browser/BrowserHost.cpp:127-137`、呼び出し `BrowserHost.cpp:285` (取り込みボタンを押したとき窓ごとに 1 回) — 期待: `GetWindowTextW` (他プロセスの窓では送信しない) に替え、コメントを事実に合わせる
検証した手段:
  - ビルド: MSBuild `MyEngine.sln` Debug|x64 / Release|x64。Release は exit 0。Debug はコンパイル・リンク成功 (`bin\x64\Debug\Editor.exe` 04:29 更新) だが post-build の `copy nethost.dll` が MSB3073 で失敗 = 残存プロセス (PID 2412 / 14332 / 38416 = Debug の Editor.exe、タイトル "MyEngine Browser Dummy Window"。Chrome のプロファイル選択窓 PID 8540) が掴んでいるため。レビュー開始時・終了時とも 4 プロセスは生存
  - `bin\x64\Debug\Editor.exe --selftest-full` を cwd = リポジトリルートで 1 回通し → `[selftest] total 127 suites, 0 failed, 882512 ms (mode: full)`、exit 0。BrowserEmbed PASS / BrowserEmbedProc PASS (特定 119 ms、応答なし中の Update 最大 1 ms)。実行後に新しい残存プロセスなし (ログ `<scratch>\full_stdout.txt`)
  - `tools\replay_verify.bat`: **環境要因で未取得**。bat は冒頭で Debug を MSBuild するので、上と同じ post-build の nethost.dll コピー失敗で止まる (同じ MSBuild コマンドで MSB3073 を再現済み)。M105 のコードの欠陥ではない。残存プロセスが閉じられた後に bat を 1 回通すことを申し送る (sub-03 で coder が並列部分 24 ジョブを手動で回して all PASS の記録はある)
  - `Editor.exe --browser-list` → 5 件 (Brave / Google Chrome / Microsoft Edge (default=1) / Opera GX Stable / Vivaldi)、IE なし、exit 0
  - `tools\check_rules.ps1` → 0 error (65 warning は既存の rule 17 等)、`tools\check_rules_selftest.ps1` → rule 18 / 19 / 20 とも PASS
  - パネルの見た目: cwd = `<scratch>\cwd` で `--browser-window --lang ja|en --frames 90 --screenshot` → `<scratch>\rv_panel_ja.png` / `<scratch>\rv_panel_en.png` を目視
  - 実プロセス間 (cwd はリポジトリ外): `--browser-embed-probe dummy` ×3 回 PASS (`<scratch>\probe_rv_dummy*.txt`、PrintWindow での窓キャプチャ `<scratch>\cap_rv_dummy3.png`)、`dummy-hang` + `--shot-frame 450 --screenshot` で応答なし表示 (`<scratch>\rv_hang.png` / `rv_hang_crop.png`)、Edge / Brave / Vivaldi の実ブラウザ probe 各 1 回 exit 0 (`<scratch>\probe_rv_edge.txt` / `probe_rv_brave.txt` / `probe_rv_vivaldi.txt`、`<scratch>\cap_rv_edge.png`)。Chrome はプロファイル選択窓 (PID 8540) が残っており R13 のとおり TimedOut になる環境なので回していない
  - 条件の外側: (a) 埋め込み中にメイン窓へ WM_CLOSE (正常終了) → ダミーが元のスタイル (`style=0x14cf0000`, parent=0, 可視) でパネルの位置に残る、(b) 最小化してから WM_CLOSE → 最小化中はエディタが閉じず (フレームが止まるため。M105 以前からのエンジンの挙動と推定)、復元すると閉じてダミーが普通の窓で残る (画面外へは行かない)、(c) 既に起動中のブラウザ (Edge / Brave / Vivaldi) で起動 PID と窓 PID が違っても特定、(d) 最大化で起動したブラウザ (Brave / Vivaldi) は切り離し後に最大化へ戻る。スクリプト `<scratch>\close_minimized.ps1` / `edge_probe.ps1` / `probe_run.ps1`
  - diff 全読: `src/Editor/Tools/Browser/*`、`src/Editor/Windows/Project/BrowserWindow.*`、`EditorApp.*` / `EditorMain.cpp` / `EditorGlobalSettings.*` / `ToastCenter.*` / `SelfTestSuites.cpp`、`LocalizationTable.inl`、`tools/check_rules*.ps1`、`build/Editor.vcxproj*`、ADR-040、engine_spec §9.3、test_checklists M105 節。仕様との差分に無い変更は見つからなかった (`--browser-dummy-unparent-after-ms` / `--browser-probe-clip` は sub-01 受け入れ 9 と O6 の観測用で、`BrowserProbe.h` に説明あり)
  - imgui の docking 実装 (`external/imgui/imgui.cpp:13743-13771`, `:21331`) を読み、ドック済みパネルの重なり判定 (g.Windows の並び) が成り立つことを確認 (ドック中の窓は ChildWindow 扱いで、EndFrame の並べ替えで親の直後に置かれる)
  - 後始末: 自分が起こしたダミー窓は全部終了 (probe の WM_CLOSE / 寿命 / dummy-hang は probe が TerminateProcess)。最小化テストで残ったダミー 1 枚 (PID 32640) は WM_CLOSE で閉じた。Edge / Brave / Vivaldi の probe が開いた新しい窓は各 1 枚、前後の窓一覧の差分で特定して WM_CLOSE で閉じ、残り 0 を確認 (ユーザーが元から開いていた窓には触っていない)。`--screenshot` を `--frames` なしで起動して終わらなかった自分の Editor 2 本 (PID 16344 / 42576) は Stop-Process で終了。終了時点の Editor.exe は既存の 3 本 (2412 / 14332 / 38416) のみ、Chrome 8540 も既存のまま
前回指摘の消込: (round 1 のため無し)
```

## 申し送り (司会へ)

- 人の手の確認として残るもの: spec 5-7〜5-10、5-11 / 5-12 の実ブラウザ・再起動部分、sub-03 受け入れ 1 / 3 (Chrome の後始末と「既存の窓を取り込む」のクリック操作)、R4 (O8)。手順は `docs/test_checklists.md` の M105 節。`EnumerateBrowserWindows` (取り込み一覧の列挙) は selftest では通っていない (純関数 `FilterBrowserWindows` と `AttachExisting(hwnd)` だけ) ので、受け入れ 3 の手動確認が唯一の実走になる
- `tools\replay_verify.bat` の通し: 残存プロセス (2412 / 14332 / 38416 / 8540) が閉じられた後に 1 回 (`MYE_REPLAY_JOBS=3`)。`--selftest-full` のルート通しは本レビューで取得済み (127 / 0 failed)
- 残存ダミー 3 本はタイトル "MyEngine Browser Dummy Window" の Debug Editor.exe で、寿命上限 (sub-02 round 2) を入れる前のビルドから起きたもの。今のコードでは 120 秒で自己終了するので再発はしない
