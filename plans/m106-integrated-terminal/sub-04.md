# sub-04: Terminal 窓

- 依存: sub-03
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 4.1・4.2・4.3。
- `/home/user/MyEngin/src/Editor/Windows/` 配下 (Project か新カテゴリ、手本 `/home/user/MyEngin/src/Editor/Windows/Project/BrowserWindow.h`、`/home/user/MyEngin/src/Editor/Windows/Scene/ParticleSettingsWindow.cpp`) に TerminalWindow。セルを ImGui DrawList で描画 (背景矩形の連続ランをまとめる、全角2セル、カーソル、選択ハイライト)、ツールバー、エラー/終了表示と再起動。
- 等幅フォント: ImGuiTheme (`/home/user/MyEngin/src/Engine/Renderer/ImGui/ImGuiTheme.cpp:175` 付近) に Cascadia Mono → Consolas の実行時読込を追加し日本語グリフを既存方式で merge。無ければ既定フォント+固定セル幅。Renderer 層への追加が層規則・engine_spec と合うか確認。
- 入力: フォーカス時のキー/IME 確定文字のみ (2026-11-19 ユーザー確定)/貼付/選択コピー、ショートカット抑止 (`/home/user/MyEngin/src/Editor/App/EditorApp.cpp:1970-2011`)。
- 配線: EditorApp メンバ (:199-255)、layouts_.Init (:119-148)、OnImGui (:1007-1341)、Window メニュー (:1930-1962)、SetupDockLayout (:2315-2374)、OnShutdown (:626-)。
- 設定: EditorGlobalSettings に terminalShell / terminalFontSize / terminalScrollback / terminalLoadVsDevEnv (読込のみ。VS 環境の実処理は sub-05)。設定 UI は既存のエディタ設定画面の流儀に合わせる。
- ローカライズ: LocalizationTable.inl 両言語、###ID 一意。
- check_rules: 規則20 (`/home/user/MyEngin/tools/check_rules.ps1:1503-1510`) と同型の新規則で Engine/Runtime/GameLogic/Shared/Server から `Editor[\/]Tools[\/]Terminal[\/]` の include を禁止。
- 文書: engine_spec.md に §9.4 Terminal window を追加、`/home/user/MyEngin/docs/test_checklists.md` に手動確認項目 (Claude Code CLI・git・MSBuild 出力・リサイズ・貼付・終了。sub-03 由来: `terminalconpty` Full の成功、エディタ終了・再起動・子終了時に UI が目立って止まらないこと、終了後に conhost/cmd/pwsh が残らないこと、cmd /c が引用なしスイッチで動くこと)。

## やらないこと (このサブでは)
- VS 開発者環境、AI 追跡。

## 受け入れ条件 (このサブ)
1. spec 5-6, 5-7, 5-10, 5-11。
2. Linux: 追加文字列が LocalizationTable.inl の両言語に存在し ###ID 重複なし (grep/スクリプト)、新規則の正規表現が既存コードで誤検出しない (pwsh があれば check_rules を、無ければ正規表現を grep -P で同等確認)。
3. Linux: 窓の Win32/ImGui 非依存部 (セル→描画ラン変換、選択範囲→文字列、ピクセル→行列計算) を純関数化して g++ テスト。

## 検証コマンド
- Linux: g++、grep、XML 検査。
- Windows (要確認): ビルド、`tools\check_rules.ps1`、`Editor.exe --selftest-full`、手動でスクショ (Claude Code CLI 起動画面、git log、ビルド出力、日本語表示)。

## 実装メモ (coder が追記)

