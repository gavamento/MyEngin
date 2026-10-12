# review-2 — m91-compute-foreach

- 対象: `3766e33..477939b` (master)。round 2 で新しく入ったのは `a805b6e..477939b` (913067d M91k、77269f9 M91l、bb625f3 M91m と、台帳・仕様のコミット) / 日付: 2026-10-12 / reviewer round 2

```
REVIEW: PASS
round: 2
軸 (1-5):
  製品の深度: 4 — review-1 の決定論の穴 2 つは塞がった。#1 は State と Fx の表を分ける形で直っていて、空のストアへの復元・復元後の Fx の作り直し・Fx の有無の 3 通りを、旧実装と新実装で比べて確かめた (probe)。#2 は resim 中の引数の誤り 6 通りを selftest が見ている。旧実装に戻すとその 6 件が FAIL することも確かめた。同じ exe から net の selftest を 4 本同時に回しても FAIL は 0 件
  機能性: 5 — replay_verify (21 ジョブ。compute / computeab は v50 で録り直し) が PASS。Release と Debug の --selftest は exit 0 で、Compute 系 8 本と Server/client net が PASS。Server.exe の --selftest は exit 0 で、M91m の検査 (上書きの無い sim は <base>\save_server を使う) も PASS。デモのスクショは round 1 と同じ絵。GUI の手動確認項目 (受け入れ 11 と 17 の GUI 部分) は、台帳のとおりユーザー確認のまま
  ビジュアルデザイン: 4 — 補足文が折り返され、末尾まで読めるようになった (inspector_cpp_k2.png)。compute.png は round 1 と変わらない。ほかに新しい問題は見つからなかった
  コード品質: 4 — 表を分けたので、State の採番はコードの構造上 Fx に依存しない。コメントと ADR-030 D11 / spec D29 には、却下した 2 案とその理由が書いてある。sub-11 のリトライを外した理由 (本当の原因は作業フォルダの共有だった) も記録されている。仕様との差分に書かれていない変更は見つからなかった
指摘: (なし。blocker / major / minor とも 0 件)
検証した手段:
  - 消込の確認 (旧実装に戻すと試験が落ちるか):
    - #1: probe `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\1045aa2f-0617-404e-a3b6-257b3a941d8f\scratchpad\mut1\t.cpp`。ComputeStateSelfTest の (i)(ii)(iv) と同じ操作列を、HEAD の ComputeBufferStore と a805b6e の ComputeBufferStore (git show で取り出したもの) のそれぞれに対して単体コンパイルして回した。結果は HEAD が PASS 3/3、旧実装が FAIL 3/3 (exit 3)。selftest のコードそのものは旧 API ではコンパイルできないので、同じ操作列を probe に写して比べた
    - #2: 一時 worktree (scratchpad\wt_mut2、確認後に `git worktree remove` で削除済み) で、ComputeService::ForEach の修正部分だけを旧コードに戻して Release をビルドし、--selftest を回した。exit 1 になり、`FAIL: resim 中でも無効ハンドル (0) は失敗` / `世代違い` / `解放済み` / `Fx の引数に無効ハンドル` / `引数の本数違い` / `stride 違い` の 6 件だけが FAIL した (ログは scratchpad\mut2_st.err)。HEAD ではこの 6 件は PASS
    - #3: ComputeInspectorSection.cpp:129-132 が TextWrapped になっている。画像 `C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\1045aa2f-0617-404e-a3b6-257b3a941d8f\scratchpad\inspector_cpp_k2.png` (sub-16 の coder が撮ったもの。撮影後に該当コードは変わっていない) では、「...next to REGISTER_SCRIPT.」まで 2 行に折り返して読める
    - #4: KernelLint.cpp:688 の文言が「他の要素の読み書きは禁止」になった
  - `tools\replay_verify.bat` (Debug / Release を再ビルドしてから) → exit 0、21 ジョブすべて PASS (compute 258.9 s、computeab 260.1 s)。ログは scratchpad\rv2.log
  - `bin\x64\Release\Editor.exe --selftest` / `bin\x64\Debug\Editor.exe --selftest` (CWD はリポジトリ直下) → どちらも exit 0。標準出力と標準エラーの両方で FAIL を探した。標準エラーに出ているのは、C# のコンパイル失敗を意図して起こす試験の `[csharp] compile FAILED` の 1 行だけ (round 1 の実行と同じ)
  - `bin\x64\Release\Server.exe --selftest` → exit 0、`PASS: M91m: a headless sim without saveDirOverride keeps the default <base>\save_server`
  - sub-17 の主張の追試: `MYE_SELFTEST_ONLY=servernet` で Release の Editor.exe --selftest を 4 本同時に 1 ラウンド回した → 4 本とも exit 0、FAIL 0 件。終わった後の `bin\x64\Release\cache\server_net_selftest` には `crash` だけが残り、`p<pid>` は消えていた。sub-17 の 30 ラウンドは再実施していない
  - %TEMP% の `mye_selftest_*`: selftest の前後で 19 → 19 で、増えていない (残っている 19 個は 15:43 のもの = 台帳の申し送りにある既知のもの)
  - `bin\x64\Release\Runtime.exe --compute-demo ... --screenshot` (CWD は scratchpad\rvshot2) → `scratchpad\rvshot2\compute.png` は round 1 と同じ絵 (火花・黄色の群れ・青の群れ)。ログに WARN / ERROR は無い
  - 読んだ範囲: `a805b6e..477939b` の差分すべて (ComputeBufferStore.{h,cpp}、ComputeService.cpp、WorldHasher.cpp、SimSnapshot.h、KernelLint.cpp、ComputeInspectorSection.cpp、selftest 4 本の追加分、ServerNetSelfTest.cpp、HeadlessSim.{h,cpp}、ServerSelfTest.cpp、EditorMain.cpp、replay_verify.bat のコメント、spec / ADR-030 / harness の更新)
  - 作業ツリー: 製品のソースは変えていない。一時 worktree は削除済み。`git status --short` で見えるのは、このファイル review-2.md の未追跡 1 件だけ
前回指摘の消込:
  1. 解消 — State と Fx の表を分け、スナップショットとハッシュを State の表だけにした (snapshot v50、ADR-030 D11 / spec D29)。空のストアへの復元・復元後の Fx の作り直し・Fx の有無のどれでも、State の採番は連続実行と同じ。旧実装に戻すと probe が 3 件とも FAIL する
  2. 解消 — ComputeService::ForEach で、引数を解決できなければ Skipped の判定より前に失敗を返すようになった。resim 中の 6 通りを selftest が見ていて、旧コードに戻すとその 6 件が FAIL する
  3. 解消 — 補足文が TextWrapped で折り返される (inspector_cpp_k2.png)
  4. 解消 — KL016 の文言が「他の要素の読み書きは禁止」になった
```

## 補足 (司会向け)

- round 1 の自分の検査の穴: round 1 では selftest の FAIL を標準出力でしか探していなかった。今回は標準エラーも見て、round 1 の実行にも本物の FAIL が無かったことを確かめ直した (標準エラーにあるのは、意図して起こした C# のコンパイル失敗の 1 行だけ)。
- 実行しなかったこと: sub-17 の「30 ラウンド (120 プロセス) で FAIL 0」の追試 (今回は 4 本同時を 1 ラウンドだけ)、インスペクターの撮り直し (coder の画像を使った)、GUI の手動確認項目。
