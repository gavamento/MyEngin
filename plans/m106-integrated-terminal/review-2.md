# REVIEW round 2 — m106-integrated-terminal

- 対象: 9e44a4bb..HEAD (81fc2c5)。修正の中心は 21f000e (sub-02 round 2) と 3a8e9a2 (spec 対応)
- 日付: 2026-11-19

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — 修正範囲 (SanitizeTerminalPaste) の外側を突いた。20 万件のランダム入力 (ESC、[201~、CR/LF、DEL、0xC2+0x9B、NUL、^C、日本語・絵文字の断片、任意バイトの混在) で、出力に HT/CR 以外の C0・DEL・C2 80-9F が残らず、2 回通しても結果が変わらない。1MB の入力でも同じ。正しい UTF-8 (日本語・絵文字・°€é・NBSP C2 A0) は残る。2004 有効時に本文の ESC[201~ や 8 ビット CSI で貼付を抜けられない。残るのは planner が挙げた nit 2 件だけ。
  機能性: 4 — 受け入れ条件 3 (更新後) を Linux で確認した。4 スイートは plain と ASan/UBSan で PASS (警告 0)。review-1 の edge.cpp は 29/29 ok。Windows で確かめる条件は、round 1 と同じく合意の範囲で未検証。
  ビジュアルデザイン: N/A — round 1 と同じ理由 (D3D11 と Windows フォントが要り、この環境では画像にできない)。今回の修正は見た目を変えない。
  コード品質: 4 — 21f000e の diff を全部読んだ。変更は sub-02 round 2 の差し戻しに書かれた範囲 (TerminalInput、terminalcore のテスト、engine_spec §9.4、ADR-041、VERSIONS.md) と一致する。旧名 NormalizeTerminalPasteNewlines を参照する箇所は src・docs・engine_spec に残っていない。改行は LF (git ls-files --eol)。定数は名前付きで、コメントは理由を書いている。
指摘: なし (blocker 0 / major 0 / minor 0)。申し送り: planner の nit 2 件 (CR の直後に除去対象のバイトが来ると空行が 1 つ増える / 孤立した 0x80-0x9F が ConPTY で U+FFFD になるかは Windows で要確認) に同意する。新しい指摘は無い。
検証した手段:
  - git show 21f000e / 3a8e9a2 (spec 4.1・受け入れ条件 3・§7・§8 の差分)。
  - /tmp/claude-0/-home-user-MyEngin/a4d9b156-b135-54e6-b68d-c08729d36d62/scratchpad/review2/ で 4 スイート (all.cpp) を plain と ASan/UBSan で再ビルドして実行 → すべて PASS。edge.cpp → 29/29 ok。
  - fuzz.cpp (同じディレクトリ。seed は固定で 200000 件、冪等性、正しい UTF-8 の保持、NBSP、bracketed paste を画面経由で通す、1MB) を plain と ASan/UBSan で実行 → unclean=0、冪等でない例 0、他の項目もすべて ok。
  - 規則 10/16/21 に関わる変更は無い。規則 14 は変更した 3 ファイルとも i/lf w/lf。
  - 未検証 (Windows で要確認。合意の範囲): MSVC ビルド、--selftest、ConPTY で実際に貼付する動作。
前回指摘の消込:
  1. 解消 — 有効・無効どちらでも消毒されるようになった (TerminalInput.cpp:SanitizeTerminalPaste)。spec 4.1 と受け入れ条件 3 に規則が入った。edge.cpp の FAIL 行は ok になり、fuzz でも反例が出ない。
  2. 解消 — external/VERSIONS.md の libvterm 行に、-O1 以上で -Wmaybe-uninitialized が出ること (上流のまま) を注記した。
```
