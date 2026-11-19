# sub-02: Win32 非依存のターミナルコア

- 依存: sub-01
- 状態: OK (コミット待ち)
- 往復: 2

## やること
spec 4.1・4.2。`/home/user/MyEngin/src/Editor/Tools/Terminal/` に Win32 / ImGui 非依存 (標準 C++20 + libvterm のみ) の部品を置く:
- 画面モデル: libvterm の VTerm/VTermScreen を所有し、バイト投入 → セル (コードポイント列・幅・前景/背景 RGB 解決済み・属性) の読み出し、カーソル位置/表示、代替画面中か、スクロールバック (上限付きリング、sb_pushline/popline)、リサイズ、bracketed paste 状態、libvterm が出す応答バイト (DA 等) の取り出しを提供。
- キー→VT 変換: エンジン非依存のキー列挙 + 修飾 → libvterm keyboard API 経由でバイト列。貼付文字列の包み (2004 有効時)、改行正規化 (CRLF→CR)。
- シェル解決: 設定文字列と「存在確認関数」(注入) から実行パス+引数を返す純関数。順序 pwsh→powershell→cmd、明示指定が無効ならエラー。
- 環境ブロック組立: 親環境のキー値列 + 追加変数 + PATH 前置 → Windows 環境ブロック (UTF-16 化は呼び出し側、ここでは UTF-8 のソート済み列と二重NUL 規則の検証可能な形) を返す純関数。大文字小文字無視の重複処理、既存変数非上書き。
- SelfTest: `TerminalCoreSelfTest.{h,cpp}` を `*SelfTest` パターンで追加し `/home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp:144` の kSuites に Fast で登録。spec 5 の 2・3・4 を網羅 (空入力、分割された UTF-8/エスケープ列の境界、大量出力、最小サイズ、スクロールバック上限)。
- SelfTest 本体を Linux の g++ でも回せるよう、テスト関数は SelfTest 基盤に依存しない形で書くか、最小のスタブで g++ ビルドできるようにする (方法は coder 裁量、SELF_EVAL に書く)。

## やらないこと (このサブでは)
- プロセス起動、描画、設定保存。

## 触る場所 (planner の見立て)
- 新規 `/home/user/MyEngin/src/Editor/Tools/Terminal/` (名前例: TerminalScreen, TerminalInput, TerminalShellResolve, TerminalEnvironment)。`tools\gen_project_files.ps1` 再生成相当の vcxproj/filters 更新 (Linux では手で同等の差分を入れ、整合を検査)。
- 手本: `/home/user/MyEngin/src/Editor/Tools/Browser/BrowserCatalog.cpp` (Win32 分離) と BrowserEmbedSelfTest。

## 受け入れ条件 (このサブ)
1. spec 5-2/3/4 を SelfTest で網羅し、Linux g++ -std=c++20 -Wall -Wextra で警告 0、全件成功。
2. 新ファイルが Win32 ヘッダ・ImGui を include しない (grep)。
3. vcxproj / filters に新ファイルが登録され整形式。
4. Windows (要確認): ビルド成功、`Editor.exe --selftest` Fast 層で新スイート成功・60 秒以内。

## 検証コマンド
- Linux: g++ ビルド+実行、grep、XML 検査。
- Windows: `MYE_SELFTEST_ONLY=<新スイート名>` で `bin\x64\Debug\Editor.exe --selftest`。

## 実装メモ (coder が追記)

