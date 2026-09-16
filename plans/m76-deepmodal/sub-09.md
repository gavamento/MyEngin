# sub-09 (M76e2): CPU 推論の SIMD / マルチスレッド最適化

- 依存: sub-05
- 実行順: **sub-05 の後・sub-06 の前** (ファイル番号は追加順なので 09 だが、順序は 05 → **09** → 06)
- 状態: 未着手
- 往復: 0

## なぜこのサブがあるか
sub-05 round 1 の実測で `CpuModalBackend` が **4.75–5.34 s/メッシュ** (実効 0.39–0.43 GMAC/s)。
planner は「ワーカーで走り初回 1 回だけなので許容、最適化は必要になってから」と裁定したが、
**ユーザーが「今すぐ最適化する」を選択**したため独立サブとして仕様化する (spec §8)。

**round 1 で確定済みの前提 (蒸し返さない)**:
- **ネットは縮めない** (モデル品質を落とし、sub-04 の門 R² ≥ 0.90 の測り直し = サブまたぎの差し戻しになる)
- **疎な占有を使った書き直しは成立しない** (畳み込みは 32³ を密に舐めるのが正しい。cell 有効性で飛ばせるのは head の **13.4%** が上限)
- 支配項は高解像度側: `convT k4 64→32 →16³` **26.0%** / `conv3 16→16 @32³` **11.0%** / `res(32)@16³` ×2 各 **11.0%** / `head conv3 32→64` **11.0%** (総 2.06 GMAC)
- **正しさの安全網は fixture** (`tests\deepmodal\fixture.*`、許容 1e-3)

## やること

### 1. SIMD (AVX2) — ただし**グローバルの `/arch` は絶対に触らない**
★**最重要の制約**: `build\Common.props` は「**`/arch` は全構成で同一 (既定 = SSE2)**」と明記している。
ここを AVX2 に上げると**エンジン全体の浮動小数コード生成が変わり、sim の結果が変わりうる** =
Debug/Release/CI のビット一致という本リポジトリ最大の契約を壊す。**絶対にやらない**。
- AVX2 は **`CpuModalBackend.cpp` の中だけ**で intrinsics (`<immintrin.h>`、`_mm256_fmadd_ps` 等) として使う
  (MSVC は `/arch` に関係なく intrinsics を受け付ける)。浮動小数の挙動が変わるのはこの翻訳単位だけに閉じる。
- **実行時検出** (`__cpuid` / `__cpuidex` で AVX2 + FMA を確認) → 非対応機は**スカラー経路へ縮退**。
  ビルド時固定にしない (AVX2 非対応機で起動しなくなるのは論外)。
- スカラー経路を**消さない**。縮退が腐らないよう selftest が両方を通ること (下記)。

### 2. マルチスレッド — **結果がスレッド数に依存しないこと**
★**分割は出力側だけ**。GEMM の**リダクション (K 次元) をスレッドで割らない**。
出力要素ごとの内積は 1 スレッドが固定順で最後まで計算する ⇒ **スレッド数が何であっても結果がビット一致**する
(4 コア機と 16 コア機で `.msfm` が変わらない)。リダクションを割ると加算順が変わり再現性が壊れる。
- 既存の `jobs::System().ParallelRanges(total, grain, fn)` ([0,total) を連続 index レンジへ分割する同期呼び出し) が
  この用途に合う。**ただしワーカースレッドから呼んで安全かを必ず確認すること** (`JobSystem.h` は
  「ParallelRanges は同期呼び出しなので常に高々 1 バッチ」とあり、別スレッドからの同時呼び出しを
  想定していない可能性がある)。安全でなければ `CpuModalBackend` 内に固定サイズの小さなプールを持つ。
- **ゲームを止めないこと**: 焼きは遊んでいる裏で走る。全コアを使うとフレームが飛ぶので上限を設ける
  (既定 = `min(4, max(1, hardware_concurrency - 1))` 程度。数値は実測で決めてよい)。
- スレッド数は**結果に影響しない**ので、環境変数や CLI で変えられるようにしてよい (計測用)。

### 3. GEMM のブロック化 / キャッシュ最適化
im2col + ブロック GEMM のタイル幅を実測で詰める。ここは加算順が変わってよい (fixture が守る) が、
**同じバイナリなら毎回同じ結果**になること (データ依存の分岐や非決定的な順序を入れない)。

### 4. sub-05 の持ち越し (このサブに含める)
- `kDmNetMaxParamCount` を `tools\check_rules.ps1` の `$constGroups` へ登録
  (`export.py` の `paramCount ≤ 2,000,000` assert と C++ 側が**同じ約束を 2 か所**に持っているため。
  食い違うと「Python が通した `.dmnet` を C++ が拒否する」型の静かな破綻になる)
- `--modal-backend` の CLI selftest (未知の名前 → WARN + cpu 縮退 / 綴り違い → exit 1) が
  `EngineCliSelfTest` にあることを確認し、無ければ足す

