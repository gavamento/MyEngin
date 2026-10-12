# sub-08: デモと compute A/B (`--compute-force-cpu`)

- 依存: sub-06
- 状態: 未着手
- 往復: 0
- 再編: 2026-10-12 (旧 sub-07。C# 版のデモを追加)

## やること
spec 4.3 (デモ)、D11 / D15、受け入れ 10 / 12。

1. `--compute-force-cpu` と `kCfgComputeForceCpu = 1u << 5` (`SessionTypes.h`。Net 側の別名も揃える)。
2. `--compute-demo` を作る:
   - C++ / State: 群れ (例: 512 体。前の状態を RO、次の状態を RW の 2 バッファで持つ)。可視エンティティの LocalTransform に書く
   - C++ / Fx: 火花 (例: 4096 粒)。GPU を優先し、`DebugDrawLine` で描く (spec R2。写らなければ NoHash の描画コンポーネントに替え、実装メモに書く)
   - C# 版: 同じカーネルを C# スクリプトから呼ぶ小さな群れ (C# の CPU 版が実際に動くことを見せる)。C# レーンは record / verify 中は止まるので、replay の照合には入らない
   デモのスクリプトには、使うカーネルを全部宣言する (`COMPUTE_KERNELS` / `[ComputeKernels]`。宣言なしは失敗する — U10)。
   既存のデモに足す場合は末尾に足す (memory `demo-entity-order-is-rng-stream.md`)。
3. `tools\replay_verify.bat` に、compute シーンの Debug / Release 照合と `computeab` を足す (Auto で録り、`--compute-force-cpu` で検証する。`jobsab` と同じ形)。ジョブの一覧、PASS の文言、`:diagnose` も更新する。
4. 漏れの検出が働くことを 1 回だけ確かめる (Fx の Read 結果を LocalTransform に書く一時的な改変で computeab が FAIL すること。確かめたら元に戻す)。

5. (sub-04 から移管) State のハッシュのコストを計測する。State の総量を 1MiB / 16MiB (既定の上限) にしたときの tick あたりのハッシュの ms を記録する。16MiB で 1 tick (16.6ms) の 10% を超えたら、SELF_EVAL の「不安・質問」に出す。結果は ADR-030 に載せる (sub-09)。
6. State 入りの .rep が、Debug / Release と computeab の両方で一致すること (sub-04 では State を作れるスクリプトが無く、検証できなかった分)。

## 触る場所 (planner の見立て)
- `src/Engine/Engine/Session/SessionTypes.h:34-43`、`src/Engine/Engine/App/EngineCli.cpp`、`src/GameLogic/Scripts/`、C# のデモスクリプト、`assets/shaders/kernels/`
- `tools\replay_verify.bat` (:83 / :148 付近)

## 受け入れ条件 (このサブ)
1. spec 5-10
2. spec 5-12 (C# の群れも写る)。画像のパスを実装メモに書く
3. 漏れの検出を確かめた結果を実装メモに書く

## 検証コマンド
- Debug / Release ビルド、`tools\replay_verify.bat`、`--screenshot`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

### SELF_EVAL: sub-08 (round 1)

実装:
  - src/Engine/Engine/Session/SessionTypes.h / Net/NetSession.h / Loop/EngineLoop.h / Loop/SimInit.cpp / App/EngineCli.cpp — `kCfgComputeForceCpu = 1u << 5` (Net 側の別名 kNetCfgComputeForceCpu)、EngineConfig::computeForceCpu、BuildSessionConfigBits、`--compute-force-cpu`。
  - Loop/EngineLoop.cpp / Loop/HeadlessSim.cpp — Bind の後、ApplyComputeSettings の前に forceCpu だけを起動オプションとして入れる (Apply は forceCpu を保つ)。Runtime / Editor / Server / headless verify が同じ経路。
  - assets/shaders/kernels/DemoSwarmStep.kernel.hlsl (+ .meta) — 群れ 1 ステップ (RW next / RO prev。目標の輪・隣 2 体・jitter。sin / cos を通る)。
  - assets/shaders/kernels/DemoSparkStep.kernel.hlsl (+ .meta) — 火花 (RW sparks 1 本。重力・床の跳ね・撃ち直しの乱数は HashU32)。
  - src/GameLogic/Scripts/ComputeDemoSwarm.cpp — C++ State。512 体、2 本の State バッファを毎 tick 入れ替え、DemoBoid_%03u の LocalTransform へ FindByName で書く。FIELDS はバッファのハンドル 2 本・flip・steps。
  - src/GameLogic/Scripts/ComputeDemoSparks.cpp — C++ Fx。4096 粒、Auto で GPU 優先、DebugDrawLine で描く。FIELD なし (REGISTER_SCRIPT_NO_FIELDS)。Fx のハンドルは登録しないメンバに置き、Read が失敗したら作り直す。
  - assets/scripts/ComputeCsSwarm.cs (+ .meta) — C# State。48 体、同じ DemoSwarmStep を C# の CPU 版で走らせ DemoCsBoid_%02u へ書く。
  - src/Engine/Engine/Demo/DemoContent.cpp/.h, ShowcaseScenes.cpp — BuildComputeShowcaseScene と `--compute-demo` の行 (表の末尾)。既存デモの生成関数・順序には触れていない。
  - tools/replay_verify.bat — `compute` (chain: Debug stress + Release + Server) と `computeab` (Auto で録り、--compute-force-cpu で Debug / Release を照合) のジョブ、:failed の診断、件数の文言、:diagnose の引数一覧。
  - src/Engine/Engine/Compute/ComputeStateSelfTest.cpp — TestHashCost (State 1 / 16 MiB のハッシュ 1 回の ms をログに出す。時間は合否に使わない)。
  - src/Engine/Engine/App/EngineCliSelfTest.cpp — `--compute-force-cpu` の解析と configBits の 1 bit、`--compute-demo` が表にあること。
  - build/GameLogic.vcxproj(.filters)・src/GameLogic/Generated/Kernels.gen.{h,cpp} — gen_project_files.ps1 の出力 (版管理)。

仕様との差分:
  - [追加] kernel の params に time / radius を足し、群れを「輪を回る目標へ引かれる」形にした (最初の案は中心への引力 + 渦で、薄い帯に潰れて絵にならなかった)。sub-08.md は例 (512 体) までしか決めていない。
  - [追加] 3 つの塊の配置 (左 = 火花、中央 = C++ の群れ、右 = C# の群れ) とカメラ。群れの輪の半径は C++ 4.2 / C# 2.0。
  - [追加] 火花の Fx バッファのハンドルは FIELD に入れず、登録しないメンバに持たせた (spec 4.1.4 「Fx のバッファのハンドルを FIELD に保存しない」を守るため。無効になったら作り直す自己修復つき)。スナップショット復元での挙動は replay_verify の stress で PASS を確認。
  - [追加] State のハッシュのコスト計測を ComputeStateSelfTest に恒久テストとして足した (結果のログだけ。合否には使わない)。
  - [追加] C# の群れ 48 体は DemoSwarmStep を共用 (カーネルは 1 本のまま)。
  - R2 は解消: DebugDrawLine は Runtime の --screenshot に写る (NoHash の描画コンポーネントへの置換は不要)。
  - [未実装] 実機 GPU での Compute 欄 (GUI) の数値の目視確認。GUI を起動して見る手段が無かった。対応表 (GPU の回数・upload / readback バイト) は、下記の実機 GPU 一時実行の selftest で PASS を確認。

検証:
  - gen_project_files.ps1 (pwsh) → kernels 6 本 (DemoSparkStep, DemoSwarmStep, SelfTest*)、GameLogic.vcxproj 44 files。
  - MSBuild Debug|x64 / Release|x64 → エラーなし。警告は既存ファイル (SelfTestUnused.kernel.hlsl の C4100、ServerNetSelfTest / TagSelfTest) のみで、この sub の変更からは出ていない。
  - tools\check_rules.ps1 → 0 error(s), 50 warning(s) (すべて既存の規則 7 の WARN)。
  - tools\replay_verify.bat (全体、Debug + Release ビルド込み) → `[parallel] all 21 jobs passed in 233.0s` / `[PASS] replay consistency (... 14 scenes ... compute) + snapshot round-trip + jobs A/B + compute A/B + ...`。compute ジョブ (600 tick): Debug record → Debug stress 37 → Release → Server.exe (Release、GPU なし) の 3 つとも VERIFY PASS。computeab ジョブ: Auto で録って Debug / Release の --compute-force-cpu で PASS。
  - State が .rep のハッシュに入っていること: `--hash-dump` の tick 20 に `Compute slot / generation / owner / count=0x200 / stride=0x18 / data n=512` の行が 2 スロットぶん (Fx のスロットは出ない)。受け入れ 6 (State 入り .rep の Debug / Release / computeab の一致) は上の PASS で満たす。
  - Editor.exe --selftest (リポジトリ直下で実行): Release exit 0 (1 分 26 秒)、Debug exit 0 (約 7 分)。Compute 系 9 本 + Engine CLI + Kernel lint 全 PASS。stderr の `compile FAILED` は BadIndexProbe の既存の負例。
  - 漏れの検出 (受け入れ 3): ComputeDemoSparks の Update に「Fx の Read 結果 (sparks[17].pos.y * 0.0001) を Floor の LocalTransform に足す」一時改変 → `MYE_RV_TICKS=200 tools\replay_verify.bat --job computeab` が `[ERROR] [replay] HASH MISMATCH at tick 0` / `[FAIL] compute A/B Debug verify` で FAIL。元に戻して (一時改変は diff に無い) 全体 PASS を確認。
  - スクリーンショット (受け入れ 2、実機 GPU = RTX 3060、Release Runtime、`--compute-demo --font-embedded --width 960 --height 540 --frames 130 --shot-frame 120`): 左に火花 (DebugDrawLine)、中央に黄色い C++ の群れ、右に青い C# の群れが写る。
      C:\Users\akita\AppData\Local\Temp\claude\C--HAL-MyEngin\1045aa2f-0617-404e-a3b6-257b3a941d8f\scratchpad\compute_release_gpu.png (Auto)
      同じフォルダの compute_release_forcecpu.png (--compute-force-cpu。群れ 2 つは Auto と同じ絵、火花だけが少し違う = Fx の GPU 遅れ / 誤差)
      同じフォルダの compute_release_warp.png (--warp)
  - 実機 GPU (NVIDIA GeForce RTX 3060、debug layer on) での測定 (一時改変: KernelSelfTest と ComputeServiceSelfTest の WARP 初期化を実機に替え、FloatToInt の GPU プローブを足して Debug selftest を実行し、終わって元に戻した。ADR-030 用):
      Saxpy GPU/CPU: 最大 64 ULP / 最大絶対誤差 1.52588e-05 / 不一致 2660 / 10007 要素 (WARP は 0 ULP)。a*x+y を GPU が融合 (mad) するため。
      Mix GPU/CPU: 最大絶対誤差 6.19888e-06 (WARP の 6.2e-6 と同じ) / 最大 8890 ULP (値が 0 近傍の要素) / tag (整数ハッシュ) 不一致 0 / 4099 要素。
      FloatToInt / FloatToUint (sub-02 申し送り): NaN・±Inf・範囲外・-0.5・境界値など 18 値で GPU == CPU (int 0 / uint 0 不一致)。
      ComputeService の Fx GPU 経路: 遅延 Read・Wait・枠・極小バッファ・デバイス消失 (模擬) の 40 項目超が PASS。FAIL したのは「GPU と CPU の差が 1〜2 ULP 以内」などを要求する 8 項目だけ (実機では 64 ULP 出るため。WARP 前提の許容差)。
    つまり実機では Fx の結果が CPU とビットで一致しない (CI の WARP では一致する)。これが「State は常に CPU」の根拠になる。
  - State のハッシュのコスト (ComputeStateSelfTest の TestHashCost、ワールドハッシュ 1 回):
      Release: 1 MiB = 1.125 ms (1 tick の 6.7%) / 16 MiB = 17.379 ms (104.3%)
      Debug:   1 MiB = 3.113 ms (18.7%) / 16 MiB = 49.522 ms (297.1%)
    → 16 MiB で 1 tick の 10% を大きく超える (「不安・質問」へ)。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ 1〜3 (replay_verify の compute / computeab、画像、漏れの検出) と sub の 1〜6 を実施。実機 GPU の Compute 欄 (GUI) の目視は未確認。
  正しさ: 4 — replay_verify 21 ジョブ PASS (Debug / Release / Server)、selftest 両構成 PASS、漏れ検出の FAIL を実測。computeab は WARP (CI の --warp) では未実行。
  コード品質: 4 — 既存の流儀 (表の末尾、AttachScriptIfRegistered、カーネルの規則) に合わせた。C++ の群れが FindByName で毎 tick 512 回引く (BtDemoDriver と同じ流儀だが O(N^2)) のは割り切り。
  テスト: 4 — CLI / configBits の解析・表・ハッシュコスト計測を selftest に、デモの決定論は replay_verify に。computeab は WARP で別途確認していない。

不安・質問:
  - 【planner 判断】State のハッシュのコストが 16 MiB (既定の上限) で 1 tick の 104% (Release、17.4 ms)、1 MiB で 6.7%。ハッシュはバイト単位で畳んでいる (約 1 GB/s)。選択肢: (a) 上限を下げる (1 MiB 前後なら 10% 未満)、(b) ハッシュを 8 バイト単位 / 並列にする (ハッシュ値が変わるので .rep の録り直しと rep の版が要る)、(c) 変更のあった tick だけ畳む (ダーティ追跡)、(d) 現状を ADR に書いて「大きい State は重い」と注意する。このサブでは変更していない (ハッシュ値が動くため)。
  - 【確認】computeab の検出力は、Auto の Fx が「記録側と検証側で値が違う」ことに依存する。実機 GPU では tick 0 から割れるのを確かめたが、CI (--warp) では WARP の遅延 Read が同じ tick に完了する場合に検出が鈍る可能性がある (未確認)。CI で 1 回走らせて確かめるのが確実。
  - 実機 GPU の Fx GPU 経路の selftest は、WARP 前提の 1〜2 ULP の許容差 8 項目が実機で FAIL する。開発機の実機 GPU で selftest を回す運用が出たら許容差の扱いを決める必要がある (今は WARP 固定で PASS)。

触ったファイル:
  - src/Engine/Engine/Session/SessionTypes.h
  - src/Engine/Engine/Net/NetSession.h
  - src/Engine/Engine/Loop/EngineLoop.h
  - src/Engine/Engine/Loop/EngineLoop.cpp
  - src/Engine/Engine/Loop/HeadlessSim.cpp
  - src/Engine/Engine/Loop/SimInit.cpp
  - src/Engine/Engine/App/EngineCli.cpp
  - src/Engine/Engine/App/EngineCliSelfTest.cpp
  - src/Engine/Engine/Compute/ComputeStateSelfTest.cpp
  - src/Engine/Engine/Demo/DemoContent.cpp
  - src/Engine/Engine/Demo/DemoContent.h
  - src/Engine/Engine/Demo/ShowcaseScenes.cpp
  - src/GameLogic/Scripts/ComputeDemoSwarm.cpp (新規)
  - src/GameLogic/Scripts/ComputeDemoSparks.cpp (新規)
  - src/GameLogic/Generated/Kernels.gen.h / Kernels.gen.cpp (gen_project_files の出力、版管理)
  - build/GameLogic.vcxproj / build/GameLogic.vcxproj.filters (gen_project_files の出力、版管理)
  - assets/shaders/kernels/DemoSwarmStep.kernel.hlsl (+ .meta) (新規)
  - assets/shaders/kernels/DemoSparkStep.kernel.hlsl (+ .meta) (新規)
  - assets/scripts/ComputeCsSwarm.cs (+ .meta) (新規)
  - tools/replay_verify.bat
  - plans/m91-compute-foreach/sub-08.md (実装メモ)

申し送り:
  - sub-09: ADR-030 に (1) 実機 GPU の差 (Saxpy 64 ULP、Mix 6.2e-6、FloatToInt 一致、Fx の GPU 経路の許容差 8 項目) (2) State のハッシュのコスト表 (3) compute A/B の仕組みと「漏れを入れると tick 0 で FAIL」の実測 (4) `--compute-force-cpu` = kCfgComputeForceCpu (1u << 5) を書く。test_checklists へ: 実機 GPU の Compute 欄 (GUI) の数値、Editor のスクリプトのインスペクターで ComputeCsSwarm / ComputeDemoSwarm / ComputeDemoSparks を選んで Compute 欄を見る手順。
  - shot_verify の golden にはまだ入れていない (golden を触らない指示のため)。--compute-demo の画像は実機 GPU / WARP / force-cpu で撮れる (火花は Fx なので GPU と CPU で絵が少し違い、golden にすると WARP 依存になる点に注意)。
  - 一時ファイル (TmpFloatToInt.kernel.hlsl とその .meta、実機 GPU 用の selftest 改変、漏れの一時改変) は全部消して元に戻した。Debug selftest を scratchpad を CWD にして回すと Part / Fracture 系が相対パス不足で FAIL する (リポジトリ直下で回すこと)。

### SELF_EVAL: sub-08 (round 2)

実装:
  - #1(a): src/Engine/Engine/Replay/WorldHasher.cpp: HashStateBytes を新設し HashComputeState の中身の畳みに使う。8 バイト語を memcpy で読み、32 バイトごとに 4 本のレーンへ交互に流して依存の鎖を断つ。端数の 8 バイト語は先頭のレーンから順に、残りのバイトは従来の HashBytes。最後にレーンとサイズを HashCombine で 1 本に畳む。固定値の golden は無く、State が空のときは節ごと畳まれないので、既存シーンのハッシュは変わらない (snapshot の版は上げていない)。
  - #1(b): State の総量の既定値を 16 MiB から 8 MiB に下げた (ComputeBufferStore.h の ComputeStoreLimits、ComputeService.h の ComputeConfig、ComputeSettings.h のコメント、ComputeSettingsSelfTest.cpp の期待値 8388608)。spec 4.2 の project_settings.json の例も `"stateTotalBytes": 8388608` へ、「16MiB」の記述は「8MiB」へ (spec は planner が直す)。
  - #1(c): ComputeStateSelfTest.cpp の TestHashCost を 1 / 2 / 4 / 8 / 16 MiB に広げ、比較のため 16 MiB 用に limits を明示した。
  - #2: コード変更なし (一時改変で確認し、元に戻した)。

測定 (ワールドハッシュ 1 回。tick = 16.667 ms):
  - 旧 (バイト単位 FNV、Release): 1 MiB 1.125 ms (6.7%) / 16 MiB 17.379 ms (104.3%)
  - 新 Release、3 回の実測 (別々の selftest 実行): 16 MiB = 1.469 ms (8.8%) / 1.856 ms (11.1%) / 1.768 ms (10.6%)。8 MiB = 0.639 ms (3.8%) / 0.864 ms (5.2%) / 0.944 ms (5.7%)。1 MiB = 0.062 / 0.116 / 0.066 ms (0.4〜0.7%)。16 MiB が 10% の線をまたいで揺れる (メモリ帯域律速で約 9 GB/s) ので、安全側の 8 MiB を既定にした。32 MiB は確実に超える。
  - 最終コードの 3 点 (c):
      Release: 1 MiB = 0.066 ms (0.4%) / 8 MiB (既定) = 0.944 ms (5.7%) / 16 MiB = 1.768 ms (10.6%)
      Debug:   1 MiB = 0.550 ms (3.3%) / 8 MiB (既定) = 4.811 ms (28.9%) / 16 MiB = 9.134 ms (54.8%)
    Debug は旧 (16 MiB で 297%) から約 5 倍速くなったが、8 MiB でも 28.9% ある。Debug は計測用ではなく、既定値は Release で決めた。

#2 (--warp の computeab の検出力):
  - round 1 と同じ一時改変 (Fx の Read 結果 sparks[17].pos.y * 0.0001 を Floor の LocalTransform へ足す) を入れ、`MYE_EXTRA_ARGS=--warp --no-audio`、`MYE_RV_TICKS=200`、`tools\replay_verify.bat --job computeab` を実行 → `D3D11 device created: Microsoft Basic Render Driver (WARP ...)` のうえで `[ERROR] [replay] HASH MISMATCH at tick 0` / `[FAIL] compute A/B Debug verify`。WARP でも tick 0 から検出できる (WARP の遅延 Read / 初期の CPU ミラーとの差のため)。改変は元に戻した (差分に無い)。検出を確実にする追加の手段は不要。

仕様との差分:
  - [追加] 既定の State 総量を 16 MiB から 8 MiB に下げた (U19 の「10% 以内の最大の 2 のべき乗」を、16 MiB が 8.8〜11.1% で揺れるため 8 MiB と判断)。planner が 16 MiB を妥当と見るなら、既定の 4 か所と selftest の期待値を戻すだけで済む。
  - [追加] State のハッシュ関数を変えた (HashStateBytes)。State を持つ .rep のハッシュ値は round 1 とは別物 (外に出た .rep は無い)。

検証:
  - MSBuild Debug|x64 / Release|x64 → エラーなし (replay_verify.bat が最終コードで両方をビルド)。
  - tools\replay_verify.bat (最終コード) → `[parallel] all 21 jobs passed in 223.5s` / `[PASS] replay consistency (... 14 scenes ... compute) + ... jobs A/B + compute A/B ...`。
  - tools\check_rules.ps1 (最終コード) → 0 error(s), 50 warning(s)。
  - Editor.exe --selftest (リポジトリ直下、最終コード): Release exit 0、Debug exit 0。Compute service / state / settings すべて PASS。
  - #2 の --warp の漏れの検出: 上記 (HASH MISMATCH at tick 0)。

自己採点 (1-5):
  仕様適合: 4 — #1 (a)(b)(c) と #2 を実施。既定値は planner の「10% 以内の最大」を安全側に読んだ (8 MiB)。GUI の Compute 欄の目視は引き続き未確認。
  正しさ: 4 — replay_verify 21 ジョブ・selftest 両構成 PASS。ハッシュの新旧で既存シーンのハッシュ列は動かないことを replay_verify で確認 (State を使うのは compute のみ)。
  コード品質: 4 — 1 関数・定数 1 個・コメントは理由のみ。
  テスト: 4 — TestHashCost で 5 点を記録、ComputeSettingsSelfTest の既定値を更新。
不安・質問:
  - 16 MiB を既定に残すか 8 MiB にするかの判断 (上記)。16 MiB の実測は 8.8〜11.1% で線をまたぐ。
  - ハッシュを更に速くするなら (SIMD、並列、ダーティ追跡) は今回やっていない。メモリ帯域律速に近いので効果は限定的。

触ったファイル (round 2 で増えたもの。round 1 の一覧に加える):
  - src/Engine/Engine/Replay/WorldHasher.cpp
  - src/Engine/Engine/Compute/ComputeBufferStore.h
  - src/Engine/Engine/Compute/ComputeService.h
  - src/Engine/Engine/Compute/ComputeSettings.h
  - src/Engine/Engine/Compute/ComputeSettingsSelfTest.cpp
  - src/Engine/Engine/Compute/ComputeStateSelfTest.cpp (round 1 から継続)
  - plans/m91-compute-foreach/sub-08.md (実装メモ節のみ)
申し送り:
  - sub-09 の ADR-030: State のハッシュの方式 (8 バイト語、4 レーン) と、既定 8 MiB の根拠の表 (上の 3 点)。engine_spec の stateTotalBytes の既定値も 8388608。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-12)。
  - must: State のハッシュのコスト。既定の上限 16 MiB で、Release でも 1 tick の 104% かかる。M91 は未 push で、State の入った .rep はまだ外に無い。今のうちに、ハッシュを 8 バイト単位で畳む形に変えて速くし、Release で既定の上限のときに 1 tick の 10% 以内になるように stateTotalBytes の既定値を決め直す (U19)。
  - should: CI (--warp) で computeab が漏れを検出できるかを確かめる。
- round 2: VERDICT OK (planner、2026-10-12)。must #1 は解消した (8 バイト単位のハッシュ、既定 8 MiB)。should #2 も確かめた (--warp でも tick 0 で FAIL を検出できる)。