### round 1
SELF_EVAL: sub-02 (round 1)
実装:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalScreen.h / .cpp:TerminalScreen, TerminalCell, TerminalCursor, TerminalLineText — libvterm の VTerm / VTermScreen を pimpl (TerminalScreen::Impl) で所有する画面モデル。Feed / Resize (最小 20x5 に切り上げ) / Cols / Rows / GetCell (0..Rows-1 = 画面、-1..-N = スクロールバック) / ScrollbackCount / GetCursor / IsAltScreen / IsBracketedPaste / TakeOutput / AppendOutput / RawVTerm。スクロールバックは上限付き deque、ED 3 で消去
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.h / .cpp:TerminalKey, TerminalModifier, TerminalSendKey, TerminalSendChar, TerminalSendPaste, NormalizeTerminalPasteNewlines — エンジン非依存のキー列挙と修飾ビット → vterm_keyboard_key / vterm_keyboard_unichar。貼付は改行を CR に揃え、2004 有効時だけ 200~/201~ で包む
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalShellResolve.h / .cpp:ResolveTerminalShell, TerminalShellCommand, TerminalShellError, TerminalExecutableFinder, kTerminalAutoShells — 設定文字列と注入された存在確認関数から実行パス+引数
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalEnvironment.h / .cpp:BuildTerminalEnvironment, FormatTerminalEnvironmentBlock, CompareTerminalEnvName — 親環境+追加変数+PATH 前置 → 大文字小文字無視でソート済みの列、および二重 NUL 終端の UTF-8 ブロック
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalCoreSelfTest.h / .cpp:RunTerminalCoreSelfTest — 11 群 119 件 (Win32・子プロセス不使用)
  - /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp:kSuites — { "terminalcore", RunTerminalCoreSelfTest, Fast } を末尾 (browserembedproc の後) に登録、include 追加
  - /home/user/MyEngin/build/Editor.vcxproj, /home/user/MyEngin/build/Editor.vcxproj.filters — tools\gen_project_files.ps1 が生成する区画 (BEGIN/END FILES 内の ClCompile/ClInclude 10 行、filters の Filter "Tools\Terminal" と 10 項目) を pwsh が無いので手で同じ規則 (Kind→Rel の大文字小文字無視順、CRLF、GUID = MD5("myengine-filter-"+名) を .NET Guid(byte[]) 解釈) で更新。GUID 算出式は既存 "Tools\Browser" の {80B4F782-…} を再現できることで照合済み
