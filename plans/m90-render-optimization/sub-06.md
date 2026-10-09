# sub-06: 描画側の並列化と URO (パレット・LOD 選択・カスケードの並列、ビュー間キャッシュ)

- 依存: sub-04, sub-05
- 状態: OK (コミット待ち)
- 往復: 2

## やること
spec §4.1.5、§2 #9 #10 #11。
1. `CollectDrawables` のステージ 2 (並列) に LOD 選択とスキンの視錐台判定を入れる (sub-04/05 で直列に置いたものを移す)。
2. パレット評価を並列化: 直列で「パレットが要る項目」を数えてスロット (事前確保した互いに素の出力) を割り当て → `ParallelRanges` で評価 → 直列で index 順にキューへ。`skinPalettes_` のポインタ安定性 (emplace_back で再確保されない) を構造で保証する。WARN の 1 回出し (`boneOverflowWarned_`) は並列段で書かない。
3. カスケードごとの影のカリングを並列 (カスケード × 範囲、出力はカスケード別)。
4. URO: screen-size が閾値未満のスキンは `updateInterval` tick に 1 回だけパレットを作り直す。作り直す tick = `(tickIndex + entity.index) % interval == 0`。間は前のパレット、補間なし。ラグドール作動中・クロスフェード中は間引かない。閾値と間隔の既定表をコメントの根拠付きで。`RenderSystem::enableAnimUro` (既定 true) とエディタのメニュー (`Tr()`)。
5. パレットのキャッシュをビュー間で共有。キー = エンティティ + ポーズ入力 (`SamePoseInputs` の等価) + 補間 alpha + URO の更新 tick。描画専用 (sim から見えない)、エンティティの破棄・シーン切り替えで捨てる。
6. 統計: パレット評価数 / 再利用数。
7. (sub-02 VERDICT から) オクルージョンのフェーズ 2 は、run / 単発ごとに状態設定と CB 更新をもう 1 回出す (どれを間引くかを CPU が知らないため)。ユニークなメッシュ・材質が多いシーンでは、CPU の提出コストが最大で約 2 倍になる。`render_bench` に「ユニークなメッシュ/材質が多い」変種を足し (CLI の引数で切り替え。既定のシーンは変えない)、GBuffer の提出にかかる CPU 時間をオクルージョン ON/OFF で計って報告する。OFF より明らかに重ければ planner が対処を決める (候補: 可視ビットの読み戻しで、フェーズ 2 に候補が無い run を飛ばす。数フレーム遅れの読み戻しは描画判断に使わない規約なので、使うなら spec の変更が要る)。
8. (sub-05 VERDICT から) LOD の履歴 `RenderSystem::lodHistory_` を、シーン切り替え・リプレイの巻き戻し・デバイス消失の後に捨てる (5. のパレットキャッシュを捨てるのと同じ契機・同じ口)。今は世代でしか区別しておらず、ヒステリシスの帯の分だけ前のシーンの段を引きずる。selftest で「捨てた直後の段は距離だけで決まる」を確かめる。LOD 選択の式 (ステージ 2 に直書き) は 1. で関数へ出す。

## やらないこと (このサブでは)
- sim 側のポーズ評価 (部位追従・IK・ラグドール) に触ること。
- deferred context。

## 触る場所 (planner の見立て)
- `src\Engine\Engine\Rendering\RenderSystem.cpp` (`CollectDrawables` ステージ 2/3、`RenderCascadeShadows`)、`RenderSystem.h` (キャッシュ・設定)
- `src\Engine\Engine\Animation\SkinningSystem.h` (`SamePoseInputs`)
- `src\Engine\Core\Jobs\JobSystem` (使うだけ。API を足すなら理由を書く)
- tickIndex の取り方: 描画へ渡っている tick 番号 (実時間・描画フレーム数を使わない)

## 受け入れ条件 (このサブ)
1. `render_bench` と既存のスキン入りデモで、jobs あり / `--no-jobs` のスクショの画素差 0。 — img-diff
2. URO: dump で遠いキャラのパレット評価数が減り、近いキャラは毎 tick 評価。同じ tick を 2 回撮ると同じ絵。 — dump、`--screenshot` 2 回
3. 複数ビュー (Scene View + Game View) で同じキャラのパレットがフレームあたり 1 回だけ評価される。 — selftest または dump のビュー別欄
4. selftest: URO の更新 tick 関数、キャッシュのキーの等価 (ポーズ入力が変わったら必ず作り直す)。
5. 既存 golden 全 PASS、`tools\check_rules.ps1` PASS、`tools\replay_verify.bat` PASS (描画の変更が sim に漏れていない)。

## 検証コマンド
- ビルド Debug / Release、`--selftest` 両構成、`tools\check_rules.ps1`、`tools\replay_verify.bat`
- A/B スクショ (`--no-jobs`)、dump

