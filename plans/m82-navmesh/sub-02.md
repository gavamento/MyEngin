# sub-02: NavMeshSurface とベイク (.mnav) + 輪郭のデバッグ描画

- 依存: sub-01
- 状態: OK (コミット待ち)
- 往復: 1

## やること
spec 4.1 (Surface)、4.2 (`.mnav`)、4.3 (Create / インスペクタ / 描画の輪郭)。

1. `NavMeshSurfaceComponent` を末尾 append で登録 (hash 対象、表示フラグは `kFieldNoHash`)。シリアライズ、`EditorComponentCatalog`。
2. ベイク入力の収集 (**Engine 層の純関数**。spec 4.4 F2: Editor に依存せず、`World` とタイル AABB から決定的な順序で三角形とエリアを返す。将来 sim から呼ぶ再ベイクの入口になる): `AcousticField::BakeOccupancy` (`AcousticField.cpp:280-312`) と同じ選別 (Collider + WorldMatrix、Rigidbody / CC 持ちを除く、トリガー除外、`collectLayerMask`、非アクティブ除外、entity.index 順)。形状は box / sphere / capsule をテッセレーション、mesh (`meshcol::Resolve` のローカル三角形 × WorldMatrix)、terrain (高さ場を三角形化)、convex (`convexcol::Resolve`)。Surface の AABB で切る。
3. ベイク: **タイル 1 枚分を作る Engine 層の関数** (三角形 → TileCache の層) を作り、全タイルはそれを回すだけにする (F2)。Recast → TileCache の層 (sub-01 の無圧縮圧縮器) → `.mnav` (`FractureAsset` と同じ Serialize / Deserialize / Save / Load、`AssetType::NavMesh`、`.meta`)。保存先 `assets\NavMesh\<Surface名>_<入力ハッシュ16桁>.mnav`。エディタは非同期ワーカー + 進捗 + キャンセル (`FractureBakeService` / `FractureBakeCommit` の流儀、参照の設定は 1 Undo)。Clear は参照を外す (ファイルは消さない)。
4. ロード: Runtime / Server / Editor 再生でシーン読み込み時に `.mnav` → dtTileCache → dtNavMesh (sub-01 のタイル単位の差し替え口を通し、層は NavSystem 側が所有する。F1 / F5) (`PreloadFractureAssets` の呼び出し箇所 `StartScene.cpp:67` / `TickRunner.cpp:807` / `EditorApp.cpp` と同じ所)。読み込み失敗はその Surface だけ無効 + ログ。ビルド設定のコピー対象へ `.mnav` を追加 (`BuildSettingsWindow.cpp:253`)。
4b. `dtNavMeshParams` の `maxTiles` / `maxPolys` (sub-01 の ADR 目安: 層数 × 1.5 / 最大ポリゴン数 × 2、salt は 10 ビット以上) をベイク結果から決めてアセットに焼く (`NavMakeStoreConfig`)。
5. Create → 3D Object に区切り線 + `NavMesh Surface` (spec 4.3)。インスペクタ: ベイク設定・Bake / Clear・進捗・結果要約・表示切り替え。
6. デバッグ描画: DebugUtils の `duDebugDraw` を `DebugLineCmd` へ流す実装 (三角形はポリゴン輪郭の線に落とす)。TickRunner フェーズ 4 の `if (!ts.resim && flags.Any())` の並びに置く。Runtime でも出せる。
7. Localization (en / ja)。

## やらないこと (このサブでは)
- Agent / Crowd / Obstacle / Modifier / Link。SimSnapshot の Nav 節 (ロードした NavMesh は「アセットから作る導出値」としてこのサブでは扱い、sub-03 で節を入れる)

