# sub-04: State バッファのスナップショット・ハッシュ・寿命

- 依存: sub-03
- 状態: OK (M91d)
- 往復: 0
- 再編: 2026-10-12 (旧 sub-03。中身は同じ。U3 はユーザー確認済み)

## やること
spec D7 / D8、4.2 (SimSnapshot・WorldHash)。

1. **GameLogic 用ストア**の State 部を、SimSnapshot の新節「ComputeState」に保存・復元する (スロット表 = 世代・使用中・所有エンティティ・stride・count + 中身)。
   `kSimSnapshotVersion` を着手時点の次の版に上げる (今なら 48→49)。版を固定値で見ているテスト (`AcousticAudioSelfTest.cpp:129` など) も直す。
2. `HashSources` に ComputeState を足す (使用中のスロットを昇順に)。`HashWorldDetailed` と `--hash-diff` にも出す。
3. 所有: 作成したエンティティの破棄 (tick 末) で解放する。Play 停止とシーン読込では全部解放する (`ReleasePlaySessionEngineState`、`PlayModeController.cpp:99`)。C# 用のストアも Play 停止で解放する。
4. Fx と C# 用のストアは、スナップショットにもハッシュにも入れない。
4b. (sub-03 から移管) **HeadlessSim の経路に ComputeService を配線する** (所有・Bind、tick 頭の BeginTick(resim)、SimRefs。device-less なので GPU は無し)。Server.exe (`ServerLoop.cpp` は HeadlessSim を使う) と headless の replay 検証で、State の ForEach が EngineLoop と同じ結果になること。
5. 復元した後、FIELD のハンドルが同じバッファを指すことを selftest で確かめる。

## やらないこと (このサブでは)
- ABI (sub-05)

## 触る場所 (planner の見立て)
- `src/Engine/Engine/Replay/SimSnapshot.{h,cpp}`、`HashSources` の定義、エンティティを破棄する処理、`PlayModeController.cpp:99`
- `HeadlessSim` / `EngineLoop` で SimRefs を組み立てる箇所 (Server / Runtime / Editor のどれでも同じ参照が入るように)

## 受け入れ条件 (このサブ)
1. spec 5-5 (selftest)
2. `tools\replay_verify.bat` の snapshot round-trip と既存のシーンが PASS する
3. HeadlessSim に ComputeService があり、デバイスが無くても State の ForEach が動くこと (selftest または headless で確認)

