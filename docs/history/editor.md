# エディタ (src/Editor、SourceControl 以外) の経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## AssetOps.cpp / AssetOps.h — C++ スクリプトのビルドの起動口
- (M66e) 可視の cmd 窓で bat を投げる `RebuildGameLogic` を削除した。fire-and-forget ではプロセスハンドルが誰の手にも残らず、走っている間 `GateBlocker::ScriptBuildRunning` が立たない = ビルドが bin\ と cache\ を書いている最中に checkout / pull が通ってしまう (spec §7 の穴)。起動口はハンドルを返す `StartGameLogicBuild` 1 本にした。
- (M66e) Asset Browser の [Rebuild Scripts] も ShellExecuteW の fire-and-forget で観測できず、その間だけゲートに穴が開いていた。EditorApp がハンドルを持ち、`GateInputs::scriptBuildRunning` に OR する形にした。
- (M66e → M66h) 旧経路は可視の cmd 窓 + pause で、コンパイルエラーをその場で読めた。M66e で窓を消した分を、M66h で build_scripts.log の error 行を Console へ流す形で戻した (`ParseBuildErrorLines` / `EditorApp::ReportScriptBuildErrors`)。生成する bat の失敗系 `pause` は窓があった頃のもので、今は stdin=NUL ですぐ抜ける。
- (2026-09-04) `StartGameLogicBuild` の呼び出し元が Build Settings の Stage::Scripts と EditorApp の 2 経路だけであることを全文検索で再確認した。

## AssetOps.cpp — ScriptsRoot
- 旧 `RepoRoot()` は常に「assets\ の親」を返していたため、プロジェクト起動時に <project>\src\GameLogic\Scripts へ書いた上で <project>\tools\build_scripts.bat を探しに行き、ビルドできないまま無言で失敗していた。プロジェクト起動では <project> を返すようにした。

