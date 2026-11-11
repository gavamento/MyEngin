# sub-12: 画面サイズ・距離カリングの撮影の決定性を回帰に入れる

- 依存: sub-11
- 状態: 未着手
- 往復: 0

## 経緯
sub-11 round 1 で「画面サイズ + 枠表示の撮影が実行ごとに違う」と報告され、切り分けのために sub-12 を切った。ところが sub-11 round 2 で、これは coder の測り違いだったと訂正された (線あり − 線なしの差を揺れと見誤った。`--warp` で同じ条件の撮影を 2 回撮ると差 0)。そこで切り分けは不要になり、**縮小して残す**ことにした。

残す理由: sub-07 / sub-08 で決定性を確かめたのは `counts` の構成間一致だけで、画素の run 間一致は見ていない。`counts` が同じでも、落ちた物の集合が違えば絵は変わる。画面サイズカリングには、ビュー別の履歴という状態がある。だから「同じ exe・同じ引数・`--warp` の 2 回の撮影が tol 0 で一致する」ことを cull_verify の回帰に入れ、今後の変更から守る。

## やること
spec 受け入れ条件 17。
- `tools\cull_verify.bat` に run 間一致の検査を足す。同じ Release exe・同じ引数で 2 回撮り、`Editor.exe --img-diff --tol 0` で比べる。
  - (B) `--render-bench-demo --cull-min-screen-size 0.02 --cull-debug`
  - (D) `--culling-distance-demo --cull-max-distance 40`
  - 撮影の共通条件は既存の `%COMMON%` + `--warp`。`--frames` と `--shot-frame` は固定する。`cache\` の保存済みシーンは先に消す。
  - 空振りしないための検査も 1 つ入れる: (B) で `culledScreenSize > 0`、(D) で `culledDistance > 0` を `cull_counts.ps1 -Greater` などで確かめる。
- CI (`MYE_CULL_SKIP_CONFIGS=1`) でもこの検査が走るようにする (WARP だけで完結するため)。所要時間の増分を書く。
- もし一致しなければ、その時点で原因を切り分ける (旧 sub-12 の手順: 実 GPU か WARP か → 組み合わせを変えて絞る → M99 由来なら直す)。その場合は SELF_EVAL の差分に書く。
- engine_spec §6.14 *Verification* と `docs\test_checklists.md` の cull_verify の説明に 1 行足す。

## やらないこと (このサブでは)
- 実 GPU の撮影の run 間一致の保証 (M90 から約束していない)
- カリングの判定の変更 (一致しなかった場合を除く)

## 触る場所 (planner の見立て)
- `tools\cull_verify.bat`、`tools\cull_counts.ps1` (必要なら)
- `engine_spec.md` §6.14 *Verification*、`docs\test_checklists.md`

## 受け入れ条件 (このサブ)
1. cull_verify に (B) と (D) の run 間一致の検査 (tol 0) と、空振り防止の検査が入り、フル実行と `MYE_CULL_SKIP_CONFIGS=1` の両方で PASS する。所要時間の増分を書く。
2. わざと失敗させると非 0 で終わる (例: 2 回目の撮影だけ `--cull-min-screen-size` の値を変えて流す一時的な呼び方。作業ツリーは汚さない)。
3. 文書の説明に、増えた検査が載っている。
4. `tools\check_rules.ps1` が 0 error。

## 検証コマンド
- `tools\cull_verify.bat` (フル)、`MYE_CULL_SKIP_CONFIGS=1` で `tools\cull_verify.bat`
- `tools\check_rules.ps1` (pwsh)

## 実装メモ (coder が追記)

## フィードバック履歴
