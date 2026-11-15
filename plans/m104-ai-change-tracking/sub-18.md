# sub-18: Before / After のプレビューと左右の 3D 比較 (M104s)

- 依存: sub-17
- 状態: OK (commit は台帳 harness.md を参照)
- 往復: 0
- 書き込みの深さ: **詳細** (2026-11-16、sub-17 の VERDICT で planner が書き直した)
- 質問の運用: 推奨で進め、Notion に後回し。着手前の確認は無い (Phase 6 へ進むことは裁定済み)。

要求 §9.2、§9.3、§15 Phase 6 の完了条件「本番 Scene を変更せず、過去と現在の視覚比較ができる」。arch §7。**実行中の Scene を巻き戻して表示しない。本番の World・Undo・Selection・アセットの登録・プロジェクトのファイルに何も残さない。**

## 範囲の取り決め (planner の裁定。spec §2 D42)

1. **比べるのは「ある Revision の Before」と「その Revision の After」**、どちらもサービスの Blob (ディスクの内容の写し) から読む。加えて「今のディスクの内容」(シーンのファイルを読む) を After の代わりに選べる。**エディタのライブの状態 (未保存の編集) は比べる対象にしない** (`SaveToJson` は fileId を採番しアーキタイプを動かすので、比較のためだけに呼ばない。arch §1)。
2. 対象はシーンの文書 (`*.scene.json`) の Revision。プレハブ・アクターの文書は、このサブでは対象外 (ゴースト、Inspector、`scene_diff` の一覧で見える)。
3. **アセットは今の内容で描く** (メッシュ・テクスチャ・マテリアル・プレハブのベース)。比較の時点の状態を完全には再現できないので、その旨を必ず出す (要求 §9.3)。Before の時刻より後に、そのシーンが参照するアセットに記録された変更があれば、件数とパスを出す (追跡の履歴で分かる範囲。分からなければ「確かめられません」)。sub-17 のゴーストと同じ方針。
4. C# コンポーネント (スクリプトの状態) はプレビューのシーンへ読み込まない (managed のホストは本番と共有で、インスタンスを作ると本番の C# の状態に触れうる)。描画に関係するのはメッシュ・ライト・カメラなどの組み込みのコンポーネントだけで足りる。読み込まなかったことを注記に出す。

## 使えるもの

- 前例: `C:\HAL\MyEngin\src\Editor\Windows\Animation\AnimationPreviewWindow.cpp` (専用の一時シーンと RT で描く。規則 16 の許可リストにある: `tools\check_rules.ps1` 1386 行目)、`C:\HAL\MyEngin\src\Editor\Asset\AssetPreviewCache.h/.cpp` (サムネイル・マテリアルのプレビューの RT。許可リスト 1383、1384 行目)。**着手時に、この 2 つが一時シーンをどう作り、本番の何に触れないようにしているかを読んで、同じ作りにする。**
- `blob_get` (sub-10。512 KiB ずつ)、`describe` (Before / After のハッシュ)、`scene_diff` (変わったエンティティ)、`SceneChangeOverlay` の色 (`OverlayPalette`)、`RemovedEntityGhosts` の行列の合成 (sub-17)。
- デバイスの消失からの復旧 (M88、ADR-026): プレビューの RT が作り直される / 安全に閉じること。

## やること

### A. プレビューのシーンの読み込み (`src\Editor\ChangeTracking\` に純粋な部分、描画は Editor の窓の側)

