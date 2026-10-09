# harness 台帳: m90-render-optimization

- 依頼原文: メッシュ LOD と GPU オクルージョンカリング (Hi-Z を流用)などの軽量化手法をエンジンに実装したい。
マルチスレッドなどのほかの軽量化なども
- 開始: 2026-10-09 / 基点コミット: 3b30251c082095ae900a55eecf88ac9c4834c8d2
- フェーズ: 実装

## ユーザー判断 (策定前の AskUserQuestion、2026-10-09)
- 範囲: 計測の土台 / メッシュ LOD / GPU オクルージョン / 影とスキンのカリング (全部)
- 並列化: 描画側の CPU 処理 / アニメ更新の距離間引き / シミュレーションの並列化 (deferred context は不採用)
- 進め方: /harness
- 事前調査: plans/m90-render-optimization/design-draft.md
- (2026-10-09、planner 裁定後に司会が確認) spec §2 の [聞] 4 件はすべて planner 裁定どおり: #2 オクルージョンは 2 フェーズ / #5 LOD は .meta でオプトイン (既定は段なし) / #9 URO は描画側だけ / #12 sim 並列化はアルゴリズムを変えない系だけ (CPU 粒子・Perception・PartFollow・IK)
- (2026-10-09、sub-06 VERDICT の [聞] 2 件) オクルージョン: **既定 ON、設定で ON/OFF を切り替えられるようにする** (読み戻しで run を飛ばす案は不採用)。URO の描画履歴への依存: **許す** (planner 裁定どおり)。
- (2026-10-09、planner PLAN_RESULT の [聞]) オクルージョンの保存先: **プロジェクト設定 (assets/project_settings.json の rendering.occlusionCulling)** (planner 裁定どおり)。新サブ sub-09 で実装。

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| sub-01 計測の土台 | OK | 1 | 72ff316 | Debug selftest で ServerNetSelfTest が 1 回だけ一過性 FAIL (原因未断定) |
| sub-02 GPU オクルージョン縦切り (Deferred) | OK | 1 | 1bd74ec | ServerNetSelfTest 一過性 FAIL 2 回目 |
| sub-03 オクルージョンを Forward へ + hzb-debug | OK | 2 | 24781ec | 前セッションの途中差分を引き継いで再開。受け入れ 2 の目視はユーザー待ち |
| sub-04 影のカスケード別カリング + スキン AABB | OK | 2 | df7b1d3 | golden 3 枚更新 (画面外キャスターの影、planner 了承) |
| sub-05 メッシュ LOD | OK | 1 | 29a775e | meshoptimizer v1.3、kCookVersion 6。LOD 見た目の目視はユーザー待ち |
| sub-06 描画側並列化 + URO | OK | 2 | (このコミット) | URO 位相を fmix32 ハッシュへ (round 1 REWORK) |
| sub-07 sim 並列化 (ADR-028) | 未着手 | 0 | | 依存 01 |
| sub-09 オクルージョン ON/OFF をプロジェクト設定に保存 | 未着手 | 0 | | 依存 06 (2026-10-09 ユーザー要求で新規) |
| sub-08 文書 + 全体検証 (ADR-029) | 未着手 | 0 | | 依存 03,06,07,09 |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

順序: 01 → 02 → 03 → 04 → 05 → 06 → 07 → 09 → 08 (RenderSystem.cpp の同じ関数を触るので逐次)

## 申し送り (セッション跨ぎ)
- design-draft.md と本台帳は未コミット。最初のサブのコミットに含める。
- (planner 2026-10-09、sub-02 VERDICT) Debug selftest の ServerNetSelfTest (`V1 LoadPersist / LoadGame in a session`) が初回だけ FAIL する件は 2 回目の再発。M90 の差分にネット・セーブの経路は無いが、まだ断定はできない。**sub-08 の前に、基点コミット 3b30251 の Debug selftest を 2〜3 回回して、M90 より前から出ていたかを確かめる** (前から出ていれば別件として切り出す。出なければ M90 の差分を二分探索する)。
- (planner 2026-10-09) spec.md 確定 (planner 裁定)。AskUserQuestion が使えなかったので、spec §2 の `[聞]` 4 件 (#2 2 フェーズ / #5 LOD オプトイン / #9 URO は描画側 / #12 sim 並列化はアルゴリズムを変えない系だけ) と全体の確定確認を司会がユーザーへ。差し戻されたら planner が該当行と §6 を直す。
- (planner 2026-10-09、sub-03 VERDICT nit) OcclusionSelfTest の Forward のサーフェス項目は ON/OFF 一致で間接確認のみ (プローブ色の直接検査なし)。selftest が `%TEMP%\mye_occlusion_selftest` を残す (次回開始時に remove_all で消える)。
- (coder sub-03) `Runtime.exe --screenshot` 単体ではスクショ後に終了しない (M90 以前から、コード読みのみで確認)。`--frames N` か `--render-stats-dump` と併用する。`tools\gen_project_files.ps1` は pwsh で実行する。shot_verify がタイムアウトすると Runtime / Editor が残って exe をロックする。
- (planner 2026-10-09、sub-04 VERDICT nit) `WorldAabbInFrustumNoNear` は `WorldAabbInFrustum` の複製。除外面を引数にして 1 本化する (sub-06 で FrustumCull.h を触るとき)。`RenderSystem::Render` を通した画面外スキンの影の画素テストは無い (一時シーンの目視のみ)。
- (coder sub-04 → sub-06) カスケード判定は RenderCascadeShadows の直列ループ (CascadeCasterMask)。画面外スキンのパレットは入ったカスケードがあるときだけ毎回評価 (間引き未)。EvaluateSkinPalette が評価の唯一の入口。SkinBoundsCache::Get はステージ 1 (直列) でだけ呼ぶ。AllocateShadowAtlas (局所影) は画面内の queue_.opaque のみ。スキンは GPU オクルージョンの判定箱に載せない (M90 の間は据え置き)。
- (planner 2026-10-09、sub-05 VERDICT) lodHistory_ を捨てる契機・LOD 選択式の関数化は sub-06 やること 8 へ。indirect 引数の startIndex と ShadowPass の段ありの run は画素一致の間接確認のみ。硬い面のメッシュに段が作れない件と ServerNet 一過性 FAIL (計 3 回) は spec §7。
- (coder sub-05 → sub-08) ADR-029 に .meta "lod" 形式、blob の並び、選択式 (screen-size × lodBias、ヒステリシス 10%)、溶接 + simplifyWithAttributes + LockBorder、硬い面の限界、--lod-bias / --lod-force、tools\gen_lod_test_gltf.ps1 を書く。engine_spec の kCookVersion を 6 へ。render_bench の cache\render_bench.scene.json が古いと LOD 球が出ない。
- (planner 2026-10-09、sub-06 VERDICT nit) RenderSystem 経由のパレット統合 (確保 → 評価 → 画素) はスクショ A/B のみ。CollectDrawables が長い。実 GPU は run 間で 51 画素 maxDiff=1 揺れる (M90 前から、spec §7 別件) ので画素 A/B は WARP で。
- (coder sub-06 → sub-08) ADR-029 に URO の表 (kUroTiers 5%/2%/0.8% → 1/2/4/8 tick)・位相 UroPhase (fmix32)・窓の再利用規則・キャッシュの鍵・cpuMs 節・--no-uro・--render-bench-unique-demo・ResetRenderHistory の契機を書く。オクルージョン ON の CPU 提出は unique 1500 個で OFF 0.84 → ON 1.45 ms (sub-08 で GPU ms と合わせ ADR へ)。