技術解説:
  - 技術: libvterm の Screen 層 + コールバック — vterm_new で作った VTerm から VTermState / VTermScreen を取り、VTermScreenCallbacks に settermprop (CURSORVISIBLE / ALTSCREEN の追跡)、sb_pushline / sb_popline (スクロールバック)、sb_clear (ED 3) だけを登録する。damage / movecursor は使わない — 描画側 (sub-04) は毎フレーム全セルを GetCell で読む前提で、差分管理を二重に持たないため。カーソル位置は vterm_state_get_cursorpos で都度読む。UTF-8 の分割・CSI/OSC の分割は libvterm のパーサが内部に保留するので、Feed は受け取ったバイトをそのまま vterm_input_write へ渡すだけでよい (テストで 1 バイトずつ・2+2 バイト・CSI を 3 分割・OSC を 2 分割で確認)。
  - 技術: スクロールバックを libvterm の生セル (std::deque<std::vector<VTermScreenCell>>) で保持 — 行数を増やすリサイズで libvterm は sb_popline を呼んで画面上端へ行を戻す。そのとき VTermScreenCell を返す必要があり、自前の TerminalCell (RGB 解決済み) から逆変換すると既定色の印 (VTERM_COLOR_DEFAULT_FG/BG) や色種別を失うので、生のまま持つ。上限超過は pop_front (古い行から捨てる)。却下: 固定長の自前リングバッファ — deque で上限付き FIFO の要件を満たし、行ごとに列数が違う (リサイズ前後) ことも自然に扱えるため。popline で列数が増えた分は vterm_state_get_default_colors の既定色の空白で埋める。
  - 技術: 色の解決 — VTermColor は indexed / RGB と、既定色の印のビットを持つ。vterm_screen_convert_color_to_rgb は indexed をパレット (16 色は libvterm 既定の 224 系、256 色は 6x6x6 キューブ ramp6 と灰 24 段) で RGB にするが、同時に印を消す (pen.c:263-269)。なので ConvertCell は印を先に読み (defaultFg / defaultBg)、その後 RGB にする。描画側は default 印のセルだけテーマ色に差し替えられる。
  - 技術: bracketed paste の状態取得 — libvterm 0.3.3 は mode.bracketpaste の getter を公開していない。vterm_keyboard_start_paste は「印が立っていれば CSI 200~ を出す」だけで他の状態を変えない (keyboard.c:216-220) ことを利用し、出力コールバックを一時的に「出たかどうかだけ記録する」関数へ差し替えて呼び、元に戻す。却下: 内部構造体 (vterm_internal.h) を読む — 同梱物の非公開ヘッダに依存する。自前で DECSET 2004 を解析 — libvterm と状態を二重に持ち、分割列で食い違う。
  - 技術: Windows 環境ブロックの規則 — CreateProcess の lpEnvironment は "NAME=VALUE\0" の並びで、名前を大文字小文字無視で昇順に並べ、最後に追加の '\0' (空なら "\0\0")。名前比較は ASCII を大文字に畳んだ符号なしバイト比較 (CompareTerminalEnvName)。大文字に畳むため '_'(0x5F) は英字の後になり、Windows の並びと一致する。"=C:" のような '=' 始まりの隠し変数は残す (cmd がドライブ別カレントディレクトリに使う)。
  - コード: TerminalScreen (入力: 子の出力バイト / 出力: セル・カーソル・モード、子へ送るバイト)。状態はすべて Impl に置く (vt, screen, state, scrollback, scrollbackLimit, output, cursorVisible, altScreen)。コールバックの user は Impl* なので、TerminalScreen をムーブ禁止にして Impl のアドレスを固定している (コピー/代入は delete、unique_ptr の中身は動かない)。呼ぶ順は sub-03/04 の想定で「UI スレッドが毎フレーム: 読み取りキューから Feed → TakeOutput を入力パイプへ → GetCell で描画」。
    ```cpp
    int OnPushLine(int cols, const VTermScreenCell* cells, void* user)
    {
        auto* impl = static_cast<TerminalScreen::Impl*>(user);
        if (impl->scrollbackLimit <= 0) {
            return 0;
        }
        impl->scrollback.emplace_back(cells, cells + cols);
        while (static_cast<int>(impl->scrollback.size()) > impl->scrollbackLimit) {
            impl->scrollback.pop_front();
        }
        return 1;
    }
    ```
  - コード: ConvertCell — 全角の右半分は libvterm が chars[0] = (uint32_t)-1 で表す (screen.c:190) ので width = 0・charCount = 0 にする。それ以外は chars を 0 まで (最大 6) 写し、width・属性 (bold / italic / underline != OFF / reverse / strike) と色を詰める。TerminalLineText は width 0 を飛ばし、末尾の空白を落として UTF-8 にする (選択コピー・テスト用)。
  - コード: TerminalSendKey / TerminalSendChar (入力: TerminalKey + 修飾ビット / コードポイント → 出力: screen の output バッファへバイト)。列挙を VTermKey へ写すだけで、CSI と SS3 の切り替え (DECCKM)、修飾付きの "\x1b[1;5A"、Shift+Tab の "\x1b[Z"、Ctrl+文字の制御文字化、Alt の ESC 前置は libvterm の keyboard.c に任せる。F1..F12 は列挙が連続している前提で VTERM_KEY_FUNCTION(1 + i)。
  - コード: TerminalSendPaste — libvterm の出力コールバックは同期なので、start_paste → AppendOutput(本文) → end_paste の順に同じ output に並ぶ。本文は unichar を通さず生で積む (Ctrl 修飾や CSI u の経路に乗せない・再符号化しない)。
    ```cpp
    VTerm* vt = screen.RawVTerm();
    vterm_keyboard_start_paste(vt);
    screen.AppendOutput(NormalizeTerminalPasteNewlines(utf8));
    vterm_keyboard_end_paste(vt);
    ```
  - コード: ResolveTerminalShell (入力: 設定文字列 UTF-8、finder(名前またはパス) → 絶対パス or 空 / 出力: TerminalShellCommand {error, path, args})。空・空白のみ → kTerminalAutoShells (pwsh.exe → powershell.exe → cmd.exe) を順に finder、全滅で NoAutoShell。明示指定 → 先頭が '"' なら閉じ引用までをパス、無ければまず全体を 1 つのパスとして finder (引用し忘れた "C:\Program Files\…" を救う)、駄目なら最初の空白で分ける。残りは '"' で空白を保護する簡易分割。見つからなければ ConfiguredNotFound を返し、自動検出へは落とさない (spec 4.1 の明示指定尊重)。PATH 探索・ファイル存在確認は finder として Win32 側 (sub-03) が注入するので、この関数は純関数。引数の引用は呼び出し側の QuoteCommandLineArg。
  - コード: BuildTerminalEnvironment (入力: parent, additions, pathPrefix / 出力: ソート済み vector<TerminalEnvVar>)。parent を順に、無効名 (空、2 文字目以降に '=') を捨て、大文字小文字無視の重複は先勝ち → additions は同名が既にあれば足さない (非上書き) → pathPrefix が空でなければ既存 PATH (綴り Path 等は保つ) の先頭へ "prefix;"、無ければ PATH を作る → stable_sort。FormatTerminalEnvironmentBlock は UTF-8 のまま二重 NUL ブロックを作る。UTF-16 化と CREATE_UNICODE_ENVIRONMENT は sub-03/05 の Win32 側。
    ```cpp
    if (!pathPrefix.empty()) {
        if (TerminalEnvVar* path = FindByName(env, "PATH")) {
            path->value = path->value.empty() ? std::string(pathPrefix) : std::string(pathPrefix) + ";" + path->value;
        } else {
            env.push_back(TerminalEnvVar{ "PATH", std::string(pathPrefix) });
        }
    }
    ```
  - コード: RunTerminalCoreSelfTest — BrowserEmbedSelfTest と同じ check ラムダ + MYE_LOG_INFO/ERROR の形。Linux で回す方法: テスト本体は Engine/Core/Diagnostics/Log.h の 2 マクロにしか依存しないので、スクラッチパッドに printf へ落とす最小スタブ Log.h を置き、それを -I で先に見せて g++ ビルドした (/tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub02/stub/Engine/Core/Diagnostics/Log.h、ドライバ /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub02/driver.cpp)。リポジトリには入れていない。
  - 選ばなかった案: (1) 公開ヘッダに vterm.h を含める — TerminalScreen.h は struct VTerm の前方宣言だけにし、sub-04 の窓が libvterm の型を見ずに済むようにした (VTermScreenCell は無名 typedef で前方宣言できないため pimpl)。(2) スクロールバックを自前の TerminalCell で保持 — 上記 popline の理由で却下。(3) 環境ブロックを std::wstring で返す — Linux g++ でテストでき、Win32 型を持ち込まないよう UTF-8 にした。(4) 貼付を 1 文字ずつ vterm_keyboard_unichar — 制御文字の扱いがキー入力と混ざるため却下。
  - 前提・落とし穴: TerminalScreen はスレッド安全ではない。libvterm と画面モデルは UI スレッドだけが触る (spec 4.4)。読み取りスレッドから Feed しないこと。IsBracketedPaste は const だが内部で出力コールバックを一瞬差し替える — 同じ VTerm の Feed と並行に呼ばない (UI スレッド専用なら問題ない)。sb_pushline は全画面スクロール時のみで、DECSTBM の領域内スクロールや代替画面では積まれない (テストで代替画面 30 行スクロールで 0 件を確認)。ScrollbackLimit は構築時固定 (設定変更は再構築で反映する想定、sub-04)。GetCell の line<0 はスクロールバック行が古い列数のまま返る (列数を超える col は空白)。環境の並び順は非 ASCII 名で Windows (CompareStringOrdinal の Unicode 大文字化) と異なりうる (現実の環境変数名は ASCII)。F キーの列挙順 (F1..F12 連続) を変えると ToVTermKey が壊れる。決定性: Editor 層のみでシミュレーションに触れない。