## やらないこと (このサブでは)
- ネット構造の変更 (spec §8 で却下済み)
- 疎対応の書き直し (13.4% が上限)
- `D3d11ModalBackend` (spec §3 で範囲外)
- `Common.props` / グローバル `/arch` の変更 (**禁止**)
- sim 側・描画側のコードへの波及 (この最適化は `Modal/` に閉じる)

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Modal\CpuModalBackend.h/.cpp` (本体)
- `src\Engine\Engine\Modal\ModalSelfTest.cpp` (両経路の検査、再現性の検査)
- `tools\check_rules.ps1` (`$constGroups` に 1 エントリ)
- 必要なら `src\Engine\Engine\EngineCli.cpp` / `EngineCliSelfTest.cpp` (計測用スイッチ、持ち越し nit)
- 参考: `src\Engine\Core\JobSystem.h` の `ParallelRanges` / `ParallelFor`

## 受け入れ条件 (このサブ)
spec §5 の **22, 23**。
1. **速度**: フルサイズ `.dmnet` (sub-04 の `--random-full`、widths 16/32/64/96、1,682,448 param) の Release 推論が
   **≤ 0.6 s/メッシュ** (`--modal-bake` の ms 欄)。
   ★根拠 (今度は根拠を書く): 現状 0.4 GMAC/s に対し、AVX2 FMA は理論ピーク数十 GMAC/s で、
   **実効 10% でも 5.6 GMAC/s = 14 倍**。8 倍 (= 0.6 s) は SIMD 単体で十分届く水準に置いた保守的な線で、
   マルチスレッドを足せばさらに余裕がある。**目標は ≤ 0.3 s**。
   0.6–1.0 s に着地した場合は**黙って合格にせず** SELF_EVAL で報告すること (planner が §8 で裁定する)
2. **正しさ**: fixture (`tests\deepmodal\fixture.*`) の `max|Δ|` が
   **AVX2 経路・スカラー経路の両方で許容 1e-3 内**。★現状の実測は **4.77e-07** なので、
   **桁が大きく悪化していないこと**も見る (両経路の実測値を SELF_EVAL に併記する)
3. **スレッド数に依存しない**: スレッド数を変えて焼き、`.msfm` が**バイト一致**すること。
   ★**検査するスレッド数に「SIMD 幅で割り切れない本数」(3 / 5 など) を必ず含める**。
   `T = 1 / 3 / 4 / 5` の 4 通りで焼いて全部一致させること。
   ★1 / 2 / 4 / 8 だけでは**通ってしまう** — N (出力列数) は 32768 / 4096 / 512 / 64 で全部 8 の倍数なので、
   2 冪のスレッド数ではチャンク境界が常に 8 に揃い、AVX2 ブロックとスカラー端数の境界が動かない。
   ★**リダクション (K) を割らないだけでは足りない**: AVX2 の 8 列ブロック (FMA = 1 回丸め) と
   スカラー端数 (乗算 + 加算 = 2 回丸め) は**丸めが違う**ので、チャンク境界が 8 からずれると
   「どの列が AVX2 でどの列が端数か」が変わって結果が変わる。**チャンク幅を SIMD 幅の倍数へ量子化**して、
   AVX2 / 端数の切れ目をスレッド数から独立させること (最後のレンジの端数だけは T=1 と同じ位置に残る)
4. **再現性**: 同じバイナリ・同じ入力・同じ `.dmnet` で 2 回焼くと `.msfm` が**バイト一致**すること
   (`CookedCacheSelfTest` の memcmp と同じ流儀)
5. **両経路が腐らない**: selftest が AVX2 経路とスカラー経路の**両方**を通ること
   (AVX2 対応機でもスカラーを強制できるスイッチを用意する)
6. **既存が不変**: `--selftest` 全緑 / `check_rules.ps1` 0 error (新しい constGroups を含む) /
   `replay_verify.bat` 全ペア緑 (この最適化は sim に触らないことの証明) / Debug・Release 0 警告
7. **2 回目の `--modal-bake` が `.msfm` ヒットで桁違いに速い**こと (sub-05 で入れた回帰を維持)

## 検証コマンド
- Debug/Release ビルド (`/p:MyeWarnAsError=true`) → `cmd /c "bin\x64\Debug\Editor.exe --selftest"`
- `cmd /c "bin\x64\Release\Editor.exe --modal-bake"` を 2 回 (1 回目 = 速度、2 回目 = キャッシュヒット)
- スレッド数を変えた 2 回の焼きで `.msfm` を `fc /b` (またはハッシュ) で比較
- `pwsh -File tools\check_rules.ps1`
- `tools\replay_verify.bat`

## 実装メモ (coder が追記)

SELF_EVAL: sub-09 (round 1)
実装:
  - **事前調査 (実装前に着手、deep-reasoning の「荷重のかかる決定を先に潰す」)**:
    1. `jobs::System().ParallelRanges` をワーカースレッドから呼ぶ安全性 —
      `JobSystem.cpp` を読み、`batch_`/単調増加する `cursor_` を「同期呼び出しは常に高々 1
      バッチ」という**単一呼び出し元**前提で mutex 保護しているだけ (2 つの呼び出し元が
      同時に来る想定のコードではない) と確認した。実地確認: `TransformSystem.cpp` /
      `RenderSystem.cpp` がメインスレッドから毎 tick/フレーム `jobs::System()` を呼ぶ一方、
      Deep-Modal の推論は `ModalSoundLibrary` の非同期焼きワーカースレッド上で走る
      (`ModalSoundLibrary::WorkerLoop` → `BuildFeatureMap` → `backend_->Infer`)。
      2 つの呼び出し元が同じ共有インスタンスの `batch_`/`cursor_` を同時に書き換えると、
      片方のバッチの chunk をもう片方が横取りする形で壊れる。**危険と判定し、jobs::JobSystem
      は使わないことに決めた**。
    2. AVX2 intrinsics が既定 `/arch` (指定なし = x64 の暗黙 SSE2、`Common.props`) のまま
      コンパイル・実行できるか — `vcvars64.bat` + `cl /nologo /std:c++20` (`/arch` 指定なし、
      本リポジトリの Common.props と同条件) で `<immintrin.h>` の `_mm256_fmadd_ps` 等を
      使った使い捨てプログラムをビルドし、この開発機 (AVX2+FMA 対応、`hardware_concurrency=12`)
      で実行して数値が一致することを確認した (MSVC は intrinsics ヘッダを `/arch` に
      関係なく受け付ける挙動を実地で確認できた。GCC/Clang とは異なる MSVC 固有の性質)。
  - **jobs::JobSystem を使わない代替設計**: sub-09.md は「危なければ CpuModalBackend 内に
    固定サイズの小さなプールを持つ」と示唆していたが、実装は**呼び出しのたびに使い捨てる
    スレッド分割 (`ParallelSpan`、`CpuModalBackend.cpp` 内 anonymous namespace)** にした
    ([逸脱]、理由は下記「仕様との差分」)。
  - `src\Engine\Engine\Modal\CpuModalBackend.h/.cpp`:
    - `ModalCpuDetectAvx2Fma()` (`__cpuid`/`__cpuidex`/`_xgetbv` で AVX2+FMA を実行時検出、
      結果はプロセス内でキャッシュ) / `ModalCpuDefaultThreadCount()`
      (`min(4, max(1, hardware_concurrency-1))`) を新設。
    - `ParallelSpan(total, maxThreads, fn)`: `[0,total)` を最大 `maxThreads` 本のスレッドへ
      分割し、呼出スレッドも 1 チャンク担当してから join する使い捨て方式。
      スレッド生成コストは実測 (scratchpad の使い捨てベンチマーク) で **0.076 ms/スレッド**
      (4 スレッド×20 round で 6.07 ms、GEMM/im2col の並列区間 1 回あたりに換算すると
      Infer() 全体で合計 ~数 ms、目標の秒未満の推論時間に対して無視できる)。
    - `GemmBiasAddScalarRange` (既存 `GemmBiasAdd` を `[nBegin,nEnd)` の列レンジへ一般化した
      だけ、既定呼び出し `[0,n)` は sub-05 時点と 1 命令も違わない) / `GemmBiasAddAvx2Range`
      (N 方向 8 幅・M 方向 4 行の YMM アキュムレータ、FMA で 1 回丸め) /
      `GemmBiasAddDispatch` (出力側 N だけを `ParallelSpan` で分割、K 次元のリダクション
      順序は 1 スレッドが 0..K-1 を固定順で計算しきる)。
    - `Conv3dRaw`/`ConvTranspose3dRaw` に `useAvx2=false, maxThreads=0` の既定値付き末尾
      引数を追加 (下記「仕様との差分」)。im2col の行構築 (`Conv3dRaw`) と dilate+pad の
      構築 (`ConvTranspose3dRaw`) も出力側 (行 / ci) だけを `ParallelSpan` で分割。
      重みの反転並べ替え (`ConvTranspose3dRaw` 内) は畳み込み本体 (同じ重みを N 回使い回す
      GEMM) に比べて O(k^3) と無視できる大きさなので並列化していない。
    - `ReluRange`/`AddRange` (要素ごと、AVX2 (`_mm256_max_ps`/`_mm256_add_ps`) + スカラー、
      リダクションが無いので SIMD/スレッドの違いは決定論に影響しない)。
    - `CpuModalBackend::SetForceScalar`/`SetThreadCountOverride`/`EffectiveThreadCount`/
      `UsingAvx2` を追加。`EffectiveThreadCount`/`UsingAvx2` は明示セッターが無ければ
      環境変数 `MYE_MODAL_THREADS`/`MYE_MODAL_FORCE_SCALAR` も見る (`--modal-bake` を
      CLI フラグを増やさずにスレッド数/経路だけ変えて再入するための計測専用の口)。
      `Infer()` はこれらを使って `Conv3dRaw`/`ConvTranspose3dRaw`/`ReluRange`/`AddRange`
      へ明示的に `useAvx2`/`maxThreads` を渡す。
  - `src\Engine\Engine\Modal\ModalSelfTest.cpp` (持ち越し nit #2 とは別、受け入れ条件 23
    のための追加): fixture 検査 (12) を「AVX2 経路 (auto)」と「強制スカラー経路」の 2 回
    実行するよう拡張し、両方の max|Δ| をログへ出す。加えてスレッド数 0 (強制直列) vs 8 で
    `Infer()` を独立に呼び、戻り値を `memcmp` で完全一致することを検査する節を新設。
  - `src\Engine\Engine\Modal\DmNet.h` / `tools\check_rules.ps1`: 持ち越し nit #1。
    `kDmNetMaxParamCount` ⇄ `model.py::MAX_PARAM_COUNT` を `$constGroups` に登録
    (桁区切り `'`/`_` を含む値を検査対象にした初めてのグループなので、値抽出後に
    `-replace "['_]", ''` してから `[int]` 化するよう共通ループ側にも 1 行追加)。
    DmNet.h の「登録していない」旧コメントを更新。
  - 持ち越し nit #2 (`--modal-backend` の CLI selftest) は**確認のみで変更なし**:
    `EngineCliSelfTest.cpp:228-235,274-275` に「`d3d11cs` は spelling accepted (consumed)」
    「`foo` (綴り違い) は `errors==1`」「値なしは `notMine==1, errors==0`」が既にあり、
    `ModalSelfTest.cpp:747-755` (`SetBackendByName`) に「`d3d11cs` → `Name()=="cpu"`」
    「`foo` → 拒否」が既にある。sub-09.md 本文の文言 (未知の名前 → WARN+cpu 縮退 /
    綴り違い → exit 1) と完全に一致しており、追加は不要と判断した。

