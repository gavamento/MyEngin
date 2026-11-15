# sub-25: 追跡クライアントの試験から実時間への依存を外す (M104m3)

- 依存: sub-24
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 順序: sub-24 → **sub-25** → sub-13 → sub-14 (番号は末尾。sub-24 の VERDICT で足した。spec §2 D37)
- 書き込みの深さ: 詳細
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い。

## 事実 (2026-11-15 に planner が再現した)

- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest` を 5 回回して、**5 回とも失敗** (CPU 使用率 100%。ユーザーの別のプログラムが動いている状態)。落ちる項目は毎回 `poll cap: it spreads over many frames (a bounded number), not one` と、回によって `model: describe is stored`、`hunks: describe is followed by a hunks request, …`、`hunks: a binary change asks for no hunks` など (1〜8 件)。同じ状態で Release の `--selftest` 全体は 112 スイート 0 失敗。
- 原因: `C:\HAL\MyEngin\src\Editor\ChangeTracking\TrackClient.cpp` 178 行目と 199 行目が、`Poll()` の時間の上限 (`kTrackPollBudget` = 2 ms、`TrackClient.h` 52 行目) を **`std::chrono::steady_clock` で直接測っている**。他の時刻 (時間切れ、再接続) は差し替えられる時計 `io_->NowMs()` (`TrackIo.h` 41 行目) を使っているのに、ここだけ実時間。Debug で機械が重いと、1 回の `Poll()` が 2 ms で打ち切られて数行しか進まず、「N フレームで終わる」と数えている試験が外れる。
- sub-09 / sub-10 の試験で、sub-24 の変更によるものではない (sub-24 の coder の報告と planner の再現が一致)。CI のランナーでも落ちうる。

## やること

1. **`Poll()` の時間の上限を、差し替えられる時計で測る。** `TrackIo` に、マイクロ秒かそれより細かい単調時計の口を足す (`NowMs()` は 1 ms の粒度で 2 ms の上限には粗い。例 `NowMicros()`)。Win32 の実装は `steady_clock` か `QueryPerformanceCounter`。偽の I/O (`ChangeTrackingFakeIo.h`) は、試験が進める時計 (呼ばれるたびに決まった量だけ進む、または試験が明示的に進める)。
2. **製品の挙動は変えない**: 実時間で 2 ms (定数はそのまま) を超えたら残りを次のフレームへ回す。行数の上限 (256 行) と読み取りの回数の上限 (4 回) もそのまま。
3. 試験を決定的にする: 時間の上限に当たる場合と当たらない場合を、偽の時計で作る (「1 行処理するごとに 1 ms 進む時計なら、1 回の `Poll()` は 2〜3 行で止まる」「進まない時計なら行数の上限 256 で止まる」)。「何フレームで終わるか」を実時間に依らない固定の数で確かめる。フレーム数の許容幅 (今は 500 フレームまで、など) を広げて通すのは不可。
4. 同じ型の実時間への依存が `src\Editor\ChangeTracking\` の他に無いかを洗う (`steady_clock`、`system_clock`、`GetTickCount`、`QueryPerformance`、`Sleep` を grep)。`ChangeModel` の間引き (500 ms)、`SceneChangeOverlay` の間引き、`ChangeTrackingSession` の順番待ち、`AiChangesWindow`、sub-24 の試験 (「フレーム時間の試験は実時間で測っていて、閾値は 50〜200 ms と緩い」と sub-09 の申し送りにある) を含む。見つけたものは、製品のコードなら差し替えられる時計へ、試験の「実時間の閾値」なら、偽の時計か、呼び出し回数・処理量の上限の確認へ置き換える。置き換えられないもの (本当に実時間でしか意味が無いもの) は、理由を書いて残す。
5. `MYE_TRACK_PROBE=1` の結合 (実サービス) は実時間のまま (上限つきの待ち合わせ)。既定の `--selftest` では動かないので対象外。ただし待ちの上限が短すぎないかは見る。
6. 文書: `C:\HAL\MyEngin\docs\ai-change-tracking\architecture.md` §3.1 か §7 に 1 行 (Editor 側の時刻は全部 `TrackIo` の時計を通す)。`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` の進捗表に M104m3。

## やらないこと (このサブでは)

- `kTrackPollBudget` などの定数の値の変更。`Poll()` の作りの変更 (スレッド化など)。
- `tools\track` の変更 (Rust 側の時間に依存する試験は別。ただし `tests\pipe.rs` の `unused_mut` の警告 1 件は、ついでに直してよい)。
- 他のスイート (`computemanaged` など、sub-23 の coder が「並走で落ちた」と書いたもの) の直し。M104 の範囲の外なので、見つけたことだけ報告する。
- ユーザーのプロセスを止めること。負荷の再現のために止めたり優先度を変えたりしない。

## 受け入れ条件 (このサブ)

1. `src\Editor\ChangeTracking\` の製品のコード (試験と Win32 の実装 `TrackIo.cpp` を除く) に、`steady_clock` / `system_clock` / `GetTickCount` / `QueryPerformance` の直接の呼び出しが無い → grep。
2. **負荷の下で通る**: CPU を使い切る負荷 (論理コア数ぶんの計算だけのプロセスかスレッドを、coder が自分で起動して自分で止める。`mye_track bench load` が使える) をかけた状態で、`MYE_SELFTEST_ONLY=changetracking` の `bin\x64\Debug\Editor.exe --selftest` を 20 回続けて、20 回とも通る。負荷なしでも 5 回 → 実行結果 (回数と失敗数をそのまま書く)。
3. `Poll()` の上限の試験が、偽の時計で、時間の上限に当たる場合・行数の上限に当たる場合・どちらにも当たらない場合を、固定のフレーム数で確かめている。許容幅で逃げていない → コードの確認。
4. 製品の挙動が変わっていない: 実サービスとの結合 (`MYE_TRACK_PROBE=1`、Debug と Release) が通る。`Poll()` 1 回の実時間が、実サービスに 1 万行相当を流したとき (既存の結合の範囲でよい) に数 ms に収まる、または既存の計測と同じ → 実行結果。
5. `Editor.exe --selftest` 全体 (Debug / Release) が、負荷なしで通る。負荷の下で他のスイートが落ちた場合は、スイート名と項目を報告する (直さない) → 実行結果。
6. `tools\check_rules.ps1` が 0 error。下位の層に差分なし (`git diff --stat HEAD -- src\Engine src\Runtime src\Server src\GameLogic src\Shared src\Scripting` が空)。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- 負荷あり: `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest` を 20 回
- 負荷なし: 同じものを 5 回、`--selftest` 全体を Debug / Release で 1 回ずつ
- `MYE_TRACK_PROBE=1` + `MYE_SELFTEST_ONLY=changetracking` (Debug / Release)
- `pwsh -NoProfile -File tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-25 (round 1)
実装:
  - TrackIo.h / TrackIo.cpp: `NowMicros()` を足した (Win32 は steady_clock のマイクロ秒)。
  - TrackClient.cpp: `Poll()` の時間の上限を `io_->NowMicros()` で測る (定数 2 ms / 256 行 / 4 回は不変)。`steady_clock` の直接呼びが製品のコードから消えた。
  - ChangeTrackingFakeIo.h: `FakeState::nowMicros` / `microsPerRead` (NowMicros が呼ばれるたびに進む量。既定 0 = 進まない)。
  - ChangeTrackingSelfTest.cpp: (f) を 40 フレームちょうど (行数の上限だけ) に。(f2) 新設: 1 ms/読み取り → 3 行/Poll・100 行で 34 フレームちょうど、500 us/読み取り → ちょうど 2 ms は超過に含めず 5 行で止まる、10 行・時計停止 → 1 Poll。TimedPoll から実時間 (`ms`) を除き、`worst < 50/100 ms` と `pollMs < 200` の実時間の閾値は削除 (I/O 回数の上限の確認は残す)。
  - docs/ai-change-tracking/architecture.md §3.1 に 1 行、plans/m104-ai-change-tracking.md に M104m3 の行。
