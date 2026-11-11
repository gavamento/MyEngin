# review-2: m99-culling

- 対象: `3f97eb03..afdec5b5` (M99j: cull_verify を CI に載せる + review-1 #1 / #3 への対応)。round 1 の範囲 `d52e91f3..3f97eb03` の結果 (review-1.md) は引き継ぐ
- 日付: 2026-11-11

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — CI と同じ条件で cull_verify を手元で流した (cmd /D /E:ON /V:OFF /S /C "CALL ..."、MYE_CULL_SKIP_CONFIGS=1)。PASS の経路と、失敗の終了コードが step まで伝わる経路の両方を見た。CI の時間枠 (timeout-minutes 90) にも余裕がある。round 1 の minor #2 が残っている
  機能性: 5 — spec の受け入れ条件 15 を、手元で確かめられる範囲はすべて満たす (YAML の解析・step の位置・env・cmd での PASS・失敗時に exit 1)。CI 上での実走は観測できない (master の CI は M98e 以降 failure のまま。仕様どおり未検証として残っている)
  ビジュアルデザイン: 4 — round 2 では見た目は変わっていない。round 1 の #2 (枠の色が淡い) は未対応のまま
  コード品質: 4 — 3f97eb03..afdec5b5 では src / tools は 1 行も変わっていない (`git diff --stat -- src tools` が空)。文書 3 か所と ci.yml のコメントは、実装・既存の step の書き方と合っている。check_rules は 0 error
指摘:
  1. [minor] 宛先: coder — (round 1 #2 を持ち越し。未解決) デバッグ枠の色が名前付き定数の色で出ず、淡く化ける。明るい床の上では距離の青が見分けにくい — 根拠: review-1.md の指摘 2 と同じ (scratchpad\out\tag_dist_img.png の枠の画素 (175,214,229) 前後 / 定数は 0x3399FF)。afdec5b5 では src に変更なし — 期待: ユーザーの目視判断を待つ (sub-11 が切られたらそこで対応する)
検証した手段:
  - `git log 3f97eb03..HEAD` → afdec5b5 の 1 本だけ。作業ツリーで変わっているのは plans\m99-culling\harness.md だけ。`git diff 3f97eb03..afdec5b5 --stat -- src tools` → 空 (コードの変更なし)。ci.yml / engine_spec.md / ADR-038 / test_checklists / spec.md / sub-07.md の diff はすべて読んだ
  - ci.yml を `python yaml.safe_load` で読んだ: jobs.windows.steps の 10 番目 (0 始まりで 9) が `culling verify (...)`、その直前が `screenshot regression (...)`。`shell: cmd`、`env: {MYE_CULL_SKIP_CONFIGS: 1}`、`run: tools\cull_verify.bat`。同じジョブの前の step (replay_verify) が Release をビルドするので、前提は shot_verify と同じ
  - CI と同じ形で手元実行: `cmd /D /E:ON /V:OFF /S /C "CALL tools\cull_verify.bat"` を MYE_CULL_SKIP_CONFIGS=1、MYE_EXTRA_ARGS="--warp --no-audio" で実行 → `[PASS] culling verify (13 checks ...)`、exit 0、158 秒
  - 失敗の伝わり方: 同じ形で MYE_CULL_BASE_ROOT に存在しない場所を渡し、検査 4 を必ず失敗させた → `[FAIL] culling verify: 1 of 13 check(s) failed`、cmd の終了コード 1 (step は非 0 で落ちる)
  - `--no-audio`: tools\cull_verify.bat:50 の `set COMMON=--no-audio ...` を Runtime の起動すべて (:pair / :dump / :config_counts / 基点比較) が通ることを確かめた (review-1 で書いた行番号「:40」は誤りで、:50 が正しい)
  - CI の時間枠: 最後に緑だった run 41417552137 (2026-11-02) はジョブ全体で 33 分 24 秒、shot_verify の step が 2 分 40 秒。手元の WARP で 158 秒の step を足しても timeout-minutes 90 に収まる。同じ run では shot_verify (flow_title を含む) が CI 上で通っていたので、手元で既知の flow_title FAIL が CI の後続 step (cull_verify) を止めるかどうかは、CI が緑に戻るまで分からない (推論。CI 上では未観測)
  - `pwsh tools\check_rules.ps1` → 0 error / 60 warning (round 1 と同じ件数)
  - engine_spec §6.14 の画面サイズの段落: 「サイズ変更では捨てない。s は画面の高さに対する比率なのでリサイズで値が変わらない」に直っている。spec 4.1.4・sub-07 も同じ内容。実装 (RenderSystem.cpp:2003-2010 の ResetRenderHistory だけが消す、proj._22 は fovY から作る :958) と合っている
前回指摘の消込:
  1. (サイズ変更で履歴を捨てる、宛先 planner) — 解消。spec 4.1.4 / sub-07 / engine_spec.md:1082 から削除し、理由 (比率なので値が変わらない) を添えた。コードは変えていない (要らないので正しい)
  2. (枠の色が淡い、宛先 coder) — 未解消 (ユーザーの目視判断待ち。上の指摘 1 で持ち越し)
  3. (sub-10 の --no-audio、宛先 planner) — 解消。sub-10 は「付いていることを確かめる」に直り、afdec5b5 では cull_verify.bat を変えていない。spec 8. の行番号 (:50) も正しい
```
