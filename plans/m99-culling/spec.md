# M99 カリングの拡充 — 仕様書

- slug: m99-culling
- 状態: 確定 (2026-11-11。U1〜U5 は planner の裁定どおりユーザーが確定、受け入れ条件 14 項目と仮置きの値もそのまま確定)
- 依頼原文:
  1. 「カリング処理を複数個実装したい
     オクルージョンカリング、フラスタムカリング」
  2. 「プロジェクト設定でカリングのON/OFFを変えられるように」
  3. 「追加でカリングで足りない機能あれば追加したい」

## 1. 目的 (なぜ作るか)

- 既にある視錐台カリングとオクルージョンカリング (M16 / M90) を、**プロジェクト設定から種類ごとに ON/OFF できる**ようにする。
  OFF にした絵と比べて「カリングが絵を変えていない」ことをユーザー自身が確かめられる状態にする (エンジンの観測可能性・再現性の方針、AGENTS §3.2)。
- 広い屋外シーンで、遠くの物・画面上で小さすぎる物を落とす手段 (距離カリング・画面サイズカリング) を持つ。
- 視錐台判定が抜けている描画経路 (GPU パーティクル、トレイル、ビルボード、デカール、2D) と、局所影の画面外キャスターの抜けを塞ぐ。
- 「なぜこの物が描かれていないか」を目で追えるようにする (カリング用カメラの固定、理由別の可視化、理由別の数)。
- 空間分割 (BVH) は、計測して効果が出る条件を確かめてから入れる (AGENTS §3.5)。

## 2. 疑った点と結論