## 検証コマンド
- Debug / Release ビルド、`Editor.exe --selftest`、`tools\replay_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-04 (round 1)
実装:
  - ComputeBufferStore: Slot に owner (EntityID)、Create に owner 引数、WriteSnapshot / ReadSnapshot / ApplySnapshot / HasLiveState。ReleaseAll はスロット表ごと初期化
  - ComputeService: CreateBuffer に owner、ReleaseDeadOwners (tick 末)、ReleaseAllBuffers (Play 停止・シーン読込)、Write/Read/ApplyGameSnapshot (落とした Fx は GPU 側も Forget)
  - SimSnapshot: CMP 節 (v49) を BT 節の後・World 節の前に追加、SimRefs.compute、SimSourcesOf の末尾引数、ComputeGameStoreOf
  - WorldHasher: SimSources.computeState (内容ゲート)、HashComputeState (スロット昇順。世代・持ち主・形・中身)、ダンプに Compute 行
  - TickRunner: ApplyStructuralChanges の直後に ReleaseDeadOwners、ハッシュの源へ computeState、シーン読込で ReleaseAllBuffers
  - EngineLoop / HeadlessSim: simRefs.compute と ctx.computeService を配線。HeadlessSim は ComputeService を所有 (Bind(nullptr, nullptr))、tickServices.computeService、Compute() アクセサ
  - PlayModeController: ReleasePlaySessionEngineState に computeService (既定 nullptr) を追加。EditorApp / EditorToolbar の 3 呼び出しを更新
  - AcousticAudioSelfTest の版固定値を 49 へ。ComputeServiceSelfTest の ReleaseAll の期待を「採番も戻る」へ更新
  - ComputeStateSelfTest.{h,cpp} を新設 (EditorMain の --selftest と ServerSelfTest から呼ぶ)
仕様との差分:
  - [逸脱] ComputeBufferStore::ReleaseAll が世代カウンタも初期化する (sub-03 では「世代は戻さない」でテストもそう固定)。Play 停止・シーン読込の後の採番を新規起動と同じにするため。sub-03 のテスト 1 行を更新した
  - [追加] Fx は SimSnapshot に「居た印と世代」だけを入れる。復元では世代が同じものだけ残し、外れた Fx は捨てる (GPU 側も Forget)。State と Fx が 1 つのスロット空間を共有しており、世代表を撮らないと復元後の State の採番が連続実行とずれるため
  - [追加] 自動解放は C# 用ストアにも掛ける (D8 の「作成したスクリプトのエンティティが所有」)。C# 用ストアはスナップショットにもハッシュにも入れない
  - [追加] 復元は State の総バイト枠 (stateTotalBytes) を検査しない (設定が録画時と違っても復元を拒まないため)。1 本あたりの上限と stride は検査する
  - [追加] HeadlessSim::Compute() アクセサ、EngineContext::computeService (Editor が Play 停止で全解放するため)
  - [未実装] 所有エンティティを CreateBuffer に渡す口は ComputeService 内部まで (ABI 経由は sub-05)。ABI を足すまでスクリプトは owner を渡せない
検証:
  - Debug / Release ビルド → エラーなし (警告は既存のもののみ)
  - bind\Debug\Editor.exe --selftest → exit 0、FAIL 0 件。Compute state (M91d) self test: PASS (48 項目)。server-net selftest (HeadlessSim のロールバック=スナップショット往復) も PASS
  - bind\Release\Editor.exe --selftest → exit 0、FAIL 0 件
  - bind\Debug\Server.exe --selftest → ALL PASS (M91d: HeadlessSim が ComputeService を持ち、device 無しで State の ForEach が CPU で完了し参照実行とビット一致)
  - tools\check_rules.ps1 → 0 error (50 warning は既存)
  - tools\replay_verify.bat → PASS (19 jobs、13 シーン + snapshot round-trip + jobs A/B + time travel + rule check)
自己採点 (1-5):
  仕様適合: 4 — 1〜5 と受け入れ 1〜3 を満たした。ReleaseAll の意味変更と Fx の扱いは差分に明記
  正しさ: 4 — 往復・ハッシュ・解放・採番を selftest で確認。ただし replay_verify の 13 シーンは State を使わない (内容ゲートで節が出ない) ので、State 入りの実 .rep の照合は ABI が付く sub-05 以降
  コード品質: 4 — 既存の 3 点セット契約 (ハッシュ・スナップショット・配線) に沿った。ComputeService::ReleaseDeadOwners が World を直接引く
  テスト: 4 — ストア単体・ハッシュ・SimSnapshot 往復・破棄での解放・HeadlessSim を確認。State 入りの replay 照合は未実施
不安・質問:
  - State のハッシュは中身を全バイト FNV で畳む。上限 (16MiB) まで使うと tick あたり数 ms 級になりうる。実測は sub-08 のデモで取るのが妥当か
  - Fx と State が 1 つのスロット空間を共有するため、Fx の作成有無 (デバイスの有無で Fx の作成を分けるスクリプト) が State のハンドル値を変える。ADR-030 に規約として書くべきか、ストアを種別ごとに分けるか
触ったファイル:
  src/Engine/Engine/Compute/ComputeBufferStore.{h,cpp}, ComputeService.{h,cpp}, ComputeServiceSelfTest.cpp, ComputeStateSelfTest.{h,cpp} (新規)
  src/Engine/Engine/Replay/SimSnapshot.{h,cpp}, WorldHasher.{h,cpp}
  src/Engine/Engine/Loop/TickRunner.{h,cpp}, EngineLoop.{h,cpp}, HeadlessSim.{h,cpp}
  src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp
  src/Editor/App/EditorApp.cpp, EditorMain.cpp, src/Editor/Scene/PlayModeController.{h,cpp}, src/Editor/Widgets/EditorToolbar.cpp
  src/Server/ServerSelfTest.cpp
  (生成物: build/Engine.vcxproj, build/Engine.vcxproj.filters は gen_project_files.ps1 の出力で、追跡対象なので一緒にステージが要る)
申し送り:
  - sub-05: ABI の CreateBuffer は ctx から実行中スクリプトのエンティティを取って ComputeService::CreateBuffer の owner に渡すこと (渡さないと自動解放されない)
  - sub-05: スクリプトの FIELD にハンドルを持つなら、Fx のハンドルは種別違いで復元後に無効になりうる (State は戻る)
  - docs の snapshot 版の記述 (engine_spec 等) は sub-09 で v49 に合わせる

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-12)。逸脱と追加はどれも採用した (spec 4.1.4 に規約を追記)。State のハッシュの実測 (上限の 16MiB 付近) と、State 入りの .rep の照合は sub-08 で行う。
