# sub-11: 計測・ベンチ・上限の決定・RT / 描画のスパイク対策

- 依存: sub-08, sub-10
- 状態: OK (コミット待ち)
- 往復: 1

## やること

推測で上限を決めない (AGENTS §3.5)。実測で「60 Hz の tick 予算に収まる破片数」を決め、既定値と Inspector の上限に反映する。

1. **プロファイル**: 物理内部 (収集 / 広域 / 狭域 / ソルバ / 書き戻し。sub-05 では入れていないので、このサブで足す) と FractureSystem に `MYE_PROFILE_SCOPE`。状態に影響しないこと
2. **ベンチ**: 破片数 N (16 / 32 / 64 / 128 / 256) の破壊物 1 つと、複数 (例 8 個) を並べたシーンを、フラグ付きの計測モード (例 `--fracture-bench N`、ヘッドレスで決まった tick 数を回して各スコープの平均と最大を出す。既存の `--perf-rate` 等のフラグの流儀に倣う) で測る。壊れる前 / 割れた瞬間 / 割れた後 (全塊が動く) の 3 区間を分けて出す
3. **焼き時間**: sub-02 / sub-04 の焼きを N ごとに計測
4. **上限の決定**: Release で「物理 + FractureSystem が 1 tick (16.6 ms) の半分程度に収まる」破片数を推奨値に、ハード上限 256 のまま Inspector の範囲と既定値を見直す (決め方の閾値は SELF_EVAL で数値と共に提示し、planner が判定する)。結果を `plans/m80-destruction/bench.md` に記録 (計測環境・コマンド・表)
5. **スパイク対策**: RT on (`--rt`) で割れた瞬間の BLAS 焼き直し (`RtScene.cpp:64-120`) の時間を計測。問題なら対策 (例: 破壊物の破片メッシュを最初から BLAS の集合に含める) を入れる。描画 (インスタンシングが破片で切れる) のコストも計測だけはする
6. 対策が状態・replay に影響しないこと (描画側のみ)

## やらないこと (このサブでは)

- 物理のアルゴリズム改善 (広域の分割など)。重すぎる場合は計測結果と改善案を SELF_EVAL に書き、planner が別サブにするか判断する

## 触る場所 (planner の見立て)

- `src/Engine/Engine/Physics/PhysicsSystem.cpp`、`FractureSystem.cpp` (スコープ)
- `src/Engine/Engine/EngineCli.cpp` (計測フラグ)、`DemoContent.cpp` (ベンチシーン)
- `src/Engine/Engine/RayTracing/RtScene.cpp` (対策するなら)
- `Components.cpp` の範囲、`InspectorWindow.cpp` の推奨表示
- `plans/m80-destruction/bench.md` (新規、計測記録)

## 受け入れ条件 (このサブ)

1. `bench.md` に N ごとの 3 区間の物理 / FractureSystem 時間 (Release)、焼き時間、RT の割れた瞬間の時間が表で残る
2. 推奨値と上限が表の数値から導かれ、Components.cpp の範囲 / 既定値と Inspector の表示に反映 — 差分
3. 対策を入れたなら前後の数値。replay / golden に影響なし — `replay_verify.bat`、`shot_verify.bat`
4. `check_rules.ps1` PASS、WIP 不変

## 検証コマンド

```
（Release|x64 をビルド）
bin\x64\Release\Editor.exe --fracture-bench ...   (名前は coder 判断)
bin\x64\Release\Runtime.exe --fracture-demo --rt ...
tools\replay_verify.bat
tools\shot_verify.bat
tools\check_rules.ps1
```

## 実装メモ (coder が追記)