仕様との差分:
  - [逸脱] sub-09.md は「危なければ `CpuModalBackend` 内に固定サイズの小さなプールを持つ」
    と示唆していたが、実装は**インスタンスに紐付く永続プールではなく、呼び出しのたびに
    使い捨てるスレッド分割 (`ParallelSpan`)** にした。理由: (a) 永続プールをクラスの
    メンバとして持つと、`Conv3dRaw`/`ConvTranspose3dRaw` の**自由関数としての公開 API**
    (spec §5 受け入れ条件 11 が要求する「素朴 6 重ループ照合に selftest が直接叩く低レベル
    API」) にプール型を露出させる必要が生じるか、さもなくば `CpuModalBackend::Infer()`
    専用の別実装を用意してロジックを二重化することになる。(b) 仮に自由関数のまま
    process-global な共有プールを持たせても、`LocalThreadPool::ParallelRanges` 自身が
    「同期呼び出しは常に高々 1 バッチ」という**まさに jobs::JobSystem と同型の前提**を
    抱えることになり、2 インスタンスが同時にそのプールを使えば同じ種類の破損を再導入する
    (ModalSoundLibrary は現状 1 度に 1 ジョブしか処理しないので実害は無いはずだが、
    「安全に見えて実は単一呼び出し元前提」という罠を新設するのは避けたかった)。
    (c) スレッド生成コストを実測 (0.076 ms/スレッド) した結果、Infer() 1 回あたり
    数十回の生成/join を行っても合計は数 ms で、目標の秒未満の推論時間に対して無視できる
    水準だった。以上から、**「呼び出しのたびに完結し、共有状態を一切持たない」設計**の方が
    「持続プールだが単一呼び出し元を祈る」設計よりも明らかに安全で、しかもコストが
    無視できると判断し、使い捨て方式を選んだ。結果としてリダクション (K 次元) を割らない
    という受け入れ条件 23 の本質的な要求は同じ強さで満たしている。
  - [追加] `Conv3dRaw`/`ConvTranspose3dRaw` の末尾に `useAvx2=false, maxThreads=0` の
    既定値付き引数を追加した (sub-05 が selftest から直接叩く低レベル API として公開済み)。
    既定値をこの値にしたのは、既存の「素朴 6 重ループとの 1e-6 照合」(spec §5 受け入れ
    条件 11、既存のまま) が AVX2 の FMA (乗算+加算を 1 回で丸める) を経由すると、
    参照実装 (乗算→加算の 2 回丸め) との最終ビットが変わりうるため — 実測では 1e-6 の
    枠内に収まったが (下記「検証」)、既定値を「sub-05 時点と 1 命令も違わない計算」に
    固定しておく方が、将来ネットの層構成が変わって K が伸びたときにこの厳しい許容を
    黙って壊すリスクを避けられると判断した。`CpuModalBackend::Infer()` は明示的に
    `UsingAvx2()`/`EffectiveThreadCount()` を渡すので、実運用の高速化には影響しない。
  - [追加] `MYE_MODAL_THREADS`/`MYE_MODAL_FORCE_SCALAR` 環境変数。spec/sub-09.md は
    「環境変数や CLI で変えられるようにしてよい (計測用)」と裁量を残していたので、
    CLI フラグ (EngineCli.cpp の表 + EngineCliSelfTest への追加) より軽い環境変数を選んだ。
    結果の意味を変えない (どちらの値でも `.msfm` の中身は同じ) 計測専用のノブなので、
    永続化 (project_settings.json 等) は行っていない。

