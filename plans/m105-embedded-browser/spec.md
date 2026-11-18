# M105 エディタに外部ブラウザを埋め込む — 仕様書

- slug: m105-embedded-browser
- 状態: 確定 (2026-11-18、planner 裁定。ユーザーへの直接質問なしの運用。`[ユーザーに聞ける]` の論点は司会が Notion で確認し、逆の答えが来たら 8. 変更履歴に積んで直す)
- 依頼原文: エンジン上で任意のブラウザをセットし起動・普通に使えるようにしたい

## 1. 目的 (なぜ作るか)

エディタで作業しながら、資料・リファレンス・AI チャット・タスク管理などの Web ページを**エディタの外へ切り替えずに**見たい・操作したい。
しかも「普段使っているブラウザそのもの」(拡張機能・ログイン状態・IME・ショートカット) がそのまま効く必要がある。
達成したい状態は次のとおり。

- エディタのドック可能な「ブラウザ」窓の中で、インストール済みのブラウザ (どれかを選べる) が普段どおり動く。
- ドックの移動・リサイズ・タブ切替・最小化に窓が追従し、エディタのメニューやポップアップを隠さない。
- ブラウザ側が閉じても落ちても固まっても、**エディタは巻き込まれない** (固まらない・落ちない・状態を失わない)。
- エディタを閉じても、ブラウザの窓 (タブ・入力中のフォーム) を失わない。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| D1 | 既に同等機能があるのでは (二重実装) | エンジン内の Web 関連は `ShellExecuteW` で既定ブラウザに URL を渡す 2 箇所だけ (`src/Editor/App/EditorApp.cpp:991`、`src/Editor/Update/UpdateWindow.cpp:64`)。`external/` にブラウザ系ライブラリ無し | — | 二重実装ではない。新設する |
| D2 | **別プロセスのトップレベル窓を SetParent で子にするのは Microsoft が勧めていない** | Raymond Chen「Is it legal to have a cross-process parent/child or owner/owned window relationship?」(https://devblogs.microsoft.com/oldnewthing/20130412-00/?p=4683): 親子関係は両スレッドの**入力キューを暗黙に結合**し (推移的)、自覚の無いプロセスの窓を子にすると "things will definitely stop working" とある。SetParent の公式解説 (https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setparent) も WS_POPUP→WS_CHILD の付け替え・UISTATE の同期・DPI 認識の不一致での強制リセットを注意している | 方式 A (SetParent 埋め込み) は 2026-11-18 にユーザーと司会で合意済み。蒸し返さない | **採用 (ユーザー判断)。反対意見を記録する**: 入力キュー結合により「エディタが長いフレームで止まる間はブラウザの入力も止まる」「ブラウザの UI スレッドが止まるとエディタからの同期操作が待たされる」危険が残る。緩和策を 4.4 に置き、**sub-01 を実現性の試作 (スパイク) にして撤退条件を決める** (6. sub-01)。撤退時の代案は D3 |
| D3 | 撤退時の代案 | 親子にも所有関係にもしない「ドック矩形に毎フレーム追従する独立トップレベル窓」なら入力キューは結合しない (所有関係も結合する: Raymond Chen 2011-03-31 の記事。記憶による要約・本文未確認)。代わりに Z 順と最小化の同期が自前になり、Alt+Tab に別窓として出る | 未確認 | **今回は作らない**。sub-01 の撤退条件に当たったら司会がユーザーに「代案へ切り替えるか」を聞く `[ユーザーに聞ける]` |
| D4 | 既存エンジンの類似機能 | UE の Web Browser plugin / UMG Web Browser widget、Unity の Asset Store の埋め込みブラウザは CEF のオフスクリーン描画 (テクスチャ化) が主流 (検索結果: https://unrealcommunity.wiki/web-browser-widget-13f406、https://cefview.github.io/UCefView/ucefview_overview.html。Epic 公式文書は取得できず = 一部は記憶による)。OSR なら airspace 問題は起きないが、拡張機能・普段のプロファイルは使えない | WebView2・CEF・CDP 画面配信は不採用 (ユーザー明示「WebView 以外で」) | 大手と違う方式になる理由 = 「普段のブラウザそのもの (拡張・ログイン・IME) を使いたい」。この差の代償が airspace (4.1.5) と入力キュー結合 (D2) |
| D5 | 起動した exe のプロセス ID で窓を特定できるか | Chromium 系・Firefox は既に起動中なら新プロセスが既存プロセスへ依頼して即終了し、窓は**既存プロセス**が作る。Opera GX はレジストリの `...\Opera GX\opera.exe` がランチャーで、実体は `...\Opera GX\139.0.6053.63\opera.exe` (この PC で確認) | — | PID では特定しない。**起動前後のトップレベル窓の差分**で特定し、窓の所有プロセスの実行ファイルが「登録 exe と同じディレクトリ配下 + 同じファイル名」のものに絞る (4.1.3)。却下: 専用 `--user-data-dir` で別プロセスを強制する案 (拡張・ログインが使えず目的に反する)、`--app=URL` (タブ・アドレスバー・拡張ボタンが消え「普段どおり」でない) |
| D6 | 普段のプロファイルか専用プロファイルか | 目的 (1.) が「拡張・ログインがそのまま効く」 | — | **普段のプロファイル**。専用プロファイルは作らない。既に開いている普段の窓とタブを共有するのは仕様 |
| D7 | IEXPLORE.EXE を候補に出すか | この PC のレジストリに `IEXPLORE.EXE` がある。IE は Windows 11 では退役済みで、起動すると Edge に転送される (記憶による。未確認) = 起動した exe と窓の所有プロセスが一致せず特定に失敗する | — | **候補から外す** `[ユーザーに聞ける]` |
| D8 | 候補の重複 | HKLM と HKLM\WOW6432Node に Brave / Chrome / Edge / IE が同じ exe で二重登録 (この PC で確認) | — | 実行ファイルの正規化パス (大文字小文字無視) で重複除去。HKCU → HKLM → WOW6432Node の順で先勝ち |
| D9 | 「任意のブラウザ」はレジストリ検出だけで足りるか | ポータブル版や StartMenuInternet に登録しないブラウザは検出できない | — | 候補の末尾に「その他 (exe を指定)」を置き、ファイルダイアログで exe を選べるようにする。起動引数は exe 名から推定 (4.1.2) |
| D10 | airspace: 子 HWND は ImGui より常に手前に出る | ImGui は `ViewportsEnable` 無効 (`src/Engine/Renderer/ImGui/ImGuiRenderer.cpp:28`) = メニュー・ポップアップ・浮動窓・モーダルは全部メイン HWND の中に描かれ、子 HWND の下になる | — | **重なる間は子窓を隠す** (4.1.5)。却下: `SetWindowRgn` で穴を開ける (他プロセスの窓の領域を書き換え、ブラウザ側が戻す可能性があり脆い)、隠している間に `PrintWindow` で撮った静止画を出す (D3D テクスチャ経路が要り重い → 後回し) |
| D11 | フォーカスの取り合い | 子窓がフォーカスを持つとキー入力は子窓へ行き、メイン窓をクリックしても `DefWindowProc` はフォーカスを戻さない (記憶による。sub-01 で観測) | — | ブラウザがフォーカス中はキーは全部ブラウザへ (エディタのショートカットは効かない)。**エディタの ImGui 領域でマウスボタンを押したらメイン窓へ `SetFocus`** で戻す (4.1.6) |
| D12 | 入力キュー結合でエディタが固まる | D2。`SetWindowPos` の `SWP_ASYNCWINDOWPOS` は「異なる入力キューのとき」だけ非同期になる (https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos) = 結合後は効かない。`IsHungAppWindow` は 5 秒 PeekMessage しない窓を応答なしと見る (https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-ishungappwindow) | — | 表示切替は `ShowWindowAsync`、閉じるは `PostMessage(WM_CLOSE)`、位置合わせは**矩形が変わったときだけ** `SetWindowPos` し、毎回直前に `IsWindow` / `IsHungAppWindow` で弾く。`SendMessage` 系の同期呼び出しはブラウザ窓へ投げない (4.4)。エディタ側が長く止まる間ブラウザの入力も止まるのは**既知の制限**として受け入れる |
| D13 | DPI の差 | SetParent 公式: DPI 認識モードが違うと cross-proc で「子窓のプロセスを強制リセット」。エディタは Per-Monitor V2 (`src/Engine/Platform/Win32Window.h` `EnsureProcessDpiAwareness`) | — | 埋め込み前に `GetWindowDpiAwarenessContext` + `AreDpiAwarenessContextsEqual` で比べ、違えば**埋め込まずに外に置いたまま**理由を出す |
| D14 | パネルの × とエディタ終了で窓をどうするか | 閉じるとタブ・入力中のフォームを失う。エディタのクラッシュ時に子窓がどうなるかは未確認 (sub-01 で観測) | — | **切り離して普通の窓に戻す** (元のスタイル・画面上の同じ位置)。閉じるのは「閉じる」ボタンを押したときだけ `[ユーザーに聞ける]` |
| D15 | 設定の置き場所 | 個人設定は `<project>\.mye\editor_settings.json` (`src/Editor/App/EditorSettings.h`)、マシンローカルは `%LOCALAPPDATA%\MyEngine\editor_global.json` (`src/Editor/App/EditorGlobalSettings.h`) | — | **editor_global.json** (EditorGlobalSettings)。インストール済みブラウザはマシンの性質で、プロジェクトごとに違う理由が無い。project_settings.json (チーム共有) には置かない |
| D16 | 複数のブラウザ窓 | タブはブラウザ自身が持つ | — | パネルは 1 つ、埋め込む窓は 1 つ `[ユーザーに聞ける]` |
| D17 | 起動時にブラウザを自動で立ち上げるか | 自動化起動 (`--selftest` / `--screenshot` 等) で外部プロセスを起こしてはいけない (M98d / M104j と同じ流儀: `EditorApp::allowAutoUpdateCheck` / `allowChangeTracking`) | — | **自動起動しない**。パネルが開いていても「起動」を押すまで何もしない `[ユーザーに聞ける]` |
| D18 | `--screenshot` で検証できるか | `--screenshot` はスワップチェーンのバックバッファを保存する (`src/Engine/Engine/Loop/EngineLoop.cpp:3420` `SaveBackbufferPng`) = 子 HWND は写らない | — | パネルの UI は `--screenshot`、埋め込んだブラウザの絵は**デスクトップのキャプチャ** (画面座標の `CopyFromScreen` 等) で検証する |
| D19 | Play 中にブラウザへ打ったキーがゲームに入らないか | エンジンの入力は `WM_KILLFOCUS` でキーを離す (`src/Engine/Platform/Input.cpp:112`)。生マウス (`WM_INPUT`) は `RIDEV_INPUTSINK` 無しで登録 (`Input.cpp:217`) — 子窓がフォーカス中も前景扱いで届くかは未確認 | — | キーは入らない想定 (sub-02 で手動確認)。生マウスのデルタが入るかを sub-01 で観測し、入るなら Game ビューのマウスルック中だけの問題として 7. に記録 |
| D20 | ブラウザ側の全画面 (F11・動画) / Ctrl+N / タブのドラッグで外へ | 子窓のままモニタ全面に広がろうとする。新しい窓は普通のトップレベル窓として外に出る | — | 全画面は**非対応** (エディタが矩形を合わせ直す)。新しい窓・切り出したタブは外に出たままで取り込まない (「既存の窓を取り込む」で後から入れられる) |
| D21 | 権限 (UIPI) の差 | 管理者権限のエディタから普通権限のブラウザ窓は SetParent できない (記憶による) | — | 失敗を `GetLastError` 付きでログし、窓は外に置いたまま理由を出す |
| D22 | 自動検証で実ブラウザが無い (CI) | CI にはブラウザが無い | — | 実プロセス間の埋め込み・切り離し・消滅・応答なしは **Editor.exe 自身をダミー窓プロセスとして起動**して selftest で通す (4.5)。実ブラウザはこの PC で検証フラグ + デスクトップキャプチャ + 手動 |

## 3. スコープ

- やる:
  - インストール済みブラウザの検出 (StartMenuInternet: HKCU / HKLM / HKLM\WOW6432Node) と「その他 (exe を指定)」
  - 選んだブラウザを普段のプロファイルで新しい窓として起動し、その窓を特定して「ブラウザ」パネルの子窓にする
  - 既に開いているブラウザの窓を選んで取り込む (自動特定に失敗したときの逃げ道を兼ねる)
  - ドック追従 (移動・リサイズ・タブ切替・最小化)、airspace の回避 (重なる間は隠す)、フォーカスの戻し
  - 切り離す / 閉じる / ブラウザ側で閉じた・落ちた・応答なしの検出と表示 / エディタ終了時の切り離し
  - 設定の保存 (選んだブラウザ・exe・開始 URL)、ローカライズ (ja/en)
  - Editor 層への閉じ込めの機械検査 (check_rules 規則 20)、ADR-040、engine_spec §9.3、test_checklists
- やらない (明示的に外したもの):
  - Runtime / ゲーム内でのブラウザ (ユーザー合意)
  - WebView2・CEF・CDP 画面配信とそのフォールバック (ユーザー合意)
  - 専用プロファイル (D6)、`--app` モード (D5)、IE (D7)
  - 複数パネル・複数の埋め込み窓 (D16)、起動時の自動起動 (D17)
  - ブラウザの全画面 (D20)、ブラウザとエディタ間のデータ連携 (URL の受け渡し以外)
- 後回し:
  - 隠している間に静止画 (`PrintWindow`) を出す (D10)
  - 撤退時の代案「追従する独立トップレベル窓」(D3。撤退条件に当たったときだけ)

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 ブラウザの候補 (BrowserCatalog)

- 読む場所: `HKCU\SOFTWARE\Clients\StartMenuInternet` → `HKLM\SOFTWARE\Clients\StartMenuInternet` → `HKLM\SOFTWARE\WOW6432Node\Clients\StartMenuInternet` の順。各サブキー:
  - id = サブキー名 (例 `Google Chrome`、`Vivaldi.4DSJJDYTNGWYGQXBYF3HH6Y5U4`)
  - 表示名 = サブキーの既定値 (空ならサブキー名)
  - exe = `shell\open\command` の既定値から実行ファイルパスだけを取り出す (先頭が `"` なら次の `"` まで、そうでなければ最初の `.exe` まで。後ろの引数・`%1` は捨てる)
  - 既定ブラウザの印 = `HKCU\Software\Microsoft\Windows\Shell\Associations\UrlAssociations\https\UserChoice` の `ProgId` が、そのサブキーの `Capabilities\URLAssociations\https` と一致するもの
- 除外: exe が存在しないもの、exe のファイル名が `iexplore.exe` のもの (D7)。
- 重複除去: exe の正規化パス (大文字小文字無視) が同じなら先に読んだもの (HKCU → HKLM → WOW) を残す。
- 並び: 表示名の大文字小文字無視の昇順 (同名は exe パスで)。末尾に「その他 (exe を指定)…」。
- **この PC での期待結果**: Brave / Google Chrome / Microsoft Edge / Opera GX Stable / Vivaldi の 5 件 (+ その他)。
- 検出は**パネルを開いたとき**と「再検出」ボタンで行う (毎フレーム読まない)。

#### 4.1.2 起動 (BrowserHost::Launch)

- 引数の系統を exe のファイル名 (小文字) で決める:
  - Chromium 系 (`chrome.exe` / `msedge.exe` / `brave.exe` / `vivaldi.exe` / `opera.exe` / `chromium.exe` 等の表): `--new-window` + (開始 URL があれば) URL
  - Firefox 系 (`firefox.exe` / `librewolf.exe` / `waterfox.exe` / `floorp.exe` 等の表): `-new-window` + URL (URL が空なら `-new-window` のみ)
  - 不明: URL があれば URL だけ
- `CreateProcessW` で起動 (作業ディレクトリ = exe のディレクトリ、窓は普通に表示)。起動したプロセスのハンドルはすぐ閉じる (D5: 窓の持ち主と一致しない)。
- 起動の**直前**に、可視・所有者なしのトップレベル窓の HWND 集合を控える (snapshot)。

#### 4.1.3 窓の特定 (純関数 + ポーリング)

- 起動後 100 ms ごとにトップレベル窓を列挙し、次を全部満たすものを候補にする:
  1. snapshot に無い HWND
  2. `IsWindowVisible`、所有者なし (`GetWindow(GW_OWNER) == nullptr`)、`WS_EX_TOOLWINDOW` でない
  3. 窓の大きさが 200 x 150 px 以上 (起動途中の小窓・スプラッシュを除く)
  4. 所有プロセスの実行ファイル (`QueryFullProcessImageNameW`) のファイル名が登録 exe と同じで、かつ**登録 exe のディレクトリの配下** (大文字小文字無視。Opera GX の版ディレクトリ対策 D5)
- 候補が 1 つ以上 → Z 順で最上位のもの (列挙順の先頭) を採る。採ったら 4.1.4 へ。
- 15 秒たっても候補が無い → `TimedOut`。起動したブラウザの窓は外にあるまま。パネルは「見つけられなかった。既存の窓を取り込むで選べる」を出す。
- 判定ロジック (1〜4 と選び方) は Win32 を呼ばない純関数に切り出し、selftest で通す。

#### 4.1.4 埋め込み (Attach) と切り離し (Detach)

- Attach の最後に `GetParent(child) == main` を確かめる。違えば (ブラウザ側が親を外した。Opera GX で観測) 元のスタイルへ戻し、親がデスクトップでなければ `SetParent(child, nullptr)` して `Failed` (理由「ブラウザが埋め込みを受け付けませんでした」)。
- Attach の順:
  1. `IsWindow` / `IsHungAppWindow` / DPI 認識の一致 (D13) を確認。だめなら `Failed` (理由付き) で外に置いたまま。
  2. 元の `GWL_STYLE` / `GWL_EXSTYLE` / 画面上の矩形 / 最大化状態を控える。
  3. スタイルから `WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX` を外し `WS_CHILD` を足す (**SetParent の前**。公式の注意どおり)。最大化中なら先に元のサイズへ戻す。
  4. `SetParent(child, editorMainHwnd)`。失敗は `GetLastError` を付けて `Failed`、手順 3 のスタイルを戻す。
  5. `SetWindowPos(... SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOZORDER)` でパネルの内容矩形 (メイン窓のクライアント座標) へ合わせる。
  6. UISTATE を同期する (`WM_CHANGEUISTATE` を Post。SetParent 公式の注意)。
- Detach の順: `SetParent(child, nullptr)` → 元のスタイルへ戻す (SetParent 公式: 親を NULL にした**後**で WS_CHILD を外し WS_POPUP 等を戻す。sub-01 round 1 で修正) → 画面上の**今の位置** (パネルがあった場所) に控えたサイズで置き直し、表示・前面化。応答なしなら Detach をしない (同期で待たされるため。D12)。
- エディタのメイン窓には埋め込み時に `WS_CLIPCHILDREN` を足す (Editor 層から `SetWindowLongPtrW`。Runtime には影響しない)。要否は sub-01 で観測し、不要なら足さない (結果を ADR-040 に残す)。

#### 4.1.5 追従と表示切替 (airspace)

- パネルは毎フレーム「内容矩形」(ツールバー行の下、メイン窓のクライアント座標・px) を記録する。**全 ImGui 窓の提出が終わった後** (OnImGui の末尾) に BrowserHost へ渡して適用する (メニュー・ポップアップがその前に積まれていないと重なりを判定できないため)。
- 子窓を**見せる**条件 = 次を全部満たす:
  1. パネルがこのフレーム描かれた (Begin が true、ドックのタブが選ばれている、折り畳まれていない)
  2. 内容矩形の幅・高さが 1 px 以上
  3. メイン窓が最小化されていない
  4. ImGui のマウスによる窓の移動・ドッキングのドラッグ中でない (`g.MovingWindow == nullptr` 相当)
  5. モーダルが開いていない
  6. パネルより手前 (ImGui の表示順で後ろ) に描かれる窓のうち、パネル自身とその子・ツールチップ・**ToastCenter の通知**を除くものが内容矩形と重なっていない (メインメニュー・ポップアップ・コンボ・コンテキストメニュー・浮動窓を含む)。通知は入力を取らない一時表示なので、重なった部分はブラウザの下に隠れてよい (通知のたびにブラウザが消える点滅を避ける。sub-02 round 1 で変更)。除外の識別は窓の名前の文字列一致ではなく、ToastCenter が自分の窓を識別できる形で渡す (ImGuiID の集合・専用フラグ等。方法は coder)
- 判定は「矩形の配列 + フラグ → 見せる/隠す」の純関数にして selftest で通す。ImGui 内部 (`imgui_internal.h`) の読み出しはパネル側の薄い層に閉じる。
- 見せる/隠すは `ShowWindowAsync` (状態が変わったときだけ)。位置・大きさは矩形が変わったときだけ `SetWindowPos(SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER)`。ブラウザが自分で大きさを変えた (全画面・最大化・最小化ボタン) ときは合わせ直すが、**1 秒あたり 10 回まで** (取り合いで点滅させない)。最小化・最大化された子窓は元に戻してから合わせる。
- 隠れている間、パネルの内容矩形には「メニュー表示中は隠れています」等の 1 行と枠を描く。

#### 4.1.6 フォーカスと入力

- ブラウザをクリック → ブラウザがキーボードフォーカスを持つ (ブラウザ自身の挙動)。この間エディタのショートカット (`ShortcutHub`) は効かない (キーはブラウザへ行く)。これは仕様。
- エディタのメイン窓のクライアント領域でマウスボタン (左・右・中・X) が押されたとき、フォーカスが**埋め込んだ子窓かその子孫**にあるときだけ `SetFocus(main)` で戻す (他の正当なフォーカス先 (ダイアログ等) を奪わない。sub-02 round 1 の coder の逸脱を採用)。Editor が `Win32Window::AddMsgHandler` で登録する (メッセージは消費しない = ImGui にもそのまま届く)。
- 子窓を隠すとき、フォーカスが子窓にあればメイン窓へ戻す (見えない窓にキーを打たせない)。
- パネルの「切り離す」「閉じる」ボタンを押したときも同様にメイン窓へ戻す。

#### 4.1.7 状態機械 (BrowserHost)

```
Idle ──Launch──> Launching ──窓を特定──> Embedded ──Detach/パネル×/エディタ終了──> Idle (窓は外で生きている)
  │                 │ 15 s                 │ 閉じる → PostMessage(WM_CLOSE) → (消えたら) Closed
  │                 └──> TimedOut          │ IsWindow == false (ブラウザ側で閉じた) → Closed
  │                                        │ 所有プロセスが終了 (落ちた) → Lost
  └──AttachExisting(hwnd)──> Embedded      │ IsHungAppWindow → hung 印を立てる (状態は Embedded のまま。戻れば消す)
                                           │ GetParent(child) != main (ブラウザ側が親を外した) → 元のスタイルへ戻して Failed (理由)
                                           └ Attach 失敗 → Failed (理由)
Closed / Lost / TimedOut / Failed ──「起動」/「既存の窓を取り込む」──> Launching / Embedded
```

- 「閉じた」(Closed) と「落ちた」(Lost) の区別: 窓が消えた後、所有プロセスが**0 以外の終了コード**で終了していれば Lost。0 で終了した、または猶予 `kBrowserGoneGraceMs` (400 ms) の間に終了しなければ Closed (猶予は TerminateProcess 直後に窓が先に消えて終了コードが遅れる競合のため。WM_CLOSE で正常終了したプロセスを Lost と誤判定しないため。sub-01 round 1 で修正)。
- 親の喪失: 毎フレーム `GetParent(child)` (同期メッセージではない) を見て、メイン窓でない状態が `kBrowserParentLostConfirmMs` (150 ms) 続いたら確定し、元のスタイルへ戻して Failed。WS_CHILD のままデスクトップに取り残さない。150 ms の確定は、強制終了で壊れかけの窓が `IsWindow` が偽になる直前に一瞬 `GetParent` を空にする実測があり、Lost を Failed と誤判定しないため (sub-01 round 2)。Attach 直後の確認は確定時間なし。応答なしの窓は触らずに Failed だけにする (R12)。
- `BrowserHost::Update` はパネルが描かれないフレームでも毎フレーム呼ぶ (上の確定時間と Closed / Lost の観測がフレームごとの呼び出しに依存する)。
- 観測 (窓の有無・応答なし・プロセスの生死・時刻) を引数で受け取る遷移関数を純関数として切り出し、selftest で通す。Win32 を叩く部分は観測を集めて遷移関数へ渡すだけにする。
- 毎フレームの観測コストは `IsWindow` + `IsHungAppWindow` + (ポーリング中のみ) 窓列挙。Launching 中の窓列挙は 100 ms 間隔。

#### 4.1.8 既存の窓を取り込む

- 選んだブラウザ (4.1.3 の 2〜4 を満たす、snapshot 条件なし) の今ある窓を「タイトル (PID)」で並べ、選んだものを Attach する。
- 自動特定が TimedOut のとき、および Idle のときに使える。

#### 4.1.9 エディタ終了・パネルの ×

- `EditorApp::OnShutdown` (メイン窓の `Destroy` より前。`src/Engine/Engine/Loop/EngineLoop.cpp:3632` → `:3659`) で Embedded なら Detach する (D14)。
- パネルの × (閉じる) も Detach。ブラウザを閉じるのは「閉じる」ボタンだけ。
- エディタがクラッシュしたときの子窓の運命は sub-01 で観測し、ADR-040 と 7. に記録する (エディタ側で対策できる範囲は無い)。

### 4.2 データ・保存形式・互換性

- `editor_global.json` (`EditorGlobalSettings`) にキーを足す (3 点セットの作法どおり、`root.value(key, default)` で前後方互換):
  - `browserId` (string): 選んだ候補の id。`"custom"` = その他 (exe 指定)。空 = 未選択 (既定ブラウザの印があるものを初期選択、無ければ先頭)
  - `browserCustomExe` (string, UTF-8): その他のときの exe パス
  - `browserStartUrl` (string, UTF-8): 開始 URL。空 = ブラウザの既定 (新しいタブ)
- 保存は変更したときだけ (毎フレーム書かない)。
- ABI・SimSnapshot・TypeId・シーン形式・imgui.ini 以外のプロジェクトファイルは**変更しない**。

### 4.3 UI / ビジュアル

- メニュー *Window > ブラウザ / Browser* でパネルを開く (既定は閉じている)。ドック可能な普通の ImGui 窓。
- Idle / Closed / Lost / TimedOut / Failed のとき (内容領域):
  - 「ブラウザ」コンボ (表示名、既定ブラウザには「(既定)」を添える、末尾に「その他 (exe を指定)…」) + 「再検出」
  - その他のとき: exe パスの表示 + 「参照…」(ファイルダイアログ。ダイアログの呼び出しは EditorApp 側のフック経由で、窓に `Windows.h` を持ち込まない)
  - 「開始 URL」入力欄 (空 = ブラウザの既定)
  - 「起動」ボタン、「既存の窓を取り込む」ボタン (選んだブラウザの窓の一覧ポップアップ)
  - 直前の結果の 1 行 (例「ブラウザが閉じられました」「ブラウザが終了しました (落ちた可能性があります)」「15 秒以内に窓が見つかりませんでした」「埋め込めませんでした: DPI 設定が違います」)
- Launching のとき: 「起動中… (n 秒)」と「中止」(待つのをやめるだけ。起動したブラウザは閉じない)。
- Embedded のとき: 上に 1 行のツールバー (ブラウザ名、応答なし表示、「切り離す」「閉じる」)、その下を子窓が埋める。
- 文字列はすべて `LocalizationTable.inl` + `Tr()`、ja/en 両方。窓のタイトルは `"Browser###Browser"` / `"ブラウザ###Browser"` の形。
- パネル UI の見た目は `--screenshot` で撮れる (検証フラグでパネルを開く。ブラウザは起動しない)。埋め込んだブラウザの見た目はデスクトップキャプチャ。

### 4.4 非機能

- **エディタを巻き込まない** (D2 / D12):
  - ブラウザ窓へ `SendMessage` などの同期メッセージを投げない (どうしても要るなら `SendMessageTimeout` + `SMTO_ABORTIFHUNG` + 短いタイムアウト)。表示は `ShowWindowAsync`、閉じるは `PostMessage(WM_CLOSE)`。
  - `SetWindowPos` / `SetParent` は直前に `IsWindow` と `IsHungAppWindow` を見て、応答なしなら呼ばない。
  - 1 フレームあたりの BrowserHost の処理は、ブラウザが応答なしの状態でも 50 ms を超えない (Full 層 selftest で計る)。
  - 既知の制限: エディタのフレームが長く止まる間 (同期のシーン読み込み・シェーダコンパイル等)、ブラウザの入力も止まる (入力キュー結合)。
- 決定論: シミュレーション・Replay・SimSnapshot に一切触れない。実時間 (タイムアウト・ポーリング間隔) は Editor の UI 状態にだけ使う。
- レイヤー: すべて Editor 層 (`src/Editor/`)。Engine / Runtime / GameLogic / Shared / Server から include しない (check_rules 規則 20)。窓クラス (`BrowserWindow.h`) に `Windows.h` を持ち込まない (HWND は `void*`、Win32 呼び出しは `.cpp` と BrowserHost に閉じる)。生の D3D 型は使わない。
- 自動化起動 (`--selftest` / `--screenshot` / replay 等) でブラウザを起動しない。ブラウザを起動するのは「起動」ボタンと、明示した検証フラグ (`--browser-embed-probe`) だけ。
- Unicode: exe パス・URL・窓タイトルは wide で扱い、保存は UTF-8。日本語を含むユーザー名のパスで動く。
- DPI: エディタと同じ Per-Monitor V2 のブラウザだけ埋め込む (D13)。

### 4.5 検証用の口

- `Editor.exe --browser-list`: 候補の一覧 (id / 表示名 / exe / 系統 / 既定の印) をログに出して終了 (exit 0)。エンジンを初期化しない早期分岐。
- `Editor.exe --browser-dummy-window [--browser-dummy-hang-after-ms N]`: selftest 専用。タイトル固定の普通のトップレベル窓を 1 枚出してメッセージを回すだけのプロセス。N ms 後に UI スレッドを 30 秒止める (応答なしの再現用)。エンジンを初期化しない早期分岐。**寿命の上限を持つ** (既定 120 秒で自分で終了。テストの失敗経路や親プロセスの異常終了で残っても Editor.exe を掴み続けない。sub-02 round 1 で追加)。
- `Editor.exe --browser-embed-probe <id> [--browser-probe-url URL] [--browser-probe-hold-ms N]`: 通常起動したエディタでパネルを開き、id のブラウザを起動して埋め込み、観測 (特定までの時間、`GetParent`、スタイル、矩形一致、DPI) をログに出し、N ms (既定 5000) 保持してから Detach して終了。exit 0 = 埋め込みと切り離しが成功。デスクトップキャプチャは外側のスクリプト (PowerShell) で保持中に撮る。
- selftest:
  - `browserembed` (Fast): カタログの解析・重複除去・並び・除外、exe 抽出、引数の系統、窓特定の純関数、表示切替の純関数、状態機械の遷移、設定の往復。
  - `browserembedproc` (Full): ダミー窓プロセスを起動し、差分で特定 → テスト用の親窓へ Attach → 矩形・`GetParent`・`WS_CHILD` を確認 → Detach で元のスタイル・親なし・可視を確認 → 再 Attach → ダミーの終了で Lost / WM_CLOSE で Closed を 1 秒以内に検出 → hang 版で応答なしの印と 1 フレーム 50 ms 以内を確認。

## 5. 受け入れ条件

1. **候補の解析**: `browserembed` selftest で、HKCU/HKLM/WOW の重複、引用符あり・なし・引数付き・`%1` 付きの command、`iexplore.exe`、存在しない exe、日本語を含むパスの入力に対し、重複除去・並び・除外・exe 抽出・既定の印が 4.1.1 どおり。— `MYE_SELFTEST_ONLY=browserembed Editor.exe --selftest`
2. **この PC の実カタログ**: `Editor.exe --browser-list` が Brave / Google Chrome / Microsoft Edge / Opera GX Stable / Vivaldi の 5 件を出し、IE を出さない。— ログを reviewer が読む
3. **窓の特定**: `browserembed` selftest で、snapshot 差分・版ディレクトリ配下の exe (Opera GX 型)・不可視/所有者あり/ツール窓/小窓の除外・複数候補で Z 順先頭・別ブラウザの窓を拾わない、が 4.1.3 どおり。
4. **状態機械と表示切替**: `browserembed` selftest で、4.1.7 の全遷移 (Closed と Lost の区別、TimedOut、Failed、hung 印の立ち下がり) と 4.1.5 の見せる条件 1〜6 (ツールチップは除外、パネル自身の子は除外) が期待どおり。
5. **実プロセス間の埋め込み**: `browserembedproc` (Full) で 4.5 の手順が全部 PASS (Attach / 矩形 / Detach の復元 / Lost・Closed の 1 秒以内検出 / 応答なしの印と 1 フレーム 50 ms 以内)。— `MYE_SELFTEST_ONLY=browserembedproc Editor.exe --selftest`、および `Editor.exe --selftest-full` 全体 PASS
6. **実ブラウザの埋め込み (この PC)**: Brave / Google Chrome / Microsoft Edge / Vivaldi で `--browser-embed-probe <id>` が exit 0、保持中のデスクトップキャプチャでパネル矩形の中にブラウザの絵が写っている。Chrome か Edge の少なくとも 1 つは「既にそのブラウザが起動している」状態でも成功する (D5 の吸収問題)。Opera GX は**ベストエフォート**: 埋め込めなかったときに Failed + 理由で終わり、窓が元のスタイルの普通の窓として画面に残る (WS_CHILD のまま取り残されない) こと (sub-01 round 1 で変更)。— 画像のパスを SELF_EVAL / review に残す
7. **普段どおり使える (手動)**: Chrome と Edge で、アドレスバーへの日本語入力 (IME)・ページのスクロールとクリック・拡張機能のボタンのポップアップ・右クリックメニュー・新しいタブが動く。— `docs/test_checklists.md` の M105 節を実施した記録
8. **追従 (手動 + スクショ)**: ドックの移動・リサイズ・タブ切替 (非アクティブで消え、戻すと出る)・エディタの最小化と復元・浮動化に子窓が追従する。— test_checklists + デスクトップキャプチャ
9. **airspace (手動)**: メインメニュー・コンテキストメニュー・コンボ・モーダル・パネルに重なる浮動窓・ドッキングのドラッグの間、子窓が隠れてメニュー類が見える。閉じると戻る。— test_checklists
10. **フォーカス (手動)**: ブラウザでキー入力した後、Hierarchy をクリックして Ctrl+S を押すとシーンが保存される (ブラウザのページ保存にならない)。子窓が隠れたときフォーカスがエディタへ戻る。Play 中にブラウザで打ったキーでゲームが動かない。— test_checklists
11. **後始末**: 「切り離す」・パネルの ×・エディタの正常終了で、窓は元のスタイルの普通の窓として画面に残り、タブが失われない。「閉じる」で窓が閉じる。ブラウザ側で窓を閉じる → Closed、ブラウザを `taskkill /F` → Lost の表示になり、エディタは動き続ける。— `browserembedproc` + test_checklists
12. **設定**: `browserId` / `browserCustomExe` / `browserStartUrl` が editor_global.json に往復し、キーが無い古いファイルは既定値で読める (selftest)。再起動後に選んだブラウザと URL が復元される (手動)。
13. **ローカライズ**: 追加した UI 文字列は全部 `Tr()` 経由で ja/en が埋まっている。— `tools\check_rules.ps1` 0 error (既存のローカライズ検査)、`--screenshot` で ja/en 両方のパネルを撮って reviewer が見る
14. **レイヤー**: check_rules 規則 20 (Engine / Runtime / GameLogic / Shared / Server から `Editor\Tools\Browser\` を include しない) を足し、`check_rules_selftest.ps1` に違反フィクスチャを足して検出できる。`BrowserWindow.h` が `Windows.h` を include しない。— `tools\check_rules.ps1` 0 error、`tools\check_rules_selftest.ps1` PASS
15. **既存への影響なし**: `Editor.exe --selftest-full` 全体 PASS (Debug)。シミュレーションに触れないので `replay_verify` は必須にしない (最終サブで 1 回回して結果を記録する)。
16. **文書**: ADR-040 (方式・却下案・入力キュー結合の制限・sub-01 の観測結果・クラッシュ時の挙動)、engine_spec §9.3、test_checklists の M105 節が揃っている。

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | 候補の検出と埋め込みの中核 (実現性の試作を兼ねる) | なし | 1, 2, 3, 4 (状態機械・表示切替の純関数), 5, 6, 14 | `M105a: ブラウザの検出と別プロセス窓の埋め込みの中核を足す` |
| sub-02 | ドック可能な「ブラウザ」窓 (追従・airspace・フォーカス・設定) | sub-01 | 4 (適用側), 7, 8, 9, 10, 12, 13 | `M105b: エディタにドック可能なブラウザ窓を足す` |
| sub-03 | 後始末と障害時の表示、取り込み、文書 | sub-02 | 11, 15, 16 (+ 5 の Lost/Closed/応答なしの表示側) | `M105c: ブラウザ窓の後始末と障害時の表示を仕上げ、ADR-040 を足す` |

- sub-01 は**撤退条件付き** (sub-01.md 参照)。撤退条件に当たったら coder は実装を広げず SELF_EVAL の「不安・質問」に観測結果を書いて返す。planner は D3 の代案への切替を `[ユーザーに聞ける]` で司会に上げる。

## 7. 未決事項・リスク

- R1 (D2/D12): 入力キュー結合の副作用。sub-01 でエディタの長いフレーム中にブラウザ入力が止まること、ブラウザの UI スレッド停止がエディタを止めないことを観測して ADR-040 に記録する。
- R2: Chromium 系の窓を `WS_CHILD` にしたときの挙動。sub-01 で描画は Chrome / Edge / Brave / Vivaldi で確認 (タブストリップとキャプションボタンはブラウザ自身の描画のまま子窓の内側に出る)。**キー入力・IME は未確認** (自動の SendInput は埋め込まない単体の Chrome にも届かず検証手段として成立しなかった) → 人の手で確認するまで撤退条件 E1 は未判定。sub-02 の着手はこの確認待ち。
- R3: フリップモデルのスワップチェーン (`DXGI_SWAP_EFFECT_FLIP_DISCARD`、`src/Engine/Renderer/Device/SwapChain.cpp:40`) を持つ親窓の上の子窓の合成: **確認済み** (sub-01 / sub-02 で Brave / Chrome / Edge / Vivaldi の描画と、メニューで隠して戻す動作をキャプチャで確認)。
- R4 (D19): 子窓がフォーカス中に生マウスデルタ (`WM_INPUT`) がエディタに届くか。sub-01 の自動計測は基線が無く結論なし → 人の手の確認 (probe の `raw_input` 計数) に回す。届くなら Play 中のマウスルックが動く可能性 → 対処は別件。
- R5 (D14): **確認済み (sub-01 O7)**: エディタを強制終了すると、埋め込み中の窓は道連れで破棄される (ダミーは窓だけ消えてプロセスが残り、Chrome は最後の窓だったのでプロセスごと終了。タブは失われる)。エディタ側に対策は無い。ADR-040 と engine_spec の制限に書く。
- R6 (D20): ブラウザの全画面・自分でのリサイズとの取り合い。1 秒 10 回の上限で点滅を抑えるが、見た目の合否は手動確認。
- R7: Opera GX: ランチャーは `--new-window` を本体へ渡し、版ディレクトリ配下の判定で特定できた (sub-01 O9)。ただし埋め込みは初回のみ成功し、その後 3 回連続でブラウザ側が親をデスクトップへ戻した (原因未特定)。ベストエフォート扱いにし、親の喪失を検出して安全に外へ戻す (4.1.4 / 4.1.7)。
- R9: Attach は 1 回限りの同期処理で 52〜337 ms (Debug)。1 秒 (E2) には達しないが、そのフレームは引っかかる。
- R10: `IsHungAppWindow` は 5 秒無応答で初めて真になるので、ブラウザが止まってから最初の 5 秒は SetWindowPos で待たされうる (矩形が変わったときだけ呼ぶので頻度は低い)。
- R13: 単一インスタンスの窓 (Chrome のプロファイル選択窓など) が開いていると、`--new-window` が新しい窓を作らず TimedOut になる (sub-02 で観測)。「既存の窓を取り込む」(sub-03) が逃げ道。TimedOut の結果文言で取り込みを案内する。
- R12: 応答なしの窓が親を外された場合は、同期で待たされるのでスタイルを戻さず Failed にして手放すだけ (WS_CHILD のまま取り残される可能性)。実機では未発生・未検証。
- R11 (O6): `WS_CLIPCHILDREN` 無しでも静止キャプチャでは欠けが無かったが、ちらつきは静止画で判定できない。安全側で付けたまま。要否は未決として ADR に書く。
- R8 (D7): IE を外したことの確認は Notion で `[ユーザーに聞ける]`。

## 8. 変更履歴

(確定後の変更のみ)

- 2026-11-18 sub-01 round 1 (coder SELF_EVAL):
  - 4.1.4 Detach の順序を SetParent 公式の注意どおり「SetParent(nullptr) → スタイル復元」に直した (spec 側の誤り)。
  - 4.1.7 Closed / Lost の判定を「0 以外の終了コードで Lost、0 か猶予 400 ms 内に終了しなければ Closed」に直した (旧記述は WM_CLOSE の正常終了を Lost と誤判定する。spec 側の誤り)。
  - 4.1.4 / 4.1.7 に「親の喪失」の検出と復元を足した (Opera GX がブラウザ側から親を外す観測による。spec の穴)。
  - 5-6 を Brave / Chrome / Edge / Vivaldi の 4 候補 + Opera GX はベストエフォート (安全に失敗) に変えた。
  - 7. に sub-01 の観測結果 (R2 / R4 / R5 / R7) と R9〜R11 を足した。
- 2026-11-18 sub-01 round 2 (coder SELF_EVAL): 4.1.7 に親の喪失の確定時間 150 ms (`kBrowserParentLostConfirmMs`) と「Update はパネル非表示のフレームでも呼ぶ」を書き足した (coder の [追加] を採用。強制終了の競合の実測による)。7. に R12 を足した。
- 2026-11-19 sub-02 round 1 (coder SELF_EVAL / planner): 4.1.5 条件 6 で ToastCenter の通知を重なり判定から除外、4.1.6 のフォーカス返しを「子窓かその子孫にフォーカスがあるとき」に限定 (coder の逸脱を採用)、4.5 のダミー窓に寿命の上限、7. R3 を確認済みに、R13 を追加。O4 は 2026-11-19 にユーザーが確認済み (Chrome / Edge で英字・日本語 IME を入力できた。E1 非該当)。