SELF_EVAL: sub-11 (round 1)
実装:
  - `src/Engine/Engine/Physics/PhysicsSystem.cpp:PhysicsSystem::Update` — `std::optional<prof::ScopeTimer>` 1 本を `.emplace(name)` で使い回し、`phys.collect`(収集: 動的/静的/CC 収集・複合合成・質量慣性・関節収集・車輪配線)→`phys.broad`(サブステップ内の積分・外力・ブロードフェーズ・起床判定)→`phys.narrow`(関節拘束ブロック構築・接触制約生成)→`phys.solve`(接触解決・位置補正・CCD・位置積分・接触/インパルス出力)→`phys.writeback`(書き戻し) の 5 区分を計測。emplace は前段の Pop→新段の Push を自動で行うので、関数内に早期 return が増えても対応が崩れない (関数自体は実測トップレベルに早期 return が無いことを確認済みだが、ラムダ内 return と将来の変更に備えて RAII にした)
  - `src/Engine/Engine/FractureSystem.cpp:FractureSystem::UpdateImpl` — `fracture.collect`(CollectAllPieces + Destructible 列挙) / `fracture.process`(ProcessRoot + ProcessAfterBreak) の 2 区分を追加
  - `src/Engine/Engine/Physics/FractureBenchmark.h/.cpp` (新規) — `--fracture-bench` の実体。ウィンドウ・D3D を作らず `Scene`/`World` を直接操作し、`PhysicsSystem::Update`/`FractureSystem::Update` を手動で回すヘッドレスループ (`froxel-probe` と同じ「専用の軽量パス」の流儀)。破片数 16/32/64/128/256 × 破壊物 1/8 個、150 tick、3 区間 (壊れる前/割れた瞬間 10 tick/割れた後) で `prof::FrameScopes()` を集計してログへ出す
  - `src/Editor/EditorMain.cpp` — `--fracture-bench` フラグ (値なし)。`--modal-voxelize` と同じ「連鎖の手前で拾って continue」の置き方、実行は `--froxel-probe` と同じ早期 return 群に合流
  - `src/Engine/Core/Components.cpp:DestructibleComponent.pieceCount` — 生 `FieldDesc` (`PhysicsEnvironmentComponent.substeps` と同じ流儀) でレンジ + ツールチップを両方付け、推奨上限 64 を明記。範囲 (2..256) と既定値 (16) は実測に照らして変更不要と判断しそのまま
  - `src/Engine/Core/LocalizationTable.inl` / `src/Editor/Windows/InspectorWindow.cpp:DrawDestructibleNotes` — 生成済みの破片数が 64 超のとき黄色警告 (`Insp_FracturePieceCountHigh`) を Inspector に表示
  - `src/Engine/Engine/RayTracing/RtScene.cpp:RtScene::RebuildBlasIfNeeded` — 実際に連結 BLAS を焼き直した回だけ (毎フレームではない) 焼き直し時間をログへ出す。破壊で参照メッシュ集合が変わった瞬間のスパイクを観測できる恒久ログとして残した (sim/ハッシュ/リプレイに関与しない)
  - `plans/m80-destruction/bench.md` (新規) — 焼き時間・物理/FractureSystem のフェーズ別時間・上限の決定根拠・RT スパイク・描画コストの計測記録
仕様との差分:
  - [追加] `--fracture-bench` の名前・引数形式・ベンチシーンの詳細 (破片数/個数の組み合わせ、球の質量・速度、tick 数、区間の切り方) は sub-11.md が「coder 判断」としている範囲内で決めた。bench.md §1 に明記
  - [追加] RT スパイク対策として「BLAS 焼き直し時間のログ」を恒久機能として残した (対策候補の 1 つである「破片メッシュを最初から集合に含める」までは実装していない — 計測の結果 `--fracture-demo` 規模では 1ms 未満で問題なしと判断したため、spec/sub-11.md の「問題なら対策」に従い対策そのものは入れていない)
  - [未実装] 描画 (インスタンシングが破片で切れる) コストの自動計測ツール。一時プローブ (`prof::GetRenderStats()` を毎フレームログへ) で計測しコミット前に revert 済み (diff は残らない)。ドローコール数が割れる前後で明確に切り分けられなかった理由は bench.md §7 に記載。恒久的な計測フックは追加していない (要求は「計測だけ」であり、ツール化までは求められていないと解釈)
  - [追加] `Destructible.pieceCount` の上限 (ハード 256) は変更しなかった。理由は bench.md §5 (spec §4.1 の凸包頂点上限 64 と並ぶ既存の仕様値で、[ユーザーに聞ける] の対象にもなっていないため coder 側で下げる判断はしなかった)
