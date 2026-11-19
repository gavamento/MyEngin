# REVIEW round 1 — m106-integrated-terminal

- 対象コミット範囲: 9e44a4bb69201beca557be445dae13290658f324..HEAD (412ddcf、ブランチ claude/nice-goodall-kwzmxi)
- 日付: 2026-11-19
- 検証環境: Linux コンテナ (g++ 13 / gcc / cargo / python3)。MSVC・Windows・Editor.exe・check_rules.ps1・ConPTY・実画面は使えない

```
REVIEW: FAIL
round: 1
軸 (1-5):
  製品の深度: 3 — 境界の外側を 6 系統突いた (UTF-8/CJK/絵文字の 1 バイト分割と不正バイト、リサイズ 0/負/300x200 とスクロールバック 0/5、環境の大小文字重複 Path/PATH・小文字 mye_terminal_id・"=C:"・非 ASCII 名・';' を含むルート・"C:\"、set 出力の目印欠落・値中の '='・空値、ID の接頭辞衝突 "142-" 対 PID 42、キューの満杯待ち中の Discard と並行順序)。いずれも期待どおりだったが、貼付本文に ESC[201~ を含めると bracketed paste を途中で閉じられる (指摘 1)。
  機能性: 4 — Linux で確かめられる受け入れ条件 1〜4・9 は再ビルドと再実行で PASS (terminalcore / terminalsession / terminalwindow / terminalvsenv は plain・ASan/UBSan・TSan すべて 0 failure、TSan は 10 回繰り返しても PASS。track の新規テスト 5 件 PASS、失敗集合は coder の報告と同じで、どれも Windows API 由来)。条件 5〜8・11 と 9 の実操作は Windows で要確認として合意済みの範囲で、未検証 (PASS の根拠にしていない)。
  ビジュアルデザイン: N/A — 描画は ImGui DrawList と D3D11 バックエンドを使うので、この環境では画像にできない。ヘッドレスの ImGui でも atlas と Windows フォント (Cascadia/Consolas/日本語) が無いため、見た目の評価にならない。Windows での目視 (test_checklists M106) に委ねる。
  コード品質: 4 — diff を全部読んだ。各 sub の「仕様との差分」に載っていない変更は見つからなかった (変更されたファイルはすべてどれかの sub で言及されている)。Engine 層の ImGuiTheme は等幅フォントの読み込みと getter を足しただけで、Editor への include や依存は無い (src/Engine から "Editor/" を include している箇所は 0)。規則 10/14/16/21 相当の検査は 0 件。指摘は minor 1 件。
指摘:
  1. [major] 宛先: planner — bracketed paste の本文を消毒していない。貼付文字列に ESC[201~ を含めると、端末が付ける終了マーカーより前で貼付が閉じ、その後ろの本文 (例 "rm -rf x\r") がシェルや Claude Code CLI にキー入力として届く (貼付を使った乗っ取り)。Windows Terminal や xterm は貼付本文から ESC を除くか置き換える。spec 4.1 / 受け入れ条件 3 は「200~/201~ で包む」としか書いておらず、本文の扱いが決まっていない。 — 根拠: /home/user/MyEngin/src/Editor/Tools/Terminal/TerminalInput.cpp:90-99 (本文は NormalizeTerminalPasteNewlines だけを通して AppendOutput する)。再現: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/edge.cpp で "\x1b[?2004h" を Feed したあと TerminalSendPaste(s, "echo safe\x1b[201~rm -rf x\r\n") を呼ぶと、出力は `\x1b[200~echo safe\x1b[201~rm -rf x\x0d\x1b[201~` (edge_plain.log の FAIL 行)。 — 期待: spec 4.1 に貼付本文の規則を決める (少なくとも bracketed paste が有効なとき ESC (0x1b) を除くか置き換える。無効なときに C0 制御文字をどう扱うかも決める)。そのうえで coder が TerminalSendPaste に入れ、terminalcore に回帰テストを足す。
  2. [minor] 宛先: coder — sub-01 の検証では「gcc -std=c99 -Wall で警告 0」とあるが、それは最適化なしの結果。-O1 では上流の parser.c:145 (string_start) と state.c:1881 (reply) に -Wmaybe-uninitialized が出る (計 16 行)。上流のコードを変えないこと自体は正しく、MSVC は TreatWarningAsError=false なので実害も無い。ただ、記録が条件を書いていないので、後から読む人が「警告 0」を一般の事実と誤読しうる。 — 根拠: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/vterm_warn.txt。 — 期待: external/VERSIONS.md か ADR-041 に「上流無改変。最適化ありでは maybe-uninitialized が出る」と 1 行残す (任意)。
検証した手段:
  - 読んだ範囲: 役定義、harness-protocol 3.4/4、AGENTS.md、spec.md、sub-01〜06.md の仕様との差分・検証、harness.md、`git diff 9e44a4bb..HEAD` の全ソース・docs・tools (external/libvterm は上流の同梱物なので一覧の確認だけ)。
  - 黙った差分の探索: 変更された全ファイル (libvterm と plans を除く) のファイル名を sub-0*.md と突き合わせ → 言及の無いものは 0 件。
  - C++ の再ビルドと実行: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/all.cpp (4 スイート) を g++ -std=c++20 -Wall -Wextra で plain・-fsanitize=address,undefined・-fsanitize=thread の 3 通りにビルド (警告 0) → すべて PASS。drv_tsan を 10 回繰り返しても PASS。plain の所要は 0.47 秒。
  - 境界の外側: /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/edge.cpp を plain・ASan/UBSan・TSan で実行 → 29 項目中 28 件 ok。FAIL は指摘 1 の 1 件のみで、サニタイザの報告は無し。
  - track: sub06 の shim を /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review1/shim へ複製し、現在の hook.rs / ops.rs / ops_record.rs / session.rs / schema / tests/storage.rs で上書きして cargo test を実行。shim と本物の差は Windows API の置き換え (winshim / winstubs と use 行 2 か所) だけであることを diff で確認した。新規テスト (hook 単体 2 件、session 2 件、storage の v3→v4 移行) は PASS。失敗は lib 2 / storage 1 / session 5 で、coder が報告した変更前からの失敗集合 (Windows のロック・パイプ・PID) と同じ。tests/hook.rs の e2e は未実行 (名前付きパイプと実 exe が要る)。
  - 移行と互換: db.rs:179-186 で DB の版がサービスより新しいときは db_degraded として書き込まないこと、protocol.rs:174-175 で null を欠落と同じに扱うこと (旧フック・null の hostTerminal が NULL になる) を確認。旧サービスに新フックが hostTerminal を送ると bad_args になる件は spec 7 に planner の裁定として記載済みで、合意の範囲内。
  - 規則の同等確認 (python/grep): 規則 10 (Tr を書式に使う 3 パターン → 0 件、check_loc.py で entries 2784 / errors 0、使われている StrId がすべて表にある、新しい Term_* に未使用が無い)、規則 14 (変更ファイルの git ls-files --eol がすべて i/lf)、規則 16 (Renderer の外の変更ファイルに ID3D/IDXGI/D3D11_/DXGI_ が 0 件)、規則 21 (Engine/Runtime/GameLogic/Shared/Server から Editor/Tools/Terminal を include している箇所が 0 件)。新しい .cpp/.h はすべて Editor.vcxproj と .filters に載っている。
  - スレッドと寿命: ConPtyTerminalHost::Close の順序 (Discard/Unbounded → ClosePseudoConsole → join → 解放、TerminalConPtyHost.cpp:186-201)、TerminalVsEnvLoader (shared_ptr で状態を共有し、破棄時は 2 秒待ってから切り離す。TerminalVsEnv.cpp:178-195)、EditorApp::OnShutdown での terminal_.Shutdown() を読んで確認。ImGui 1.92.8 で PlatformImeData.WantTextInput が次のフレームの io.WantTextInput へ写ること (imgui.cpp:6275、5684) を確認。
  - 未検証 (Windows で要確認。合意の範囲): MSVC での Debug/Release ビルド、check_rules.ps1 の実行、--selftest / --selftest-full、ConPTY の実動作と終了時に子孫が残らないこと、VsDevCmd の読み込み、Claude Code CLI の表示、AI Changes 窓の実表示、上流 libvterm の sha256 照合。
前回指摘の消込: (round 1 のため無し)
```
