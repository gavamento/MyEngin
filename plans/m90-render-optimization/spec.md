# M90 描画の軽量化 (計測 / メッシュ LOD / GPU オクルージョン / 影・スキンのカリング / 並列化) — 仕様書

- slug: m90-render-optimization
- 状態: 確定 (2026-10-09、planner 裁定。AskUserQuestion が使えない環境のため、§2 の `[ユーザーに聞ける]` 印の論点は司会がユーザーへ確認する。差し戻されたら該当行と §6 を直す)
- 依頼原文: (一字も変えずに)

> メッシュ LOD と GPU オクルージョンカリング (Hi-Z を流用)などの軽量化手法をエンジンに実装したい。
> マルチスレッドなどのほかの軽量化なども

- 事前調査: `plans/m90-render-optimization/design-draft.md` (現状のファイル位置はそちらが正本。本書は決定だけを書く)
- 策定前のユーザー判断 (台帳 `harness.md`): 範囲 = 計測の土台 / メッシュ LOD / GPU オクルージョン / 影とスキンのカリング、並列化 = 描画側の CPU 処理 / アニメ更新の距離間引き / シミュレーションの並列化。D3D11 deferred context は範囲外。

## 1. 目的 (なぜ作るか)

物が多いシーン (三校の街・群衆・破壊の破片) で、**見えない物・遠くて細部が見えない物に GPU と CPU を使わない**状態にする。
ただし本エンジンの契約 (sim のビット一致、決定的撮影、golden) を 1 ビットも崩さないこと、
そして「効いたか」を数字で示せること (§3.5 計測に基づく最適化) が前提。

達成したい状態:
1. 描画の各段 (GBuffer / Forward / 影 / フレーム全体) の GPU ms と、ビュー別の draw / tri / カリング数が見える。
2. 遠景のメッシュは自動生成した粗い段で描かれ、近づくと元に戻る (コライダー・NavMesh・RT は元のまま)。
3. 壁の裏の物は GPU が描かない。**欠け (見えるはずの物が 1 フレーム消える) を起こさない。**
4. 画面外のキャスターの影が消えない (現状の不具合)。スキンメッシュも画面外なら落ちる。
5. 描画側の CPU 処理 (ボーンパレット、LOD 選択、影のカリング) が並列に走り、`--no-jobs` と絵が一致する。
6. sim の重い系の一部が並列に走り、`--no-jobs` とハッシュが毎 tick 一致する。

## 2. 疑った点と結論