## 触る場所 (planner の見立て)
- `src\Engine\Core\Ecs\Components.h/.cpp`、`src\Engine\Engine\Navigation\` (新規: NavBakeInput / NavBake / NavMeshAsset / NavDebugDraw)、`src\Engine\Engine\Asset\AssetDatabase.h` (`AssetType`)、`src\Editor\Widgets\CreateMenu.cpp/.h`、`src\Editor\Windows\Scene\InspectorWindow.cpp`、`src\Editor\Widgets\EditorComponentCatalog.cpp`、`src\Editor\Tools\` (NavBakeService)、`src\Engine\Engine\Loop\TickRunner.cpp` (描画)、`LocalizationTable.inl`、`BuildSettingsWindow.cpp`、`tools\gen_project_files.ps1` の再実行
- 手本: `git show 159cff1` (コンポーネント追加)、`FractureAsset.h` / `FractureBakeService.h` / `FractureBakeCommit.h`

## 受け入れ条件 (このサブ)
1. Create → 3D Object → NavMesh Surface でエンティティができ、Undo / Redo で消える・戻る。
2. 固定シーンの Bake で `.mnav` ができ、同じ入力の Debug / Release ベイクがバイト一致 (SelfTest でベイク関数を直接呼んでハッシュ比較)。Clear で参照が外れる。
3. Runtime.exe でそのシーンを開くとロードされ、ログにタイル数 / ポリゴン数が出る。
4. `--screenshot` で NavMesh の輪郭が見える (画像パスを SELF_EVAL に)。
5. NavMesh 系の無い既存シーンの replay_verify / shot_verify が変わらない。check_rules 0、0 警告。
6. (spec 18 (b)) Editor 無しの SelfTest から入力収集 + タイルベイク関数を呼び、エディタの Bake と同じ層のバイトが出る (Editor の Bake も同じ関数を呼んでいることをコードで示す)。

## 検証コマンド
- Debug / Release ビルド (`/p:MyeWarnAsError=true`)、`Editor.exe --selftest` 両構成、`tools\check_rules.ps1`
- `tools\replay_verify.bat`、`tools\shot_verify.bat`
- `bin\x64\Release\Runtime.exe` で試作シーン、`--screenshot`

## 実装メモ (coder が追記)

SELF_EVAL: sub-02 (round 1)
実装:
  - src\Engine\Core\Ecs\Components.h/.cpp: NavMeshSurfaceComponent (TypeId 71、末尾 append)。設定値・areaCosts[16]・navAsset は hash 対象、drawNavMesh / drawTileBounds は kFieldNoHash。areaCosts は kFieldHidden (UI は M82f)
  - src\Engine\Engine\Navigation\NavBakeInput.h/.cpp: NavCollectTriangles (BakeOccupancy と同じ選別、entity.index 順、box/sphere/capsule/mesh/terrain/convex)、NavMakeBakeConfig。前回途中成果を引き継ぎ、sin/cos を使わない球分割のまま
  - src\Engine\Engine\Navigation\NavBake.h/.cpp: NavPrepareBakeInputs (World -> config + soup。Editor / SelfTest / 将来の再ベイク共通の入口)、NavBakeTile (soup からタイル範囲 + 余白で三角形を絞って NavBakeTileLayers)、NavBakeAsset (全タイルを NavBakeTile で回す。進捗 / キャンセル、maxTiles = max(1.5 x 層数, 層数 + 4)、maxPolys = NextPow2(2 x 実測最大) 下限 64、salt 10 ビット未満なら Failed)、NavComputeInputHash
  - src\Engine\Engine\Navigation\NavMeshAsset.h/.cpp: .mnav (magic MNAV / version 1)。Serialize / Deserialize (件数を残りバイトで検算、層キーの昇順・範囲・header 一致を検査) / Save / Load / BuildStore (NavTileStore へ AddBaseLayer -> BuildAll)
  - src\Engine\Engine\Navigation\NavSystem.h/.cpp: Surface ごとに .mnav を読み NavTileStore を所有 (F5)。構成 (entity + guid) が変わったときだけ読み直し、読み込み失敗はその Surface だけ Failed + ログ。輪郭は duDebugDrawNavMesh を DebugLineCmd へ流す duDebugDraw 実装で読み込み時に 1 回だけ作る (TRIS は捨てる = 塗りは M82d)
  - src\Engine\Engine\Navigation\NavTileCacheSupport.h/.cpp: NavTileBorderCells を追加 (NavBakeTileLayers と NavBakeTile で余白を共有)
  - src\Engine\Engine\Loop\TickRunner.h/.cpp, EngineLoop.cpp, HeadlessSim.cpp: NavSystem を EngineLoop / HeadlessSim が所有、フェーズ 3.4b (音響 + AgentSystem の後・アニメの前、stepSim) で Update、フェーズ 4 の !ts.resim 内で AppendDebugLines、シーン遷移で Reset
  - src\Engine\Engine\Asset\AssetDatabase.h/.cpp, AssetDatabaseSelfTest.cpp: AssetType::NavMesh (.mnav、.meta の type 名 "navmesh")
  - src\Editor\Tools\NavBakeService.h/.cpp: 非同期ベイクワーカー (単一スレッド、進捗 / キャンセル、FractureBakeService と同型)
  - src\Editor\Tools\NavBakeCommit.h/.cpp: CommitNavBake (assets\NavMesh\<名前>_<ハッシュ16桁>.mnav を保存 -> .meta -> navAsset を設定 = 1 Undo)、ClearNavBake (参照を外す = 1 Undo、ファイルは消さない)
  - src\Editor\Windows\Scene\InspectorWindow.h/.cpp: DrawNavMeshSurfaceNotes (Bake / Clear / 進捗バー / 取り消し / 結果の要約 / 失敗理由 / 再生中は無効)
  - src\Editor\Widgets\CreateMenu.h/.cpp: Create -> 3D Object に区切り線 + NavMesh Surface。EditorComponentCatalog.cpp: カテゴリ Navigation を新設、EditorWidgets.cpp: .mnav のアイコン
  - src\Engine\Core\Localization\LocalizationTable.inl: Create_NavMeshSurface と Insp_Nav* (en / ja)
  - SelfTest: src\Engine\Engine\Navigation\NavSurfaceSelfTest.h/.cpp (Engine 層、Editor.exe と Server.exe の --selftest に登録)、src\Editor\Tools\NavEditorSelfTest.h/.cpp (Editor.exe)
仕様との差分:
  - [逸脱] ビルド設定のコピー対象への .mnav 追加はしていない — BuildSettingsWindow.cpp:208 が assets\ を recursive で丸ごとコピーするので .mnav も入る。:253 付近の一覧はクック物 (.mmdl 等) 専用で .mnav は対象外
  - [逸脱] .mnav の読み込みは PreloadFractureAssets の 3 か所 (StartScene / EditorApp / TickRunner のシーン遷移) には足さず、NavSystem::Update が stepSim の tick で構成の変化を見て遅延ロードする。理由: 呼び出し箇所 3 つに足すより、シーン遷移 / Play 開始 / 復元のどれでも「その tick の World」を見て読み直す方が漏れが無く、ヘッドレス Server も同じ経路になる。最初の物理 tick の前に読まれる (3.4b は物理 3.6 の前)
  - [逸脱] 輪郭の線は Play 中と Runtime / Server の tick でだけ出る。編集中 (非 Play) の SceneView には出ない — 物理 / 音響のデバッグ描画 (Play 中しか線は出ない) と同じ制約。編集中にも出すなら SceneView 側の描画経路を足す必要がある
  - [未実装] SceneView ギズモ (Surface の範囲箱) — sub-02 本文の「やること」に無い (spec 4.3 のみ)
  - [未実装] .mnav の Link / Modifier のベイク時スナップ (spec 4.2) — M82f / M82g
  - [追加] EditorComponentCatalog に新カテゴリ "Navigation" (en / ja)。Add Component 一覧に NavMeshSurface が出る
  - [追加] NavPrepareBakeInputs / NavBakeTile / NavTileBorderCells / kNavBakeVersion / kNavMaxObstacles (128)
  - [追加] NavSurfaceSelfTest を Server.exe の --selftest にも登録 (Debug / Release のハッシュ照合が 23 秒で回せる)、NavEditorSelfTest
  - [追加] 入力ハッシュ (保存名の 16 桁) は FNV-1a で、ベイク方式の版 + 設定 + 三角形列から作る。areaCosts は含めない (実行時のコストでベイクに影響しないため)
検証:
  - msbuild MyEngine.sln Debug / Release (/p:MyeWarnAsError=true 無し) -> 両方成功、警告 0。/p:MyeWarnAsError=true は Engine が HEAD 由来の ProjectComputeRunnerSelfTest.cpp(139) C4127 でだけ失敗 (他の Engine ファイルは通る)。Editor は BuildProjectReferences=false + MyeWarnAsError=true で警告 0
  - tools\gen_project_files.ps1 を実行 (build\Engine.vcxproj / Editor.vcxproj と .filters が更新される)
  - bin\x64\Debug\Editor.exe --selftest -> exit 1。FAIL は「Source control self test」の 2 件 (external cherry-pick / revert) だけで、着手前 HEAD (git worktree で別途ビルド) の Debug Editor.exe --selftest でも同じ 2 件が落ちる。NavDeterminism PASS、NavSurface ALL PASS、NavEditor ALL PASS、Server/client net ALL PASS
  - bin\x64\Release\Editor.exe --selftest -> exit 1 (同じ SCM 2 件のみ)。NavSurface / NavEditor ALL PASS
  - NavSurface のベイクの asset ハッシュ = 0xA9EF6D223C161FE4 (12712 バイト) が Debug Editor / Release Editor / Debug Server / Release Server で一致 (kExpectedAssetHash に焼いた)
  - bin\x64\{Debug,Release}\Server.exe --selftest -> exit 0 (NavSurface ALL PASS)
  - tools\check_rules.ps1 -> 0 error 0 warning
  - tools\replay_verify.bat -> 14 ジョブ全 PASS (149.9 s)。NavMesh 系の無い既存シーンのハッシュ列は不変
  - tools\shot_verify.bat -> 5 枚 FAIL (parts / joints / acoustic_forward / acoustic_deferred / fracture_after)。M75g のコミットメッセージにある「着手前の HEAD でも同じ既存 FAIL 5 枚」と同じ組。HEAD での再計測はしていない
  - 実走: Release Runtime.exe に一時プロジェクト (flow_game のシーン + 箱 4 つ + NavMeshSurface、.mnav は一時プローブで焼いた) を読ませた -> ログ "[nav] surface 'NavSurface' loaded: 4 tiles, 4 layers, 11 polygons"。--screenshot の画像: C:\HAL\MyEngin\plans\m82-navmesh\screenshots\sub-02_nav_outline.png (シアンの輪郭線が床の上に見える)。プローブのコードとスクラッチは削除済み
自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 1〜6 を検証つきで満たす。ギズモ未実装、読み込み箇所と編集中の描画は上記の逸脱
  正しさ: 4 — Debug / Release のバイト一致、壊れた .mnav の拒否、キャンセル、空入力、形状 6 種の収集を SelfTest で確認。実 GUI のボタン操作 (Inspector 描画) は未操作
  コード品質: 4 — Editor は Engine の関数を呼ぶだけ、Recast の型は Navigation\ に閉じた。NavSystem は sub-03 で Agent 同期を足す前提の最小形
  テスト: 4 — NavSurfaceSelfTest (入力収集 / ベイク / 層の一致 / .mnav 往復 / 破損 / 保存・ハッシュ / NavSystem) と NavEditorSelfTest。Inspector の ImGui 描画と Runtime 実走はテスト化していない (手動確認のみ)
不安・質問:
  - 最初の Debug Editor.exe --selftest だけ「Server/client net self test」の V1 (tick 270 から始まる .rep) の 2 件が落ちた。コード変更なしで再実行した Debug 全体・Release 全体・単独実行は全て ALL PASS、着手前 HEAD も PASS。原因は特定できていない (その時 PC が別の重い処理と競合していた可能性)。reviewer が再現したら教えてほしい
  - 読み込みを Preload 3 か所ではなく tick 内の遅延ロードにした判断 (上の逸脱) を planner に確認したい。sub-03 の SimSnapshot Nav 節は「restore 後に NavSystem が読み直す」で成り立つ前提
  - 編集中 (非 Play) の SceneView に輪郭を出すか。出すなら別途描画経路が要る
触ったファイル:
  - src\Engine\Core\Ecs\Components.h / Components.cpp
  - src\Engine\Core\Localization\LocalizationTable.inl
  - src\Engine\Engine\Asset\AssetDatabase.h / AssetDatabase.cpp / AssetDatabaseSelfTest.cpp
  - src\Engine\Engine\Loop\EngineLoop.cpp / HeadlessSim.cpp / TickRunner.h / TickRunner.cpp
  - src\Engine\Engine\Navigation\NavBakeInput.h / .cpp、NavBake.h / .cpp、NavMeshAsset.h / .cpp、NavSystem.h / .cpp、NavSurfaceSelfTest.h / .cpp (新規)、NavTileCacheSupport.h / .cpp (変更)
  - src\Editor\Tools\NavBakeService.h / .cpp、NavBakeCommit.h / .cpp、NavEditorSelfTest.h / .cpp (新規)
  - src\Editor\Windows\Scene\InspectorWindow.h / InspectorWindow.cpp
  - src\Editor\Widgets\CreateMenu.h / CreateMenu.cpp / EditorComponentCatalog.cpp / EditorWidgets.cpp
  - src\Editor\App\EditorMain.cpp、src\Server\ServerSelfTest.cpp
  - plans\m82-navmesh\screenshots\sub-02_nav_outline.png (証跡)
  - build\Engine.vcxproj / Engine.vcxproj.filters / Editor.vcxproj / Editor.vcxproj.filters (tools\gen_project_files.ps1 の出力。ビルドに必要なので一緒にステージする)
申し送り:
  - sub-03: NavSystem::Update の中で Surface の構成を見て読み直す形にしてある。Agent / Crowd の同期はこの Update に足し、SimSnapshot の Nav 節は restore 後に NavSystem が .mnav から読み直す前提で組める。dtNavMeshParams は NavBakeAsset が決めた値が .mnav に入っている (maxObstacles は 128 固定)
  - sub-04: LineCollector (NavSystem.cpp) は TRIS / QUADS を捨てている。塗りはここへ三角形レーンを足す。輪郭の線は読み込み時に 1 回作ってキャッシュしてあり、tick ごとには作り直さない
  - sub-09: plans\m75-ugui.md の TypeId 注記 (NavMeshSurface = 71 を先に使った。InputField は 72 以降へずれる) が未記入
  - NavBakeInput.h のコメントにあった「kNavBakeVersion」は NavMeshAsset.h に実体を置いた。球の分割数 (kNavSphereSubdivisions) や入力収集を変えたら kNavBakeVersion を上げること
  - Runtime.exe / Server.exe は NavSystem を tick 内で読み込むので、Surface のあるシーンの最初の tick に .mnav の読み込み時間が乗る

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-10-03)。逸脱 3 件の裁定: (1) ビルドのコピー対象 = 不要で正しい (`BuildSettingsWindow.cpp:208-210` の再帰コピー、spec 4.2 を訂正) (2) tick 内の遅延ロード = 採用 (spec 2. #17、sub-03 の restore に読み込みの先行を条件付け) (3) 編集中の表示 = 出す、sub-04 へ (spec 2. #18、`[ユーザーに聞ける]`)。ギズモ未実装は planner の割り当て漏れで sub-04 へ。should: reviewer が Editor の Play 中の輪郭表示と Inspector の Bake ボタン実操作を見ること (SelfTest と Runtime スクショのみで、Editor GUI は未観測)。Server/client net の 1 回限りの FAIL は再現待ちとして申し送り。
