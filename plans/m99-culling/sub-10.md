# sub-10: cull_verify を CI に載せる

- 依存: sub-09 (レビュー round 1 の結果が出てから、修正のサブと一緒に回す — 司会の指示)
- 状態: OK (コミットは司会)
- 往復: 1

## やること
spec 3. / 受け入れ条件 15。2026-11-11 のユーザー判断「今載せる」による (spec 2. ではなく 8. 変更履歴と 7. のリスクに経緯)。
- `.github\workflows\ci.yml` の「screenshot regression」step (④) の**次**に、cull_verify の step を 1 つ足す。
  - `shell: cmd`、`run: tools\cull_verify.bat`、step の `env:` に `MYE_CULL_SKIP_CONFIGS: 1` (runner に実 GPU も Debug の Runtime 撮影の前提も無いため、構成間の counts 比較 = 検査 3 を飛ばす)。
  - 前提は shot_verify と同じ「replay_verify が先に Release をビルドしている」(bat はビルドしない)。コメントで④b として、何を検査し何を飛ばすかを既存の step と同じ書き方で書く。ファイル冒頭の環境変数の説明 (`MYE_SHOT_SKIP_*` の並び) に `MYE_CULL_SKIP_CONFIGS` を 1 項足す。
- **CI の runner に合わせた撮影条件の確認**: CI の runner (windows-2022) には音声デバイスが無い前提で、ジョブは `MYE_EXTRA_ARGS: --warp --no-audio` を Editor.exe に付けている (`ci.yml:96`)。`tools\cull_verify.bat` は `set COMMON=--no-audio ...` (:50) で既に `--no-audio` を付けている (review-1 #3 で planner の読み違いを訂正)。**Runtime.exe / Editor.exe の起動がすべて `%COMMON%` か `--no-audio` を通ることを確かめる**だけでよい (コードは変えない)。shot_verify が CI 向けに付けていて cull_verify に無い引数が他にあれば、揃えて実装メモに一覧を書く。
- **review-1 #1 の文書修正**: `engine_spec.md:1082` (§6.14 *Screen-size culling*) の "is cleared by `ResetRenderHistory()` and by a size change" から "and by a size change" を消し、「画面高さに対する比率なのでビューのサイズ変更では値が変わらず、履歴は捨てない」旨を 1 文足す (spec 4.1.4 と同じ内容)。ADR-038 に同じ記述があれば合わせる。コードは変えない。
- 文書: `engine_spec.md` の cull_verify の段落 (§6.14 *Verification* と :3305 付近の *Culling regression*) と `docs\test_checklists.md:1009` 付近に「CI でも `MYE_CULL_SKIP_CONFIGS=1` で回る」と書く。ADR-038 の検証の節にも 1 行。

## やらないこと (このサブでは)
- master の CI を緑に戻す作業 (M98e 以降のビルド段の failure は M99 と別件)
- cull_verify の検査内容の変更 (CI 向け引数を揃える以外)
- 画面サイズの履歴の挙動の変更 (#1 は文書だけ)
- デバッグ枠の色 (review-1 #2。ユーザーの目視の結果しだいで planner が別途決める)

## 触る場所 (planner の見立て)
- `.github\workflows\ci.yml` (④ screenshot regression の step は :149-158 付近、ジョブの env は :93-106)
- `tools\cull_verify.bat` (撮影条件の set 行、:49-51 付近)
- `engine_spec.md` (:1082 と cull_verify の段落)、`docs\test_checklists.md`、`docs\adr\ADR-038-culling.md`

## 受け入れ条件 (このサブ)
1. `ci.yml` が YAML として解析できる (`python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1],encoding='utf-8'))" .github\workflows\ci.yml` — 開発機に PyYAML がある)。差分は step 1 つ・冒頭の説明 1 項・コメントだけ。
2. CI と同じ条件でローカル実行して PASS: cmd で `set MYE_CULL_SKIP_CONFIGS=1` と `set MYE_EXTRA_ARGS=--warp --no-audio` を立て、Debug の bin を一時的に使わない状態 (Release だけで足りること。Debug の exe を参照しないことをログか bat の分岐で示す) で `tools\cull_verify.bat` → exit code 0。所要時間を書く。
3. 失敗が step を落とすこと: cull_verify が検査失敗時に非 0 で終わる経路 (`exit /b 1`) を、1 つの検査をわざと失敗させて (例: 一時的に存在しない exe パスを指す環境で起動する、または比較対象をすり替える。作業ツリーを汚さない方法で) 確かめ、終了コードを書く。GitHub Actions の `shell: cmd` は最後のコマンドの終了コードで step の成否が決まるので、`run:` は bat 1 本の呼び出しだけにする。
4. cull_verify の Runtime.exe / Editor.exe の起動がすべて `--no-audio` を通る (grep の結果を実装メモに)。ローカルのフル実行 (`MYE_CULL_SKIP_CONFIGS` なし) が sub-09 と同じく PASS。
6. `engine_spec.md` §6.14 の画面サイズの項から「サイズ変更で捨てる」が消え、理由の 1 文がある (review-1 #1)。ADR-038 も矛盾しない。
5. 未検証として明記: CI 上で step が実際に走って PASS するかは、master の CI が緑に戻るまで観測できない。SELF_EVAL の検証欄に「未実行 (CI が赤)」と書く。

## 検証コマンド
- `python -c "import yaml,sys; yaml.safe_load(open(sys.argv[1],encoding='utf-8'))" .github\workflows\ci.yml`
- cmd: `set MYE_CULL_SKIP_CONFIGS=1 && set MYE_EXTRA_ARGS=--warp --no-audio && tools\cull_verify.bat` (PowerShell ツールから cmd を起動する。memory: exe の実行は PowerShell ツールから)
- `tools\cull_verify.bat` (フル)
- `tools\check_rules.ps1` (pwsh)

## 実装メモ (coder が追記)
SELF_EVAL: sub-10 (round 1)
- ci.yml: ④ の次に ④b step 1 つ (shell: cmd / env MYE_CULL_SKIP_CONFIGS: 1 / run: tools\cull_verify.bat)、冒頭の説明に MYE_CULL_SKIP_CONFIGS を 1 項。
- engine_spec.md: §6.14 画面サイズの項から「and by a size change」を削除し理由 (比率なのでリサイズで値が変わらない) を追記。Verification と Culling regression に CI 実行を追記。ADR-038 には「サイズ変更で捨てる」記述が元々無く、検証の節に CI 1 行を追加。test_checklists.md:1009 を「CI でも回る」に変更。
- cull_verify.bat は変更なし。--no-audio: Runtime の全起動 (:pair / :dump / 基点比較) は %COMMON% (--no-audio 入り) を通る。Editor.exe --img-diff は shot_verify と同じく撮影しないので付けない。shot_verify にあって cull_verify に無い CI 向け引数は無い (--warp --no-audio --font-embedded --no-fxaa が揃っている)。
検証:
- YAML 解析 → OK
- CI 相当 (MYE_CULL_SKIP_CONFIGS=1, MYE_EXTRA_ARGS=--warp --no-audio) → PASS 13 checks, exit 0, 157 秒。この分岐では Debug の exe を参照しない (bat の :33 ガードと :84 の goto)。
- 失敗経路: MYE_CULL_BASE_ROOT=C:\nonexistent_m99 を足して実行 → [FAIL] 1 of 13、exit 1 (157 秒)。
- フル実行 → PASS 23 checks, exit 0, 324 秒。
- check_rules.ps1 (pwsh) → 0 error / 60 warning (既存)。
- CI 上での実走 → 未実行 (CI が赤。master は M98e 以降ビルド段で failure)。
自己採点: 仕様適合 5 / 正しさ 4 (CI 上未観測) / コード品質 5 / テスト 4
不安・質問: なし
触ったファイル: .github/workflows/ci.yml, engine_spec.md, docs/test_checklists.md, docs/adr/ADR-038-culling.md, plans/m99-culling/sub-10.md
申し送り: 実行で tests\actual\cull と cache の保存済みシーンが更新される (git 管理外)。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-11)。受け入れ 1 (yaml.safe_load OK、ci.yml の差分は step 1 つ・冒頭の説明 1 項・コメントのみ = git diff で確認)、2 (CI 相当 13 checks PASS、157 秒、Debug の exe を参照しない分岐)、3 (MYE_CULL_BASE_ROOT に存在しないパスで 1 check FAIL・exit 1)、4 (起動はすべて %COMMON% = --no-audio、フル 23 checks PASS)、6 (engine_spec.md:1082 の size change を理由つきで削除、ADR-038 は元々記述なし) を確認。5 は未実行 (CI が赤) と明記済み。申し送り: master の CI が緑に戻ったら ④b のログで PASS を確かめる。