仕様との差分:
  - [追加] 改行正規化は CRLF→CR に加えて単独 LF→CR も行う — sub は「CRLF→CR」のみ記載。LF だけの文字列 (他アプリからの貼付) を送るとシェルが改行と見なさないため。Windows Terminal と同じ挙動。単独 CR は維持
  - [追加] シェル設定の書式 (先頭引用=パス、残りを空白区切りで引数・引用で空白保護、引用なしは全体を先にパスとして試す) と、エラー種別 ConfiguredMalformed (閉じない引用) — spec は「実行パス+引数」とだけ記載。最小の解釈
  - [追加] 自動検出時の引数は空 (-NoLogo 等を付けない) — spec に記載なし。付けるなら sub-03/04 で決める
  - [追加] エラーは文字列でなく列挙 TerminalShellError で返す — UI 文字列は Tr() で窓側 (sub-04) が出すため
  - [逸脱] 環境ブロック組立を「ソート済み列を返す BuildTerminalEnvironment」と「二重 NUL ブロック文字列を作る FormatTerminalEnvironmentBlock」の 2 関数に分けた — 検証可能な形 (sub の記述) を両方で出すため。名前比較は ASCII 大文字畳み込みで、非 ASCII 名は Windows と並びが違いうる
  - [追加] TerminalScreen::AppendOutput (公開) — libvterm に生バイトを出力へ積む API が無いため、貼付本文を同じ出力順に並べる口として追加
  - [追加] スクロールバック上限 0 は「保持しない」 — spec の範囲 100〜100000 のクランプは設定側 (sub-04) の責務とした
  - 「触る場所」の確認: 手本 BrowserCatalog は Win32 を使う実装、BrowserEmbedSelfTest は Windows.h/imgui を include しているため、そのままは g++ で通らない。今回のテストは Log.h 以外に依存しない形にした