検証:
  - `tools\gen_project_files.ps1` → Engine.vcxproj 511→513 ファイル (FractureBenchmark.h/.cpp 追加)
  - Engine/Editor/Runtime/GameLogic を Debug/Release 両方ビルド → 成功
  - `bin\x64\Release\Editor.exe --fracture-bench` → 完走、ログを `plans/m80-destruction/bench.md` に反映。実行中に発見したバグ (`DestructibleComponent*` を `AddComponent` 呼び出しを跨いで保持していたためのダングリングポインタ、objects=8 で tick0 の誤検出) を `EntityID` + 毎 tick `GetComponent` に直して解消済み (修正後は 5 pieceCount 全てで期待どおり break-tick=39)
  - `bin\x64\Release\Runtime.exe --fracture-demo --rt-gi --warp --no-audio --frames 200` → BLAS 再構築ログ 3 件、いずれも 1ms 未満 (bench.md §6)
  - `tools\check_rules.ps1` → `0 error(s), 0 warning(s)`
  - `bin\x64\Release\Editor.exe --selftest` → exit 0、FAIL 0 件
  - `bin\x64\Debug\Editor.exe --selftest` → exit 0、FAIL 0 件 (完走に約 10 分。理由は下の不安・質問参照)
  - `tools\replay_verify.bat` → `[PASS]`、全 14 ジョブ (9 シーンチェーン + タイムトラベル×2 + What-if×2 + rules) PASS
  - `tools\shot_verify.bat` → `parts` / `joints` / `acoustic_forward` / `acoustic_deferred` の 4 枚のみ差分 (harness.md に記録済みの M80 着手前からの既知差分と同一枚) で、それ以外 (fracture_before / fracture_after を含む) は全 PASS
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜4 (bench.md・反映・replay/shot 無影響・check_rules) は満たした。RT/描画の「対策」は計測の結果不要と判断した分岐で、spec の想定 (対策を入れる分岐) を完全にはなぞっていない
  正しさ: 4 — 全検証 PASS。ベンチ実装中に見つけたダングリングポインタのバグは自己検証で発見・修正し、修正後の再検証で確認済み。他ケース (objects=8 全 N) も期待どおりの break-tick になることを確認した
  コード品質: 4 — 5000 行超の関数への侵襲を `std::optional::emplace` 1 変数に抑え、既存の `{}` 構造を変えていない。RtScene のログは早期 return の手前にしか無いので常時ログ化していない
  テスト: 3 — 新規の `--fracture-bench` 自体には自動 assert が無く (計測専用、ログを人が読む前提)。ダングリングポインタのバグは手動デバッグで見つけた形で、再発防止の自動チェックは足していない
不安・質問:
  - Debug ビルドで `FractureSelfTest.cpp` の開いたメッシュボクセル化ベンチ (`bakeOpenMesh`、sub-04/14 由来) が Release 比で 8〜12 倍遅く、`--selftest` 全体の完走に約 10 分かかった (res=32 の open box だけで 56〜130 秒)。これは今回変更した箇所とは無関係な既存コードの性質だが、開発時の `--selftest` の実行体験に影響する規模だと考えたため報告する。対応要否は planner 判断
  - 「割れた瞬間の描画コスト」の自動計測ツール化は見送った (bench.md §7)。恒久的なドローコール計測フックを RenderSystem に足すかどうかは、この先のサブ (ADR-021 を書く sub-12 など) で判断してほしい
触ったファイル:
  - src/Engine/Engine/Physics/PhysicsSystem.cpp
  - src/Engine/Engine/FractureSystem.cpp
  - src/Engine/Engine/Physics/FractureBenchmark.h (新規)
  - src/Engine/Engine/Physics/FractureBenchmark.cpp (新規)
  - src/Editor/EditorMain.cpp
  - src/Engine/Core/Components.cpp
  - src/Engine/Core/LocalizationTable.inl
  - src/Editor/Windows/InspectorWindow.cpp
  - src/Engine/Engine/RayTracing/RtScene.cpp
  - plans/m80-destruction/bench.md (新規)
申し送り: sub-12 (ABI v22・デモ仕上げ・文書) へ — `engine_spec.md` に破壊の性能特性 (推奨 64 破片・ハード上限 256・ドローコールは破片数に比例して増える) を書く際は bench.md を参照。RT の BLAS 再構築ログ (`[rt] BLAS rebuild: ...`) は今回追加した恒久ログなので、ADR-021 の「観測可能性」節に載せてよい

## フィードバック履歴
- round 1: VERDICT OK (planner)。推奨上限 64 / Destructible は bench.md §3 の数値から導かれている (割れた瞬間の平均 64 = 4.69 ms、128 = 10.01 ms、予算の半分 8.3 ms)。RT の焼き直しは 1 ms 未満で対策なし。draw call の恒久計測は見送りでよい (計測のみの項目)。Debug の --selftest が約 10 分かかる件は sub-12 で軽くする (sub-12 やること 7)。同時に 8 個割れる場合の予算超過 (N=32 で 16.4 ms) は engine_spec に書く (sub-12)