## AssetOps.cpp — SanitizeFileName
- (M50b) 許可リスト (英数のみ) から禁止リスト (\/:*?"<>| + 制御文字) へ緩めた。非 ASCII (日本語名) のアセット名を通すため。

## AssetOpsSelfTest.cpp — 反射クラスの受理規則
- (M67 → M67h) M67 は「非整数」を JSON の型で弾いていたので、jq / Python / 手編集が書いた `3.0` を黙って 4 に落としていた (静かなデータ損失。review-1 minor 5)。M67h で値で判定するようにした。

## AssetOpsSelfTest.cpp — Content Browser のタイルラベル
- タイルラベルを手書きの判定から `AssetDatabase::ClassifyPath` 1 本に寄せた。小文字のサフィックスは同じラベルのまま。大文字を含むサフィックスだけ変わった (以前は Orc.Actor.json が "prefab"、Walk.ANIM.json が "json"、Red.MAT.JSON が "model")。

## AssetPreviewCache.cpp — Quad の法線
- `Quad()` の法線は -Z (実装が正)。GpuResources.h の宣言コメントが法線を誤記していた時期があり、プレビュー側のコメントに「宣言コメントは誤記」と注記していた。

## DiskCompare.h — ファイル名
- (M66h) DocumentDirty.h から改名した。`DocumentDirty` は GitTransaction.h 側にある別の型 (ゲートが集める未保存の集計) で、同じ名前のファイルが隣にあると「この関数はあの構造体を作るのだろう」と読まれてしまうため。

## EditorApp.cpp — 再生中の保存を止める (BlockSaveWhilePlaying)
- (三校 2026-09-13) タイトルを Play → ステージ 1 へ遷移 → 保存で title.scene.json がステージ 1 の途中状態に上書きされ、起動するとタイトルを飛ばしてステージ 1 が始まった。再生中の保存を止めた理由。

## EditorApp.cpp — Rendering メニューの影
- (M40d → M54e) M40d までは Rendering メニューの「影」1 個で全部を切っていた。M54e で局所ライトのアトラス (4096^2 = 64MB + タイル数ぶんの深度パス) が別勘定のコストとして増えたので、影サブメニューに平行光 / 局所ライトを分け、旧トグルはそこへ移した。

## EditorApp.cpp — ReSTIR メニュー
- (M67 → M67h) M67 はトグルを直接読んで子を無効表示にしていたため、デバッグ 12 / 14 で RenderSystem 側が ReSTIR を強制する条件を知らず「絵は出ているのにスライダが灰色」だった。M67h で `RenderSystem::RtRestirEffective()` 1 本にした。既定値の確定も M67h で決着 (ADR-016「S5 の結論」)。

## EditorApp.cpp — プローブのプレビュー窓
- (M56e) 焼いた 6 面のサムネイルを Inspector ではなく専用の小窓にしたのは、M56e の時点では反射プローブのコンポーネントが無く (M56f で入った)、どのエンティティのインスペクタに出すかが決まらなかったため。

## EditorApp.cpp / EditorToolbar.cpp — メニューバーとドックレイアウト
- (M27c) Play / Pause / Step をメニューバー中央から EditorToolbar へ移した。
- (M66c → M66e) Source Control 窓は下段帯に置いていたが、高さ ≒ 200px では変更一覧が 2 行で切れ、コミット欄が窓の外へ落ちた (M66c で実測)。M66e で左列 (Hierarchy と同じ束) へ移した。束へ窓を足しただけで起動時に Source Control が前に出たので、束の既定タブを明示するようにした。

## EditorSettings.h — パーティクルの調査用トグル
- (M66h) 以前は assets\project_settings.json (チームで共有され、git に載る) にあり、比較モードを 1 回入れただけで「全員の画面に粒子が 2 重に出る」差分が push できてしまう状態だった。個人設定の editor_settings.json へ移した。

## ShortcutHub.h — リバインド
- (M8) 既定割り当て固定で作り、M10 で editor_settings 経由のリバインドに拡張する予定だったが、リバインドは入っていない。

## StatusBar.cpp — 右側の情報文字列
- (M47b) 旧実装は char[256] 固定で、日本語のプロジェクト名/シーン名だと溢れていた。

## Windows/AssetBrowserWindow.cpp / .h — ダブルクリック
- (M13 → M48k) M13 では .prefab.json のダブルクリックでシーンへインスタンス化していた。M48k でダブルクリックを「ミニシーンで編集」に変え、配置は右クリックメニューの「シーンに配置」(`InstantiateComposeAsset` として切り出し、挙動は M48b 以降と同一) と D&D に残した。

## Windows/BuildSettingsWindow.cpp — .NET ランタイム同梱
- (M16-M25 → 2026-09-13) `fs::copy(recursive)` の前に親 (dotnet\ 等) を create_directories していなかったため、ERROR_PATH_NOT_FOUND で 2 本とも失敗し、追加時から 2026-09-13 まで dotnet\ が 1 つも同梱されていなかった。

## Windows/BuildSettingsWindow.cpp — 封印キャッシュ
- (M51j → M74a) M51j 当時はサブアセット AssetID が絶対パス由来で、封印が参照の正しさそのものだった。M74a で .meta の GUID 由来になり ID は移設で変わらなくなった。封印は再パースの回避と DDS 後の元画像不在のために残している。

## Windows/InspectorWindow.cpp — Collider.shape のコンボ
- (M60f) それまでコンボが 3 件しか出さず、3 (Mesh) / 4 (Terrain) / 5 (Convex) は "(invalid)" 表示になり、シーン JSON を手で書く以外に選ぶ手段が無かった (直したのはエディタ表示のみ)。ラベル件数を static_assert で番号の表と揃えるようにした。

## Windows/InspectorWindow.cpp — AssetRef ピッカーの推定
- フィールド名を素のまま照合していたため、"cubemapTexture" / "lutTexture" / "normalTex" が "tex" に一致せず総当たり一覧へ落ちていた。小文字化する前は "physMaterial" の find("material") が npos で、代わりに混合リストへ落ちていた (小文字化したので physmat を先に見る順序が要るようになった)。

## Windows/ParticleSettingsWindow.cpp — バックエンド選択
- (M66h) 以前は窓でバックエンドを触った瞬間に共有ファイル (project_settings.json) へ書いていたため、GPU の絵を 1 度見ただけでチーム全員のバックエンドが変わる差分が commit 待ちになっていた。M66h でこのセッションだけの切替にし、プロジェクト既定は Project Settings 窓の「プロジェクト既定にする」で書くようにした。

## Windows/SceneViewWindow.cpp — 射影
- (M55b) それまで RenderSystem は fovYDeg から透視を組み直していたので、Ortho トグルがオーバーレイ/ギズモ/ピッキングにしか効かず、絵は常に透視のまま = 3 者が食い違っていた。射影を SceneView の 1 箇所で組むようにした。

## Windows/SceneViewWindow.cpp — ツールバー
- ツールバーはかつて高さ 30px 固定 + "|" テキスト区切り + ハードコード青だったが、テーマの余白変更で中身が縦にはみ出す事故を起こしたので、EditorWidgets の統一規格 (サイズはフレーム高から導出) へ寄せた。
- (M47b) 幅 830px 固定では訳文が長いとボタンが見切れたので、中身から自動決定にした。(M47b追補) それは「中身 vs 器」の解決で、パネルが器より狭いと右端が親にクリップされて操作不能のままだったので、区切り単位の折り返し (ToolbarFlow) を足した。
## PartSelfTest.cpp — SkinnedModelLibrary の名前
- (M18) M18 の積み残しで SkinnedModelLibrary だけ names_ を持たず Enumerate も無かったため、Inspector の AssetRef ピッカーは SkinnedMesh.model の候補を 1 件も作れず、メッシュ + マテリアル + テクスチャの混合リストへ落ちていた (正解が出ないうえ、選ぶと参照が壊れる)。

## TerrainSelfTest.cpp — LOD 境界のクラック検査
- 旧 MakeTestTerrain は、地形の高さが縁に沿って 1 次式になっている fixture だった。「> 0」の判定だと丸め誤差だけの 1e-5 が通り、クラック検査が何も検査しないまま緑になっていたので、1 m 以上の食い違いを要求するようにした。