検証:
  - g++ -std=c++20 -Wall -Wextra -O1 (libvterm は gcc -std=c99 でオブジェクト化してリンク) → 警告 0 (build.log 0 行)、ドライバ実行 119 PASS / 0 FAIL、所要 約 61 ms (1MB 投入を含む)
  - 同じものを -g -fsanitize=address,undefined → 0 FAIL、サニタイザ報告なし、exit 0 (約 790 ms)
  - grep -nE '#include *[<"](Windows|windows|imgui)' /home/user/MyEngin/src/Editor/Tools/Terminal/* → 0 件 (受け入れ 2)
  - python xml.dom.minidom で /home/user/MyEngin/build/Editor.vcxproj と .filters を parse → 整形式。CRLF 維持、git diff は追加行のみ (vcxproj +10、filters +33)
  - Windows: MSVC ビルド、`MYE_SELFTEST_ONLY=terminalcore bin\x64\Debug\Editor.exe --selftest`、tools\check_rules.ps1 → 未実行 (Linux コンテナで MSVC / Windows なし)。MSVC 固有の警告・libvterm の MSVC コンパイル可否は未確認
自己採点 (1-5):
  仕様適合: 4 — sub のやること 5 項目を実装、受け入れ 1〜3 は Linux で確認。差分は [追加]/[逸脱] として全部列挙。受け入れ 4 (Windows) が未確認
  正しさ: 4 — 119 件 + ASan/UBSan で確認。期待バイト列は xterm 慣例 (libvterm の表とも照合)。MSVC と実 ConPTY 経由の挙動は未検証
  コード品質: 4 — Win32/ImGui 非依存、公開ヘッダから libvterm を隠蔽、純関数は注入で外部状態なし。IsBracketedPaste のコールバック差し替えはやや技巧的 (理由をコメント)
  テスト: 4 — spec 5-2/3/4 の項目と境界 (空入力、分割 UTF-8/CSI/OSC、1MB、最小サイズ、上限、上限 0、ED 3、日本語パス、閉じない引用、空ブロック) を網羅。Windows の Fast 層実行と 60 秒以内は未確認
不安・質問:
  - 単独 LF→CR の正規化を [追加] したが、sub の文言どおり CRLF→CR のみに絞るべきか
  - 自動検出時に pwsh / powershell へ -NoLogo を付けるか (未指定のため付けていない)
  - MSVC で libvterm と本コードがビルドできるかは未確認 (sub-01 から持ち越しのリスク)
触ったファイル:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalScreen.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalScreen.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalShellResolve.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalShellResolve.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalEnvironment.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalEnvironment.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalCoreSelfTest.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalCoreSelfTest.cpp
  - /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp
  - /home/user/MyEngin/build/Editor.vcxproj (gen_project_files.ps1 生成区画を手で更新)
  - /home/user/MyEngin/build/Editor.vcxproj.filters (同上)
  - /home/user/MyEngin/plans/m106-integrated-terminal/sub-02.md (実装メモ節のみ)
申し送り:
  - sub-03: シェルの存在確認は TerminalExecutableFinder として注入する (SearchPathW 等)。ResolveTerminalShell の path/args を QuoteCommandLineArg で組む。環境は BuildTerminalEnvironment → FormatTerminalEnvironmentBlock の UTF-8 を UTF-16 化し CREATE_UNICODE_ENVIRONMENT で渡す
  - sub-04: 毎フレーム Feed (上限 256KB) → TakeOutput を入力パイプへ。DA 応答なども TakeOutput に混ざるのでキー入力が無いフレームでも送ること。default 印のセルはテーマ色へ。width 0 のセルは描かない。ScrollbackLimit は構築時固定なので設定変更時は再構築。設定値の 100〜100000 クランプは窓/設定側で
  - 恒久テストのため、Windows で `MYE_SELFTEST_ONLY=terminalcore` を一度回すこと
### round 2
SELF_EVAL: sub-02 (round 2)
実装:
  - #1: /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.cpp:SanitizeTerminalPaste — 旧 NormalizeTerminalPasteNewlines を置き換えた Win32 非依存の純関数。改行の CR 正規化と制御文字の除去を 1 パスで行う。TerminalSendPaste は本文をこれに通してから AppendOutput する (bracketed paste の有効・無効によらず常に通す)。宣言と規則は /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.h に書いた。呼び出し元は TerminalSendPaste だけだったので旧関数は残していない (未使用コードを残さない)
  - #1: /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalCoreSelfTest.cpp:TestPasteSanitize — 新規。(a) 2004 有効で本文に ESC[201~ (b) 2004 無効で C0/DEL/C1 除去と CJK・絵文字保持 (c) 改行の正規化、に加え不正 UTF-8 と途中で切れた列の 5 件。既存 TestPaste は旧関数名を Sanitize へ替え、ESC を含んでいた 1 ケースを ESC なしへ直した (ESC が消えるのが新しい正)。terminalcore は 119 → 131 件
  - #1 (planner の追記分): /home/user/MyEngin/engine_spec.md (§9.4 Input の項) と /home/user/MyEngin/docs/adr/ADR-041-integrated-terminal.md (決定 7) に貼付消毒の 1 文ずつ
  - #2: /home/user/MyEngin/external/VERSIONS.md の libvterm 行に「gcc -O1 以上で上流 parser.c / state.c に -Wmaybe-uninitialized が出る (上流のまま、実害なし。最適化なしの gcc -Wall では 0)」を追記 (表の行を壊さないよう、取得元列の前の説明欄の末尾に足した)
技術解説:
  - 技術: 出力側スタックでの C1 除去 — UTF-8 の C1 (U+0080-U+009F) は 0xC2 0x80-0x9F の 2 バイト。入力を 1 バイトずつ見て、通すバイトを out に積んだ直後に「out の末尾 2 バイトが 0xC2 + 0x80-0x9F か」を見て、そうなら 2 バイトとも out から落とす。入力側の隣接で見ないのは、間に挟まった ESC などを除いた結果として 0xC2 と 0x9B が隣り合う "\xC2\x1b\x9B" や、入れ子の "\xC2\xC2\x9B\x9B" から 8 ビット CSI (U+009B) が再び出来るのを防ぐため (除去後の出力は C2 + 80-9F を含まないことが不変条件になる)。却下: 先に C0 を落とす → 次に C1 を落とす 2 パス方式 — "\xC2\xC2\x9B\x9B" で 2 パス目の除去が新しい C1 を作る。UTF-8 を完全にデコードして U+0080-U+009F を判定する方式 — 不正列の扱いを自前で決め直す必要があり、「不正な UTF-8 は既存の扱いのまま」という確定事項に反する。
  - 技術: 常に消毒する理由 — bracketed paste (DECSET 2004、xterm 由来) は本文を CSI 200~ ... CSI 201~ で包み、アプリ側は 201~ が来るまでを貼付として扱う。本文に ESC[201~ があるとそこで貼付が終わり、残りがキー入力として実行される。有効時は ESC を落とせば足りるが、無効時も ESC・^C・C1 がそのままシェルや CLI に解釈されるので、確定事項どおり有効・無効を区別せず同じ消毒をかける。置換文字 (U+FFFD) にしないのは、貼付した文字列に見えない差分を足さないため。落とした後の "[201~" は無害な文字列として残る。
  - コード: SanitizeTerminalPaste (入力: 貼付文字列 UTF-8 / 出力: 消毒済みバイト列。状態は直前の入力バイトが CR かを示す afterCr だけ。呼び出し元は TerminalSendPaste だけ)。分岐は CR → CR を出して afterCr、LF → 直前が CR なら畳み、そうでなければ CR、HT または 0x20 以上かつ DEL 以外 → 通す、それ以外 (HT 以外の C0、DEL) → 捨てる。0x80 以上のバイトは全部「通す」側に入るので、不正な UTF-8 や途中で切れた列はそのまま通る。
    ```cpp
    } else if (c == kTab || (c >= 0x20 && c != kDel)) {
        out += ch;
        if (c >= kC1TrailFirst && c <= kC1TrailLast && out.size() >= 2 &&
            static_cast<unsigned char>(out[out.size() - 2]) == kC1Lead) {
            out.resize(out.size() - 2);
        }
    }
    ```
  - コード: 決めた境界 (差分欄にも [追加] で記載) — (1) 単独の 0x9B など 0x80 以上の孤立バイトは通す (UTF-8 では C1 ではなく不正バイト。ConPTY 側の UTF-8 デコードが U+FFFD にするので 8 ビット CSI としては働かない)。(2) 末尾で切れた 0xC2 や 0xE3 0x81 も通す。(3) ただし ESC 等を落とした結果として 0xC2 と 0x80-0x9F が隣り合えば C1 として落とす。(4) CRLF の畳みは入力上の隣接で判定する ("a\x1b\r\nb" は ESC を落とした後も CRLF として 1 個の CR)。TerminalSendPaste の呼び順は round 1 のとおり start_paste → AppendOutput(本文) → end_paste。
  - 前提・落とし穴: 消毒は TerminalSendPaste の中にあり、窓側 (sub-04) の貼付経路が TerminalSendPaste を通る限り自動で効く。別経路で本文を AppendOutput へ直接積むとこの保護を迂回する。消毒後の本文はバイト列として UTF-8 の妥当性を保証しない (不正入力は不正のまま)。ESC を含むキー入力 (Escape キー、Alt+文字) は TerminalSendKey / TerminalSendChar の経路で、貼付とは別。決定性: 純関数で入力だけで出力が決まる。他の層は round 1 のとおり。
仕様との差分:
  - [追加] 不正な UTF-8・途中で切れた列の扱い (上記 (1)〜(4)) — spec は「既存の扱いのまま」のみ。0x80 以上のバイトは通し、除去の結果隣り合った 0xC2 + 0x80-0x9F だけ C1 として落とす。理由: 通すだけだと ESC を挟んだ入力で 8 ビット CSI を作れてしまうため
  - [追加] NormalizeTerminalPasteNewlines を公開 API から外し SanitizeTerminalPaste に統合 — 改行正規化と消毒が 1 パスで、旧関数の呼び出し元が TerminalSendPaste と旧テストだけだったため。spec・sub に旧関数名の規定は無い
  - 仕様変更 (spec 4.1・受け入れ条件 3・§7・§8) は読み直して反映済み。逸脱なし
検証:
  - g++ -std=c++20 -Wall -Wextra -O1 で terminalcore + terminalsession + terminalwindow(Layout) + terminalvsenv の 4 スイートと libvterm (round 1 でビルド済みの gcc -std=c99 オブジェクト) をリンク → 警告 0 (ビルドログ 0 行)、vsenv=1 layout=1 core=1 session=1、terminalcore 単体は 131 PASS / 0 FAIL (旧 119 + 新規 12)
  - 同じ構成を -g -fsanitize=address,undefined → 4 スイートとも成功、サニタイザ報告なし
  - reviewer の /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/edge.cpp を再ビルド (plain / ASan+UBSan) → 29 項目すべて ok、FAILS=0。FAIL だった "pasted text cannot terminate bracketed paste early" が ok になり、出力は `\x1b[200~echo safe[201~rm -rf x\x0d\x1b[201~` (ESC は包みの 2 つだけ)
  - grep で TerminalInput.* に Windows / imgui の include → 0 件。新規テスト (a)(b)(c) と不正 UTF-8 の期待値は、実装前に手で期待バイト列を決め、失敗 1 件 (自分の期待値の CR 数の書き間違い: CRLF は 1 個) を直して再実行
  - 作業ツリーはコミットしていない。ADR / engine_spec / VERSIONS は文書のみの変更 (整合は diff で確認、エンジンのビルドは不要)
  - MSVC ビルド、`MYE_SELFTEST_ONLY=terminalcore bin\x64\Debug\Editor.exe --selftest`、tools\check_rules.ps1、TSan: 未実行 (Linux コンテナで MSVC / Windows / pwsh なし。TSan は本変更がスレッドに触れない純関数のため省略)
自己採点 (1-5):
  仕様適合: 4 — 確定事項 (常に消毒・HT/CR 以外の C0・DEL・C1 を除去・置換文字なし・CJK/絵文字保持・CR 正規化) と #1 #2 の指示をすべて実装。Windows の実行が未確認
  正しさ: 4 — 4 スイートと reviewer の edge で plain / ASan+UBSan 確認、元の故障条件 (ESC[201~) の出力で解消を確認、ESC を挟んだ C1 の再構成も反証テスト済み。MSVC と実 ConPTY 経由の貼付は未検証
  コード品質: 4 — 純関数 1 つに統合し旧関数を残さず、マジックナンバーは名前付き定数、コメントは理由のみ
  テスト: 4 — 指定 3 ケース + 不正 UTF-8・切れた列・入れ子の C1 を追加。Windows の Fast 層実行は未確認
不安・質問:
  - 孤立した 0x80-0x9F の単独バイトを通す判断 (ConPTY の UTF-8 デコードが U+FFFD にする前提、記憶による・この環境では未確認)。厳しくするなら 0x80 以上の不正バイトも落とす案がある (日本語の不正入力は想定しにくい)
触ったファイル:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalCoreSelfTest.cpp
  - /home/user/MyEngin/engine_spec.md
  - /home/user/MyEngin/docs/adr/ADR-041-integrated-terminal.md
  - /home/user/MyEngin/external/VERSIONS.md
  - /home/user/MyEngin/plans/m106-integrated-terminal/sub-02.md (実装メモ節のみ)
申し送り:
  - Windows で `MYE_SELFTEST_ONLY=terminalcore` を再度回すこと (新規 12 件)。sub-04 の窓は貼付を TerminalSendPaste 経由で送る前提を保つこと

- round 1: VERDICT OK (planner)。差分7件はすべて承認: 単独 LF→CR (Windows Terminal と同じ挙動、CR は維持) / シェル設定書式と ConfiguredMalformed / 自動検出の引数は空 (-NoLogo は付けない。プロファイルのバナーはユーザーのシェル設定の領分) / エラー列挙 / 環境組立の2関数分割 (非 ASCII 名の並び差は既知の制約として残す) / AppendOutput / 上限0=保持しない (クランプは sub-04)。MSVC ビルドと SelfTest Fast の実行は Windows の未検証として申し送り。
- review-1 対応 (planner REVIEW_RESPONSE): 差し戻し。
  - #1 [must] /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.cpp:TerminalSendPaste (と NormalizeTerminalPasteNewlines の周辺) に spec 4.1「貼付本文の規則」を実装する。消毒は Win32 非依存の純関数にし、bracketed paste の有効・無効どちらでも通す。terminalcore に回帰テストを追加: (a) 2004 有効で "echo safe\x1b[201~rm -rf x\r\n" → 出力に ESC[201~ が末尾の 1 回だけ、(b) 2004 無効で ESC・^C(0x03)・NUL・DEL・U+009B が消え、HT/CR・日本語・絵文字は残る、(c) CRLF・LF → CR は従来どおり。検証: g++ で terminalcore を plain と ASan/UBSan で実走 (reviewer の /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/edge.cpp の FAIL 行が ok になること)。engine_spec §9.4 と ADR-041 に 1 文。
  - #2 [nit] /home/user/MyEngin/external/VERSIONS.md の libvterm 項に「上流無改変。gcc -O1 以上では parser.c / state.c に -Wmaybe-uninitialized が出る (最適化なしでは 0)」と 1 行。
- round 2 (出所 REVIEW round 1): VERDICT OK (planner)。SELF_EVAL は scratchpad 原文と一致 (差はフィードバック履歴行のみ)。git status の 7 ファイルと一致。TerminalInput.cpp の差分で、1 回の走査で HT/CR 以外の C0・DEL を除去、LF→CR (CRLF は畳む)、出力側で 0xC2+0x80-0x9F を落とすことを確認 (除去で隣接した組も落ちる)。NormalizeTerminalPasteNewlines は src に残存参照なし。VERSIONS.md 注記あり (review-1 #2 消込)。[追加] 2 件 (不正 UTF-8 の素通し・旧関数の統合) は追認。質問 (孤立 0x80-0x9F の素通し) は可: UTF-8 として不正な単独バイトで C1 として解釈される経路は Latin-1 解釈時のみ、ConPTY が U+FFFD 化するという前提は記憶による・Windows で test_checklists に 1 項目足して確認。nit: CR の直後に除去対象 (例 CR ESC LF) があると afterCr が解除され CR が 2 つ出る。実害は空行 1 つで小さいので申し送り。
