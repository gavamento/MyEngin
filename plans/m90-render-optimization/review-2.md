# review-2: m90-render-optimization

- 対象: `3b30251..9905ede` (M90a〜M90j)。今回の修正は `9905ede` (M90j、sub-10)
- 日付: 2026-10-10
- 作業ツリー: 台帳 `harness.md` / `sub-10.md` の未コミット差分は対象外 (触っていない)

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — 統計の読み戻しが待たなくなり、ON の CPU 提出は解像度に比例しなくなった。再測定 (各 3 回) では増分が 320x180 で +0.06〜0.12 ms、1080p で +0.08〜0.13 ms、4K で +0.08 ms 前後。カット直後の 11 画素は描画順による z-fight と断定され、受け入れ 8 の「原因を示して許容」に当たる。対話の描画 (待たない経路) の CPU 時間は dump で測れない (#1)
  機能性: 4 — 受け入れ 2 / 8 / 9 / 12 / 14 を再実行して PASS。dump の counts は 4 構成 (Release / Release+WARP / Debug / Debug+WARP) と Deferred の 2 構成で一致 (occluded 2793)。カット直後の差は許容済みの 11 画素のまま (Forward maxDiff 47 / Deferred 49、場所も同じ)
  ビジュアルデザイン: 4 — round 1 で見た絵から描画のコードは変わっていない。golden 30 枚が PASS。エディタの GUI は目視待ち
  コード品質: 4 — 読み戻しは `PollStats` の 1 か所に集まり、待つかどうかは `RenderView::occlusionStatsWait` で切り替える。読む位置は GPU / CPU の計測区間の外へ移った (Forward の `opaqueTimer_.End` も前へ移した)。`ResolveOcclusionCulling` は本体と selftest で共有された。残りは #2 の小さい警告だけ
指摘:
  1. [minor] 宛先: coder — 対話の描画 (DO_NOT_WAIT) の経路を観測した証拠は、selftest の範囲チェック (`TestStats` の待たない版) と、`Runtime.exe --frames 300` が exit 0 で終わることだけ。CPU 時間が待ちを含まないことは、コード (`OcclusionCullPass.cpp` の `ReadStats`) を読んで確かめたにとどまる。dump は撮影モード (待つ) なので、この経路の `cpuMs` を出せない — 根拠: ADR-029 の計測節「対話の描画の cpuMs は dump から取れない」、sub-10 の申し送り — 期待: 次に計測の口を触るとき、対話モードの cpuMs を出す手段 (例: dump に待たないモードを足す) を検討する。M90 の合否には影響しない
  2. [minor] 宛先: coder — `TagSelfTest.cpp:304` の `const std::wstring root` が外側の同名の変数を隠している (C4456。M90h 由来、sub-10 の申し送りどおり)。round 1 では増分ビルドの警告を見ておらず、拾えなかった — 根拠: `src\Engine\Engine\Scene\TagSelfTest.cpp:304` — 期待: 名前を変える
検証した手段:
  - ビルド: MSBuild Release / Debug x64 → どちらも exit 0
  - `Editor.exe --selftest`: Release exit 0 (81 秒)、Debug exit 0 (449 秒)。Occlusion は「LOD 付きの検査用メッシュ」と「Deferred 統計 (読み戻しで待たない)」を含めて PASS
  - `tools\check_rules.ps1` → 0 error / 50 warning (round 1 と同じ数)
  - `tools\shot_verify.bat` → [PASS] 30 枚。`tools\replay_verify.bat` → [PASS] 19 ジョブ (jobs A/B を含む、207.7 秒)
  - dump の counts: render_bench 960x540 shot 30 で Release / Release+WARP / Debug / Debug+WARP が一致。Release Deferred と Debug Deferred+WARP も一致
  - CPU 提出 (render_bench --deferred、Release、実 GPU、shot 300、各 3 回、ON / OFF): 320x180 で 0.260 / 0.324 / 0.275 と 0.208 / 0.161 / 0.203。1920x1080 で 0.313 / 0.268 / 0.269 と 0.180 / 0.173 / 0.191。3840x2160 で 0.240 / 0.275 / 0.245 と 0.163 / 0.160 / 0.205。修正前の 1.4〜3.5 ms は解消した
  - カット直後の A/B (WARP、tol 0): Forward も Deferred も 11 画素で、最悪の画素は (365,326)。許容した差と同じもの
  - 対話モード: `Runtime.exe --render-bench-demo --deferred --frames 300` (撮影なし = DO_NOT_WAIT) → exit 0、ERROR ログ無し
  - 読んだ範囲: `git diff be31dbd..9905ede` の全部 (src、ADR-029、test_checklists、spec §4.1.1 / §6 / §8)、sub-10.md
  - 黙った差分: 無し (変更はすべて sub-10 のやること 1〜6 の範囲)
前回指摘の消込:
  1. 解消 — `ReadStats` に DO_NOT_WAIT (対話) / 待つ (撮影) を入れ、呼び出しを計測区間の外の `PollStats` へ移した。再測定で ON の CPU 提出は解像度に比例しない。ADR-029 の計測表と判断表も書き直された
  2. 解消 (許容) — z-fight と断定された。可視ビットの履歴を捨てる一時コードで ON と OFF が全画素一致したことが証拠で、ADR-029 3-2 / 3-6 / 検証節に記録された。selftest に LOD 付きの run (カット直後を含む) の ON/OFF 一致が足された。私の再実行でも差は同じ 11 画素のままで、増えていない
  3. 解消 — planner が基準 (1080p、10 回の中央値で増分 0.5 ms 以下) を spec §8 と sub-10 に置き、+0.064 ms で既定 ON を続けると判断した。私の再測定 (+0.08〜0.13 ms) もこの基準を十分に下回る
  4. 解消 — `LocalizationTable.inl:88` から「(Deferred)」が消えた (両言語)
  5. 解消 — `ResolveOcclusionCulling` を `TagNames.{h,cpp}` に作り、`EngineLoop.cpp:387` と `EngineCliSelfTest.cpp` の両方から呼ぶ
  6. 解消 — `TagNames.h` の冒頭に描画設定を追記した
  7. 解消 — `test_checklists.md` の `--forward` を「`--deferred` あり / なし」に直した
```

## 目視待ち (ユーザー。指摘ではない)

`docs\test_checklists.md` の M90 節が正本。レビューで見られなかったものは次のとおり。

- ProfilerWindow の新しい欄 (GPU ms、オクルージョン、LOD の分布、ビュー別)
- Rendering メニューのオクルージョンの切り替えと保存 (再起動後も保たれるか、`scmhint::Changed`)、ツールチップの両言語、ラベルから「(Deferred)」が消えたこと
- Scene View と Game View を同時に出したときの、ビュー別の統計とキャラの姿勢の一致
- LOD の見た目 (ポップ、法線・UV の崩れ)、URO でカクつきが気になる距離
- Inspector のモデル LOD 設定 UI (適用 → `.meta` を書く → 再クック)
