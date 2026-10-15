# review-1: m87-mcp-server

- 対象コミット範囲: 32d1e1e206dde87d6e21a0d9f10516397bb0b903..HEAD (54f22f1 M87a, 75d3004 M87b, bc8f5d4 M87c, de38970 M87d)
- 日付: 2026-10-15

```
REVIEW: PASS
round: 1
軸 (1-5):
  製品の深度: 4 — 受け入れ条件の外側も試したが壊れなかった。実索引で固定 10 問を kind の絞り込みなしで流しても 10/10。絵文字だけの q は 0 件で正常に返る。text の 65536 / 65537 バイト境界、title 120 / 121 文字 (日本語)、latest=0、本文に偽の `## <時刻>` を入れる、NUL・ESC を含む本文、の挙動は spec と docs のとおりだった。複数プロセスでの同時起動 (#6) と、エスケープが多い本文で 64 KB の上限を超える件 (#4) が残る
  機能性: 4 — 自分で実行して確かめた条件: 1 / 2 / 3 / 6 / 7 / 8 / 9〜11 / 12 / 13 / 18 / 19 / 20 / 22〜27 / 29。すべて期待どおり。15 / 30 / 31 (実クライアント) と 21 (chcp) は再実行していない。coder の実測記録を読んだだけ
  ビジュアルデザイン: 4 — GUI は無いので、JSON の出力と docs で見た。summary・isError の文面・docs の表は読みやすい。ただし docs\test_checklists.md の 1 項目が改行で崩れている (#1)
  コード品質: 4 — transport・handler・道具が分かれている。依存は serde と serde_json だけ。規則 15 で、子プロセス起動とファイル書き込みを決まったファイルに閉じ込めている。ただし 15-d に抜け道がある (#2)。Cargo.toml のコメントが書く「なぜ」が実装と合っていない (#3)
指摘:
  1. [minor] 宛先: coder — docs\test_checklists.md の M87 節で、パスの `\n` が改行文字になって行が割れている。sub-03 の `\r` 混入と同じ種類の誤り — 根拠: docs/test_checklists.md:722-723 が「確認後に `.mye」で切れ、次の行が「otes\m87-check.md` は手で消してよい」で始まる (od -c で `.mye` の直後が `\n` (0x0A) であることを確認) — 期待: `.mye\notes\m87-check.md` の 1 行に戻す
  2. [minor] 宛先: coder — 規則 15-d が `std::fs::File::options()` (中身は OpenOptions と同じ) を見逃す。notes.rs と verify.rs 以外のファイルでも、ファイルの切り詰めや上書きを書けてしまう — 根拠: tools\mcp\src の一時コピーの search.rs 末尾に `std::fs::File::options().write(true).truncate(true).open("x")` (748 行)、`fs::File::options()...` (749 行)、`std::fs::write(...)` (750 行) を足して `check_rules.ps1 -McpSourceDir <copy>` を回すと、ERROR になるのは 750 行だけ。パターンは tools/check_rules.ps1 の Get-McpWritePattern にあり、`File\s*::\s*(create|create_new)` だけを見ている — 期待: `File\s*::\s*options` も禁止パターンに入れる (OpenOptions を許可されている notes.rs だけで使えるようにする)
  3. [minor] 宛先: coder — Cargo.toml のコメントは「panic = "unwind" なら、1 件の道具呼び出しの panic でサーバ全体は落ちない」と読めるが、crate のどこにも `catch_unwind` が無い。メインスレッドで panic すれば unwind でもプロセスは終わる — 根拠: tools/mcp/Cargo.toml:8-9。`grep -rn catch_unwind tools/mcp/src` は 0 件。main.rs のループは handler::handle をそのまま呼んでいる — 期待: main.rs で `handle` を `std::panic::catch_unwind` で包み、panic した 1 件は -32603 (または isError) で返して続ける。またはコメントを事実に合わせる
  4. [minor] 宛先: coder — note_read の 64 KB の上限は、見出しと本文の生のバイト数で数えている。実際の応答では content[0].text (JSON 文字列) と structuredContent に同じ内容が 2 回入り、JSON のエスケープも掛かるので、応答は 64 KB を大きく超える — 根拠: 一時プロジェクトで `note_append {topic:"q", text: "\"" x 65536}` の後に `note_read {topic:"q"}` を呼ぶと、応答 1 行が 393,403 B。`truncated` は false。tools/mcp/src/notes.rs:342-357 の budget は `e.heading.len() + e.text.len()` で数えている。Windows のパス (`\`) を多く含む普通のメモでも、モデルに見える text は 2 倍近くになる — 期待: 上限を JSON に直した後の長さで数える。または spec と docs の「64 KB」を「生の本文で 64 KB」と書き直す (spec 4.1.6 の意図を planner が決めるなら planner 宛てに替える)
  5. [minor] 宛先: coder — docs\mcp-server.md は「(Debug の `Editor.exe` が先にビルドされている必要がある)」と書くが、gen_engine_index.ps1 が先に探すのは Release の Editor.exe で、無いときだけ Debug を使う — 根拠: docs/mcp-server.md のビルド節。tools/gen_engine_index.ps1 の `foreach ($cfg in 'Release', 'Debug')`。spec 4.1.2 も「Release (無ければ Debug)」 — 期待: 「Release か Debug の Editor.exe」に直す
  6. [minor] 宛先: planner — verify_job が止めるのは、同じプロセスの中での同名ジョブの二重起動と 3 本目だけ。MCP サーバの別プロセスとの重なりは見ていない。docs には「走行中のジョブは MCP が終わっても止まらない」とある。そのため、MCP を再起動した直後や、Claude Code のセッションを 2 つ開いた状態で同じ名前のジョブを起動すると、2 本目も通る。2 本は同じ `cache\mcp_logs\<name>.log` と `cache\golden*.rep` を奪い合う。docs の注意書きは「全体実行と同時に叩かない」しかない — 根拠: tools/mcp/src/verify.rs の JobTable はプロセス内の Vec。check_admission は自分の表しか見ない。docs/mcp-server.md の「注意」節 — 期待: v1 で許容するなら、docs の注意書きに「MCP の再起動直後と、複数のセッションからの同名ジョブ」を足す。止めたいなら、ログを排他で開く (またはロックファイル) という手段を spec に足す
  7. [minor] 宛先: planner — spec.md の変更履歴の最終行に、CR (0x0D) が生のまま入っている (`\r` の誤りを説明した箇所そのもの) — 根拠: plans/m87-mcp-server/spec.md:234 (Python でバイトを走査し、行内に `\r` があることを確認) — 期待: `\r` を文字で書く
検証した手段:
  - `cargo test --manifest-path tools\mcp\Cargo.toml` → 47 passed
  - `cmd /c tools\mcp_verify.bat` → 15 シナリオすべて PASS、exit 0
  - `pwsh -NoProfile -File tools\check_rules.ps1` → ERROR 3 件 (rule 1 EditorMain.cpp:741、rule 14-b engine_dist の 2 件) = 既知 3 件だけ
  - `pwsh -NoProfile -File tools\gen_engine_index.ps1` → 352,368 B、abi 178 / components 81 / fields 932。続けて `mcp_verify.ps1 -RealIndex` → 10/10、exit 0。同じ 10 問を kind の絞り込みなしで直接流しても 10/10
  - `Editor.exe --dump-index` を Debug で 2 回、Release で 1 回 → 3 本とも SHA256 A56D43A8...95C1 で一致。書けないパス (Z:\nope\x.json) では exit 1。components 81、LocalTransform.position = Float3
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0。出力に「EngineIndexDump (M87a) self test: ALL PASS」(9 項目)
  - 実ジョブ (scratch の drive.py。modern の `_meta` 付きで mye_mcp を cwd = ルートから起動): demo (ticks 60) → done / exitCode 0 / PASS (ログに "60 ticks" があり MYE_RV_TICKS が届いている)。rules → exitCode 1 / FAIL で、errors に既知 3 件。同名の二重起動、3 本目 (busy)、`demo & calc`、未知の jobId → いずれも isError。stdout の非 JSON 行は 0
  - legacy の initialize (2025-06-18 の echo) から search / describe (component / field / cli / ADR) を実索引で呼んだ。notes の境界値は一時プロジェクトで試した (上記と #4)
  - 規則 15-d の抜け道の再現 (#2)。一時コピーを scratch に置き、作業ツリーには何も足していない
  - diff を読んだ範囲: src/ (C++ 全部)、tools/mcp/src (main / stdio / rpc / handler / tools / index / notes / verify は全部、search は前半)、tools/check_rules.ps1 の規則 15、tools/gen_engine_index.ps1、tools/mcp_verify.ps1 の -RealIndex、docs/mcp-server.md、ADR-032、test_checklists、計画書の差分、.gitignore、.mcp.json、build/*.vcxproj (Startup* の 5 行は CRLF に揃い直っただけで、内容は変わっていない)
  - 未実施: 実クライアント (claude CLI) での接続 (条件 15 / 30 / 31)、chcp の前後比較 (条件 21)、15-a / 15-b / 15-e の違反コピーでの ERROR の再確認 (coder と planner の実測記録だけを見た)
前回指摘の消込: (round 1 のためなし)
```