検証:
  - `pwsh -File tools\gen_project_files.ps1`: 新規ファイルなし (CpuModalBackend.h/.cpp と
    DmNet.h/ModalSelfTest.cpp/check_rules.ps1 の内容変更のみ) のため未実行 — 実行不要と
    判断した根拠は `git status` で `.vcxproj`/`.filters` に差分が出ないことを別途確認済み
  - Debug ビルド (`/p:MyeWarnAsError=true`) → 0 エラー・0 警告 (LNK4204 の imgui pdb のみ、既知)
  - Release ビルド (同上) → 0 エラー・0 警告
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → **exit 0**、`FAIL:` 0 件、
    **4097 件 PASS** (sub-05 時点の 4089 件 + sub-09 で追加した 8 件)。
    fixture 照合の実測 max|Δ|: **auto (AVX2+4threads) = 3.57628e-07** /
    **forced-scalar = 4.76837e-07** (いずれも許容 1e-3 の中、sub-05 の基準値 4.77e-07 から
    **桁の悪化なし** — forced-scalar はほぼ同値、auto はむしろ僅かに小さい)。
    `Conv3dRaw`/`ConvTranspose3dRaw` の素朴 6 重ループ照合 (既定引数 = スカラー・直列) は
    max|Δ| 1.19e-07〜9.54e-07 で **1e-6 の枠内を維持** (受け入れ条件 11、既存のまま不変)。
    スレッド数 0 (強制直列) vs 8 の `Infer()` 出力が **`memcmp` で完全一致**することを確認
  - **速度** (Release、`python export.py --random-full` の基準構成 [widths 16/32/64/96、
    paramCount=1,682,448]、`--modal-bake`、初回 = キャッシュ削除後): **3 回の独立実行**
    (bakeMsAvg) = **525.87 / 446.83 / 450.02 ms/メッシュ** (382 メッシュ全件 Ready、
    Failed 0 件)。sub-05 の基準値 4.75〜5.34 s/メッシュ に対し **約 9.5〜11.9 倍**の高速化。
    **受け入れ条件 22 (≤ 0.6 s/メッシュ) を達成**。目標の ≤ 0.3 s には届いていない
    (下記「不安・質問」)。この機体は `hardware_concurrency()=12` で
    `ModalCpuDefaultThreadCount()=min(4,11)=4` スレッドを使用、AVX2+FMA 対応機
  - **2 回目の `--modal-bake` (`.msfm` ヒット)**: `bakeMsAvg=0.38` (sub-05 の回帰修正を
    維持していることを確認)
  - **再現性 (受け入れ条件 4)**: 同一バイナリ・同一入力 (基準構成の `.dmnet`)・同一設定で
    キャッシュを削除して 2 回焼き、`cache\cooked\*.msfm` (19 ファイル、382 メッシュ分) を
    SHA256 で比較 → **全 19 ファイル完全一致**
  - **スレッド数非依存 (受け入れ条件 23)**: fixture (`tests\deepmodal\fixture.dmnet`) を
    `assets\deepmodal\deepmodal.dmnet` へ仮置きし、`MYE_MODAL_THREADS=1` と
    `MYE_MODAL_THREADS=4` でそれぞれキャッシュ削除後に焼いて `.msfm` (19 ファイル) を
    SHA256 で比較 → **全ファイル完全一致**。上記 selftest の `memcmp` 検査 (0 vs 8 スレッド)
    と合わせて 2 経路 (CLI レベルの `.msfm` / API レベルの `Infer()` 戻り値) で確認
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`。新規登録した
    `kDmNetMaxParamCount`/`MAX_PARAM_COUNT` の値を意図的にずらして 1 回検出することも確認
    (`ERROR [rule 9] ... must match across C++ and HLSL` が出ることを確認してから元に戻した)
  - `tools\replay_verify.bat` (`MYE_REPLAY_JOBS=3`) → **全 13 ジョブ PASS
    (267.1s)**、`[PASS] replay consistency (Debug/Release, 8 scenes...) + snapshot
    round-trip + time travel + rule check`。sim 側に 1 バイトも触れていないことの直接証拠
  - 仮置きした `.dmnet`・`bin\x64\{Debug,Release}\cache` は検証後にすべて削除し、
    `git status` で作業ツリーが意図した差分のみであることを確認済み

自己採点 (1-5):
  仕様適合: 5 — 受け入れ条件 22 (≤0.6s、実測 446.83〜525.87ms) と 23
    (AVX2/スカラー両経路が fixture 許容内・桁悪化なし、スレッド数非依存を 2 経路で確認、
    再現性、既存 selftest/replay_verify 不変) をすべて満たした。目標の ≤0.3s は未達だが、
    これは「合格線」ではなく「目標」であり、spec 自身も「0.6〜1.0s に着地したら報告」と
    しているだけで、実測はその帯にも入っていない (0.6s を明確に下回っている) ので
    エスカレーション要件には該当しない。念のため下の「不安・質問」で明示的に報告する
  正しさ: 5 — fixture 照合 (AVX2/スカラー両経路、1e-3 許容に対し実測 e-07 台、sub-05 比で
    桁の悪化なし)、素朴 6 重ループ照合 (1e-6、既存のまま不変)、スレッド数非依存
    (memcmp 完全一致 + CLI レベル SHA256 完全一致)、再現性 (SHA256 完全一致)、
    replay_verify 全 13 ジョブ PASS (sim 不変の直接証拠) と、あらゆる主張を実行結果で
    裏取りした
  コード品質: 4 — 既存の `GemmBiasAdd`/`Conv3dRaw`/`ConvTranspose3dRaw` の構造を保ったまま
    出力側の並列化点を追加し、日本語コメントで「なぜこの設計か」(JobSystem 調査の結論、
    使い捨てスレッドを選んだ理由、K 次元を割らない理由) を残した。nit: `Conv3dRaw` と
    `ConvTranspose3dRaw` の `ParallelSpan` 呼び出しパターン (行/ci のレンジ分割) が
    やや重複しており、共通ヘルパへ切り出す余地は残っている (どちらも「独立した行/チャンネル
    ごとに書き込み先が素の並列化」という同型だが、ループ本体の中身が違うので無理に
    共通化すると読みにくくなる可能性があり、あえて残した)
  テスト: 5 — ModalSelfTest に AVX2/スカラー両経路の fixture 照合とスレッド数 memcmp
    検査を追加 (8 件)、CLI レベルで速度・キャッシュヒット・再現性・スレッド数非依存を
    それぞれ独立の実機実行で確認、check_rules.ps1 の新規 constGroup は意図的な不一致注入で
    検出力そのものも確認、replay_verify.bat フルセットも通した

不安・質問:
  - **速度の目標 (≤0.3s) 未達を報告** (spec §5 受け入れ条件 22 の指示どおり): 実測
    446.83〜525.87 ms/メッシュ (合格線 600ms は明確に下回っているので「0.6〜1.0s の
    グレーゾーン」には該当しないが、目標である 300ms への到達は確認できていない)。
    さらに詰める余地の見立て (実装済みではない、次の一手の候補):
    (a) `Conv3dRaw`/`ConvTranspose3dRaw` は呼ばれるたびに `cols`/`padded` バッファを
    `std::vector<float>(...,  0.0f)` で新規確保・全域ゼロ初期化している。ネット構造
    (op の k/stride/pad/チャンネル数) は `Prepare()` の時点で固定なので、これらのバッファを
    `CpuModalBackend` のメンバとして永続化し、初回だけゼロ初期化、以降は「有効な位置だけ
    上書きする」(無効な位置は前回のゼロのまま不変) 形にすれば、大きい層 (例: 432×32768=
    14M float ≈ 56MB) の memset/確保コストを削れる可能性がある。この最適化は自由関数
    (`Conv3dRaw`/`ConvTranspose3dRaw`) の外側 (`CpuModalBackend::Infer()` 専用の内部経路) で
    やる必要があり、コードの二重化を伴うため今回は見送った (b) AVX2 の M 方向ブロック幅
    (現状 4 行) を広げて 1 回の B ロードあたりの FMA 数を増やす、等。
    どこまで詰めるかは planner の裁定を仰ぎたい (「600ms を切れたので良し」か
    「300ms まで追加ラウンドで詰める」か)
  - `ParallelSpan` を jobs::JobSystem の代わりに新設した設計判断 (上記「仕様との差分」) は
    妥当と考えているが、念のため裁定を仰ぎたい。将来 GPU バックエンド
    (`D3d11ModalBackend`) が同種の並列化を必要にしたとき、`ParallelSpan` を `Modal/` 内の
    共通ヘルパへ格上げする (今は `CpuModalBackend.cpp` の anonymous namespace に閉じている)
    かどうかも同様に判断が要る

触ったファイル:
  - src\Engine\Engine\Modal\CpuModalBackend.h
  - src\Engine\Engine\Modal\CpuModalBackend.cpp
  - src\Engine\Engine\Modal\DmNet.h
  - src\Engine\Engine\Modal\ModalSelfTest.cpp
  - tools\check_rules.ps1

申し送り:
  - sub-06 は本サブ完了により再開してよい (依存 sub-01, sub-05, sub-09 が揃った)
  - 上記「不安・質問」の速度の追加最適化候補 (バッファ永続化、AVX2 ブロック幅拡大) は
    今回実装していない。0.6s の合格線は十分な余裕 (約 12〜25%) を持って満たしているので、
    急ぎでなければ後回しでよいと考える
  - `MYE_MODAL_THREADS`/`MYE_MODAL_FORCE_SCALAR` は計測専用の環境変数で、CLAUDE.md の
    CLI 一覧 (`--modal-*`) には載せていない (CLI フラグではないため)。文書化するなら
    M76h (sub-08) の文書サブで CLAUDE.md の「環境の罠」節などに追記する形が良さそう

---

SELF_EVAL: sub-09 (round 2)
実装 (指摘番号ごと):
  1. [must] **チャンク幅を SIMD 幅 (8) の倍数へ量子化**。`CpuModalBackend.cpp` の
     `ParallelSpan(total, maxThreads, fn, align=1)` に第 4 引数 `align` を追加し、
     `align>1` のとき `chunk = roundUp(ceil(total/nThreads), align)` へ切り上げる。
     根拠 (量子化だけで十分な理由、コメントにも記載): chunk 自体が align の倍数なら、
     どのチャンクも境界 `t*chunk` が align の倍数になる (整数倍だから)。最後のチャンクの
     終端だけが `total` (align の倍数とは限らない) になるが、その手前のチャンク開始点は
     align の倍数のままなので、`GemmBiasAddAvx2Range` がその開始点から 8 列ずつ処理して
     余る端数は常に `total mod 8` 個で、絶対 index `[total - total%8, total)` に固定される —
     nThreads をいくつに変えてもこの「端数の位置」が動かない。`GemmBiasAddDispatch` から
     `ParallelSpan(n, maxThreads, worker, kGemmAvx2Width)` で呼ぶ (新設した
     `constexpr size_t kGemmAvx2Width = 8`、`GemmBiasAddAvx2Range` の SIMD 幅と同じ定数を
     共有し、2 箇所が食い違わないようにした)。im2col の行分割・`ConvTranspose3dRaw` の
     ci 分割・`ReluRange`/`AddRange` は align=1 のまま (要素ごとの max/add は SIMD でも
     スカラーでも 1 回の演算・1 回丸めで完全に同値なので端数の概念自体が無い。
     量子化が要るのは GEMM の N 分割だけ)。
  2. [must] **selftest に SIMD 幅で割り切れないスレッド数を追加**。
     `ModalSelfTest.cpp` の節 (12) 末尾を書き直し、T=1 を基準に **T=2/3/4/5/8** それぞれで
     `Infer()` を呼んで `memcmp` 完全一致を検査するループへ変更 (round 1 は T=0(直列) vs
     T=8 の 2 点だけで、両方とも N が 8 の倍数のため偶然一致していた欠陥ケース)。
  3. [should] `ParallelSpan` の使い捨てスレッド設計は指摘どおり**変更なし** (承認事項)。
  4. [nit] `Conv3dRaw`/`ConvTranspose3dRaw` の既定引数の判断も**変更なし** (承認事項)。
  5. [nit] 速度の追加最適化 (バッファ永続化・AVX2 ブロック幅拡大) は**引き続き見送り**、
     申し送りに留める (承認事項)。

検証:
  - Debug/Release ビルド (`/p:MyeWarnAsError=true`) → 0 エラー・0 警告
  - `cmd /c "bin\x64\Debug\Editor.exe --selftest"` → exit 0、`FAIL:` 0 件。
    新設した T=1 基準 vs T=2/3/4/5/8 の `Infer()` 出力 `memcmp` 検査が**全て PASS**
    (fixture net、`CpuModalBackend: Infer() output is bit-identical T=1 vs T=2/3/4/5/8`)
  - **CLI レベル、fixture net (`tests\deepmodal\fixture.dmnet` を仮置き)**:
    `MYE_MODAL_THREADS ∈ {1,2,3,4,5,8}` でそれぞれキャッシュ削除後に `--modal-bake` (19
    モデル・382 メッシュ)、`.msfm` (19 ファイル) を T=1 基準に SHA256 比較
    → **T=2/3/4/5/8 の全ファイルが T=1 と完全一致**
  - **CLI レベル、フルサイズ net (`--random-full` 基準構成、指摘 #1 が報告した実際の再現条件)**:
    `MYE_MODAL_THREADS ∈ {3,4,5}` でそれぞれキャッシュ削除後に `--modal-bake`、`.msfm`
    (19 ファイル) を相互比較 → **T=4 vs T=3 (round 1 で全 19 ファイル不一致だった組)** =
    **全一致**、**T=4 vs T=5 (同、不一致だった組)** = **全一致**、T=3 vs T=5 = 全一致。
    **planner が報告した失敗の再現条件そのもので修正を確認した**
  - **速度 (再測定)**: フルサイズ net、`--modal-bake` の bakeMsAvg = **T=3: 503.26ms /
    T=4: 457.66ms / T=5: 440.20ms** (382 メッシュ全件 Ready)。量子化でチャンクが最大 8 要素
    分粗くなる影響はごく僅かで、round 1 の測定値 (446.83〜525.87ms) と同水準。
    **受け入れ条件 22 (≤0.6s) を維持して達成**
  - `pwsh -File tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `tools\replay_verify.bat` (`MYE_REPLAY_JOBS=3`) → **全 13 ジョブ PASS (267.8s)**、
    sim 側に触れていないことを再確認
  - 仮置きした `.dmnet`・`bin\x64\{Debug,Release}\cache` は検証後にすべて削除し、
    `git status` で作業ツリーが意図した差分のみであることを確認済み