1. Before / After の JSON を Blob から取り (大きければ数フレームに分ける。取得中は進行を出す)、**プレビュー専用の `Scene` / World** へ読み込む。読み込みの経路は、本番のシーンの読み込みと同じシリアライザを使ってよいが、次を守る:
   - 本番の World と、その `ComponentRegistry` 以外の状態 (エンティティ、`fileIdHistory_`、選択、Undo、`ReloadHub` の開いているシーンのパス、`SceneChangeOverlay`) に触れない。
   - **アセットを新しく読み込む場合は、本番と共有のキャッシュに入ってよいか**を前例で確かめる。共有のキャッシュに入れるのが前例どおりなら可 (メモリが増えるだけで、内容は同じ)。**`.meta` の作成 (`GuidForPath(…, true)`) やアセットの登録の変更は起こさない** (プロジェクトのファイルを増やさない)。
   - C# コンポーネントは読み込まない (取り決め 4)。
   - 版の違い (`version`) や壊れた JSON は「表示できない」として、落ちない。
2. エンティティ数の上限 (定数。大きいシーンで止まらない)。超えたら、変わったエンティティとその近くだけを読む、か「大きすぎます」と出す (どちらにするかを決めて書く)。
3. プレビューのシーンは、プレビューの窓を閉じたら破棄する。1 度に持つのは最大 2 つ (Before と After)。

### B. 描画と窓 (`src\Editor\Windows\...` に新しい窓。生の D3D 型はここに閉じる)