| # | 疑い | 根拠 (コード / 事実) | ユーザーの判断 | 結論 |
|---|---|---|---|---|
| D1 | 依頼 1 の「オクルージョン・フラスタムを実装」は二重実装ではないか | 視錐台 = `src\Engine\Renderer\Pipeline\FrustumCull.h` と `RenderSystem::CollectDrawables` (`src\Engine\Engine\Rendering\RenderSystem.cpp:1476-1504`)。オクルージョン = `src\Engine\Renderer\Passes\OcclusionCullPass.cpp` (M90b/c、engine_spec §6.14) | 司会の事前質問で「既存を活かし、切り替え口と不足機能を足す」に合意済み | 新規実装しない。切り替え口と穴埋めを足す |
| D2 | 視錐台だけを切る口が無い | `f.cullEnabled = f.cameraFound` (`RenderSystem.cpp:1014`) が「カメラがあるか」を兼ね、LOD・URO・地形の収集も同じフラグで止まる | — | 「カメラがある」と「視錐台で落とす」を別の旗に分ける。視錐台 OFF でも LOD・URO・地形・ライト選別は今と同じに動く |
| D3 | 視錐台 OFF で CSM のフィット AABB が広がり、影の解像度が変わる | `sceneMin/sceneMax` は可視物だけで作る (`RenderSystem.cpp:1610-1618`) | — | 視錐台判定そのものは OFF でも計算し、フィットには「視錐台の内側の物」だけを入れる。OFF は「落とさない」だけ。これで **視錐台 ON と OFF の全画素一致** を不変量にできる (受け入れ条件の柱) |
| D4 | 視錐台 OFF にすると局所影アトラスの絵が変わる (= 今の局所影は画面外キャスターを見ていないバグの裏返し) | `ShadowAtlas::Render` は `queue_` (画面内) だけを描く (`RenderSystem.cpp:1913`)。CSM は `offscreenCasters_` を持つが、`collectShadowCasters` は平行光があるときだけ (`:1132`) | ユーザーは「局所影の画面外キャスター」を選択 | sub-03 で直す。直した後は「局所影のあるシーンでも視錐台 ON/OFF が全画素一致」が修正の証拠になる |
| D5 | 物ごとの距離を `MeshRendererComponent` に足すか | `MeshRendererComponent` は hash 対象 (`src\Engine\Core\Ecs\Components.cpp:118`、`kComponentNoHash` 無し)。SimSnapshot は NoHash も含めカラムの生バイトを持つ (`src\Engine\Engine\Replay\SimSnapshot.h` の v61 / v76 の記録)。どちらの案も SimSnapshot は上がる | 2026-11-11 ユーザー確定 (U1): RenderCulling コンポーネントを足す | **別の描画専用コンポーネント `RenderCulling` (kComponentNoHash、TypeId 100、SimSnapshot v82)** を足す。付けていない物は 1 ビットも変わらない。MeshRenderer に足す案は全シーンの MeshRenderer カラムが伸びるので却下。「タグ表と既定値だけで、契約番号を動かさない」案は却下 (U1 で確定) |
| D6 | 距離をどこから測るか | Unity `Camera.layerCullDistances` は既定でカメラ平面基準、`layerCullSpherical` で球 ([Unity](https://docs.unity.com/en-us/engine/current/script-reference/unityengine/camera/layerculldistances))。UE は Actor の Min/Max Draw Distance と Cull Distance Volume ([UE](https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine)) | — | **カメラ位置からワールド AABB の最近点までの距離** (球状)。平面基準だとカメラを回すだけで画面端の物が出入りするので採らない。AABB の内側にカメラがあれば 0 |
| D7 | 距離の出所が複数あるときの優先 | UE3 の文書: 重なった Cull Distance Volume は「0 より大きい最小値 (最も厳しい値)」を採る ([UE3](https://docs.unrealengine.com/udk/Three/VisibilityCulling.html)) | — | 実効距離 = {物ごと, その物が持つ各タグの距離, プロジェクト既定} のうち **0 より大きい値の最小**。全部 0 なら距離では落とさない |
| D8 | 距離・画面サイズで落とした物の影を残すか | UE の Max Draw Distance が影も止めるかは公式文書で確認できなかった (記憶では止める。未確認) | 2026-11-11 ユーザー確定 (U2): 影も消す | **影も落とす** (画面外キャスターのリストにも入れない)。遠く・小さい物を落とすのはコストを削るためで、影だけ残すと削れない。影だけ残す案は却下 (U2 で確定) |
| D9 | 距離・画面サイズカリングをエディタの Scene View にも掛けるか | UE は通常のエディタビューに Cull Distance を掛けず、Game View (G) でだけ確かめる (上記 UE 文書) | 2026-11-11 ユーザー確定 (U3): Scene View は既定で掛けず、セッション限りのトグルで掛ける | viewKey 1 (Runtime) と 3 (Game View) に掛ける。2 (Scene View) は既定で掛けず、Rendering > カリング の「Scene View にもゲームのカリングを掛ける」(セッション限り) で掛ける。0 (プレビュー・プローブ焼き) には掛けない。編集中に遠くの物が消えて選べなくなるのを避ける |
| D10 | 画面サイズの物差しと既定値 | `BoxScreenSize` (`src\Engine\Renderer\Mesh\MeshLod.h`) は外接球の画面高さ比で、LOD と URO が既に使う。Unity の LODGroup の Culled 段は「画面高さに対する比率」 ([Unity LODGroup](https://docs.unity3d.com/6000.4/Documentation/Manual/class-LODGroup.html)) | — | `BoxScreenSize` を `lodBias` を掛けずにそのまま閾値と比べる。既定は OFF、閾値の既定 0.005 (1080p で約 5 px。2026-11-11 にこの値で確定。ユーザーが Project Settings で変えられる)。点滅防止に LOD と同じ 10% のヒステリシス (ビュー別の履歴、`ResetRenderHistory` で捨てる) |
| D11 | 距離にもヒステリシスが要るか | 距離はカメラ位置だけで決まり、カメラが止まれば揺れない。UE の Max Draw Distance にヒステリシスがあるかは未確認 (記憶による) | — | 付けない |
| D12 | GPU パーティクルの箱をどう得るか | 粒子位置の CPU readback は無い (`GpuParticleBackend.h:79-104`)。readback は GPU の進み具合で遅れが変わり、`counts` の決定性 (Debug / Release / WARP 一致) を壊す。UE の Niagara は GPU エミッタに固定の Bounds を要求する (記憶による。未確認) | 2026-11-11 ユーザー確定 (U4): 記述子から上界を計算する | **記述子から解析的に上界を出す**: 形状の広がり + `speedMax·T + ½|g+w|·T²` (T = `lifetimeMax`) + ビルボードの膨らみ (`ParticleBillboardExpand` と同じ式で `sizeMax` を使う)。乱流・かき乱しフィールドの影響・速度継承のどれかが 0 でない、またはワールド空間で直近 T 秒の間に原点が動いたエミッタは **常に描く** (落とさない)。固定 Bounds を物に持たせる案 (UE 方式) はコンポーネントが伸びるので却下 (U4 で確定) |
| D13 | どの描画経路に視錐台を足すか | Explore の確認: 判定が無いのは GPU パーティクル (`GpuParticleBackend.cpp:1179`)、トレイル (`VfxRenderer.cpp:299-308`)、SpriteRenderer / TextMesh (`VfxRenderer.cpp:258-297`)、デカール (`RenderSystem.cpp:1181-1236`、engine_spec.md:637 が v1 の制限として記録)、Sprite2D / Line2D / SpriteShape2D (`Sprite2DRenderer.cpp:939/993/1014`)、水面 | ユーザーは「視錐台判定の無い経路」を選択 | GPU パーティクル・トレイル・SpriteRenderer・デカール・Sprite2D・Line2D・SpriteShape2D に足す。TextMesh (文字の広がりは組版後にしか分からない、数も少ない)、水面 (1 枚の巨大な板)、マスク (`CollectMasks`、画面外のマスクが画面内に効く経路を見落とす危険) は足さない |
| D14 | 2D の判定に何を使うか | タイルマップのチャンク判定が `OrthoWorldBounds` で既にある (`Sprite2DRenderer.cpp:114-141`、`:1089-1101`) | — | 同じ `OrthoWorldBounds` の矩形と各項目のワールド矩形を比べる。透視カメラ・行列が無い・逆行列が無いときは今のタイルマップと同じく落とさない。Parallax の繰り返しがある項目は落とさない |
| D15 | 視錐台スイッチの対象範囲 | 地形の `CullChunks` (`TerrainSystem.cpp:390`) はチャンク選択と一体 | — | スイッチが効くのは: メッシュ、CPU パーティクルのプール、タイルマップのチャンク、M99 で足す経路。**地形は対象外** (今と同じく常に判定する) と文書に書く |
| D16 | BVH を入れるか | 静的物の印が ECS に無い (Components に Static 相当が無い)。ステージ 1 (直列の ECS 走査・`GetComponent`) は BVH があっても減らない。既定ベンチは 3660 個 | ユーザーは BVH を選択。司会の事前質問で「計測してから」に合意、2026-11-11 に確定 (U5) | sub-01 で段ごとの CPU 時間を測り、**採用条件**を満たしたときだけ planner が BVH のサブを足す。予測は「満たさない」(ステージ 2 の判定より ステージ 1 が重い)。満たさなければ ADR-038 に数字ごと「見送り」と残して閉じる。計測を待たずに入れる案は却下 (U5 で確定) |
| D17 | ON/OFF の保存の仕方 | 既存のオクルージョンはメニューで切り替えた瞬間に `project_settings.json` へ書く (`EditorApp.cpp:1274-1277`)。Project Settings 窓のパーティクル既定は保存ボタン方式 (`ProjectSettingsWindow.cpp:80-90`) | 依頼 2 | オクルージョンに揃えて **切り替えた瞬間に実行中の RenderSystem へ反映し、ファイルへも書く**。数値欄は編集を終えたとき (`IsItemDeactivatedAfterEdit`) に書く。CLI の `--no-*` はその起動中だけで、ファイルを書かない (既存の `--no-occlusion` と同じ) |
| D18 | カリング用カメラの固定でオクルージョンも止めるか | UE の `FreezeRendering` は遮蔽結果ごと止める (UE 文書)。こちらのオクルージョンは現フレームの描画カメラの深度で判定する 2 相式 (§6.14) | — | 固定は視錐台・距離・画面サイズ (CPU 側の判定) にだけ効く。オクルージョン・ライト選別・CSM は描画カメラのまま。固定した視点の深度ピラミッドを別に持つのは費用に見合わない |

参考にした公式情報 (2026-11-11 取得):
- UE: https://dev.epicgames.com/documentation/en-us/unreal-engine/visibility-and-occlusion-culling-in-unreal-engine (Min/Max Draw Distance、Cull Distance Volume、`FreezeRendering`、`r.VisualizeOccludedPrimitives`、`stat initviews` の理由別の数)
- UE3: https://docs.unrealengine.com/udk/Three/VisibilityCulling.html (重なった Cull Distance Volume は最も厳しい値)
- Unity: https://docs.unity.com/en-us/engine/current/script-reference/unityengine/camera/layerculldistances (0 = far、`layerCullSpherical`)
- Unity: https://docs.unity3d.com/6000.4/Documentation/Manual/class-LODGroup.html (Culled 段は画面高さ比)

## 3. スコープ

- やる:
  - 種類ごとの ON/OFF (視錐台・オクルージョン・距離・画面サイズ) を Project Settings > Rendering と Rendering メニューに置き、`project_settings.json` に保存する。CLI で起動中だけ OFF にできる
  - 距離カリング (物ごと / タグ別 / プロジェクト既定)、画面サイズカリング (閾値 + ヒステリシス + 物ごとの除外)
  - 視錐台判定の追加: GPU パーティクル、トレイル、SpriteRenderer (3D ビルボード)、デカール、Sprite2D / Line2D / SpriteShape2D
  - 局所影アトラスの画面外キャスター
  - カリング用カメラの固定、落とした物の理由別の枠表示、固定した視錐台の線、理由別の数 (Profiler と `--render-stats-dump`)
  - BVH の計測と採否の判定 (採るならサブを足す)
  - 検証用のコード生成シーン、回帰テスト、engine_spec / ADR-038 / test_checklists
  - `tools\cull_verify.bat` を CI (`.github\workflows\ci.yml`) の shot_verify の次に載せる (`MYE_CULL_SKIP_CONFIGS=1`、WARP のみ。2026-11-11 ユーザー判断、sub-10)
- やらない (明示的に外したもの):
  - ポータル / PVS、クラスタードライトカリング、オクルージョンの対象拡大 (スキン・影パス) — ユーザーが選ばなかった
  - 距離・画面サイズカリングをメッシュ以外 (パーティクル・2D・デカール・地形) に掛けること
  - TextMesh・水面・2D マスクの視錐台判定 (D13)
  - 地形を視錐台スイッチの対象にすること (D15)
  - ゲーム内の設定画面・GameLogic / C# からの切り替え API (ABI は変えない)
  - RT の BVH への収集の変更 (距離で落とした物も今どおり RT のシーンには入る。§6.14 の「RT の収集は視錐台で落とさない」と同じ扱い)
- 後回し:
  - BVH の実装 — sub-01 の計測で採用条件を満たさず**見送り** (2026-11-11、ADR-038。K=16 で視錐台判定 0.477 ms・全体の 3.7%、支配的なのはステージ 1 の 62%)。58k 個を超える規模で CPU が問題になったら、先にステージ 1 を測り直す
  - GPU パーティクルの固定 Bounds の手指定 (U4 で不採用。必要になったら別マイルストーン)

## 4. 仕様

### 4.1 振る舞い

**4.1.1 設定と実効値**

| 項目 | 保存キー (`assets\project_settings.json` の `"rendering"` 内) | キーが無い / 読めないとき | CLI (その起動中だけ。ファイルは書かない) |
|---|---|---|---|
| 視錐台カリング | `frustumCulling` (bool) | true | `--no-frustum-culling` |
| オクルージョンカリング | `occlusionCulling` (bool、既存) | true | `--no-occlusion` (既存) |
| 距離カリング | `distanceCulling` (bool) | true (距離が 1 つも無ければ何も落ちない) | `--no-distance-culling`、`--cull-max-distance D` (既定距離をその起動中だけ D にする) |
| 画面サイズカリング | `sizeCulling` (bool) | false | `--no-size-culling`、`--cull-min-screen-size F` (F > 0 でその起動中だけ ON・閾値 F) |
| 画面サイズの閾値 | `minScreenSize` (float、0 < x ≤ 1) | 0.005 | 上記 |
| 既定の最大描画距離 | `defaultMaxDrawDistance` (float m、0 = 無制限) | 0 | 上記 |
| タグ別の距離 | `tagCullDistances` (配列 `[{"tag": 3, "distance": 50.0}]`、tag は 0..63、distance > 0) | 空 | なし |

- 実効値 = ファイルの値 && CLI の OFF が無いこと (bool)。CLI の値の上書きは数値にだけ。`--cull-min-screen-size F` はファイルが OFF でもその起動中は ON にするが、`--no-size-culling` と併記したら OFF が勝つ (sub-02 で確定)。優先の式は既存の `ResolveOcclusionCulling` と同じ考え方で、EngineLoop と selftest が同じ関数を使う。
- 範囲外・型違いの値は既定値に落とし、ログに 1 行出す。読めないファイルは上書きしない。`"rendering"` 内の他キーと他の節 (`rayTracingTags` 等) は壊さない。
- Editor と Runtime は起動時に読む。エディタでの変更は即座に実行中の RenderSystem へ反映する (Play 中も可。描画専用なので sim に影響しない)。

**4.1.2 視錐台カリングのスイッチ**
- 「カメラがある」(`cameraFound`) と「視錐台で落とす」を分ける。OFF のとき、視錐台判定の結果は計算するが物を落とさない。
- OFF でも変わらないもの: LOD の段、URO、地形の収集、ライト選別、CSM / アトラスのライト側のカリング、**CSM のフィット AABB (視錐台の内側の物だけで作る)**、ベロシティ履歴の扱い (画面外の物を描いても `prevRender` へ記録するかどうかは ON と同じ規則にする: 視錐台の外の物は記録しない)。
- 対象: メッシュ、CPU パーティクルのプール、タイルマップのチャンク、M99 で足す経路 (4.1.5)。地形は対象外。
- 不変量: **固定条件の撮影で、視錐台 ON と OFF の絵が全画素一致し、`drawCalls` だけが増える** (オクルージョンの ON/OFF 全画素一致 (`OcclusionSelfTest`) と同じ作り)。

**4.1.3 距離カリング (メッシュのみ)**
- 実効距離 Dmax = {`RenderCulling.maxDrawDistance`, 物が持つ各タグの `tagCullDistances`, `defaultMaxDrawDistance`} のうち 0 より大きい値の最小。どれも 0 なら判定しない。
- **継承 (sub-06 round 1 で確定、RT の適用範囲と同じ規則)**: タグは `Tags::EffectiveMask` (自分 + 祖先の OR) で引く (`src\Engine\Engine\Scene\Tags.h` の「描画はこちらを使う」の規則。FBX / glTF のモデルはメッシュが子孫に分かれるので、ルートに付けたタグが効かないと使えない)。`RenderCulling` は自分 → 祖先の順に探して最初に見つかった 1 個を使う (`ResolveRtScope` と同じ「近い方が勝つ」)。見つかった物の `maxDrawDistance` と `exemptFromSizeCulling` を両方そのまま使う (子に置いた `RenderCulling` は祖先の値を丸ごと上書きする)。祖先を辿るのは、ワールドに `RenderCulling` が 1 つでもあるか、タグ別の距離が 1 つでもあるときだけ (存在の門)。判定は物 (メッシュのエンティティ) ごとの箱で行うので、1 つのモデルの部品が少しずつ違う距離で消えることはある。
- 距離 d = カメラ位置からワールド AABB (`boundsPadding` 込み、スキンは全姿勢の箱) の最近点までのユークリッド距離。d > Dmax で落とす。正射影でもカメラ位置から測る。
- 箱を持たない物 (`alwaysVisible`、例: ラグドール中のスキン) は落とさない。
- 掛けるビュー: viewKey 1 と 3。2 は「Scene View にもゲームのカリングを掛ける」が ON のときだけ。0 には掛けない。
- 落とした物は描かず、CSM / アトラスのキャスターにも入れず、CSM のフィットにも入れない (D8)。オクルージョンの判定にも渡らない。

**4.1.4 画面サイズカリング (メッシュのみ)**
- s = `BoxScreenSize(ワールド AABB, カメラ位置, proj._22, 正射影か)` (LOD と同じ式、`lodBias` は掛けない)。s < 閾値で落とす。
- ヒステリシス: 前フレームにこのビューで画面サイズで落とした物は s < 閾値 × 1.1 の間は落としたまま。履歴は視錐台・距離で落ちた物にも画面サイズの判定結果として書く (理由の計上とは独立。sub-07 で確定)。閾値が 0 以下なら判定しない。履歴はビュー別 (viewKey 1..3)、`ResetRenderHistory()` で捨てる。画面サイズは画面の高さに対する比率 (`proj._22` は fovY から作る) なので、ビューのサイズ変更では値が変わらず、履歴を捨てる必要はない (review-1 #1 で「サイズ変更で捨てる」を削除。LOD の履歴も同じ扱い)。履歴が無いときは閾値だけで決まる。
- `RenderCulling.exemptFromSizeCulling` が true の物は落とさない。`alwaysVisible` も落とさない。
- 掛けるビュー・影の扱いは距離と同じ。
- 判定の順序 (理由の付け方): 視錐台 → 距離 → 画面サイズ。先に当たった理由を 1 つだけ付ける (数は二重に数えない)。視錐台 OFF のときに視錐台の外にある物は、距離・画面サイズで落ちればその理由で数える。

**4.1.5 視錐台判定の追加 (描画だけを落とす。sim・粒子の更新は止めない)**
- GPU パーティクル: エミッタ単位。解析的な上界 (D12) を持てるエミッタだけ判定し (sub-04 で確定した細目: 上界は球。深度衝突・解析床の衝突を持つエミッタも「持てない」側。上界 (中心・半径・張り出し) が前 tick と 1 ビットでも違った tick から ceil(T/dt)+2 tick の間は使わない = 移動・スケール・記述子の編集のどれでも待つ。新規エミッタは過去の粒子が無いので待たない。ローカル空間でも乱流などがあれば一律に「持てない」)、落としたエミッタは描画前の並べ替えも描画も飛ばす。上界を持てないエミッタは常に描く (ログは出さない。数は「未判定」として統計に出す)。
- トレイル: 点列の AABB を幅の半分だけ広げて判定。
- SpriteRenderer (3D ビルボード): 位置を中心、`0.5·|size|` × max(1, ワールド行列の最大軸スケール) を半径とする球の外接箱で判定 (描画は size をそのまま使いワールドのスケールを掛けないので、縮小スケールで半径を縮めると見えている物を落とす。sub-04 で修正)。
- デカール: 投影ボックス (ワールド行列 × デカールの単位箱。実際の単位箱は `FillDecalTransform` で確かめる) で判定。engine_spec §6.6 の「No frustum culling」の行を消す。
- Sprite2D / Line2D / SpriteShape2D (判定は Resolve 後の Emit 側。項目は items_ に残し、頂点を作らない。表示範囲の箱に 1 % + pixelSnapUnit の余裕。SpriteShape の張り出しは `profile.height/2 × マイター上限`): `OrthoWorldBounds` の矩形と項目のワールド矩形 (XY) を比べ、z は既存のタイルマップと同じ扱い。回転・スケールは 4 隅 / 点列を変換した外接矩形で扱う。透視カメラなどで `OrthoWorldBounds` が false なら落とさない。Parallax の繰り返し項目とマスクは落とさない。残る項目の描画順は変えない。

**4.1.6 局所影の画面外キャスター**
- 影を落とす点光源・スポットが選ばれたフレームでも、視錐台で落とした不透明物を画面外キャスターとして集める (今は平行光があるときだけ)。
- アトラスの各タイルは「画面内のキャスター + 画面外のキャスター」をタイルの視錐台で判定して描く。面カリング (`RenderSystem.cpp:1905`) の判定箱には画面外キャスターの箱も含める。**CSM のフィット AABB は変えない**。
- 画面外のスキンは、どれかのタイル (またはカスケード) が必要とするときだけパレットを評価する (CSM と同じ)。
- 距離・画面サイズで落とした物は入れない。

**4.1.7 デバッグ機能 (どれもセッション限り。保存しない)**
- カリング用カメラの固定: Rendering > カリング の「カリング用カメラを固定」。ON にした後に各ビューが最初に描かれたときのビュー別 (1..3) のカメラ (カメラの無いビューは控えない。sub-08 で確定) (非ジッタの view-proj、位置、`proj._22`、正射影か) を控え、以後そのビューの視錐台・距離・画面サイズの判定はその控えで行う。描画カメラ・ライト選別・CSM・オクルージョンは今のカメラのまま (D18)。固定中は控えた視錐台を線で描く。CLI `--cull-freeze-frame N` (撮影の N フレーム目で固定。`--render-bench-cut-frame` と組んで検証する)。
- 理由別の枠表示: Rendering > カリング の「落とした物を表示」と CLI `--cull-debug`。落としたメッシュのワールド AABB を理由別の色で線描きする (視錐台・距離・画面サイズ・オクルージョン。色は名前付き定数、凡例はメニューのツールチップ)。オクルージョンの箱は既存の `OcclusionDebugBoxes` を使う (`--hzb-debug-max` が無くても集める)。描く箱は全理由の合計で上限 4096、超えた数をメニューとログ (1 回) に出す。固定は 2D の表示範囲判定・GPU / CPU パーティクル・トレイル・ビルボード・デカールにも効く (視差の基準は今のカメラ)。
- 理由別の数: `prof::RenderStats` に `culledDistance`、`culledScreenSize`、`culledNonMesh` (メッシュ以外で視錐台により落とした項目数。4.1.5 の経路に加え、既存の CPU パーティクルのプールとタイルマップのチャンクも数える。地形のチャンクは数えない。sub-05 round 1 で確定)、`unculledGpuEmitters` (上界を持てず判定しなかった GPU エミッタ数) を足す。既存の `culled` は「視錐台で落としたメッシュ数」のまま意味を変えない。Profiler の表と `--render-stats-dump` の `counts` に出す。

**4.1.8 計測 (BVH の採否)**
- `--render-stats-dump` の `cpuMs` に `CollectDrawables` のステージ 1 / 2 / 3 の CPU 時間 (参考値) を足す。
- ベンチの物量を増やす `--render-bench-scale K` (グリッドを K 倍。K = 1 で今と同じシーン、`counts` も同じ)。
- **採用条件**: Release・実 GPU・開発機で、K = 16 (約 58k 個) の中央値 (`--shot-frame 300` を 5 回) で、ステージ 2 の視錐台判定部分が 1.0 ms 以上 **かつ** `CollectDrawables` 全体の 30% 以上。満たせば planner が BVH のサブを設計して足す (結果は既存の線形判定とビット一致を不変量にする)。満たさなければ ADR-038 に数字と「見送り」を残す。

### 4.2 データ・保存形式・互換性
- `project_settings.json` の `"rendering"` にキーが増える (4.1.1)。古いファイル (キー無し) は既定値で読まれ、絵は今と同じ (画面サイズは既定 OFF、距離は既定 0)。
- 新コンポーネント `RenderCulling` (`RenderCullingComponent`、kComponentNoHash、Inspector の Add Component に出す):
  - `maxDrawDistance` (float、0..100000、0 = 物ごとの上限なし)
  - `exemptFromSizeCulling` (bool)
  - 付けたエンティティと、その子孫の MeshRenderer に効く (近い方が勝つ。4.1.3 の継承)。シーン JSON・プレハブ上書きは既存のリフレクション経路に載せる。
  - **TypeId 100、SimSnapshot v82** (着手時に実コードで末尾を再確認し、台帳に書く)。**ABI は v35 のまま** (C ABI・`Interop.cs` は触らない)。WorldHash は変わらない。
- 統計の JSON は欄が増える (既存の欄の名前と意味は変えない)。

### 4.3 UI / ビジュアル
- Project Settings > Rendering に「カリング」の区切り:
  - チェック 4 つ (視錐台・オクルージョン・距離・画面サイズ)。画面サイズの閾値 (スライダー、対数表示可)、既定の最大描画距離 (m)。
  - タグ別の距離の表: 名前のあるタグと距離が設定済みのタグを行に出し、距離 (0 = 設定なし) を編集する。
  - 「Runtime は起動時に読む」旨の注記。
- Rendering メニューに「カリング」サブメニュー: 4 つのスイッチ (既存のオクルージョンの項目はここへ移す。保存の挙動は同じ)、「Scene View にもゲームのカリングを掛ける」、「カリング用カメラを固定」、「落とした物を表示」。
- 文字列はすべて `LocalizationTable.inl` に英日両方。`Tr()` を書式文字列に直接渡さない。
- 見た目の確認: 枠表示と固定した視錐台の線は一時プローブ + `--screenshot` で撮って確かめる。最終の目視はユーザー。

### 4.4 非機能
- すべて描画専用。sim・WorldHash・`.rep`・Replay・ABI に影響しない。SimSnapshot は `RenderCulling` の追加で v82 になる (カラムの生バイト)。
- 判定は tick とビュー別の描画履歴だけで決まり、実時間・描画フレーム数に依存しない。並列ステージで判定してもキュー化は直列・候補順 (今の 3 段構成を守る)。`--no-jobs` と結果が一致する。
- `--render-stats-dump` の `counts` が Debug / Release / `--warp` で一致する。
- 既定設定 (新キー無し) では既存の golden と `counts` が変わらない。ただし sub-03 (局所影) で正しく直った結果として変わる golden は、視錐台 ON/OFF の全画素一致を根拠に差分を説明したうえで更新してよい。
- `/fp:precise` のまま。`_DEBUG` 分岐で判定を変えない。

## 5. 受け入れ条件

1. 設定の往復: 7 キーを保存 → 読み戻しで一致。キー無し / 型違い / 範囲外 → 既定値。読めないファイルは上書きしない。他キーを壊さない。— CullingSettings の selftest (TagSelfTest か新スイート)
2. CLI: `--no-frustum-culling` / `--no-distance-culling` / `--no-size-culling` / `--cull-max-distance` / `--cull-min-screen-size` / `--cull-freeze-frame` / `--cull-debug` / `--render-bench-scale` が解析でき、実効値の式どおりになり、ファイルを書き換えない。— EngineCliSelfTest
3. 視錐台 OFF: 既定ベンチ (`--render-bench-demo`) で ON と OFF の撮影が全画素一致し、`drawCalls` は OFF の方が多く、`culled` は OFF で 0。CSM のフィットが変わらない (`shadowCascadeCasters` 等の影の数が ON と同じ)。— selftest または `--screenshot` + img-diff (tol 0) と `--render-stats-dump`
4. 局所影: 影付きの点光源・スポットと画面外のキャスターを含む検証シーンで、視錐台 ON と OFF が全画素一致する (修正前は一致しないことも記録する)。アトラスの `culledDraws` / `culledFaces` が妥当。— 同上 + shot_verify の結果説明
5. 追加経路: GPU パーティクル・トレイル・SpriteRenderer・デカールを画面内外に置いた 3D 検証シーンと、Sprite2D・Line2D・SpriteShape2D を置いた 2D 検証シーンで、視錐台 ON/OFF が全画素一致し、ON で `culledNonMesh` > 0、OFF で 0。乱流付き GPU エミッタは `unculledGpuEmitters` に数えられる。— 同上
6. 距離: 既定距離・タグ別・物ごとの 3 つの出所それぞれで、閾値の内外の物が期待どおり落ちる / 残る (最小値の規則を含む)。Scene View (viewKey 2) は既定で落とさず、トグルで落とす。viewKey 0 は落とさない。落とした物は影も消える。— 単体テスト (判定関数) + `--render-stats-dump` の `culledDistance`
7. 画面サイズ: 閾値の内外、ヒステリシス (閾値の 1.0〜1.1 倍の帯で前フレームの状態を保つ)、`exemptFromSizeCulling`、履歴のリセット。— 単体テスト + `culledScreenSize`
8. 既定値の不変: 新キー無しの `project_settings.json` で、既存の golden (`tools\shot_verify.bat`、sub-03 で説明付きで更新したものを除く) と既定ベンチの `counts` (新しい欄を除く) が M99 前と同じ。— shot_verify + dump の比較
9. デバッグ: `--cull-freeze-frame` + `--render-bench-cut-frame` で、固定後の `culled` が固定時の視点の値のまま。`--cull-debug` の撮影で理由別の色の枠が出る (スクショで確認)。Profiler に理由別の数が出る。— dump + スクショ
10. `RenderCulling` コンポーネント: シーン保存 → 読み込みで値が残る、プレハブ上書きが効く、WorldHash が変わらない、SimSnapshot v82 の往復。— 既存のシリアライズ / SimSnapshot の selftest に追加
11. 計測: K = 1 の `counts` が今と同じ。K = 16 の段別 CPU 時間を測り、採否と数字が ADR-038 にある。— dump と文書
12. 決定性: 新しい欄を含む `counts` が Debug / Release / `--warp` で一致。`--no-jobs` と結果が一致。— dump 3 本の比較
13. 文書とルール: engine_spec §6.14 (切り替え・距離・画面サイズ・追加経路・局所影・デバッグ・CLI 表)、§6.6 の制限表、ADR-038、`docs\test_checklists.md` を更新。ローカライズは両言語。`tools\check_rules.ps1` が 0 error。
14. 通し: `Editor.exe --selftest-full` PASS、`tools\replay_verify.bat` PASS、`tools\shot_verify.bat` PASS (8 の例外を除く)。視錐台 ON/OFF の全画素一致を回帰として回せるスクリプト `tools\cull_verify.bat` が PASS。
16. 線と塗りの色 (review-1 #2 / review-2 #1、2026-11-11 ユーザー判断で直す。対象は同じ線パスを通る物すべて): RenderSystem の線パス (カリングの枠、固定した視錐台の白線、`--hzb-debug-max` の赤、`debugLines` = スクリプトの `DebugDrawLine` と物理・BT・知覚・ナビ経路・かき乱し・音響のデバッグ線、ナビメッシュの輪郭線) とナビメッシュの塗りが、定数の色どおりに最終画像へ出る (判定は sub-11 round 1 で見直した: 線で変わった画素の 95% 以上が「線なしの背景と定数色の α 混合」で各チャネルの残差 3 以下で説明でき、修正前のビルドではその割合が明らかに低い。1 px の AA 線では定数色そのものの画素がほとんど出ないため、「差 ≤ 8 の画素数」の式は撤回)。深度テストのある線は今どおり壁の裏で隠れる。動いた golden は、差分が線と塗りの画素に限られる (D ⊆ L_old ∪ L_new、線以外は tol 0 で一致) ことを確かめてから更新する。枠表示 OFF の counts は変わらない。— sub-11 の検証
17. 撮影の決定性 (run 間一致): 同じ exe・同じ引数・`--warp` の 2 回の撮影が tol 0 で一致する。画面サイズカリング ON (`--render-bench-demo --cull-min-screen-size 0.02 --cull-debug`) と距離カリング ON (`--culling-distance-demo --cull-max-distance 40`) で確かめ、cull_verify の回帰 (CI でも走る) にする。— sub-12
15. CI: `ci.yml` の shot_verify の次に cull_verify の step があり (`MYE_CULL_SKIP_CONFIGS=1`)、YAML として解析でき、CI と同じシェル (cmd)・引数・環境変数でローカル実行して PASS、検査が失敗したら step が非 0 で終わる。CI 上の実走は master の CI が緑に戻るまで観測できないので未検証として残す。— sub-10 の検証

## 6. サブ分割

| サブ | 題名 | 依存 | 受け入れ条件 (5. の番号) | コミット件名候補 |
|---|---|---|---|---|
| sub-01 | 段別の CPU 時間とベンチの物量、BVH の採否 | なし | 2 (`--render-bench-scale` のみ), 11 | M99a: カリングの CPU 時間を段ごとに計り、BVH を入れるかを決める |
| sub-02 | カリング設定の土台と視錐台のスイッチ | sub-01 | 1, 2, 3, 12 (一部) | M99b: カリングの設定をプロジェクト設定へまとめ、視錐台カリングを切り替えられるようにする |
| sub-03 | 局所影の画面外キャスター | sub-02 | 4 | M99c: 局所影のアトラスに画面外のキャスターを描く |
| sub-04 | 3D の経路に視錐台判定を足す (GPU パーティクル・トレイル・ビルボード・デカール) | sub-03 | 5 (3D) | M99d: GPU パーティクル・トレイル・ビルボード・デカールを視錐台で落とす |
| sub-05 | 2D の経路に視錐台判定を足す | sub-02 | 5 (2D) | M99e: Sprite2D・Line2D・SpriteShape2D を画面外で落とす |
| sub-06 | 距離カリングと RenderCulling コンポーネント | sub-03 | 1, 2, 6, 10 | M99f: 距離カリングと RenderCulling コンポーネントを追加する (SimSnapshot v82) |
| sub-07 | 画面サイズカリング | sub-06 | 1, 2, 7 | M99g: 画面上で小さすぎる物を落とす画面サイズカリングを追加する |
| sub-08 | カリング用カメラの固定と理由別の可視化 | sub-07 | 2, 9 | M99h: カリング用カメラの固定と、落とした物の理由別表示を追加する |
| sub-09 | 文書と通し検証 | sub-08, sub-04, sub-05 | 8, 12, 13, 14 | M99i: カリングの拡充を engine_spec と ADR-038 に記録する |
| sub-10 | cull_verify を CI に載せる (+ review-1 #1 の文書修正) | sub-09 | 13 (#1 の文書), 15 | M99j: カリングの回帰 (cull_verify) を CI に載せる |
| sub-11 | RenderSystem の線パスを定数の色で描く (カリングの枠・デバッグ線・ナビの線と塗り) | sub-10 | 9, 16 | M99k: デバッグの線とナビの線を定数どおりの色で描く |
| sub-12 | 画面サイズ・距離カリングの撮影の決定性を回帰に入れる | sub-11 | 12, 17 | M99l: 画面サイズ・距離カリングの撮影が実行ごとに一致することを cull_verify で確かめる |

(sub-01 の計測で BVH は採用条件を満たさず見送り。BVH のサブは足さない。)

## 7. 未決事項・リスク

- U1〜U5 は 2026-11-11 にユーザーが確定済み (2. の D5 / D8 / D9 / D12 / D16。どれも planner の裁定どおり)。仮置きだった値 (画面サイズは既定 OFF、閾値 0.005、ヒステリシス 10%、距離にヒステリシスなし) も同日に確定。残る未決は下の実装リスクだけ。
- sub-03 で正しく直った局所影の結果、既存 golden が動く可能性 (今の 32 枚に影付き局所光 + 画面外キャスターがあれば)。動いたら視錐台 ON/OFF の全画素一致を根拠に、差分の場所と理由を SELF_EVAL に書く。
- 視錐台 OFF の全画素一致が、TAA / モーションブラーの履歴の差で崩れる可能性。4.1.2 のとおり `prevRender` の記録規則を ON と揃える。崩れたら原因を特定して報告 (受け入れ条件を緩めない)。
- GPU パーティクルの解析的上界が、衝突・`stretchMax`・`prewarmTime`・ワールド空間の原点移動で外れる可能性。外れる条件のエミッタは「常に描く」側へ倒す (落としすぎより描きすぎ)。
- 2D の描画順: 落とした項目を抜いても残りの順序キーが変わらないこと (ソートが安定でないと絵が変わる)。
- sub-11 で線パス全体の描き方を変えるので、線を含む golden (`nav.png` ほか `perception.png`、`bt.png`、`physics.png`、`joints.png`、`acoustic_*.png` 等のうち線が写る物) が動く。更新は差分が線と塗りの画素に限られることを確かめてからにする (sub-11 の手順)。深度テストのある線を LDR の後へ移すと、深度バッファと最終目標の寸法・ジッタの食い違いが起きうる。食い違う構成は sub-11 で扱いを決め、文書に書く。
- 既知の制限 (M99 では直さない。sub-03 round 1 で判明、どちらも視錐台 ON/OFF で同じ挙動): (a) 画面内に不透明の物が 1 つも無い (`hasScene == false`) フレームは局所影アトラスを作らないので、画面外キャスターの影も出ない。(b) `ShadowAtlas::Render` は各物をメッシュの bind 姿勢の AABB でタイル判定するので、スキンの姿勢が bind の箱の外へ出た部分は局所影で欠けうる (画面内のスキンも同じ、M90 からの挙動)。画面外キャスターの選別に使う全姿勢の箱は bind 姿勢を含む (engine_spec §6.14) ので、選別で落ちてアトラスが描くはずの物を失うことはない。どちらも sub-09 で engine_spec §6.14 の制限に書く。
- `flow_title` の shot_verify FAIL (diffPixels 944、画面下の操作説明の文字行): sub-09 で基点 `d52e91f3` の exe でも同じ差 (maxDiff 62、worst pixel (278,469)) と確認。**M99 と無関係の既存 FAIL**。golden は更新しない。
- `--selftest-full` の `disturbancelong` FAIL (tick 600 の hash が焼いた値と違う): sub-09 で基点 `d52e91f3` でも同じ hash で FAIL と確認。**M99 と無関係の既存 FAIL** (M96 のスイート)。受け入れ条件 14 の「`--selftest-full` PASS」はこの 1 スイートを除いて満たす。
- `Runtime.exe --screenshot` を `--frames` 無しで使うと終了しない: 基点でも同じ既存の挙動 (EngineLoop は maxFrames > 0 でしかループを抜けない)。M99 では変えない。
- CI への `tools\cull_verify.bat` の組み込み: planner は「CI が緑に戻ってから」と裁定したが、**2026-11-11 にユーザーが「今載せる」と判断** (sub-10)。リスク: master の CI は M98e 以降ビルド段で failure のままなので、載せた step が CI 上で実際に PASS するかは CI が緑に戻るまで観測できない。sub-10 はローカルで確かめられる範囲 (YAML の構文、CI と同じ引数・環境変数・シェルでの PASS、失敗の終了コードの伝わり方) を受け入れ条件にし、CI 上の実走は未検証として残す。

## 8. 変更履歴

- 2026-11-11 (ユーザー): U1〜U5 と仮置きの値を planner の裁定どおり確定。仕様の中身は変更なし。
- 2026-11-11 (sub-01 round 1 の結果): BVH は採用条件を満たさず見送り (ADR-038 の計測節)。3. の後回しと 6. の注記を更新。サブ構成は sub-02〜09 のまま。
- 2026-11-11 (sub-02 round 1、coder の質問への回答): `--no-size-culling` と `--cull-min-screen-size` の併記は OFF が勝つ規則を 4.1.1 に追記。視錐台 ON/OFF の全画素一致は RenderSystem を回す selftest の先例が無いため、Runtime.exe の撮影 + img-diff (tol 0) を回すスクリプト `tools\cull_verify.bat` を sub-09 で作り、受け入れ条件 14 に足した。
- 2026-11-11 (sub-03 round 1): 既知の制限 2 件と `flow_title` の切り分けを 7. に追記。sub-09 の作業に反映。
- 2026-11-11 (sub-04 round 1、coder の逸脱・追加を仕様側の誤りとして採用): SpriteRenderer の半径を max(1, スケール) に、GPU エミッタの「上界を持てない」条件に衝突 2 種を、安定待ちの基準を「原点の移動」から「上界の変化」に変えた (4.1.5)。
- 2026-11-11 (sub-05 round 1): 2D の判定位置・余裕・SpriteShape の張り出しを実装に合わせて 4.1.5 に記録 (縁スプライトの厚みは帯の厚みに使われていなかった = 仕様の誤り)。`culledNonMesh` の数え方を「メッシュ以外の視錐台による落とし全部 (CPU プール・タイルチャンク込み、地形除く)」に確定。sub-04 で数えていない CPU プールは sub-08 で足す。
- 2026-11-11 (sub-06 round 1、coder の質問を受けて planner の誤りとして修正): タグを自分の mask だけで引くとした sub-06 本文は、`Tags.h` の「描画は EffectiveMask」という既存の規則に反していた。タグは EffectiveMask、`RenderCulling` は自分 → 祖先の最初の 1 個 (RtScope と同じ) に変更 (4.1.3 / 4.2)。sub-07 の除外フラグも同じ解決を使う。
- 2026-11-11 (sub-07 round 1): 画面サイズの履歴は理由の計上と独立に全候補へ書くこと、閾値 0 以下は判定しないことを 4.1.4 に記録。
- 2026-11-11 (sub-08 round 1): 固定の控えを取る時点、枠の上限の共有、固定が非メッシュ経路にも効くことを 4.1.7 に記録。
- 2026-11-11 (sub-09 round 1): 既存 FAIL 2 件 (flow_title、disturbancelong) は基点でも同じと確認し、受け入れ条件 8 / 14 の例外として 7. に記録。`--frames` 無しの撮影が終わらない件は既存挙動。cull_verify の CI 組み込みは見送り (CI が赤のため)。
- 2026-11-11 (ユーザー): sub-09 の `[ユーザーに聞ける]`「cull_verify を CI に載せる時期」に「今載せる」と回答 (planner の裁定の逆)。3. のやる・5. の受け入れ条件 15・6. の sub-10・7. のリスクを更新。実装はレビュー round 1 の結果が出てから、修正のサブと一緒に回す。
- 2026-11-11 (reviewer round 1 #1): 4.1.4 の「サイズ変更で履歴を捨てる」は実装されておらず、必要もない (比率なのでリサイズで値が変わらない) ので削除。engine_spec.md:1082 の同じ記述は sub-10 で直す。(reviewer round 1 #3): sub-10 の「cull_verify に --no-audio が無い」は planner の読み違い (`tools\cull_verify.bat:50` の `COMMON` に付いている)。sub-10 を「付いていることを確かめる」に直した。
- 2026-11-11 (ユーザー、reviewer round 1 #2 = round 2 #1): デバッグ枠の色が定数どおりに出ない件を「直す」と判断。sub-11 と受け入れ条件 16 を追加。原因 (線を HDR 中間へ描いてトーンマップ等を通す、または sRGB / リニアの取り違え) は推定で、sub-11 で切り分けてから直す。範囲はカリングのデバッグ表示 (M99 の枠・固定した視錐台の線・M90 の `--hzb-debug-max` の赤い箱) だけ。同じ線パスを使うスクリプトの `DebugDrawLine` とナビメッシュの線は変えない (`nav.png` の golden とユーザーのスクリプトの見た目が動くため)。
- 2026-11-11 (ユーザー): sub-11 の `[ユーザーに聞ける]`「DebugDrawLine とナビメッシュの線も直すか」に「一緒に直す」と回答 (planner の裁定「カリングの枠だけ」の逆)。sub-11 の対象を、RenderSystem の線パスを通る物すべてと、同じシェーダと色の約束を持つナビメッシュの塗り (`NavFillPass`) に広げた。受け入れ条件 16・6.・7. を更新。golden の更新は、差分が線の画素に限られることを確かめてから行う。エディタ側の `EditorLinePass` / `GhostMeshPass` は別インスタンスなので範囲外 (問題があれば報告だけ)。
- 2026-11-11 (sub-11 round 1): 受け入れ条件 16 の判定式を「差 ≤ 8 の画素数 ≥ 20」から「α 混合で説明できる割合 ≥ 95%」へ変更 (planner の式の誤り。1 px の AA 線では定数色そのものの画素がほぼ出ない)。撮影の組み合わせの誤り (距離デモでは距離が先に当たって緑が出ない、固定 + カットの frame 30 には赤が出ない) も訂正。coder が観測した「画面サイズカリング + 枠表示の撮影が実行ごとに違う」件は M99 の決定性に関わりうるので、受け入れ条件 17 と sub-12 を追加して切り分ける。
- 2026-11-11 (sub-11 round 2): 「画面サイズ + 枠表示の撮影が実行ごとに違う」は coder の測り違いだった (`--warp` の同じ条件で差 0)。sub-12 を切り分けから「run 間一致を cull_verify の回帰に入れる」に縮小し、受け入れ条件 17 を書き直した。理由は、これまで決定性を `counts` の一致でしか見ておらず、画素の run 間一致は見ていなかったため。
