# sub-03: ForEach のエンジン核 (ストア 2 つ・実行先の判断・Fx の GPU 経路・ジョブ・対応表)

- 依存: sub-01
- 状態: OK (commit c206131)
- 往復: 0
- 更新: 2026-10-12 (U10「宣言なしは失敗」)
- 再編: 2026-10-12 (旧 sub-02。U1/U6 を反映して呼び出し元の種別・宣言・対応表を追加)

## やること
spec 4.1.3〜4.1.5、4.1.7 (対応表のデータ部)、D2 / D6 / D10 / D13 / D14 / D21 / D25 / D26。Engine 内部の C++ API として完成させる (ABI 露出は sub-05)。

1. バッファストア: State / Fx の 2 種。ハンドルは世代 32 + スロット 32 で、最小の空きスロットから決定的に採番する。上限 (要素数 4,194,240、スロット数、State の総バイト枠) を超えたら 0。
   **ストアのインスタンスは 2 つ**: GameLogic 用 (sub-04 でスナップショットの対象になる) と C# レーン用 (NoHash)。
2. 実行先の判断: spec 4.1.4 の決定表を 1 つの純粋関数で実装する。入力は、バッファ種別群、Execution、呼び出し元の種別 (C++ / C#)、その呼び出し元向けの CPU 版の有無、検査の結果、ハッシュ照合、要素数、デバイスの有無、枠の残り、resim、force-cpu、上書き、宣言の有無。
   **宣言なし (または呼び出し元がスクリプトではない) は、他のすべての判定より先に失敗にする** (spec D28、U10)。失敗したときはバッファに触らない。ログは (スクリプト型, カーネル) ごとに 1 回だけ出す。
   宣言表は「スクリプト型名 → 宣言したカーネル名の集合」として持ち、モジュールの取り込み (sub-05 / sub-06) で丸ごと差し替える。このサブではテスト用に直接入れる口だけ用意する。
3. CPU 実行: レンジ関数のポインタを JobSystem で回す (sub-01 の実行器)。C++ 版と C# 版は同じ「レンジ関数」の形で扱えるようにする (C# 版の登録は sub-06)。
4. Fx の GPU 経路: upload → bind → Dispatch → readback リング (`OcclusionCuller::PollStats` の流儀)。`Read` は最新の完了済み結果、`Wait` は明示ストール。決定的撮影と selftest では待つ。D3D の型は Renderer に閉じ込める。
5. ジョブ: `Status()` と `Target()`。
4b. (sub-01 からの申し送り) GPU のバインドは名前ではなく、リフレクションの BindPoint で行う (params = t0、RO は t1 から、RW は u0 から、宣言順)。要素数は `cbuffer __KernelCount`。読み取り専用のバッファ (params と RO) は **SRV だけ** で作る (SRV と UAV の両方を付けると、WARP の debug layer でアクセス違反を実測した。原因は特定できていない)。実例は `KernelSelfTest.cpp` の `RunKernelGpu`。
6. 負荷枠: tick ごとのカウンタ (tick の頭でリセット)。設定は構造体で受け取り、既定値を持つ (読込は sub-07)。
7. デバイス消失: M88 の `ReleaseGpu` / `RecreateGpu` の呼び出し点に並べる。
8. 統計と**対応表**: キーは (スクリプト型名, カーネル名)。値は最終の実行先、要素数、転送バイト数、CPU の ms、Wait の回数、状態。表示専用で、スナップショットにもハッシュにも入れない。呼び出し元のスクリプト型は ForEach の引数で受け取る (ABI では ctx から、C# では managed 側から渡す — 具体的な形は sub-05 / sub-06)。
9. ジョブの中からの呼び出しは失敗にする。スクリプトの Update がジョブの中で呼ばれる経路が無いことを確かめる (spec R3)。
10. `_DEBUG` で分岐させるのは統計とログだけにする。

## やらないこと (このサブでは)
- スナップショット / ハッシュ / 所有エンティティ (sub-04)、ABI (sub-05)、C# 版の登録 (sub-06)、設定の読込とインスペクター (sub-07)

## 触る場所 (planner の見立て)
- 新規: `src/Engine/Engine/Compute/` (ストア・判断・ジョブ・対応表)、`src/Engine/Renderer/Compute/` (Fx の GPU 部)
- `ComputeAbiRunner.*` は変更しない (D2)。デバイス消失の呼び出し点だけ並べる
- `TickRunner.cpp` (tick 頭の枠リセット、resim の受け渡し)

## 受け入れ条件 (このサブ)
1. spec 5-4: 決定表の全行 (呼び出し元が C++ / C# の場合、検査違反、宣言なしの失敗を含む) を selftest で確かめる。宣言なしの判定が State+Gpu の拒否などより先に効くこと、宣言なしで失敗したときにバッファの中身が変わらないことも確かめる
2. spec 5-6: Fx GPU の遅延 Read、Wait、デバイス消失→復旧
3. 採番が決定的であること。2 つのストアが独立していること。上限超過・count 0・二重解放・無効ハンドル・v21 ハンドルのどれでも落ちずに 0 / no-op になること
4. 対応表に (スクリプト型, カーネル) ごとの統計が積まれること (selftest)

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest` (両構成)、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

### round 1 (SELF_EVAL)
実装:
- Engine/Compute/ComputeBufferStore.{h,cpp} — State/Fx ストア。ハンドル = 世代32 (上位2bit=レーン印 Game 0b10 / Script 0b11、下位30bit=スロットごとの作成回数) + スロット32。最小空きスロット採番。上限 (要素 4,194,240 / スロット 1024 / State 総バイト / stride は 4 の倍数で 2048 以下 / 1 本 128MiB) 超過・count0 は 0。インスタンスは GameLogic 用と C# 用の 2 つ (ComputeService が持つ)
- Engine/Compute/ComputeDecision.{h,cpp} — 決定表 (spec 4.1.4) の純粋関数 `DecideExecution`。宣言なし/スクリプト外は最初に失敗
- Engine/Compute/KernelRange.h — 型を消したレンジ関数 `KernelRangeFn` / `KernelBufferRef` / `MakeRangeFn<&Name_Cpu>()` / `RunKernelRange` (C++ 版と C# 版は同じ形で登録する)
- Engine/Compute/ComputeService.{h,cpp} — ForEach 本体 (宣言→検査→ハッシュ→引数検証→決定→CPU/GPU 実行)、カーネル表・宣言表 (`ReplaceDeclarations` で丸ごと差し替え)・ソース情報 (`UpdateKernelSource` が KernelLintCache とハッシュを使う)、負荷枠、ジョブ (Status/Target/Wait)、対応表 (`Observations()`)、ReleaseGpu/RecreateGpu、ログは (型, カーネル, 理由) ごとに 1 回
- Renderer/Compute/FxGpuBackend.{h,cpp} — Fx の GPU 部。upload→bind (params=t0 SRV のみ / RO=t1.. / RW=u0.. / cbuffer は BindPoint をリフレクションで取得し stride も照合) → Dispatch → 3 本ステージングの遅延読み戻し + EVENT クエリ。D3D 型はここに閉じ込め
- Engine/Loop/TickRunner.{h,cpp} — `TickServices::computeService`、tick 頭で `BeginTick(resim)`
- Engine/Loop/EngineLoop.cpp — `ComputeService` を所有・Bind、デバイス消失の ReleaseGpu/RecreateGpu を ComputeAbiRunner の直後に並べた、終了時 ReleaseGpu
- Engine/Compute/ComputeServiceSelfTest.{h,cpp} + Editor/App/EditorMain.cpp — 回帰テスト 164 項目 (ストア / 決定表約 55 行 / CPU 実行・呼び出し元・失敗時の不変 / WARP の Fx GPU)
- build/Engine.vcxproj(.filters) は gen_project_files の再生成

仕様との差分:
- [追加] State に書く引数が 1 本でもあり、かつ Fx の引数 (RO/RW) が混ざるカーネルは失敗 (FailReason::MixedKinds)。GPU の結果が sim 状態へ入る穴 (D11) を ForEach の入口で塞ぐ。決定表に無い行 — 不要なら DecideExecution の 1 行を消すだけ
- [追加] Auto で CPU 版が無いカーネル (GPU しか選べない) は gpuMinElements と tick 枠を見ない。決定表の行 1 は「要素数 ≥ 下限 ∧ 枠内」が条件だが、そのままだと小さい Fx が理由なく失敗する
- [追加] 負荷枠 (dispatch 回数・upload バイト) と gpuMinElements が効くのは Auto だけ。明示の Gpu は無視する
- [追加] 作成の上限として stride は 4 の倍数かつ 2048 以下、1 本 128MiB、スロット 1024 (spec は要素数・スロット数・State 総バイトとだけ書いてあり、具体値の指定が無かった)
- [追加] ジョブハンドルは、同期の CPU 完了 = 定数 1、resim の Skipped = 定数 2、GPU の非同期だけ連番 (0x100 + serial)。ハンドルをスクリプトが FIELD に保存しても record/resim/デバイスの有無で値が割れないようにするため。Skipped は 0 ではなく定数にしたので、Status が Skipped を返せる
- [追加] 引数検証: RW が絡む同一バッファの重複渡し、2 本目以降の RW が第 1 引数より短い、params の大きさ違い、stride 違いは BadArgs 失敗 (バッファに触らない)
- [追加] GPU の dispatch 自体が失敗したときは、CPU 版が使えれば CPU で代替 (WARN 1 回)、使えなければ失敗
- [追加] ComputeService は ForEach / バッファ操作の「C++ 糖衣・ABI」を持たない (sub-05)。レーンは CallerKind で選ぶ (C++ = Game ストア、C# = Script ストア)
- [未実装] 所有エンティティによる自動解放、Play 停止/シーン読込での全解放、スナップショット (sub-04)。設定の読込と `--compute-force-cpu` の CLI (sub-07/08。`ComputeConfig` を `SetConfig` で受けるところまで)
- [未実装] HeadlessSim 経路への ComputeService の配線 (EngineLoop 経路だけ。ABI 露出と同時に sub-05 で必要になる)

検証:
- Debug / Release ビルド → 成功 (警告なし)
- `bin\x64\Debug\Editor.exe --selftest` → exit 0、Compute service self test: PASS (164 項目)。`bin\x64\Release\Editor.exe --selftest` → exit 0、Kernel / Kernel lint / Compute service とも PASS
- `tools\check_rules.ps1` → 0 error / 50 warning (rule 7。自分の追加分は無し、既存のみ)
- 実測 (WARP): 待たない Read は直後に「まだ古い」(状態 Pending) を返し、Wait の後に最新 (1 ULP 以内)。Mix の ComputeService 経由 GPU/CPU 差は 6.2e-06、tag 不一致 0。極小バッファ (1/3/64/65 要素) の SRV+UAV は WARP debug layer で落ちなかった (padding を最低 64 要素にしている)
- R3: ParallelRanges を使う 7 つの sim 系 (Render/Perception/PartFollow/CpuParticle/TwoBoneIk/Transform/CpuModal) がスクリプトを呼ばないことを grep で確認。ForEach の InJob 拒否は selftest で確認
- 未実行: replay_verify.bat (sim 状態に触っていない。TickRunner は BeginTick の呼び出しのみ追加。ABI/snapshot 変更なし)、実機 GPU

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜4 を網羅。差分の [追加] は小さく可逆。HeadlessSim 未配線と sub-04 送りの項目が残る
  正しさ: 4 — 決定表全行・採番・上限・無効ハンドル・InJob・WARP の遅延 Read/Wait/消失復旧を selftest 済み。実機 GPU と replay_verify は未実施
  コード品質: 4 — D3D は Renderer 内、純粋関数分離。ComputeService が大きめ (約 440 行) で ForEach が長い
  テスト: 4 — 164 項目。実機 GPU・本物のデバイス消失 (TDR) は未検証 (WARP の ReleaseGpu/RecreateGpu 経路のみ)

不安・質問:
- 未使用リソースの扱い: FXC が参照されない `params` や RO バッファをリフレクションから落とす場合、t レジスタの詰め方が変わるか未確認 (全カーネルが全引数を使う selftest しかない)。FxGpuBackend::Reflect は bind point と stride を照合して食い違いは失敗にするので静かな誤バインドにはなりにくいが、params を使わないカーネルの GPU 実行が失敗しうる。sub-05 でカーネルの実物が増えたときに確認が要る
- 上の [追加] のうち MixedKinds と「Auto で CPU 版なしは下限を見ない」は spec の決定表に無い解釈。採否を planner に確認したい
- `SyncFxMirror` 等で Fx の CPU 実行が GPU を待つ (明示ストール) のは、CPU⇄GPU を混ぜる Auto で毎フレーム起きうる。waitCount で観測できるが、設計として許してよいか

触ったファイル:
- src/Engine/Engine/Compute/ComputeTypes.h, ComputeDecision.{h,cpp}, ComputeBufferStore.{h,cpp}, KernelRange.h, ComputeService.{h,cpp}, ComputeServiceSelfTest.{h,cpp}
- src/Engine/Renderer/Compute/FxGpuBackend.{h,cpp}
- src/Engine/Engine/Loop/TickRunner.{h,cpp}, src/Engine/Engine/Loop/EngineLoop.cpp
- src/Editor/App/EditorMain.cpp
- build/Engine.vcxproj, build/Engine.vcxproj.filters (gen_project_files の結果。司会の判断でステージ)

申し送り:
- sub-04: ストアは `ComputeBufferStore::Slots()` / `MutableSlots()` / `HandleOf()` で全体を読み書きできる。Fx の `fxCpuDirty` はスナップショットに入れない (GPU 側の都合)。State の所有エンティティは Slot に未追加 (sub-04 で足す)
- sub-05: カーネル登録は `ComputeService::RegisterKernel(ComputeKernelDesc)`、宣言は `ReplaceDeclarations`、ソース情報は `UpdateKernelSource`。C++ 版は `kernel::MakeRangeFn<&ns::Name_Cpu>()` で作る。HeadlessSim への配線も必要

### round 2 (SELF_EVAL)
実装:
- #1: FxGpuBackend::Reflect を名前引きに変更。`__kernel_params` / 宣言名 (KernelArgDesc::name、BufferIo::name) / `__KernelCount` で引数を特定し、見つかった資源だけをリフレクションの BindPoint に束縛する。未使用で落ちた引数は束縛しない。名前の不明な資源と stride 違いは失敗。ComputeKernelDesc の各引数に `name` を追加 (RegisterKernel は空名を拒否)
- #1 確認: 新カーネル assets/shaders/kernels/SelfTestUnused.kernel.hlsl(+.meta) (params と途中の RO `skipped` を使わない) を足し、WARP の GPU 実行と CPU 式 (a+c) が一致することを selftest に追加。CPU 版もビット一致
- FXC の観測結果: 未使用の `__kernel_params` と `skipped` はリフレクションから落ち、残りは詰められた (a=t0, c=t1 で宣言位置の t3 ではない)。旧実装 (位置 = BindPoint) はこのカーネルで失敗していた
- #2: 対応なし (sub-04 へ移管済み)
- #3: ForEach の分割は今回見送り (修正範囲外)
検証: Debug / Release ビルド成功、両構成の --selftest が exit 0 で Compute service self test PASS、check_rules 0 error。
触ったファイル(追加分): FxGpuBackend.{h,cpp}、ComputeService.{h,cpp}、ComputeServiceSelfTest.cpp、assets/shaders/kernels/SelfTestUnused.kernel.hlsl(+.meta)、build/Engine.vcxproj(.filters) は変更なし(再生成済み)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。must 1 件: FXC の自動レジスタ割り当てで、未使用の params / RO が詰められたときに GPU 実行が失敗する件。BindPoint ではなく名前 (宣言名、params は `__kernel_params`) で引き、リフレクションに無い引数はバインドを飛ばす。未使用の params と未使用の RO を持つ selftest カーネルで、WARP の GPU 実行と CPU の結果が一致することを確かめる。[追加] の 7 件はすべて採用し、spec 4.1.4 / 4.1.5 に反映した。HeadlessSim への配線は sub-04 に移した。
- round 2: VERDICT OK (planner、2026-10-12)。must #1 は解消した (名前引きにした。FXC が未使用の資源を落として詰めることを観測)。nit: ForEach の分割は見送り、SelfTestUnused の .meta の guid は手で付けた値 — どちらも申し送り (sub-07 で .meta を揃える)。