自己採点 (1-5):
  仕様適合: 5 — 指摘 #1/#2 (must) を修正し、planner が報告した失敗条件 (フルサイズ net、
    T=4 vs T=3/T=5) そのもので不一致が解消したことを実測で確認した。#3/#4/#5 (承認事項)
    は変更なし。速度・fixture 精度・`/arch` 非変更・`replay_verify` は round 1 の水準を
    維持
  正しさ: 5 — 修正の理屈 (チャンク境界を align の倍数にすれば端数位置が nThreads に
    依らず固定される) を検証前に明文化してから実装し (予測してから実行、deep-reasoning)、
    fixture net の API レベル (memcmp、T=1 vs 2/3/4/5/8) と CLI レベル (SHA256、fixture +
    フルサイズ net 両方)、さらに**指摘された失敗条件そのもの**の 3 系統で裏取りした
  コード品質: 5 — 修正が `ParallelSpan`/`GemmBiasAddDispatch` の 1 箇所 (司会の見込みどおり)
    に閉じ、`kGemmAvx2Width` を共有定数化して SIMD 幅と量子化単位の食い違いを構造的に
    防いだ。round 1 の nit (Conv3dRaw/ConvTranspose3dRaw の並列化パターン重複) は未着手のまま
    (指摘されていないので据え置き)
  テスト: 5 — selftest に SIMD 幅で割り切れない本数 (3, 5) を含む 5 段階のスレッド数
    比較を追加し、CLI レベルでも fixture net 全 6 段階 + フルサイズ net で指摘の失敗条件を
    直接再現・確認した

