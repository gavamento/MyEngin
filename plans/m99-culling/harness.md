# harness 台帳: m99-culling

- 依頼原文:
  1. 「カリング処理を複数個実装したい
     オクルージョンカリング、フラスタムカリング」
  2. 「プロジェクト設定でカリングのON/OFFを変えられるように」
  3. 「追加でカリングで足りない機能あれば追加したい」
- 開始: 2026-11-11 / 基点コミット: d52e91f3103eacf6e5e07e4f44b8572063e1a166
- フェーズ: レビュー

## サブ進捗
| サブ | 状態 | 往復 | コミット | メモ |
|---|---|---|---|---|
| 1 | PASS | 4/5/4/4 | minor 3 (履歴の「サイズ変更で捨てる」記述が未実装 / 枠の色が淡く化ける / sub-10 の --no-audio は既に付いている) |
| sub-01 | OK | 1 | 877dc523 | 段別 CPU 時間・BVH 採否 → 見送り (ADR-038) |
| sub-02 | OK | 1 | acc258b4 | 設定の土台 + 視錐台スイッチ |
| sub-03 | OK | 1 | 2485dd6c | 局所影の画面外キャスター (--culling-demo) |
| sub-04 | OK | 1 | d276ed81 | 3D 経路の視錐台 (GPU 粒子・トレイル・スプライト・デカール) |
| sub-05 | OK | 1 | a0226a4f | 2D 経路の視錐台 (--culling-2d-demo) |
| sub-06 | OK | 2 | a523e363 | 距離カリング + RenderCulling (TypeId 100、SimSnapshot v82、ABI 不変)。round 1 は祖先継承で REWORK |
| sub-07 | OK | 1 | 939b246a | 画面サイズカリング (+ sub-06 の should 解消) |
| sub-08 | OK | 1 | 565a1d26 | カメラ固定 + 理由別表示 (+ CPU プールを cullFrustum へ、exempt の実経路確認) |
| sub-09 | OK | 1 | 3f97eb03 | 文書 (engine_spec §6.14/§6.6、ADR-038 確定、test_checklists) + cull_verify.bat + 通し検証 |
| sub-10 | OK | 1 | (このコミット) | cull_verify を CI に載せる (ユーザー判断で追加) + --no-audio を揃える |

## レビュー
| round | 判定 | 深度/機能/視覚/品質 | 未解決 |
|---|---|---|---|

## ユーザー判断
- 2026-11-11 (司会の事前質問への回答):
  - 追加する新しい種類: 距離カリング、画面サイズカリング (ポータル / PVS、クラスタードライトカリングは選ばれなかった)
  - 直す穴: 視錐台判定の無い経路、局所影の画面外キャスター、空間分割 (BVH) による高速化 (オクルージョンの対象拡大は選ばれなかった)
  - デバッグ機能 (カリング用カメラの固定、落とした物の可視化): 追加する
  - 進め方: /harness で回す
- 質問の運用: その場で聞く (2026-11-11)。planner に AskUserQuestion が無かったので、司会が [ユーザーに聞ける] を代わりに聞いた
- 2026-11-11 planner の裁定の確認 (どれも推奨どおり):
  - U1: 物ごとの距離は新しい描画専用コンポーネント RenderCulling (maxDrawDistance / exemptFromSizeCulling) に持たせる
  - U2: 距離・画面サイズで落とした物は影も消す
  - U3: Scene View には既定で掛けず、セッション限りのトグルで掛けられるようにする
  - U4: GPU パーティクルの箱は記述子から上界を計算する (乱流・かき乱し・速度継承あり、ワールド空間で動いたものは常に描く)
  - U5: BVH は sub-01 の計測で採用条件を満たしたときだけ足す (事前質問で合意済み)
  - 2026-11-11 cull_verify の CI 組み込み: ユーザーは「今載せる」(planner の裁定「CI が緑に戻ってから」とは逆)。planner へ補足として送り、サブを足して実装する
  - 仕様の確定: spec 5. の受け入れ条件 14 項目と仮置き (画面サイズは既定 OFF、閾値 0.005、ヒステリシス 10%) でユーザーが確定