## 実装メモ (coder が追記)

### round 1
- パレット: `SkinPaletteCache` (新規、ビュー間キャッシュ + URO の判定関数)。CollectDrawables は 直列でエントリ確保 → `RunPaletteJobs` (ParallelRanges、grain 4) → 直列でキューの項目へ繋ぐ。`RenderCascadeShadows` の画面外スキンも同じ入口 (`AcquireSkinPalette`)。`skinPalettes_` (deque) は廃止し、エントリのアドレス安定は `std::deque<Entry>` で担保。WARN は `RunPaletteJobs` の直列部。
- URO: `UroUpdateInterval` (screen-size 5% / 2% / 0.8% で 1 / 2 / 4 / 8 tick)、更新 tick = `(simTick + entity.index) % interval == 0`。`RenderSystem::simTick` を EngineLoop が `ctx.tickIndex` で設定。ラグドール作動中・`IsPoseBlending` (フェード中 / 2 層以上) は毎 tick。`enableAnimUro` + `--no-uro` + Rendering メニュー (`Menu_AnimUro`)。
- キャッシュ鍵: entity + model + `SameRenderPoseInputs` (= `SamePoseInputs` + 使用層の prevTimeQ / stepQ) + alpha。URO は窓 (窓の先頭 tick) で再利用。ラグドールは再利用しない。
- カスケード判定を `ParallelRanges` (キャスター単位、出力は自分の要素) に。
- やること 8: `ResetRenderHistory()` (lodHistory_ + パレットキャッシュ)。呼ぶ場所は ReleaseGpu / タイムトラベル等の復元後 (`ResetNonSimLanesAfterRestore`) / シーン読み込み (`TickServices::sceneLoadSerial`) / `simTick` が前に戻ったとき。LOD の screen-size は `BoxScreenSize` (MeshLod.h) へ。
- やること 7: `--render-bench-unique-demo` (メッシュ・材質が全部別の 1500 個 + 遠いスキン 6 体)、`DeferredPath::GbufferCpuMs` (RenderGeometry の CPU 時間の直近 32 回平均) を dump の `cpuMs.gbufferSubmit` に。

SELF_EVAL は司会への返信本文を参照。

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-09)
  1. [must] URO の位相を `(simTick + Hash(entity.index)) % interval` にする (spec §2 #9 / §8 を変更済み)。ハッシュは決定的な 32bit の整数ミックス (例: murmur3 の fmix32)。乱数・ポインタ・実時間は使わない。`UroIsUpdateTick` と `UroWindowStart` の両方を同じ位相に揃える。selftest には次を足す: 20 刻みの entity.index 群 (例: 0, 20, …, 380) で、interval 2 / 4 の更新 tick が 1 つの位相に偏らないこと (各位相に 1 割以上)。窓の先頭・更新 tick の既存の境界テストも新しい位相で通すこと。unique demo の dump で、tick 31〜34 の paletteEvaluated が特定の tick に集中しないことを報告する。jobs / --no-jobs / --no-uro の WARP A/B (diffPixels=0) と shot_verify は取り直す。
  2. [should] SELF_EVAL の「未実行」にある Debug の A/B スクショは不要 (WARP の Release で受け入れ 1 の根拠は足りる)。2 ビュー同時の実機確認も、受け入れ 3 が「selftest または dump」としており、Editor の dump で view 2 のキャッシュ再利用を確認済みなので不要とする。
  - 差分の判定: LodRange の取得が点在している件の整理は、sub-05 の nit であって、やること 8 の範囲ではない (8 は選択式を関数へ出すことで、BoxScreenSize で満たした)。いずれ別件で整理する。カスケード判定をキャスター単位で分割したのは結果が同じなので採用。その他の追加はすべて採用した (spec §8)。
  - 不安・質問への回答: (a) オクルージョンの CPU 提出コストは既定 ON のまま。読み戻しは採らない (spec §7。`[聞]` の印付き)。GPU ms の ON/OFF は sub-08 で計る。(b) 位相は #1 のとおり。(c) 実 GPU の 51 画素の揺れは spec §7 に別件として記録した。
- round 2: VERDICT OK (planner、2026-10-09)。#1 解消: `UroPhase` (fmix32) を更新 tick と窓の先頭の両方で使い、20 刻みの index で位相が偏らないことを selftest で確かめた。unique demo の dump で更新が 1 つの tick に集中しなくなった (2/4、3/3、3/3、4/2、2/4)。WARP の A/B (jobs / --no-jobs / --no-uro / 2 回撮り) はすべて diffPixels=0、shot_verify は 30 枚 PASS、selftest は両構成で exit 0。coder が実行しなかった check_rules は planner が現在のツリーで回し、0 error / 50 warning (既存の rule 7 のみ)。replay_verify は位相の変更が描画側だけなので、round 1 の 18 ジョブ PASS を引き継ぐ。