仕様との差分:
  - [追加] 実時間の閾値 3 件 (hung service 50 ms、poll cap 100 ms、no exe 200 ms) を、実時間を使わない確認へ置換 (削除) — 指示 4 の「実時間の閾値は偽の時計か呼び出し量の確認へ」。I/O 回数の上限 (kTrackMaxIoCallsPerPoll) の確認は残っている。
  - [未実施] `tests\pipe.rs` の unused_mut (任意) — 範囲外として触っていない。
  - 他に steady_clock 等の直接利用は、probe の待ち合わせ (ChangeTrackingSelfTest.cpp の pump、実時間のまま。上限 10〜60 s で短すぎない) と TrackIo.cpp だけ。
検証:
  - Debug/Release `MyEngine.sln` ビルド (MyeWarnAsError=true) → 警告・エラー 0。
  - 負荷 (論理 12 コアぶんの pwsh 計算ループ 12 本) の下で Debug `MYE_SELFTEST_ONLY=changetracking --selftest` 20 回 → 失敗 0 / 20 (修正前は planner が 5/5 失敗)。
  - 負荷なし 同 5 回 → 失敗 0 / 5。
  - Debug `--selftest` 全体 → 112 suites, 0 failed。Release `--selftest` 全体 → 112 suites, 0 failed。
  - `MYE_TRACK_PROBE=1` + changetracking: Debug PASS、Release PASS (実サービス)。
  - `tools\check_rules.ps1` → 0 error (66 warning、既存の規則 17)。`git diff --stat HEAD -- src\Engine src\Runtime src\Server src\GameLogic src\Shared src\Scripting` → 空。
  - Debug `--selftest-full` → Physics スイートの開始直後に exit -1073741571 (スタックオーバーフロー) で落ちた。2 回同じ。ChangeTracking とは無関係 (差分は ChangeTracking のみ)。修正前の状態では確かめていない (stash 禁止のため)。
  - 未実施: 実サービスに 1 万行流したときの `Poll()` 1 回の実時間の計測 (既存の結合に無い。製品の時計の種類は steady_clock のままで、測り方も同じ)。