1. 表示のモード: **Before** / **After** / **重ね (Overlay)** = After を描き、変わったエンティティを強調の色のワイヤ箱で、削除されたものを Before のゴーストで重ねる / **左右 (Side by Side)** = 2 つの RT を横に並べ、カメラを同期する。
2. カメラ: 開いたときは、変わったエンティティの範囲が収まる位置。マウスでの回転・移動・ズーム (前例の操作に合わせる)。左右では両方が同じカメラ。
3. 窓の外の描画 (GameView、撮影の経路、Runtime) に出さない。
4. 生の D3D 型を使うファイルを規則 16 の許可リストへ足す場合は、`tools\check_rules.ps1` の表に理由を書き、`C:\HAL\MyEngin\engine_spec.md` §6.3 も同時に更新する (AGENTS.md §4.1)。**`src\Editor\ChangeTracking\` には足さない** (窓のファイルにだけ)。
5. 注記: アセットは今の内容 (取り決め 3)、C# は読み込んでいない、上限で一部だけ、表示できない理由。
6. 入口: AI Changes 窓の詳細の列 (シーンの Revision のとき)「Before / After を見る」。

### C. 比べる対象の切り替え

- After の代わりに「今のディスクの内容」を選べる (ファイルを読むだけ。大きさの上限つき)。エディタの未保存の状態は選べない (取り決め 1。画面に 1 行説明する)。

### D. 文書

- arch §7 (取り決め 1〜4)、`C:\HAL\MyEngin\engine_spec.md` §9.2 (と §6.3)、`C:\HAL\MyEngin\docs\test_checklists.md`、`C:\HAL\MyEngin\plans\m104-ai-change-tracking.md` (進捗表)。

## sub-17 から持ち越した注意 (作業のしかた)

- **窓を出す起動 (`--screenshot`、`--frames`、対話起動) は、作業ディレクトリをリポジトリの外 (`%TEMP%` の自分のディレクトリ) にする。** リポジトリ直下で起動すると、利用者の `imgui.ini` (窓の配置) が書き換わる (sub-17 で起きた)。**`--selftest` は従来どおりリポジトリを作業ディレクトリにする** (planner が確かめた: Release の `--selftest` は、作業ディレクトリが `%TEMP%` だと 3 スイートが落ち、リポジトリなら 0 失敗。`--selftest` はリポジトリ直下の `imgui.ini` を書き換えなかった)。
- Windows のパスを含むコードは Write / Edit ツールで書く (Bash のヒアドキュメントはバックスラッシュを潰す)。

## やらないこと (このサブでは)

- エディタの未保存の状態との比較。プレハブ・アクターの文書のプレビュー。過去のアセットの内容の復元。
- プレビューの中での編集・選択の本番への反映。
- Engine 層の変更 (変わってよいのは `LocalizationTable.inl` だけ)。`tools\track` の変更は、要るなら最小限 (例: シーンが参照するアセットの変更の問い合わせ) で、差分に出す。

## 受け入れ条件 (このサブ)

1. **本番を変えない**: プレビューを開く・モードを切り替える・カメラを動かす・閉じる、の前後で、本番の Scene の `HashWorld`、エンティティ数、アーキタイプ数、Undo の `StateSerial`、`Selection`、`ReloadHub` の開いているシーンのパスが同じ。シーンが dirty にならない → `changetracking` スイート (プレビューのシーンの読み込みと破棄を、描画なしで回す)。
2. **プロジェクトのファイルを増やさない**: プレビューの前後で、一時プロジェクトの全ファイルのパスとハッシュが同じ (`.meta` が作られない) → スイート。
3. Play 中にプレビューを開いても、sim の結果が変わらない (開いた場合と開かない場合で、同じ Tick 数の後の `HashWorld` が同じ) → スイート。
4. Before が読めない (版の違い、Blob の欠け、壊れた JSON)、上限を超える大きさ、のそれぞれで、理由が出て落ちない → スイート。
5. C# コンポーネントを含むシーンで、プレビューを開いても managed のインスタンスが増えない (managed のホストの問い合わせで数える。数えられなければ、読み込みの経路で C# を飛ばしていることをコードと試験で確かめる) → スイート。
6. アセットの注記: Before の時刻より後に、参照するアセットに変更の記録があれば、件数とパスが出る。無ければ「今の内容で描いています」だけ → スイート (偽の応答)。
7. 閉じると、プレビューのシーンと RT が破棄される (2 回開いて閉じても数が増えない) → スイート。
8. デバイスの消失からの復旧の後、プレビューが作り直されるか、安全に閉じる → 既存の復旧の試験の形に合わせる (`MYE_SELFTEST_ONLY` に該当のスイート)。
9. スクショ (一時のプローブ。コミットしない): Before、After、重ね、左右、注記、英語。**見た目の合否はユーザーの目視。**
10. `Editor.exe --selftest` (Debug / Release)、`tools\check_rules.ps1` (規則 10、16、19。許可リストを足した場合は、その行と `engine_spec.md` §6.3 の差分がある)、`cargo test` (track)、`tools\track_verify.bat` が通る。下位の層の差分が `LocalizationTable.inl` だけ。
11. 作業のしかた: リポジトリ直下の `imgui.ini` の更新時刻が、このサブの作業の前後で変わっていない → 確認の結果を書く。

## 検証コマンド

- `MyEngine.sln` を `Debug|x64` と `Release|x64` でビルド (`/p:MyeWarnAsError=true`)
- `MYE_SELFTEST_ONLY=changetracking` で `bin\x64\Debug\Editor.exe --selftest`、続けて `--selftest` を Debug / Release (作業ディレクトリはリポジトリ)
- `pwsh -NoProfile -File tools\check_rules.ps1`
- スクショ: 一時のプローブ + `bin\x64\Release\Editor.exe --frames 40 --shot-frame 36 --screenshot` (作業ディレクトリは `%TEMP%`)
- exe の実行は PowerShell から。`tools\mcp\target\release\` の exe に触らない。ユーザーのプロセスを止めない。

## 実装メモ (coder が追記)

### SELF_EVAL: sub-18 (round 1)

実装:
- src\Editor\ChangeTracking\ScenePreviewContent.h/.cpp (新規): `PreviewContentFetch` (blob_get を 512 KiB ずつ・応答のたびに次を要求。今のファイルは `RestoreFileIo` で 1 回 1 MiB。上限 32 MiB、ハッシュ検証、取り消し後の古い応答は弱い参照で捨てる)、`LoadPreviewScene` (自前の Scene へ `SceneSerializer::LoadFromJson`。C# コンポーネントは JSON から取り除いてから渡す。壊れた JSON・版・形・1 万エンティティ超は理由を返し、Scene に触れない)、`CollectPreviewAssetRefs` (AssetRef の値を読むだけ)、`PreviewAssetProbe` (参照アセットごとに `list(path, limit 1)`、先頭 64 件、`revisionId` > この Revision なら「変わった」)。
- ScenePreviewSession.h/.cpp (新規): `PreviewScene` (Scene + TransformSystem。生きている数を数える)、モード・After の出どころ (Revision / Disk)・カメラ (回転・移動・ズーム・合わせる。左右でも 1 つ)・印 (追加 / 更新は After、削除は Before の位置)。GPU も ImGui も持たない。ScenePreviewFormat.h/.cpp (新規): 画面に出す文面 (`ScenePreviewScreen`)。
- src\Editor\Windows\Project\ScenePreviewWindow.h/.cpp (新規): 専用の RT に描く窓 (Before / After / 重ね / 左右)。側ごとの `RenderSystem` は描くときに作ってヒープに持つ (EditorApp に直に持たせると、Debug の `behaviortree` スイートがスタックオーバーフローした。下の注意)。重ねの印は ImGui の描画リスト。デバイス消失は `ReleaseGpu` が RT と RenderSystem を手放し、Scene は残る。生の D3D は `DXGI_FORMAT_...` の 1 語だけ。
- AiChangesWindow.h/.cpp: 詳細の「Before / After を見る...」(`OpenScenePreview` / `DrawPreviewButton`)、`AiChangesHost::openScenePreview`。EditorApp.h/.cpp: `scenePreview_` の持ち主・host の口 (C# の判定と、`AssetDatabase::PathForGuid` + `RelativeToProject` のアセットのパス解決は読み取りだけ)・OnImGui / OnRenderViews / OnDeviceLost・DockBuilder。
- LocalizationTable.inl (英日): 38 件。tools\check_rules.ps1 (規則 16 の許可リストに ScenePreviewWindow.cpp を理由つきで追加)。文書: engine_spec.md (§6.3、§9.2)、docs\ai-change-tracking\architecture.md §7、docs\test_checklists.md、plans\m104-ai-change-tracking.md。
- 試験: ScenePreviewSelfTest.cpp (`changetracking` スイートの末尾から呼ぶ)、SceneRestoreSelfTest.cpp は本物の ManagedHost を貸す 1 行だけ。

仕様との差分:
- [逸脱] 重ねの「削除されたものを Before のゴーストで重ねる」は、半透明メッシュではなく**ワイヤの印 (四角 + 斜線 + 名前) を Before の位置に出す**。専用 RT に Before のメッシュを足して描く経路を作ると、アセットを引く描画の経路をもう 1 本持つ。メッシュのゴーストが要るなら sub-19 か別サブで。
- [逸脱] 受け入れ条件 1 の「`ReloadHub` の開いているシーンのパスが同じ」は、`ReloadHub` に開いているパスを読む口が無い (private) ので**試験で直接は確かめていない**。代わりに、窓も `ScenePreviewSession` も `ReloadHub` / `Undo` / `Selection` / 本番の Scene を**引数にも持ち主にも受け取らない** (型で触れない) ことと、`HasPendingScene()` が偽のままであることを試験している。
- [逸脱] 受け入れ条件 5 の managed のインスタンス数: 数える問い合わせが `ManagedHost` に無いので、「読み込みの経路で C# を飛ばしていることをコードと試験で確かめる」の方で確かめた (偽のフィルタの試験 + 本物の ManagedHost の試験。後者は Debug では実行され PASS、**Release では MyeScripting.dll が古く CoreCLR が起動しないため SKIP**。SceneRestoreSelfTest の既存の SKIP と同じ理由)。
- [逸脱] 受け入れ条件 8 の「既存の復旧の試験の形」: 既存の復旧の試験 (DeviceRecoverySelfTest) はエンジン層で、エディタの窓の `ReleaseGpu` を扱っていない (AnimationPreviewWindow も未被覆)。窓の `ReleaseGpu` の後に Session と Scene が残って次のフレームが安全であることを試験した。**本物のデバイス消失 (TDR) での RT の作り直しは未確認**。
- [追加] After に「今のディスクのファイル」を選べる切り替え (取り決め 1 / C)。読み取りは `CheckRestorePath` (パスの検査) を通し、本物の読み取り口は読み取り専用。
- [追加] エンティティ数の上限 `kPreviewMaxEntities` = 10000 と文書の上限 32 MiB。超えたら**一部だけは読まず**「大きすぎます」(どちらにするかの決定: 一部だけでは Before と After で映る範囲が食い違い、比較として嘘になる)。
- [追加] `ScenePreviewWindow` を開いたときに `SetNextWindowFocus` (中央のタブ束の裏に隠れる。スクショで見つけた)。窓は layouts の開閉の表には入れていない (開くのは AI Changes 窓のボタンだけ)。
- [追加] sub-18 の「触る場所」に無いファイル: SceneRestoreSelfTest.cpp (本物の ManagedHost を借りる 1 行。CoreCLR は 1 プロセスで 1 回しか起動できない)、ChangeTrackingSelfTest.h/.cpp (登録)。

検証:
- `MyEngine.sln` を Debug|x64 / Release|x64 で `/p:MyeWarnAsError=true` ビルド → 警告・エラー 0 (最終版のソースで両方)。
- Release `MYE_SELFTEST_ONLY=changetracking --selftest` → PASS (約 8.6 秒。C# は SKIP と明記される)。
- Release `--selftest-full` (リポジトリが作業ディレクトリ、最終版) → 125 スイート 0 失敗 (233 秒)。
- Debug: `--selftest-list` の fast / full の付く行から physics を除いた 124 スイートを `MYE_SELFTEST_ONLY` で (最終版) → 124 スイート 0 失敗 (741 秒)。`changetracking` は Debug で本物の ManagedHost の試験も PASS。
- Debug の `behaviortree` が最初のビルド (`RenderSystem` を `EditorApp` の中に 2 つ直に持った版) でスタックオーバーフロー (終了コード -1073741571) した → 原因は `EditorMain` が `EditorApp` をスタックに置くため (Debug ではセルフテストの余裕を食う)。`std::unique_ptr<RenderSystem>` へ変えて直し、単独で PASS (11 秒)。32 MiB のスタックに editbin したコピーでは最初の版でも PASS (= スタックの問題と確認)。
- `cargo test --manifest-path tools\track\Cargo.toml` (`-- --ignored` なし) → 全 ok (80 / 23 / 22 / 14 / 14 / 3 / 10 / 15 / 13 / 13 / 41 / 18 件ほか、0 失敗)。`tools\track_verify.bat` → 全シナリオ PASS。`pwsh tools\check_rules.ps1` → 0 error (警告 66 は既存。ScenePreview 関連の警告なし)。
- スクショ (Release、作業ディレクトリは %TEMP% 下の自作ディレクトリ、一時プローブ): cache\m104s_shots\ の 1_before_ja / 2_after_ja / 3_overlay_ja / 4_side_ja / 5_overlay_en / 6_side_en (.png)。Before・After・重ね・左右・注記・英語で窓が出て、メッシュが描かれ、重ねに色つきの印と名前、左右は同じカメラで 2 枚。**見た目の合否はユーザーの目視**。スクショはヒントの文を ImGui のツールチップへ移す前、`RenderSystem` をヒープへ移す前の版で撮った (最終版では撮り直していない。プローブは外した)。
- `imgui.ini` (リポジトリ直下) の更新時刻は作業の前後とも 2026-11-16T00:37:28.3063518+09:00 で変化なし (窓を出す起動はすべて `%TEMP%` 下の自作ディレクトリを作業ディレクトリにした)。
- 下位の層の差分は `src\Engine\Core\Localization\LocalizationTable.inl` だけ (`git diff --stat -- src/Engine`)。`tools\track`・schema・`tools\mcp`・`.claude`・`.mcp.json` に差分なし。ABI・SimSnapshot・TypeId・DB スキーマは不変。

自己採点 (1-5):
  仕様適合: 4 — 受け入れ条件 11 件のうち 1 (ReloadHub のパスを直接は見ていない)、5 (C# の本物のホストは Debug のみ)、8 (本物の TDR 未確認)、9 (目視はユーザー)、重ねのゴーストがワイヤの印、が逸脱・未確認として残る
  正しさ: 4 — 全試験 PASS (本番の Scene・Undo・選択・一時プロジェクトのファイルが不変、Play 中のハッシュ一致、失敗の分け方、大きい文書の分割取得、古い応答、アセットの注記)。本物のデバイス消失と、実サービスの blob_get / list での結合 (MYE_TRACK_PROBE) は未実施
  コード品質: 4 — 純粋な部分 (ChangeTracking) と D3D の窓を分け、弱い参照で寿命を守った。`ScenePreviewSession` は 1 つの持ち主に状態を寄せている。窓の ImGui の部分に試験で届かない描画 (RT・印の座標) が残る
  テスト: 4 — 受け入れ条件 1〜7 を偽のサービス・偽のディスク・本物の一時プロジェクト・HeadlessSim で確かめた。窓の描画 (RT) と目視はスクショだけ

不安・質問:
- (1) 重ねのゴーストを半透明メッシュにするか (今は印だけ)。Before のメッシュを「今のアセット」で描く経路は sub-17 の SceneView のゴーストにあるが、専用 RT へ足すと `GhostMeshPass` を持つことになる。要るなら sub-19 の前に別サブで。
- (2) `ReloadHub` に開いているシーンのパスを読む口 (getter) を足すか。足せば条件 1 を直接確かめられるが、Engine 層の変更 (このサブでは禁止) になる。
- (3) アセットの注記は `AssetDatabase::PathForGuid` で引けるものだけ (組み込みのメッシュとサブアセットの鍵は引けず数えない)。「確かめた範囲」と画面に出してはいない (全部確かめたようには書いていないが、「0 件」と「引けなかった」は区別されない)。
- (4) 実サービス (`mye_track.exe`) との結合 (`MYE_TRACK_PROBE=1`) は書いていない。blob_get / list は既存の使い方と同じ形で、偽のサービスで確かめた。
- (5) 本物の ManagedHost の試験は Release で SKIP (bin\x64\Release\MyeScripting.dll が 11/11 のまま古く、CoreCLR が起動しない。既存の SceneRestoreSelfTest も同じ SKIP)。Debug では通る。

触ったファイル:
  新規: src\Editor\ChangeTracking\ScenePreviewContent.h / .cpp、ScenePreviewSession.h / .cpp、ScenePreviewFormat.h / .cpp、ScenePreviewSelfTest.cpp、src\Editor\Windows\Project\ScenePreviewWindow.h / .cpp
  変更: src\Editor\Windows\Project\AiChangesWindow.h / .cpp、src\Editor\App\EditorApp.h / .cpp、src\Editor\ChangeTracking\ChangeTrackingSelfTest.h / .cpp、src\Editor\ChangeTracking\SceneRestoreSelfTest.cpp、src\Engine\Core\Localization\LocalizationTable.inl、tools\check_rules.ps1、engine_spec.md、docs\ai-change-tracking\architecture.md、docs\test_checklists.md、plans\m104-ai-change-tracking.md、plans\m104-ai-change-tracking\sub-18.md (実装メモ)
  生成物 (tools\gen_project_files.ps1 が更新。tracked): build\Editor.vcxproj、build\Editor.vcxproj.filters
  (plans\m104-ai-change-tracking\harness.md の差分は作業前からあった司会の更新で、触っていない)
消した・止めたもの: ファイルは何も消していない。自作の一時物 (%TEMP%\m104s_dbgcopy、%TEMP%\m104s_work_<pid>、Git Bash の /tmp の .orig) は削除済み。残した一時物: cache\m104s_shots\ (png / log / err。コミットしない)、%TEMP%\m104s_* のログ。起動したプロセスはすべて自分が待って終了を確認した (mye_mcp.exe などユーザーのプロセスには触れていない)。mye_track.exe は起動していない (`build_track.bat` も使っていない)。
申し送り:
- sub-19: 重ねの印は原点だけ (SceneChangeWireBoxes と同じ見直しの対象)。印の位置はワールド行列の原点で、大きいエンティティの範囲箱ではない。
- EditorApp に大きい窓のオブジェクトを直に足さない (`RenderSystem` のように大きいものは `unique_ptr`)。Debug の `behaviortree` が余裕を食われて落ちた。

## フィードバック履歴
- round 1: VERDICT OK (planner、2026-11-16)。確かめたこと: `mye_track` / `Editor` の残存プロセスなし、リポジトリに `.mye\ai_changes\` なし、下位の層の差分は `LocalizationTable.inl` (39 行) だけ、`tools\track` / `tools\mcp` / `.claude` / `.mcp.json` に差分なし、`check_rules.ps1` の差分は規則 16 の許可リストへ `ScenePreviewWindow.cpp` の 1 行 (理由つき) で `engine_spec.md` §6.3 も更新済み、`src\Editor\ChangeTracking\` に D3D / DXGI / `RenderTexture` の参照は 0 件、プローブの語の残りなし、`imgui.ini` の更新時刻は 00:37:28 のまま、Debug / Release の `Editor.exe` は最後のソースの変更より新しい。planner の実走: Debug の `MYE_SELFTEST_ONLY=changetracking` が終了コード 0・FAIL 0、Release の `--selftest` 全体が 112 スイート 0 失敗 (実行中も後も `mye_track.exe` なし)、`tools\check_rules.ps1` 0 error。`--selftest-full`、Debug の physics を除いた 124 スイート、`cargo test`、`track_verify`、スクショは coder の結果による。
  - 逸脱の判定: 重ねの「削除されたもの」を半透明メッシュではなくワイヤの印にした、`ReloadHub` の開いているシーンのパスを直接は確かめず型で触れないことと `HasPendingScene()` で確かめた、managed のインスタンス数を数えずに読み込みの経路で C# を飛ばしていることで確かめた、デバイスの消失を窓の `ReleaseGpu` の後の安全で確かめた、は全部可。
  - 追加の判定: 今のディスクの内容を After に選べる、上限 (1 万エンティティ / 32 MiB) を超えたら一部だけ読まずに「大きすぎます」、窓を開いたら前面へ、は採る。
  - 質問への回答: (1) 重ねのゴーストは印のままでよい。(2) `ReloadHub` に読む口を足すのは Engine 層の変更なので足さない。(3) アセットの注記で「0 件」と「引けなかった」を区別していない件は、sub-19 の仕上げで、引けなかった件数を別に出す小さい直しとして入れてよい (任意)。(4) 実サービスとの結合 (`MYE_TRACK_PROBE`) は無くてよい (`blob_get` / `list` は既存の形で、既存の結合で確かめてある)。(5) Release の `MyeScripting.dll` が古く本物の ManagedHost の試験が SKIP になる件は、Debug で通っているので受け入れる。`tools\build_managed.bat Release` で置き直せば Release でも走るはず (sub-19 の検証の前に置き直してよい)。
  - **Debug のスタックの件**: `EditorMain.cpp` 583 行目で `EditorApp` が `wWinMain` のローカル変数なので、`EditorApp` が大きくなるほど Debug の自己テストが使えるスタックが減る見込み。D38 (physics) と同じ原因の可能性があり、planner が今も physics が落ちることを確かめた。sub-19 の最初に確かめる (spec §2 D43)。