## 申し送り (セッション跨ぎ)
- sub-01: BVH 見送り。Debug の大物量計測は K=4 まで。ADR-038 の決定の節は sub-09 で書き、nit 1 (早期脱出で落ちる物が多いほど判定は安い) を反映。nit 2 (scale≠1 で保存済みシーンを読んだら警告) は任意
- sub-02: UI (Rendering > カリング、Project Settings のカリング欄) は画面で未確認 → 最終報告の未検証事項に載せる (ユーザーの目視)。nit: CullingCliOverrides の組み立てが EngineLoop と EngineCliSelfTest で重複 (任意)。ON/OFF 一致の回帰は sub-09 で tools\cull_verify.bat を作る
- sub-03: shot_verify の flow_title FAIL (文字行のみ、影の経路外) は sub-09 で基点 d52e91f3 と比べて切り分ける。nit: AllocateShadowAtlas のマスク計算の切り出し (任意)
- 範囲外の別件: master の CI は M98e 以降 4 回連続 failure (run 42927691480 はビルド途中で exit 1)。最終報告でユーザーへ伝える
- sub-04: nit: ParticleGpuBound を軽いヘッダへ分けると GpuParticleBackend.h の依存が細くなる (任意)。sub-08 は CPU パーティクルのプール判定を cullFrustum へ寄せる。Scene View の動く GPU エミッタは sub-09 の手動確認
- sub-05: nit: Sliced/Tiled/SpriteShape の視錐台判定の単体テストなし (任意)。culledNonMesh の定義を「非メッシュの視錐台落ち全部」に確定、CPU プールの計上は sub-08。Scene View の 2D は sub-09 の手動確認
- sub-06: should (タグ継承を Tags::EffectiveMask 呼び出しへ) は sub-07 で直す。round 2 後の Release ビルドと replay_verify は sub-07 の検証で兼ねる。タグ別距離の実描画は未確認
- 契約番号: TypeId 100 = RenderCulling、kSimSnapshotVersion 82 (M99f)。ABI v35 のまま
- sub-07: should (RenderSystem 経由の exempt の確認) は sub-08、nit (counts の機械比較) は sub-09 へ転記済み
- sub-08: nit 距離の青が薄い (ユーザーの目視で判断)、RenderSystem.cpp の切り出し (任意)。--frames 無しの撮影が終わらない件は sub-09 で基点と比べる。司会もスクショ (scratchpad の s_bench.png / s_dist_zoom.png) で枠を確認済み
- sub-09: 既存 FAIL 2 件は M99 と無関係 (基点 d52e91f3 でも同じ): shot_verify の flow_title (maxDiff 62 / 944 px、golden 未更新)、selftest-full の disturbancelong (M96 の baked hash が古い)。--frames 無しの --screenshot が終了しないのも既存挙動
- sub-09: coder が作業用チェックアウト C:\HAL\_m99_base を作って自分で削除済み (司会が不在を確認)。最終報告に書く
- sub-09: nit Release の --selftest-full は未実施。cull_verify の CI 組み込みは見送り ([ユーザーに聞ける] 裁定: CI が緑に戻ってから shot_verify の次に MYE_CULL_SKIP_CONFIGS=1 で 1 行)
- ユーザー目視: docs	est_checklists.md の M99 節 (UI 一式、Scene View トグル、固定・枠、距離の青)。任意: sub-01 nit 2
- 番号 (M99 後): ABI v35 = 202、SimSnapshot v82、TypeId 100、ADR-038、check_rules 規則 18
- sub-10: CI 上の実走は master の CI が緑に戻るまで観測できない。緑に戻ったら cull_verify の step のログを確かめる
- 司会の事前調査は `plans\m99-culling\design-draft.md`。planner の出発点で、裁定で変えてよい。