```
SELF_EVAL: sub-04 (round 1)
実装:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayout.h/.cpp — 窓の ImGui/Win32 非依存の純関数 (BuildTerminalRowRuns=セル→背景ラン・文字ラン、TerminalSelection 系=選択→文字列と行ごとの列範囲、TerminalGridFromPixels / TerminalCellFromPixels=ピクセル→行列、ScrollTerminalView / ClampTerminalViewOffset、ClassifyTerminalCtrlLetter、ClampTerminalFontSize / Scrollback と既定値定数)
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayoutSelfTest.h/.cpp — Fast スイート "terminalwindow" (61 項目)
  - /home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.h/.cpp:TerminalWindow — ドック窓。Pump (毎フレーム) / OnImGui (ツールバー・状態行・セル描画・入力・設定ポップアップ・右クリックメニュー) / Shutdown。TerminalSession を 1 つ所有し、遅延起動・再起動・設定読み直し
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSession.h/.cpp — ClearScrollback() 追加 (CSI 3 J を自分の画面へ)。TerminalSessionSelfTest に 1 項目追加
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalScreen.h/.cpp — AppendTerminalUtf8 を公開、TerminalLineText に列範囲オーバーロード (選択コピー用。既存の動作は不変で terminalcore 118 PASS のまま)
  - /home/user/MyEngin/src/Engine/Renderer/ImGui/ImGuiTheme.h/.cpp — EditorMonospaceFont() 追加。SetupEditorFonts が Cascadia Mono → ユーザー Fonts の Cascadia → Consolas の順に読み、日本語を merge
  - /home/user/MyEngin/src/Editor/App/EditorGlobalSettings.h/.cpp — terminalShell / terminalFontSize / terminalScrollback / terminalLoadVsDevEnv (欠落キーは既定値)
  - /home/user/MyEngin/src/Editor/App/EditorApp.h/.cpp — terminal_ メンバ、layouts_ に "Terminal"、OnStart で設定適用、OnImGui で Pump → OnImGui と設定保存、Window メニュー、SetupDockLayout で下段 (Console と同じ束)、OnShutdown で Shutdown
  - /home/user/MyEngin/src/Engine/Core/Localization/LocalizationTable.inl — 21 エントリ (Win_Terminal ほか Term_*) 両言語
  - /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp — "terminalwindow" 登録
  - /home/user/MyEngin/build/Editor.vcxproj, Editor.vcxproj.filters — 6 ファイルを BEGIN/END FILES 区画とフィルタへ (gen_project_files.ps1 の並び = Rel の昇順で手挿入)
  - /home/user/MyEngin/tools/check_rules.ps1 — 規則 21 (Engine/Runtime/GameLogic/Shared/Server から Editor/Tools/Terminal/ の include を禁止)、/home/user/MyEngin/tools/check_rules_selftest.ps1 — 規則 21 の違反・許容フィクスチャ
  - /home/user/MyEngin/engine_spec.md — §9.4 Terminal window (既知の制約 5 点を含む)、/home/user/MyEngin/docs/test_checklists.md — M106 節 (Windows 確認項目)
技術解説:
  - 技術: ConPTY の出力 (VT バイト列) を libvterm のセル画面で受け、ImGui の ImDrawList へ自前描画する (sub-01〜03 の積み上げの最上段)。窓は「描画ラン」への変換を挟む — 1 行のセル列を (a) 背景矩形ランと (b) 文字列ランへ畳み、行ごとに AddRectFilled と AddText を数回呼ぶだけにする。80x24 でも 1920 回の AddText ではなく、同じ見た目の ASCII をまとめた数本になる。
    ・背景ラン: 既定背景色 (テーマ色) と同じ区間は作らず (窓が全面を 1 回塗る)、同色の隣接セルを 1 枚へ連結。全角は 2 セルぶんを 1 回数える。反転 (reverse) はここで前景/背景を入れ替えて解決済み RGB で返す。
    ・文字ラン: 等幅フォントのときだけ「見た目が同じ ASCII」を 1 本にまとめる (ASCII の送り幅 == セル幅なので桁が合う)。全角・結合文字は常に 1 セル 1 本で、列 × セル幅の座標へ置く — 日本語フォントの送り幅はセル幅の 2 倍ちょうどとは限らず、まとめると以降の桁がずれるため。等幅フォントが無い場合は全セル 1 本ずつ (桁優先)。線の無いランの末尾空白は閉じるときに落とす。
    ・採用しなかった案: 毎セル AddText (呼び出し数が桁数倍で、ImGui の頂点バッファが膨らむ)、全角もまとめて描く (桁ずれ)、フォントの測定幅でセルを決めずに固定ピクセル幅にする (DPI/サイズ変更で崩れる。実際は CalcTextSize("M").x をセル幅にして、ASCII ランの自然な送りと一致させている)。
  - 技術: 選択は「絶対行」座標の読む順範囲 — 絶対行 = 画面の行 + そのときのスクロールバック行数。新しい出力でスクロールバックが 1 行増えると、同じ内容は同じ番号のまま (画面の行は 1 小さくなり、増えた分と相殺)。上限で古い行を捨て始めると 1 行ずつ上へずれる (許容、§9.4 に記載)。矩形選択ではなく端末の通常選択と同じ「始点の列から行末、中間の行は全部、終点の行は終点の列まで」。コピーは TerminalLineText の列範囲版を行ごとに呼び、行末空白を落として LF で結ぶ。全角の右半分から選択が始まったら左半分から含める。
  - 技術: ImGui の入力の取り方 — (1) 窓に ImGuiWindowFlags_NoNavInputs を付ける: NavEnableKeyboard が全窓で有効なため、付けないと矢印・Tab・Enter・Space を ImGui のナビゲーションが消費して子へ届かない。(2) フォーカス中に g.PlatformImeData.WantTextInput = true を立てる: EndFrame が次フレームの io.WantTextInput へ写し、EditorApp::HandleShortcuts (:1973) が WantTextInput を見て Ctrl+S / Delete / F などを抑止する (既存の抑止機構へ乗り、EditorApp の分岐は増やしていない)。(3) ImGuiFocusedFlags_NoPopupHierarchy: 設定ポップアップの入力欄へ打った文字が子へ流れないようにする。(4) 特殊キーは IsKeyPressed (リピート付き) → TerminalSendKey、Ctrl+英字 は IsKeyPressed(A..Z) → ClassifyTerminalCtrlLetter (Ctrl+C は選択ありならコピー、無ければ ^C)、文字は io.InputQueueCharacters (IME の確定文字もここへ来る。未確定は WM_CHAR にならない) を SendChar。ImGui は Enter/Tab/BS/Ctrl+英字の制御文字も文字として積むので 0x20 未満と 0x7F は捨てる (キー経路で送るため二重送信になる)。Alt+英数字は Win32 バックエンドが WM_SYSCHAR を積まないのでキーから作る。AltGr (Ctrl+Alt) は文字として来るので Ctrl 経路へ入れない。
  - 技術: 遅延起動・再起動 — TerminalWindow は startAttempted_ を持ち、最初に面が描かれたフレームで StartSession する (失敗しても毎フレーム再試行しない)。再起動は session_.reset() (デストラクタが Close(false) = 子孫ごと終了) と startAttempted_ = false だけで、同じフレームの DrawSurface が新しい TerminalSession を作る。これにより設定のシェル・スクロールバック (構築時固定) が毎回読み直される。「設定したシェルが無い」は ResolveTerminalShell の結果を shellError_ に持ち、セッションを作らず状態行にエラー (自動検出へは落とさない)。
  - コード: TerminalLayout.cpp:BuildTerminalRowRuns — 入力: 1 行のセル配列・列数・TerminalPalette・asciiRunsOnGrid。処理: 左から col を進め、width 0 は飛ばし、背景 → 文字の順にランへ畳む。出力: TerminalRowRuns (backgrounds / texts)。状態は RunBuilder (開いている 1 本だけ) と openIsAscii。呼び出し元は TerminalWindow::DrawSurface で、毎行 1 回、ランのバッファは窓のメンバで使い回す。
    (抜粋)
    const bool plain = IsPlainAscii(cell) || (blank && span == 1);
    if (asciiRunsOnGrid && plain && builder.IsOpen() && openIsAscii && SameTextStyle(builder.Current(), cell, colors.fg)) {
        builder.Extend(span, cell);
    } else {
        builder.Open(col, span, cell, colors.fg);
        openIsAscii = asciiRunsOnGrid && plain;
        if (!openIsAscii) { builder.Close(); }   // 全角・結合文字は 1 セル 1 本
    }
  - コード: TerminalWindow.cpp:DrawSurface — 入力: 描画領域 (残りの内容領域) と設定のフォントサイズ。処理: PushFont(EditorMonospaceFont(), 設定px) → cellW = CalcTextSize("M").x、cellH = ceil(行の高さ) → TerminalGridFromPixels で行列 → 未起動なら StartSession、Resize (変化時のみホストへ) → InvisibleButton でマウス (ホイール=履歴、左ドラッグ=選択、右クリック=貼付/メニュー) → focused なら HandleKeyboard → 行ごとに GetCell でセルを集めてランへ → 背景・選択・文字・下線/取り消し線 → カーソル (フォーカス中は塗り、無ければ枠、点滅なし)。状態は TerminalWindow のメンバ (session_・selection_・viewOffset_・grid_・popupWasOpen_) で、UI スレッド専用。呼び出し順: EditorApp::OnImGui の browser_.OnImGui の直後で Pump → OnImGui。
    (抜粋)
    const auto absRowOf = [&](int row) { return row - viewOffset_ + scrollback; };
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int row = absRowOf(mouseCell.row);
        selection_ = TerminalSelection{ false, row, mouseCell.col, row, mouseCell.col };   // クリックだけなら前の選択を消す
        dragging_ = true;
    }
    if (dragging_) { /* 押している間 head を更新し、start != head で active */ }
  - コード: TerminalWindow.cpp:HandleKeyboard — 入力: フォーカス中の ImGui キー/文字キュー。処理: kKeyBindings の表と F1〜F12 を IsKeyPressed → session_->SendKey (Shift+PageUp/Down は履歴のページ送りに回して子へ送らない)、Ctrl+英字は ClassifyTerminalCtrlLetter の結果で Copy / Paste / SendChar、Alt+英数字は ESC 前置の SendChar、文字キューは制御文字を捨てて SendChar。何か送ったら最下部へ戻し選択を解く。出力: TerminalSession 経由で子へのバイト。Pump (毎フレーム) が応答の返送も担う。
    (抜粋)
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if (c < 0x20 || c == 0x7F) { continue; }   // 制御文字はキー経路で送り済み
        session_->SendChar(static_cast<char32_t>(c), kTerminalModNone);
        sent = true;
    }
  - コード: TerminalWindow.cpp:Pump / ImGuiTheme.cpp:SetupEditorFonts — Pump は session_->Pump() が 1 バイト以上投入したら viewOffset_ = 0 (spec 4.1 の「新規出力で最下部へ」)。等幅フォントは候補表 {Fonts\CascadiaMono.ttf, %LOCALAPPDATA%\Microsoft\Windows\Fonts\CascadiaMono.ttf, Fonts\consola.ttf} を最初に読めたもので採用し、直後に日本語連鎖を MergeMode で重ねる。ImGui 1.92 の動的アトラスなので窓は PushFont(font, px) でサイズを任意に変えられる (GetFontSize() ではなく DPI 前の px を渡す。EditorHeadingFont と同じ作法)。
  - 選ばなかった案 (全体): (a) 設定を別ウィンドウ/別画面にする — Browser 窓が窓内で設定を持つ流儀に合わせ、ツールバーの「設定...」ポップアップにした。(b) キー入力に InputText を使って IME を任せる — 子へ 1 文字ずつ届く構造と相性が悪く、Enter/Tab/矢印を InputText が食う。(c) ポップアップ開閉の次のフレームに入力を捨てずに済ませる — Esc でポップアップを閉じた同じ押下が子への ESC になる (ヘッドレス試験で実際に再現してから popupWasOpen_ ガードを入れた)。(d) 描画をセルごとの dirty 管理にする — 計測前の最適化。まず行ごとのラン化だけにした (AGENTS 3.5)。
  - 前提・落とし穴: スレッド — 窓・セッション・画面はすべて UI スレッド専用 (読み取りスレッドは TerminalHost 内のキューだけ)。寿命 — session_ の所有者はこの窓で、窓を閉じても生きる (spec)。EditorApp::OnShutdown の terminal_.Shutdown() が子孫ごと終わらせる (Job の KILL_ON_JOB_CLOSE)。Pump は表示状態に関係なく毎フレーム呼ぶこと (窓の OnImGui の中に入れないこと。入れるとタブを隠した瞬間にキューが 4MB で詰まり子が止まる)。ABI/決定性 — Editor 層のみ、src/Shared・Interop.cs・シミュレーションは不変で、規則 21 が include を機械検査。性能 — 1 フレームの GetCell 呼び出しは cols x rows 回 (200x50 で 1 万回)、ランのバッファは使い回し。計測はしていない。選択の絶対行はスクロールバック上限到達後に 1 行ずつずれる。貼付 (SendPaste) は同期 WriteFile で、子が読まない状態の巨大貼付は UI を止めうる (上限は設けていない)。ImGui の変更点 — window flags NoNavInputs によりこの窓内のボタンはマウス専用。imgui_internal.h を include するのは PlatformImeData (IME の位置と WantTextInput) のためだけ。
仕様との差分:
  - [未実装] ツールバーの「VS 環境の読み込み状態」(spec 4.3) — VS 環境の実処理は sub-05 のため表示する状態が無い。設定のチェックボックス (terminalLoadVsDevEnv) は値を保存するだけで、sub-05 まで効果が無い
  - [未実装] 子プロセスの環境変数 (MYE_PROJECT_ROOT / MYE_TERMINAL_ID / TERM_PROGRAM / COLORTERM / PATH 前置) — sub-05 の範囲として TerminalLaunchSpec.environment を空 (親環境をそのまま継ぐ) で起動している。COLORTERM が無いため、sub-05 まで truecolor を宣言しない CLI は 256 色になる
  - [追加] TerminalSession::ClearScrollback と TerminalScreen の AppendTerminalUtf8 公開・TerminalLineText 列範囲版 — spec 4.3 のツールバー「クリア」と選択コピーに必要。sub-02/03 のコードへの最小の追加 (既存関数の動作は不変)
  - [追加] Shift+PageUp/PageDown で履歴をページ送り、Ctrl+Shift+C/V、履歴表示中の右上タグ「履歴 +N」、ポップアップ開閉直後 1 フレームの入力破棄 — いずれも操作上の穴を埋めるため (spec に無い)
  - [解釈] 右クリック: 選択なし = 貼付、選択あり = コピー/貼り付けのメニュー。spec 4.1 の「Ctrl+V / 右クリックは貼付」と「Ctrl+C / コンテキストメニューでコピー」を両立させる最小の解釈
  - [解釈] 設定 UI は窓内の「設定...」ポップアップ (既存のエディタ設定画面ではなく、同じ流儀の Browser 窓に合わせた)。editor_global.json への保存は変更時のみ
  - [追加] 新規則の番号は 21、SelfTest スイート名は "terminalwindow" (Fast、純関数 61 項目)
  - [逸脱] 窓クラス本体 (TerminalWindow) の自動テストはリポジトリに入れていない — ConPTY ホストの生成が自由関数のため偽物へ差し替える継ぎ目が無い。代わりに Linux で ImGui (バックエンド無し) + 偽ホストのヘッドレス駆動を作って確認した (スクラッチパッド上。後述の検証)
検証:
  - g++ 13 -std=c++20 -Wall -Wextra で TerminalLayout / TerminalLayoutSelfTest / TerminalScreen / TerminalSession / TerminalSessionSelfTest / TerminalWindow → 警告 0。x86_64-w64-mingw32-g++ -Wall -Wextra -Wshadow -fsyntax-only (-DNOMINMAX -DWIN32_LEAN_AND_MEAN、Windows.h 中継ヘッダ) で TerminalWindow.cpp / TerminalLayout.cpp / TerminalLayoutSelfTest.cpp / TerminalSession.cpp / TerminalScreen.cpp / TerminalSessionSelfTest.cpp / SelfTestSuites.cpp / EditorGlobalSettings.cpp / ImGuiTheme.cpp → 警告 0
  - Linux ネイティブで terminalwindow (61) + terminalcore (118) + terminalsession (50) を libvterm とリンクして実行 → 全 PASS、-fsanitize=address,undefined で報告なし、-fsanitize=thread で報告なし
  - 変異試験 (TerminalLayout.cpp を 6 通りに壊して terminalwindow を実行) → 全て 1〜3 項目 FAIL (選択の向き正規化・行末空白・ASCII 連結・背景連結・切捨て/切上げ・代替画面スクロール)
  - ヘッドレス窓駆動: imgui 1.92.8 (外部/imgui の実物、NavEnableKeyboard + Docking 有効) + 偽 TerminalHost + 実 TerminalWindow.cpp / TerminalSession / 画面モデルで、等幅あり・なしの 2 構成 × 26 項目 → 全 PASS (ASan/UBSan でも報告なし)。確認した経路: 遅延起動 (pwsh 自動検出・作業ディレクトリ)、フォーカス中の io.WantTextInput、入力なしフレームでの DA 応答返送、文字・Enter (制御文字の二重送信なし)・矢印・IME 確定文字の UTF-8・Ctrl+C (選択なし=ETX / あり=コピー)・Alt+B・Ctrl+V (LF→CR)、ドラッグ選択 → Ctrl+C のクリップボード内容、窓リサイズ → ホストの Resize、子終了後の無出力、設定ボタン → ポップアップ中の入力が子へ行かない・Esc で閉じても子へ ESC が行かない (初回で 1 件 FAIL を出して popupWasOpen_ を入れた)、再起動ボタン、シェル設定が無いとき起動しない・Unsupported は 1 回だけ試す・窓を閉じても Pump が出力を消費する
  - LocalizationTable.inl: 自作スクリプト (check_rules 規則 10-b 同等: MYE_STR の en/ja 整合・### 右辺の一意・書式指定子の並び。python3 -I) → 2775 エントリ 0 エラー。意図的に壊した複製 (### 重複・書式差・### 不一致) では 3 エラーを検出 (スクリプト自身の検査)。Localization.cpp のコンパイル (static_assert で表の件数一致) も上のヘッドレス駆動に含まれて通る。規則 10-a 相当の正規表現を新規ファイルへ当てて 0 件
  - check_rules 規則 21 の正規表現: grep -P で既存 src の Engine/Runtime/GameLogic/Shared/Server → 0 件 (誤検出なし)、Editor 内の include はヒットするが規則は上位 4 層だけを見る。違反フィクスチャ (引用/山括弧/../ 付き) は HIT、Browser パス・TerminalLike・コメント内は miss。他の規則 (1/2/7/8/16 相当の grep) を新規ファイルへ当てて 0 件。改行は新規ファイルに CR なし
  - build/Editor.vcxproj と .filters: python xml.dom.minidom で整形式、git diff は追加行のみ (vcxproj +6、filters +18)
  - 未実行 (Windows 環境なし): MSVC ビルド (Debug/Release)、`tools\gen_project_files.ps1` の再生成との差分照合 (手挿入の並びは Sort-Object の既存並びに倣った)、`tools\check_rules.ps1` と `check_rules_selftest.ps1` の実行 (pwsh なし。正規表現は grep -P で同等確認のみ)、`Editor.exe --selftest` / `--selftest-full`、実フォント (Cascadia / Consolas) の読込と桁、IME の候補窓の位置・確定の挙動、実 ConPTY、画面のキャプチャ (Claude Code CLI・git log・ビルド出力・日本語表示)。EditorApp.cpp は DirectXMath.h が無く MinGW で構文確認できていない (配線の差分は目視のみ)
自己採点 (1-5):
  仕様適合: 3 — 窓・描画・入力・選択・貼付・リサイズ・再起動・エラー表示・設定・ローカライズ・規則 21・文書は揃えたが、ツールバーの VS 環境状態は sub-05 待ち、右クリック/設定 UI は解釈を入れた
  正しさ: 3 — 描画ランと選択・寸法の純関数は ASan/TSan・変異試験つきで検証し、窓の入力経路も実 ImGui + 偽ホストで実走したが、MSVC ビルド・実フォント・実 ConPTY・IME・実画面は一度も見ていない (見た目と桁ずれは未確認)
  コード品質: 4 — 純関数を TerminalLayout に分け窓は配線に徹した。Win32 型は窓に出ない。fake bold / 固定 3 行ホイール等の割り切りはコメントと §9.4 に記載。EditorApp の分岐は既存機構 (WantTextInput) に乗せた
  テスト: 3 — 純関数 61 項目 (Fast スイート登録済み) + セッション 1 項目は回帰として入れた。窓クラス自体の回帰テストはリポジトリに無く (ヘッドレス駆動は一時物)、Windows の実行は未実施
不安・質問:
  - [planner] 「新規出力で最下部へ戻る」を文字通り実装 (1 バイトでも出力があれば戻る)。Claude Code CLI のようにスピナーを出し続ける CLI では、その間ホイールで履歴を読めない。スクロールバックの行数が増えたときだけ戻す (または戻らず「履歴 +N」を保つ) 案に仕様を変えるか
  - [planner] ツールバーの「VS 環境の読み込み状態」は sub-05 で足す前提でよいか。terminalLoadVsDevEnv のチェックボックスは今は保存だけ (表示を一時的に隠す選択肢もある)
  - [planner] 子の環境変数 (COLORTERM=truecolor / TERM_PROGRAM=MyEngine ほか) を sub-05 へ送った。Claude Code の色の受け入れ (条件 7) は sub-05 完了後の再確認になる
  - [planner] 貼付サイズの上限を設けていない (SendPaste は同期書込)。上限や分割書込みが要るか
  - [planner] ConPTY/ターミナルの採用理由 (ADR) は未作成。M105 には ADR-040 がある。作るか
  - 実機で見てほしい点: (1) 実フォントでの桁と全角の描画位置 (2) Escape・矢印が ImGui に食われず Claude Code CLI に届くか (3) IME の候補窓の位置 (4) 右クリックの挙動 (5) 太字 (1px 重ね描き) の見た目
触ったファイル:
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayout.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayout.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayoutSelfTest.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalLayoutSelfTest.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalScreen.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalScreen.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSession.h
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSession.cpp
  - /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalSessionSelfTest.cpp
  - /home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.h
  - /home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.cpp
  - /home/user/MyEngin/src/Editor/App/EditorApp.h
  - /home/user/MyEngin/src/Editor/App/EditorApp.cpp
  - /home/user/MyEngin/src/Editor/App/EditorGlobalSettings.h
  - /home/user/MyEngin/src/Editor/App/EditorGlobalSettings.cpp
  - /home/user/MyEngin/src/Editor/SelfTest/SelfTestSuites.cpp
  - /home/user/MyEngin/src/Engine/Renderer/ImGui/ImGuiTheme.h
  - /home/user/MyEngin/src/Engine/Renderer/ImGui/ImGuiTheme.cpp
  - /home/user/MyEngin/src/Engine/Core/Localization/LocalizationTable.inl
  - /home/user/MyEngin/build/Editor.vcxproj
  - /home/user/MyEngin/build/Editor.vcxproj.filters
  - /home/user/MyEngin/tools/check_rules.ps1
  - /home/user/MyEngin/tools/check_rules_selftest.ps1
  - /home/user/MyEngin/engine_spec.md
  - /home/user/MyEngin/docs/test_checklists.md
  - /home/user/MyEngin/plans/m106-integrated-terminal/sub-04.md (実装メモ節のみ)
申し送り:
  - sub-05: TerminalWindow::StartSession (/home/user/MyEngin/src/Editor/Windows/Project/TerminalWindow.cpp) の TerminalLaunchSpec.environment が空 (親環境を継ぐ)。VS 環境差分・MYE_PROJECT_ROOT・MYE_TERMINAL_ID・TERM_PROGRAM・COLORTERM・PATH 前置はここで BuildTerminalEnvironment を通して入れる。VS 環境の読み込み待ち (30 秒) 中は窓が起動を保留する表示が要る。ツールバーの「VS 環境の状態」表示も sub-05 で足す (Localization の Term_* に追加)。設定値は TerminalWindow の settings_.loadVsDevEnv に入っている
  - sub-06: AI Changes 窓の「統合ターミナル」表示は sub-04 の範囲外。MYE_TERMINAL_ID の採番 (エディタ PID-起動時刻-連番) は窓ではなく sub-05 の環境組立で行う想定
  - Windows 確認: docs/test_checklists.md の M106 節。`MYE_SELFTEST_ONLY=terminalcore,terminalsession,terminalwindow,terminalconpty` で `bin\x64\Debug\Editor.exe --selftest-full`、`tools\check_rules.ps1`、`tools\check_rules_selftest.ps1`
  - 検証用スクリプト・ログ: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/sub04/ (win/win_driver.cpp = ヘッドレス窓駆動、build_all.sh / driver_all.cpp = 純関数+セッション、check_loc.py = ローカライズ表の 10-b 同等検査、run_*.log)
```
- round 1: VERDICT OK (planner)。実装メモと原文は書式差のみで一致。[未実装]2件 (VS 環境状態表示・子環境変数) は sub-05 に明示移管。[追加]/[解釈] 承認。[逸脱] 窓クラスの回帰テスト無しは、ホスト生成の注入点を作る価値 < コストとして今回は承認 (純関数 61 + ヘッドレス駆動で代替)。スクロール追従は spec 4.1 を変更し sub-05 で実装。貼付上限は設けない (spec 7 に記録)。ADR は sub-05 で作成。Windows 確認は test_checklists M106 節。
