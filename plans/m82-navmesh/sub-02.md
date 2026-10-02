# sub-02: NavMeshSurface とベイク (.mnav) + 輪郭のデバッグ描画

- 依存: sub-01
- 状態: 未着手
- 往復: 0

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

## フィードバック履歴