自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜3,5,6 は確認済み。4 の「Poll の実時間が数 ms」は未計測。
  正しさ: 4 — 負荷下 20/20、全体・結合 PASS。製品の時計は同じ steady_clock。
  コード品質: 4 — 口 1 つの追加で局所的。
  テスト: 4 — 3 区分を固定フレーム数で確認。1 万行の実時間は未計測。
不安・質問:
  - Debug `--selftest-full` が Physics で落ちる (原因未調査、既存の可能性が高い)。別件として確認してほしい。
  - 負荷なしの全体 selftest では他スイートの失敗は出なかったので、負荷下の他スイートの確認 (受け入れ 5 の後半) はしていない (負荷下で回したのは changetracking のみ)。
触ったファイル:
  - src/Editor/ChangeTracking/TrackIo.h, TrackIo.cpp, TrackClient.cpp, ChangeTrackingFakeIo.h, ChangeTrackingSelfTest.cpp
  - docs/ai-change-tracking/architecture.md, plans/m104-ai-change-tracking.md, plans/m104-ai-change-tracking/sub-25.md
  - (plans/m104-ai-change-tracking/harness.md は開始時から変更済みで、触っていない)
消した・止めたもの: 自分で起動した負荷用 pwsh 12 本 (pid 31252,30260,5216,29492,14732,32592,15804,35096,38536,15312,36468,42476) を停止。控えのファイル %TEMP%\mye_m25_loadpids.txt を削除。ユーザーのプロセスは止めていない。実行中に作られた %TEMP%\mye_track_probe_* は selftest 自身の後始末に任せ、手では消していない。
申し送り: 他スイート (computemanaged 等) の並走時の失敗は調べていない。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-15)。planner の実走 (CPU 使用率 約 70%、ユーザーの別のプログラムが動いている状態): `MYE_SELFTEST_ONLY=changetracking` の Debug を 6 回 → 6 回とも通過 (sub-24 の VERDICT のときは同じ条件で 5 回中 5 回失敗)。Debug の `--selftest` 全体 (fast 層) → 112 スイート 0 失敗 (133 秒。実行中も後も `mye_track.exe` なし)。その出力に、sub-24 が足した試験 (Inspector の印、ワイヤ箱、色、バッジ) の PASS が 106 行ある = **sub-24 の VERDICT で保留にした「sub-24 の新しい試験が Debug で通ること」を確認した。** `src\Editor\ChangeTracking\` の製品のコード (試験、偽の I/O、`TrackIo.cpp` を除く) に `steady_clock` / `system_clock` / `GetTickCount` / `QueryPerformance` の直接の呼び出しは 0 件。下位の層と `tools` に差分なし。
  - 受け入れ条件 4 の「実サービスに 1 万行流したときの `Poll()` 1 回の実時間」は未計測のまま。製品の時計は今までと同じ `steady_clock` で、測り方 (2 ms を超えたら打ち切る) も同じなので、未計測を理由に差し戻さない。sub-19 の計測の項目に入れる。
  - Debug の `--selftest-full` が `physics` で落ちる件: planner が `MYE_SELFTEST_ONLY=physics` の Debug で再現した (終了コード -1073741571)。M104 の外の別件として扱う (spec §2 D38)。
  - nit: `tools\track\tests\pipe.rs` の `unused_mut` は残ったまま (次に `tools\track` を触るサブで)。負荷の下で他のスイートが落ちるかは未確認。