`[聞]` = `[ユーザーに聞ける]`。planner が裁定済みで、逆を選ぶと何が変わるかは §7 に書いた。

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| 1 | 「Hi-Z を流用」はそのまま使えるか | `HzbPass.h:15,34` は **min-Z** (SSR 用、reversed-Z で手前)。Deferred で SSR/デバッグ時だけ作り、`DeferredPath` に 1 個だけ。オクルージョンは **max-Z (最も奥)** が要る | (未) | **縮小カーネル (`HzbReduceSpan` の分割規則) と mip 段の作り方は流用、演算を min/max で切り替えられるようにし、オクルージョン用は別インスタンスを viewKey ごとに持つ**。SSR 用の既存ピラミッドは 1 ビットも変えない |
| 2 | 1 フェーズ (前フレームの HZB を再投影) か 2 フェーズか | 1 フェーズはディスオクルージョン・カメラカットで 1 フレーム欠ける。UE の旧 HZB occlusion はクエリの遅延付き、Nanite と Unity 6 の GPU occlusion culling は 2 パス (記憶による。一次資料は未確認) | (未) `[聞]` | **2 フェーズ**: フェーズ 1 = 前フレームに可視だった物を描く → その深度から max-Z HZB → フェーズ 2 = 残り全部を判定して可視の物を描く → 可視ビットを次フレームへ。再投影をしないのでカメラカットでも欠けない (遅くなるだけ)。§3.6 の正しさ優先 |
| 3 | オクルージョンを既定 ON にすると golden が動くか | 2 フェーズは保守的判定なので、描かれる画素は OFF と同じになるはず。描画順だけが変わる (同深度の z-fight で差が出うる) | (未) | **既定 ON** (viewKey 1/2/3)。viewKey 0 (AssetPreview・履歴なし) と ProbeBaker は OFF。golden は既存の許容値のまま PASS が条件。ベンチシーンで ON/OFF の画素差 0 を期待し、差が出たら原因を特定して報告 (z-fight 以外なら不具合) |
| 4 | オクルージョンを影・半透明にも掛けるか | 影はライト視点の深度が別に要る。半透明はソート済みで個別描画 | (未) | **不透明 (Deferred の GBuffer と Forward の不透明) だけ**。影はカスケードごとの視錐台カリング (#7) で賄う。半透明は後回し |
| 5 | LOD を全メッシュに既定で掛けるか | 既定で掛けると、遠景にメッシュがある golden が全部動く。UE の Static Mesh LOD も Unity 6 の Mesh LOD もアセット単位のオプトイン (記憶) | (未) `[聞]` | **モデルの `.meta` でオプトイン** (既定 = 段なし = 今と 1 ビットも同じ)。段数・削減率・各段の screen-size をアセットに持つ (UE 流)。全体の `lodBias` と強制段 (デバッグ) を RenderSystem の設定に持つ (Unity の `QualitySettings.lodBias` 相当) |
| 6 | LOD の段をどう持つか | `MeshVertex` は 52B で static_assert、`CookedMesh` / `Mesh` は VB/IB 1 組 | (未) | **頂点バッファは共有し、段ごとに IB の範囲 (indexOffset / indexCount) を足す** (`meshopt_simplify` は元の頂点を指すインデックス列を返すので頂点を複製しない。Unity 6 の Mesh LOD と同じ持ち方)。`MeshVertex` は変えない。kCookVersion 5→6。コライダー・NavMesh・RT の BVH・MeshLibrary の CPU コピーは LOD0 (今の IB そのもの) を使い続ける |
| 7 | 影のキャスターをカメラの視錐台で落としてよいか | `RenderSystem.cpp:1199-1301`: 影はカメラでカリング済みの `queue_.opaque` から取る → 画面外のキャスターの影が消える (現状の不具合) | (未) | **キャスター候補をカメラの視錐台から切り離し、カスケードごとにライトの直交視錐台でカリング**する。前例は `ShadowAtlas` のタイル単位カリング。CSM のフィット AABB の取り方は変えない (変えると影の解像度配分が変わり golden が動く — 変えたら §8 に積む) |
| 8 | スキンの AABB をどう作るか | `RenderSystem.cpp:1185-1187`: バインドポーズの AABB しか無いので常に可視扱い。ポーズは IK・ラグドール・ブレンドで動く | (未) | **登録時に「全クリップの全キーフレーム × ボーンごとの頂点包絡」の和集合 + 余白** をモデル空間 AABB として SkinnedModel に持つ (クックしない = kCookVersion に関係しない)。**ラグドール作動中は従来どおり常に可視**。UE の固定 bounds / Unity の `localBounds` と同じく保守的な固定箱で、パレットを評価する前に判定できる (評価後に判定すると画面外のキャラのパレット評価を省けない) |
| 9 | アニメの距離間引き (URO) を sim に入れるか | ポーズは `SkinnedMeshComponent` の入力からの純関数 (`Components.h:317-343`)。sim で姿勢を使うのは部位追従・IK・ラグドール (`PartFollowSystem` / `FootIkSystem` / `Ragdoll`) で、描画のパレットは `RenderSystem.cpp:1241-1281` で別に評価している。sim に入れてカメラ距離で間引くとリプレイ・ネット対戦で割れる (カメラは sim の入力ではない) | (未) `[聞]` | **描画側だけ**。sim のポーズ評価には一切触れない。遠い (screen-size が小さい) スキンは N tick に 1 回だけパレットを作り直し、間はキャッシュを使う (補間もしない)。どの tick で作り直すかは `tickIndex + エンティティ番号のハッシュ` から決める (sub-06 で「+ エンティティ番号」から変更) (実時間・描画フレーム数に依存しない = 決定的撮影で再現する)。UE の URO と同じく見た目だけの最適化 |
| 10 | 同じフレームに複数ビュー (Scene View + Game View) が同じキャラのパレットを 2 回作っている | `RenderSystem.cpp:1248` のコメント「ビュー毎に Render() が呼ばれても同じ絵」= 毎ビュー評価 | (未) | URO のキャッシュを **(エンティティ, ポーズ入力, 補間 alpha)** をキーにビュー間で共有する。キャッシュは描画専用で sim から見えない |
| 11 | 描画側の並列化で絵が変わらないか | 既存の流儀: `RenderSystem.cpp:1182` ステージ 2 は要素独立の純関数を `ParallelRanges`、ステージ 3 は直列 | (未) | パレット評価・LOD 選択・カスケードごとのカリングを並列段へ出す。**出力は事前に確保した互いに素のスロット、結合は index 順**。キュー・`skinPalettes_` への push は直列のまま。`--no-jobs` とスクショの画素差 0 が条件 |
| 12 | sim の並列化は何を対象にするか | 空力と XPBD は「並列化を永久に禁止」(加算順が結果の一部)。ADR-020 は「出力次元だけを割る」流儀。他系の独立性は §4.1.7 の調査結果 | (未) `[聞]` | §4.1.7 の表で「他エンティティの今 tick の書き込みを読まない・共有 RNG を使わない・共有コンテナへ書かない」を満たす系だけ。満たさない系は**アルゴリズムを変えてまで並列化しない** (変えると挙動が変わり、それは軽量化ではなく仕様変更)。規約は ADR-028 |
| 13 | 並列化の決定性をどう証明するか | `--no-jobs` は `kCfgJobs` として `.rep` のヘッダに入る (`SimInit.cpp:197`、`SessionTypes.h:37`)。ヘッダが違うのでファイルのバイト比較はできない | (未) | **同じシーンを jobs あり / `--no-jobs` で録り、毎 tick のワールドハッシュ列を比較**する A/B ジョブを `tools\replay_verify.bat` に足す (Debug と Release の両方)。CI (WARP) でも同じ bat が回る |
| 14 | GPU ms を受け入れ条件にしてよいか | CI は WARP (`replay_verify.bat` の `MYE_EXTRA_ARGS`)。WARP の ms は実機の効果を表さない。`PerfBenchmark.cpp` の draw_submission も WARP | (未) | **受け入れ条件は決定的な数 (draw / tri / カリング数 / LOD 段の分布 / オクルージョンで落ちた数)**。ms は実機での参考値としてベンチの出力に載せるだけ (ゲートにしない) |
| 15 | 計測用のベンチシーンをどこに置くか | 既存の `--perf-bench` (`PerfBenchmark.cpp`) は ECS 等のマイクロベンチ。シーン描画を計る仕組みは無い | (未) | **コード生成のデモシーン `render_bench`** (多数のメッシュ + 遮蔽する壁 + 遠景 + スキンのキャラ + 画面外の影キャスター) と、決定的撮影モードで N フレーム描いてビュー別の統計を JSON に書く CLI `--render-stats-dump <file>` を足す。既存デモの生成順は変えない (粒子 RNG のストリーム) |
| 16 | ABI / TypeId / snapshot の版を使うか | LOD 設定はアセット (`.meta`)、オクルージョン・URO・lodBias は RenderSystem の設定。GameLogic へ出す API は無い。sim の状態は変えない | (未) | **ABI (v28)・TypeId (80)・SimSnapshot (v48) は上げない**。M75h が予定する ABI v29 と衝突しない。上がるのは kCookVersion 5→6 と ADR-028 / ADR-029 だけ。上げる必要が出たら「仕様との差分」に出す |
| 17 | 手作りの LOD (glTF `MSFT_lod`、FBX の LOD グループ) を読むか | 三校の素材は手作り LOD を持っていない (未確認だが依頼にも無い) | (未) | 後回し。段の持ち方 (#6) は手作り LOD を後で入れても変わらない形にする (段ごとに別頂点を持つ場合は VB を足す余地を残す、ではなく v1 は共有 VB 固定) |
| 18 | LOD 切り替えのポップ (dither / crossfade) | Unity の LODGroup は Cross Fade、UE は dithered LOD transition (どちらもオプション) | (未) | v1 は**ヒステリシス付きの即時切り替え**。dither は後回し (GBuffer に discard が入り、TAA との相性も見る必要がある) |
| 19 | `.meta` の LOD 設定を変えたのにクックキャッシュが古い段を返さないか | `CookedCache.h:12-19`: 無効化はソースの size/mtime/内容ハッシュと deps だけで、`.meta` を見ない | (未) | **LOD 設定を blob に記録し、読み込み時に現在の設定と比べて違えば再クック**。キャッシュの手動削除を要求しない |

## 3. スコープ

- やる:
  - 計測の土台: GBuffer / Forward 不透明 / 影 / フレーム全体の GpuTimer、影の draw の統計、ビュー別集計、`render_bench` シーン、`--render-stats-dump`。
  - メッシュ LOD: meshoptimizer の導入 (ソースをそのままコミット、`external\VERSIONS.md` に追記)、`.meta` でオプトイン、クック時生成 (kCookVersion 6)、screen-size + ヒステリシスの選択、lodBias / 強制段、エディタの設定 UI。スキンメッシュも同じ仕組みで対象 (頂点共有なので VS スキニングはそのまま動く)。
  - 影のカスケードごとのカリング、スキンの保守的 AABB による視錐台カリング。
  - GPU オクルージョン (2 フェーズ、max-Z HZB を viewKey ごと、compute 判定 → 可視インスタンスの詰め込み → `DrawIndexedInstancedIndirect`)、`--hzb-debug` の可視化の拡張。
  - 描画側の並列化 (パレット・LOD 選択・カスケードのカリング)、URO (描画側)、ビュー間のパレットキャッシュ。
  - sim の並列化 (§4.1.7 の条件を満たす系)、replay_verify の jobs A/B、ADR-028。
- やらない (明示的に外したもの):
  - D3D11 deferred context (ユーザー判断)。
  - 空力・XPBD の並列化 (永久禁止)。
  - sim 側のアニメ間引き (#9)。
  - 並列化のためのアルゴリズム変更 (Gauss-Seidel → Jacobi 等) (#12)。
  - 影・半透明へのオクルージョン (#4)。
  - コライダー・NavMesh・RT の LOD。
- 後回し:
  - 手作り LOD の読み込み (#17)、LOD の dither (#18)、半透明のオクルージョン、ハードウェアのオクルージョンクエリ、メッシュレット / クラスタ単位のカリング。

## 4. 仕様

### 4.1 振る舞い

#### 4.1.1 計測
- `GpuTimer` で GBuffer (Deferred の不透明)、Forward の不透明、CSM、局所影アトラス、オクルージョン (HZB 構築 + 判定)、フレーム全体を計る。ProfilerWindow に既存の段と並べて出す。
- `prof::RenderStats` に影の draw / tri を別欄で足す (本描画の欄に混ぜない)。ビュー (viewKey) 別に集計し、従来の累積値も残す (既存の表示が壊れないこと)。
- 新しい欄: LOD 段ごとの描画数、オクルージョンで落とした数 (フェーズ 1 / 2 の描画数)、URO で再利用したパレット数。GPU で決まる数 (オクルージョン) は読み戻しを待たない: 数フレーム遅れのステージングで読み、**統計にだけ**使う (描画判断には使わない)。
- `--render-stats-dump <file>`: 決定的撮影モード (`--screenshot` と同じ固定条件) で指定フレーム数を描き、最後のフレームのビュー別統計と各段の GPU ms を JSON に書いて終了する。決定的な数は Debug / Release / WARP で同じ値になること。

#### 4.1.2 メッシュ LOD
- モデルの `.meta` に LOD 設定: 段数 (0 = 無し、既定。最大 4 段 = LOD0 + 3)、段ごとの目標三角形比、段ごとの screen-size 閾値 (自動 = 比から決める既定表)。
- クック (フレッシュパースも同じ処理) で `meshopt_simplify` を段ごとに掛け、元の頂点を指す IB の範囲を足す。単純化が目標比に届かない (それ以上減らない) 段は作らない (段数が減る)。生成は決定的 (同じ入力から同じバイト列)。
- 選択: エンティティのワールド AABB の外接球を画面に投影した高さ比 (screen-size) を、段の閾値と比べる。`lodBias` は screen-size 側に掛ける (`screenSize × lodBias` を閾値と比べる = 1 より大きいと詳細な段を長く使う。Unity の `QualitySettings.lodBias` と同じ向き。sub-05 round 1 で確定)。上げる方向と下げる方向で閾値に幅 (ヒステリシス) を持ち、前フレームの段を viewKey ごとに覚える。履歴の無いビュー・初回は距離だけで決まる (決定的撮影で再現する)。
- 強制段 (デバッグ): -1 = 自動 / 0..3 = その段 (無ければ最も粗い段)。
- 影のキャスターはそのエンティティのカメラ基準の段を使う。画面外のキャスターも同じ式で段を決める。
- LOD0 は今の IB そのもの。物理・NavMesh・RT・MeshLibrary の CPU コピーの入力は変わらない。

#### 4.1.3 影とスキンのカリング
- CSM: カスケードごとにライトの直交視錐台 (+ 奥行き方向はシーン AABB まで伸ばす) でキャスター候補を判定し、カスケードごとの描画リストを作る。候補はカメラの視錐台で落とす前の全不透明。
  - (sub-04 round 1 で確定) ライト側の奥行きは切らない: カスケードの判定は近平面 (ライト側) を除く 5 面で行い、CSM の描画は深度クランプ (pancaking、CSM 用ラスタライザの `DepthClipEnable = FALSE`) で近平面より手前のキャスターも深度 0 に潰して書く。CSM のフィット (xy の範囲・zNear / zFar の取り方) は変えない。局所影アトラスのラスタライザは変えない。
- スキン: §2 #8 の保守的 AABB で視錐台判定。ラグドール作動中は常に可視。AABB の外へ姿勢が出るモデル (余白で足りない) が見つかったら、余白を増やすのではなく報告する (未決事項)。

#### 4.1.4 GPU オクルージョン
- 対象: viewKey 1/2/3 の不透明の本描画 (Deferred の GBuffer、Forward の不透明)。視錐台カリングを通った物だけが入る (CPU の視錐台カリングは残す)。
- 2 フェーズ:
  1. 前フレームに可視だったインスタンスを描く。
  2. その深度から max-Z の HZB を作る (`HzbReduceSpan` の分割規則を流用、演算は max)。
  3. 全インスタンスの AABB を compute で HZB と判定し、フェーズ 1 で描いていない可視の物を詰めて描く。判定結果を可視ビットとして次フレームへ残す。
- インスタンスの同一性: 可視ビットは viewKey ごと・エンティティごと (prevRender と同じ流儀の安定スロット) に持つ。新しく出たエンティティは「前フレーム不可視」扱い (フェーズ 2 で判定される = 欠けない)。
- 描画: インスタンシングの run ごとに、compute が可視インスタンスの index を詰め、`DrawIndexedInstancedIndirect` の引数 (instanceCount) を書く。CPU への読み戻しで描画を決めない。インスタンシングできない項目 (ボーン付き等) は instanceCount 0/1 の個別 indirect。
- 遮蔽物: その時点で深度に書かれている物すべて。サーフェスマテリアル・水面など別経路で描く物は判定対象に入れない (常に描く)。
- 切り替え: `RenderSystem::enableOcclusionCulling` (既定 true) と CLI `--no-occlusion`。プロジェクト設定 `assets\project_settings.json` の `"rendering": {"occlusionCulling": bool}` (キーが無い = true) に保存し、Editor と Runtime が起動時に読む。CLI `--no-occlusion` はファイルより優先し、書き戻さない。エディタのメニューで切り替えるとファイルへ保存する (ユーザー判断 2026-10-09、sub-09)。`--hzb-debug` に max-Z ピラミッドと「落とした物の AABB」の表示を足す。
- リサイズ・デバイス消失 (M88) の後は履歴を捨てる (全部を前フレーム不可視として扱う = 欠けない)。

#### 4.1.5 描画側の並列化と URO
- ステージ 2 (並列) に LOD 選択とスキンの視錐台判定を足す。パレット評価はステージ 3 の直列から外し、「可視のスキン項目を直列で数えてスロットを確保 → 並列で評価 → 直列でキューへ」にする。カスケードごとのカリングも並列 (カスケード × 範囲)。
- URO: screen-size が閾値未満のスキンは `updateInterval` tick (段階的、既定表を持つ) に 1 回だけパレットを作り直す。作り直す tick = `(tickIndex + entity.index) % interval == 0`。間は前のパレットを使い、補間しない。ラグドール作動中・クロスフェード中は間引かない。`RenderSystem::enableAnimUro` (既定 true)。
- パレットのキャッシュはビュー間で共有、キー = エンティティ + ポーズ入力 (`SamePoseInputs` と同じ等価) + 補間 alpha + URO の更新 tick。

#### 4.1.6 sim の並列化
- §4.1.7 の条件を満たす系だけ、`jobs::System().ParallelRanges` で**出力 (エンティティ) 次元だけを割る**。読むのは前 tick の値・不変データ・自分の行だけ。書くのは自分の行だけ。共有コンテナ・イベント・構造変更は並列段の後に直列で index 順に適用する。
- `--no-jobs` で全部直列に戻る (既存の `SetEnabled`)。
- 規約を ADR-028 に書く (この 3 条件、禁止リスト = 空力・XPBD、証明方法 = §2 #13)。

#### 4.1.7 sim の系の独立性 (2026-10-09 の調査で確定)
tick の順序は `src\Engine\Engine\Loop\TickRunner.cpp:211` の `RunOneTick` (EngineLoop と HeadlessSim の両方が通る)。

| 系 | 判定 | 根拠 | 並列化に要る局所修正 |
|---|---|---|---|
| CPU 粒子 (`Particles\CpuParticleBackend.cpp`) | **v1 対象** | RNG はエミッタごと (`pool.rng.Seed(desc->seed, e.index*2+1)` :235)。プールは自分のエミッタの desc とワールド行列だけを読む | `SyncEmitters` (:194-241) は直列の前段のまま。`Simulate` のメンバ scratch (`turb_` 等 :440-448) をローカル化。`stats_` / aliveTotal はレンジごとに数えて index 順で合算。GPU 比較モードは触らない |
| Perception (`Perception\PerceptionSystem.cpp`) | **v1 対象** | 各知覚者は自分の `AIPerceptionComponent` だけを書く (:607-772)。刺激・知覚者は先にスナップショットへ集めてある (:505-553) | scratch (`events` / `sightCandidates` / `bestPriority` :604-606) をレンジごとに。`ensureColliders()` (:563, :637) をループの前で確定。`stats_.losRays` をレンジごとに合算 |
| PartFollow (`Animation\PartFollowSystem.cpp`) | **v1 対象** | 各部位は自分の LocalTransform だけを書く (:204-210) | 共有のポーズキャッシュ (:189-201) を直列の前段で作る。`warned_` は並列段の後に index 順で反映 |
| TwoBoneIk / FootIk (`Animation\TwoBoneIkSystem.cpp`、`FootIkSystem.cpp`) | **v1 対象 (条件付き)** | 自分の `sm->poseIk*` だけを書く。読む LocalTransform の鎖とコライダーはこのループで書かれない | `warned_` の扱いは同上。**`RaycastWorld` (`Physics\Rigid\PhysicsSystem.h:203`) が並行読みで安全 (内部に可変キャッシュが無い) と確認できたときだけ**。確認できなければ外して報告 |
| Skinning (`Animation\SkinningSystem.cpp`) | 計測次第 | 自分の行だけ。ただし 1 行の仕事が小さい | sub-01 の計測で目立つときだけ |
| AnimatorController | **外す (後回し)** | `ApplyClipPose` が子孫の任意のコンポーネントを書き (`Animation.cpp:166-180`)、入れ子のコントローラで部分木が重なりうる。`fired_` の順序が意味を持つ | — |
| BehaviorTree | **外す** | 共有の `world.Rng()` (:479, :751, :979)、他エンティティの BT を `Locate()` で処理済み/未処理の境界ごと読む (Gauss-Seidel が仕様)、イベントの `seq` が順序依存、C# コールバック | — |
| AgentSystem (M65f) | **外す** | 共有の `world.Rng()` (:233) | — |
| Crowd (dtCrowd) | **外す (後回し)** | Detour の共有 scratch (`m_obstacleQuery`) と経路キューの反復予算が順序依存。分けられるのは Surface 単位まで | — |
| 空力・XPBD・破壊のボクセル化 | **永久に外す** | `AeroSampling.h:12-14`、`FractureVoxel.h:38` | — |
| 物理 (剛体ソルバ) | **外す** | 本 M90 では調べない (ソルバの反復は順序依存とみなす) | — |
| Transform | 既に並列 | `TransformSystem.cpp:201` (深さごと) | — |

「外す」系を並列にするにはアルゴリズムか RNG の割り当てを変える必要があり、それは挙動の変更 (§2 #12)。

### 4.2 データ・保存形式・互換性
- クック blob (`.mmdl`): `CookedMesh` に LOD の段表 (段ごとの indexOffset / indexCount / screen-size) と、生成に使った LOD 設定を足す。kCookVersion 5→6 (`CookedCache.h` に理由を 1 段落)。段なしのメッシュの IB・頂点のバイト列は v5 と同一。
- `.meta`: モデルの LOD 設定を足す。無い `.meta` は段なし (既存のプロジェクトは何も変わらない)。
- `Mesh` (GPU): 段表を足す。描画は段の範囲で `DrawIndexed*` する。
- シーン JSON・`.rep`・SimSnapshot・ABI・TypeId は変えない。
- 封印キャッシュ (`.sealed`) でも段表が blob に入っているので配布物で LOD が効く。

### 4.3 UI / ビジュアル
- エディタの描画設定メニュー (`EditorApp.cpp:1198` 付近の影の切り替えと同じ場所): オクルージョン ON/OFF、URO ON/OFF、lodBias、強制 LOD 段。 オクルージョンの ON/OFF だけはプロジェクトへ保存する (メニューに注記)。URO・lodBias・強制段は保存しない (起動ごと)。文字列は `LocalizationTable.inl` の `Tr()`、両言語。
- アセットブラウザ / Inspector のモデルの import 設定に LOD 段数・比を出す (テクスチャの import 設定と同じ流儀)。 (sub-05 で確定: v1 は Inspector のみ。アセットブラウザの右クリック「インポート設定」はテクスチャ専用のまま。後回し)
- ProfilerWindow: §4.1.1 の新しい欄と GPU ms。
- `--hzb-debug`: max-Z ピラミッドと落とした物の AABB。
- 見た目の期待: LOD を設定していないシーン・オクルージョン ON/OFF・URO の閾値より近いキャラは、今と画素一致 (golden PASS)。

### 4.4 非機能
- sim のビット一致 (Debug / Release / WARP、jobs あり / なし) を崩さない。描画の変更は sim のハッシュに入らない。
- 決定的撮影: LOD・オクルージョン・URO は実時間・描画フレーム数に依存しない (tick とビュー別の描画通番だけ)。
  - (sub-06 で確定) LOD のヒステリシスと URO の窓は「その run でどの tick を描いたか」の履歴に依存する。URO の窓の途中で初めて見えたキャラは、その tick のポーズでパレットを作る (窓の先頭 tick のポーズは持っていない)。同じ run・同じ撮影手順なら再現するが、シークや巻き戻しの直後は、連続再生と比べて遠景のキャラの姿勢が最大 interval-1 tick ずれうる (見た目だけで sim には入らない)。巻き戻し・シーン切り替え・デバイス消失・simTick の逆行では `ResetRenderHistory` で履歴を捨てる。
- `/fp:precise`。meshoptimizer も同じフラグでビルドする (vcxproj のフラグを揃える)。
- 失敗の局所化: HZB / indirect のリソース作成に失敗したらオクルージョンだけ OFF にしてログ 1 回 (描画は続く)。meshopt が段を作れなければ段なしで登録 (WARN 1 回)。
- 性能のゲートは決定的な数だけ (#14)。ms は参考値。

## 5. 受け入れ条件

1. **計測**: Profiler に GBuffer / Forward / CSM / 局所影 / オクルージョン / フレーム全体の GPU ms が出る。影の draw が別欄で数えられ、ビュー別の集計がある。 — 検証: スクショ (ProfilerWindow)、selftest (統計の集計単体)。
2. **ベンチ**: `render_bench` シーンと `--render-stats-dump` があり、Debug / Release / `--warp` で決定的な数が一致する JSON を出す。 — 検証: 3 構成で dump して比較 (ms 以外の欄の一致)。
3. **LOD 生成**: `.meta` で段を指定したモデルは、クック後に段表を持ち、段ごとの三角形数が単調に減る。同じ入力から 2 回クックしてバイト一致。キャッシュから読んだものとフレッシュパースがバイト一致。`.meta` を変えると手動削除なしで再クックされる。 — 検証: selftest (`ModelCook` / `CookedCache` 系)。
4. **LOD 選択**: screen-size とヒステリシスの選択関数が境界値で期待どおり (上げ・下げの閾値、lodBias、強制段、段の欠落)。 — 検証: selftest。
5. **LOD 効果**: `render_bench` で LOD ON は OFF より tri が減り、段の分布が dump に出る。近景は LOD0。コライダー・NavMesh は LOD0 の三角形数のまま。 — 検証: dump の比較、selftest (物理・NavMesh の入力三角形数)。
6. **影**: 画面外にあるキャスターの影が画面内に落ちるシーンで影が描かれる (現状は消える)。カスケードごとの描画数が全カスケード同じ候補より少ない。 — 検証: スクショ前後比較 (`render_bench` の該当カメラ)、dump のカスケード別 draw。
7. **スキンのカリング**: 画面外のスキンキャラが描画・パレット評価されない。画面内の全クリップの全フレームで、保守的 AABB が姿勢の全頂点を包む。 — 検証: selftest (テスト用スキンモデルの全フレームを CPU スキニングして包含を確認)、dump のパレット評価数。
8. **オクルージョン**: `render_bench` の壁の裏の物が落ち (dump の occluded > 0)、ON/OFF の画素差が 0 (z-fight 由来の差だけは原因を示して許容)。カメラカット直後のフレームでも欠けない。max-Z 縮小が selftest で正しい (min 版と同じ分割規則で max を取る)。 — 検証: `--screenshot` の A/B と img-diff、selftest、`--hzb-debug` のスクショ。
9. **golden**: 既存 golden が既存の許容値で全部 PASS (LOD 未設定・オクルージョン既定 ON・URO 既定 ON の状態)。 — 検証: `Editor.exe --selftest` (golden を含む) Debug / Release。
10. **描画の並列化**: `--no-jobs` とスクショの画素差 0 (`render_bench` と既存のスキン入りデモ)。 — 検証: img-diff。
11. **URO**: 遠いキャラのパレット評価数が減り (dump)、近いキャラは毎 tick 評価。同じ tick を 2 回撮って同じ絵。複数ビューで同じキャラのパレットが 1 回だけ評価される。 — 検証: dump、selftest (更新 tick の関数)。
12. **sim の並列化**: 対象系が並列に走り、replay_verify の jobs A/B (Debug / Release) で全 tick のハッシュが一致。既存の replay_verify も PASS。 — 検証: `tools\replay_verify.bat`。
13. **規約**: ADR-028 (並列化の規約) と ADR-029 (LOD・オクルージョンの方式) がある。engine_spec.md §12.3 の「mesh LOD and GPU occlusion culling」を §12.2 へ移す。 — 検証: 文書の差分。
14. **静的規則・ローカライズ**: `tools\check_rules.ps1` PASS。新しい UI 文字列が両言語で埋まる。 — 検証: スクリプト、selftest (ローカライズ)。
15. **失敗の局所化**: オクルージョンのリソース作成失敗を模擬するとオクルージョンだけ OFF になり描画は続く。meshopt が段を作れないメッシュは段なしで登録される。 — 検証: selftest (注入) 。

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | 計測の土台 (GPU ms・影の統計・ビュー別集計・render_bench・stats dump) | なし | 1, 2 | `M90a: 描画の計測を足す (GBuffer/Forward/影の GPU ms、ビュー別の統計、render_bench と --render-stats-dump)` |
| sub-02 | GPU オクルージョンの縦切り (max-Z HZB を viewKey ごと、2 フェーズ、Deferred の不透明だけ) | sub-01 | 8 (Deferred), 9, 15 (前半) | `M90b: GPU オクルージョンカリングを足す (max-Z HZB の 2 フェーズ、Deferred の不透明)` |
| sub-03 | オクルージョンを Forward へ広げ、デバッグ表示と統計を仕上げる | sub-02 | 8 (Forward), 9 | `M90c: GPU オクルージョンを Forward へ広げ、--hzb-debug に max-Z と落とした物を出す` |
| sub-04 | 影のカスケードごとのカリングとスキンの保守的 AABB | sub-01 | 6, 7, 9 | `M90d: 影をカスケードごとにカリングし、スキンメッシュを保守的な AABB で視錐台カリングする` |
| sub-05 | メッシュ LOD (meshoptimizer、.meta オプトイン、kCookVersion 6、選択、UI) | sub-01 | 3, 4, 5, 9, 14, 15 (後半) | `M90e: メッシュ LOD を足す (meshoptimizer の自動生成、.meta でオプトイン、kCookVersion 6)` |
| sub-06 | 描画側の並列化と URO (パレット・LOD 選択・カスケードの並列、ビュー間キャッシュ) | sub-04, sub-05 | 10, 11, 9 | `M90f: 描画側の CPU 処理を並列化し、遠いスキンのパレット更新を間引く (URO)` |
| sub-07 | sim の並列化 (A/B ジョブを先に → CPU 粒子 / Perception / PartFollow / IK、ADR-028) | sub-01 | 12, 13 (ADR-028) | `M90g: CPU 粒子・知覚・部位追従・IK を並列化し、replay_verify に jobs の A/B を足す (ADR-028)` |
| sub-09 | オクルージョンの ON/OFF をプロジェクト設定に保存する (project_settings.json、Editor のメニューで保存、Runtime も読む) | sub-06 | 8, 9, 14 | `M90h: オクルージョンの ON/OFF をプロジェクト設定 (project_settings.json) に保存する` |
| sub-08 | 文書 (ADR-029、engine_spec の移動、test_checklists) と全体の検証 | sub-03, sub-06, sub-07, sub-09 | 13, 14, 9, 全体 | `M90i: LOD とオクルージョンの ADR-029 を書き、engine_spec と検証表を更新する` |

- 並列にできる: sub-02 / sub-04 / sub-05 / sub-07 は sub-01 の後で互いに依存しない (ただし `RenderSystem.cpp` の同じ関数を触るので、司会が逐次に回すなら sub-02 → sub-04 → sub-05 → sub-07 の順を推奨)。
- 最初のリスクの高い未知 = **2 フェーズのオクルージョンが既存の Deferred の描画経路 (インスタンシングの run、velocity、TAA、サーフェスマテリアル) と WARP で破綻なく組めるか**。sub-01 は計測が無いと sub-02 の効果を示せないので先に置くが、小さく閉じる。

## 7. 未決事項・リスク

- `[聞]` #2 2 フェーズ: 逆 (1 フェーズ、前フレームの HZB を再投影) を選ぶとサブ 1 本分軽くなるが、受け入れ条件 8 の「カメラカット直後でも欠けない」が落ちる。
- `[聞]` #5 LOD のオプトイン: 逆 (全メッシュ既定 ON) を選ぶと受け入れ条件 9 の golden が動き、golden の更新が sub-05 に入る (原因の断定が要る — memory「golden が動いたときの切り分け」)。
- `[聞]` #9 URO を描画側だけ: 逆 (sim に入れる) を選ぶと、間引きの判断を tick とエンティティだけから決める必要があり (カメラ距離が使えない = 効果が薄い)、snapshot の版が上がり、受け入れ条件 12 と同じ A/B 証明が要る。
- `[聞]` #12 sim の並列化はアルゴリズムを変えない系だけ: 逆 (Crowd 等を Jacobi 化して並列化) を選ぶと挙動が変わり (ゲームの見た目も変わる)、三校の確認が要る。サブが 1〜2 本増える。
- リスク: `DrawIndexedInstancedIndirect` と UAV の詰め込みが WARP で遅すぎて CI の時間が延びる → sub-02 で WARP の所要時間を測って報告する。
- リスク: 2 フェーズで GBuffer を 2 回に分けて描くと、velocity・TAA のジッタ・ステンシルを使う経路の前提が崩れるかもしれない (sub-02 の未知)。
- (sub-05 で判明) 硬い面のメッシュ (平面と鋭い辺だけの箱・パネル。例: Lab_Door.fbx) は、既定 (LockBorder、非 Permissive) では目標まで減らず段が作れない (段なし + WARN)。三校の素材に段を付けるなら、比を緩めるか Permissive のオプトインを足す必要がある。v1 の範囲外 (後回し)。
- (sub-05 で判明) Debug selftest の ServerNetSelfTest (`V1 LoadPersist / LoadGame in a session`) が一過性に FAIL する (3 回目)。M90 は sim・ネットに触れていないので別件として扱い、sub-08 の全体検証で M90 前の基点 (`3b30251`) と比べて切り分ける。
- (sub-06 で判明) オクルージョン ON は、フェーズ 2 の状態設定と CB 更新の分だけ CPU の提出が重くなる。run がすべて別のシーン (`--render-bench-unique-demo`、1500 個) では GBuffer の提出が OFF 0.84 ms → ON 1.45 ms (約 1.7 倍)。既定の render_bench では +0.15 ms 程度。planner の裁定: 既定 ON のまま。可視ビットを数フレーム遅れで読み戻してフェーズ 2 の run を飛ばす案は採らない (新しく見えた物の run を飛ばすと欠ける = §1 の 3 に反する)。同じシーンで GPU ms の ON/OFF を sub-08 で計り、ADR-029 に「どういうシーンで OFF が得か」を書く。**確定 (ユーザー 2026-10-09)**: 既定 ON、設定で ON/OFF を切り替えられるようにする → プロジェクト設定へ保存 (sub-09)。
- (sub-06 で判明) 実 GPU (WARP でない) の Release で、同じ入力の撮影が run 間で 51 画素 maxDiff=1 だけ揺れる。M90 前の `29a775e` でも 8 回中 2 回出る (`--no-jobs --no-uro --no-occlusion` でも出る) ので別件。画素 A/B は WARP で行う。golden は既存の許容値で吸収されている。
- **確定 (ユーザー 2026-10-09、「許す」)** (sub-06) URO の窓の途中で初めて見えたキャラは、その tick のポーズで作る (§4.4 の追記)。逆 (どの描画履歴でも同じ絵) を選ぶと、全スキンのポーズ入力を更新 tick ごとに記録して窓の先頭のポーズで評価する必要があり、サブが 1 本増える (画面外のキャラにも毎 tick の記録が要る)。
- リスク: meshopt の単純化が UV の継ぎ目・法線の割れ目で崩れる → 属性付き単純化 (`meshopt_simplifyWithAttributes`) と `meshopt_SimplifyLockBorder` を既定にし、ベンチのスクショで目視 (ユーザー)。
- リスク: スキンの保守的 AABB が IK で外へ出る (余白で足りない)。出たら報告 (余白で黙って塗らない)。
- (sub-04 で判明) スキンの保守的 AABB は実アセットでバインド高の 2.4〜2.9 倍と緩い (余白 = 最長辺の半分、根拠は IK / ブレンドの安全側で実測ではない)。画面端の判定が甘いだけで正しさには影響しない。詰めるのは計測で効果が見えてから (後回し)。
- (sub-04 で判明) スキンは GPU オクルージョンの判定箱に載せない (常にフェーズ 1 で描く) を M90 の間は維持する。保守的 AABB を載せれば判定できるが、箱が IK で外へ出たときの欠けは影より目立つ。M90 の範囲外 (後回し)。
- リスク: FootIk の `RaycastWorld` が並行読みで安全か未確認 (sub-07 で確認。駄目なら IK は外す)。
  - (sub-07 で判明) 安全ではない: `RaycastWorld` は内部で `World::ForEachArchetype` を呼び、これは `iterationDepth_` の非アトミックな増減と `queryCache_` の充填をする。FootIk は外した (TwoBoneIk は並列)。**並列段で World を走査しない** (ADR-028)。IK と PartFollow の並列経路は replay のシーンに入らない (IK を含むシーンが無い / parts デモが小さく並列経路に入らない)。そのため SimParallelSelfTest (200 体、毎 tick ハッシュ) で押さえる。実シーンの A/B に入れるデモの追加は golden に響くので、M90 では足さない (後回し)。
- 番号: ABI v28 / TypeId 80 / SimSnapshot v48 / kCookVersion 5 / ADR-027 (2026-10-09 に `EngineAPI.h:50`、`SimSnapshot.h:140`、`CookedCache.h:38`、`docs\adr\` で確認)。M90 は ABI・TypeId・snapshot を使わないので M75h の ABI v29 予定と衝突しない。使うのは kCookVersion 6、ADR-028 / 029。
- Unity / UE の既存実装は記憶による照合で、一次資料 (公式ドキュメント) は planner の環境に Web が無く未確認。方式の根拠に効くのは「LOD はアセット単位のオプトインで段ごとの screen-size」(UE Static Mesh LOD / Unity 6 Mesh LOD)、「GPU 駆動のオクルージョンは 2 パス」(Nanite / Unity 6 GPU occlusion culling)、「URO は見た目だけ」(UE)。sub-02 / sub-05 の coder は着手時に公式ドキュメントで裏を取り、食い違えば「不安・質問」に出す。

## 8. 変更履歴

(確定後の変更のみ)
- 2026-10-09 ユーザー: §2 の [聞] 4 件 (#2 / #5 / #9 / #12) と全体の確定を、planner の裁定どおりで了承 (司会経由)。
- 2026-10-09 sub-02 round 1 (coder SELF_EVAL): §4.1.4 を次のとおり確定した。
  - 画面の外へはみ出す物は丸ごと可視とする (切り詰めた判定は後から緩められる)。
  - 詰め込みは順序を保つ prefix sum で行う (アトミックで詰めると、同じ深度の物の描画順が実行ごとに揺れるため)。
  - 地形はフェーズ 1 の後・HZB を作る前に描き、遮蔽物にする。
  - HZB を作れないフレームは判定せず全部描き、次のフレームから全ビューで OFF にする。
  - スキンは sub-04 まで判定せず、常にフェーズ 1 で描く。
  - §4.1.1 の `drawCalls` / `triangles` は CPU が提出した論理数と定義する。オクルージョンの効果は occlusion* 欄で見る (表示の仕上げは sub-03)。
  - リスクを 1 件足した: フェーズ 2 で CPU の提出コストが倍になりうる。計測は sub-06 で行う。
- 2026-10-09 sub-01 round 1 (coder SELF_EVAL): render_bench のカメラカットを sub-02 へ移した。描画側のカメラ上書きで入れ、sim には触れない (sub-01 の「必要なら」を確定させた)。§4.1.1 の「フレーム全体」の GPU ms は `RenderSystem::Render` 1 回分 (ビュー 1 本) と定義し直した。表示名の修正は sub-02 で行う。影のキャスターのリストは material → mesh 順に並べてインスタンシングする (sub-04)。理由: 現状は収集順のまま渡しているので run が組めず、1 カスケードあたり 3364 draw になる。
- 2026-10-09 sub-04 round 1 (coder SELF_EVAL / planner VERDICT):
  - §2 #8: スキンの保守的 AABB は「登録時に SkinnedModel へ持つ」から「初回描画時に (モデル, メッシュ) の組ごとに計算してキャッシュし、登録の通番 (revision) で無効化」へ変えた。理由: メッシュとスケルトンは別々に登録され、組はエンティティでしか決まらない (coder の指摘を採用)。クックしない・決定的、は変わらない。
  - §4.1.3: 画面外のスキンもバインドポーズの world AABB で CSM のフィット AABB に入れ続ける (従来どおりのフィットを保つため)。ライト側の奥行きを切らない規則 (5 面判定 + 深度クランプ) を足した。理由: 既存の zNear は画面内の物とカメラのスライスだけから決まるので、ライト側へ離れた画面外キャスター (街の高い建物) が判定で落ち、または近平面でクリップされて影が消える = §1 の 4 が満たせない。
  - 既存 golden 3 枚 (demo_forward / demo_deferred / demo_forward_fxaa) の更新を了承した。差は画面外の立方体の影が手前の床に増えたこと (正しい修正による差) と、並べ替えで run の組が変わったことによる 1〜2 画素の丸め差。4 点計測で断定済み。
- 2026-10-09 sub-05 round 1 (coder SELF_EVAL / planner VERDICT):
  - §4.1.2: lodBias の向きを「screen-size × lodBias を閾値と比べる (> 1 で詳細な段を長く使う)」に確定した。理由: 元の字面 (閾値 × lodBias) は、同じ spec の §2 #5 にある「Unity の QualitySettings.lodBias 相当」と向きが逆で、spec の中で食い違っていた。
  - §4.3: v1 のモデル LOD 設定 UI は Inspector だけにした (アセットブラウザの右クリックは後回し)。
  - 既定値 (ヒステリシス 10%、自動 screen-size = 0.5·√ratio を前の段の 0.8 倍で頭打ち、誤差上限 0.05、届き具合 1.25 倍、属性の重み 0.5) は coder の値を採用する。根拠は ADR-029 (sub-08) に書く。
  - LOD 設定の記録場所を blob の先頭 (ファイル単位の ModelCookData) にした。段表はメッシュ単位の CookedMesh。
  - LOD の履歴 (LodHistory) をシーン切り替えで捨てる処理は sub-06 へ移した (パレットキャッシュを捨てるのと同じ契機)。
- 2026-10-09 sub-06 round 1 (coder SELF_EVAL / planner VERDICT):
  - §2 #9: URO の位相を `(tick + entity.index) % interval` から `(tick + Hash(entity.index)) % interval` に変えた。理由: ModelLoader はキャラ 1 体ごとに 20 エンティティを作るので entity.index が 20 刻みになり、interval 2 / 4 のキャラの位相が全員揃う (unique demo の frame 33 で実測)。これでは URO の目的 (フレームごとの CPU 負荷を下げる) のうち、負荷の山が平らにならない。ハッシュは決定的な整数ミックス (乱数ではない)。
  - §4.4: LOD / URO の描画履歴への依存と、履歴を捨てる契機を明記した。
  - §7: オクルージョンの CPU 提出コスト (既定 ON を維持、読み戻しは採らない)、実 GPU の画素の揺れ (別件)、URO の履歴依存 `[聞]` を追加した。
  - 採用した追加: URO の閾値表 (5% / 2% / 0.8% → 1 / 2 / 4 / 8 tick)、窓の再利用規則、ラグドール作動中はキャッシュしない、画面外スキンにも URO、`TickServices::sceneLoadSerial`、simTick の逆行で履歴を捨てる、`--no-uro`、`--render-bench-unique-demo`、dump の cpuMs 節、カスケード判定はキャスター単位で分割。
- 2026-10-09 ユーザー (sub-06 VERDICT の [聞] 2 件、司会経由):
  - オクルージョン: 「既定 ON で、設定で ON/OFF を切り替えられるように」。既存の口 (Rendering メニュー `Menu_Occlusion`、CLI `--no-occlusion`) はどちらも保存されず、ビルド後の Runtime には CLI しか無いので、要求を満たさないと判断した。RT のタグ規則 (`rayTracingTags`) の前例に合わせ、`project_settings.json` へ保存するサブ sub-09 を新設した (§4.1.4 / §4.3 / §6)。sub-08 の依存に sub-09 を足し、コミットの記号を M90h → M90i にずらした。プレイヤー向けのオプション画面と GameLogic の API は範囲外 (ABI を上げない、§2 #16)。
  - URO の描画履歴への依存: 「許す」。§7 の `[聞]` を確定に直した。
- 2026-10-09 sub-07 round 1 (coder SELF_EVAL / planner VERDICT): FootIk を外したこと、IK / PartFollow の並列の被覆を selftest で持つことを §7 に記録した。Perception の視線コライダー表は、§4.1.7 どおり並列段の前に直列で確定させる (coder の `call_once` 案は採らない。並列段で World を走査するため)。
- 2026-10-10 sub-08 round 1 (coder SELF_EVAL / planner VERDICT): 実測で、run がすべて別のシーン (unique bench、1352 draw) ではオクルージョン ON の方が遅いと分かった (WARP の Forward 50.4 → 70.0 ms、Deferred の CPU 提出 0.71 → 2.52 ms)。既定 ON は変えず、ADR-029 の判断表で「そういうシーンは project_settings で OFF」と案内する。draw 数に応じた自動 OFF は閾値の根拠が無いので後回しにする (三校の実シーンで測ってから判断する)。ServerNetSelfTest の一過性 FAIL は M90 と別件とし、`CrashRoot` の固定パスを取り合っている可能性を仮説として残す。