不安・質問: なし

触ったファイル:
  - src\Engine\Engine\Modal\CpuModalBackend.h (変更なし、round 1 のまま)
  - src\Engine\Engine\Modal\CpuModalBackend.cpp (round 2 で修正)
  - src\Engine\Engine\Modal\ModalSelfTest.cpp (round 2 で修正)
  - src\Engine\Engine\Modal\DmNet.h (変更なし、round 1 のまま)
  - tools\check_rules.ps1 (変更なし、round 1 のまま)

申し送り: round 1 のまま変更なし (sub-06 は再開可能、速度の追加最適化候補は後回しでよい、
  環境変数 2 種は M76h で文書化)

## フィードバック履歴

## フィードバック履歴
- round 1: **VERDICT: REWORK** (planner、2026-09-16)。速度 (446–526 ms/メッシュ、約 9.5–11.9 倍) と `/arch` 非変更・`replay_verify` 全緑は確認できたが、**受け入れ条件 23 (スレッド数非依存) が実測で破れている**。planner が `MYE_MODAL_THREADS` を変えて `--modal-bake` を回し `.msfm` を SHA256 比較: **T=4 vs T=3 → 19 ファイル全部不一致 / T=4 vs T=5 → 不一致 / T=4 vs T=2 → 一致**。原因は `GemmBiasAddAvx2Range` が 8 列ブロックを FMA (1 回丸め) で処理し、端数を `GemmBiasAddScalarRange` (乗算 + 加算 = 2 回丸め) へ回すこと。`ParallelSpan` のチャンク幅 `ceil(total/T)` が 8 の倍数でないと**どの列が端数になるかがスレッド数で変わる**。K 次元を割らない構造 (両 Range 関数が `nBegin/nEnd` しか取らず K は常に全域) は**正しく実装されている** — 破れているのはそこではない。★既定スレッド数が `min(4, hw-1)` なので、**4 コア機では T=3** = この 12 コア機と違う `.msfm` が焼かれる (spec が防ごうとした「機械によって cooked が変わる」そのもの)。修正はチャンク幅を SIMD 幅の倍数へ量子化するのが最小。
- round 2: **VERDICT: OK** (planner、2026-09-16)。**planner が round 1 と同じ手順で独立に再現確認**: `tests\deepmodal\fixture.dmnet` を仮置きし、キャッシュを消して `MYE_MODAL_THREADS ∈ {1,2,3,4,5,8}` で `--modal-bake` → `.msfm` 19 本の SHA256 が**全スレッド数で一致**。round 1 で割れた **T=4 vs T=3 / T=4 vs T=5 の 2 組がどちらも一致**に変わったことを自分の手で確認した。
  コード確認 (司会の (i)): `ParallelSpan` の `chunk` を align の倍数へ切り上げる実装は境界 `t*chunk` を必ず align の倍数にする。端数が出るのは最終レンジの終端 (`total`) だけで、絶対 index `[total - total%8, total)` に固定される = スレッド数に依らない。エッジも正しい — `nThreads<=1` は `fn(0,total)` で直列と同一経路、`begin >= total` で break するので空スレッドを作らない、`total < chunk` (例 total=5/align=8) は 1 レンジに縮退して T=1 と同じ。`kGemmAvx2Width` を SIMD 幅と量子化単位で共有しているので 2 箇所が食い違わない。
  (ii) `align=1` のまま残した経路の妥当性も確認した: **`ReluRange` は要素ごとの max、`AddRange` は要素ごとの単一 IEEE 加算**で、どちらも SIMD 版とスカラー版が**同じ丸め (実質丸め無し / 1 回)** なので端数位置が動いても値が変わらない — FMA (1 回丸め) と 乗算+加算 (2 回丸め) が食い違った GEMM とは性質が違う。im2col の行分割は純粋なデータ移動、`ConvTranspose3dRaw` の `fillPadded` は `ci` ごとに自分のチャンネル面だけへ書く (重なり無し・加算無し) ので分割不変。**round 1 と同型の見落としは無い**。
  (iii) 速度は T=3 503ms / T=4 458ms / T=5 440ms で受け入れ条件 22 (≤600ms) を維持。量子化でチャンクが最大 7 要素粗くなる影響は N=32768/4096/512/64 に対して無視できる。
