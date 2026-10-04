# sub-07: NavMeshLink (Off-Mesh Link の渡り)

- 依存: sub-06
- 状態: OK (commit ade6b23)
- 往復: 2

## やること
spec 2. #12、4.1 (Link)。

1. `NavMeshLinkComponent` を末尾 append (hash 対象)。`start` / `end` (ローカル)、`width`、`bidirectional`、`area` (既定 2 = Jump)、`traversal` (Linear / Jump / Manual)、`traversalSpeed`、`jumpHeight`。
2. ベイク時は Off-Mesh Connection として `.mnav` に入れる。実行時の追加・移動・削除の反映方法 (TileCache の `dtTileCacheMeshProcess` でタイル再構築時に差し込むのが基本) を coder が決め、restore 後一致を確かめる。
3. 渡り: dtCrowd の OFFMESH 状態で NavSystem が補間を持つ (状態は Agent コンポーネントか Nav 節 = スナップショット対象)。Linear = 一定速度の直線、Jump = 放物線 (`jumpHeight`)、Manual = 端で止まり `status = OnLink` を公開し、完了の通知 (コンポーネントのフィールドを書く。ABI は sub-08) で抜ける。渡る間は CC.moveInput = 0、物理 (3.6) の後に nav が位置と CC.velocity を上書き。CC の型は変えない (変える必要が出たら `kSimSnapshotVersion` の扱いと一緒に「仕様との差分」へ)。
4. 片方向の Link は逆向きに使われない。
5. Create → 3D Object → NavMesh Link、Add Component、ギズモ (2 点と矢印)、デバッグ描画、インスペクタ、Localization。
6. `--nav-demo` に Link を足し `nav` ジョブを録り直す。

(round 2 で追加、sub-07 VERDICT round 1)
7. **渡っている途中の Link の削除・移動・無効化・Surface の読み直し**を定義どおりに処理する: 渡り始めに保存した入口 / 出口まで渡り切ってから WALKING へ戻す。dtCrowd の古い polyRef や無効な ref を使わず、出口で NavMesh に載らなければ最寄り点へ置き直す (それも無ければ Inactive)。クラッシュ・アサート・NaN を出さない。SelfTest: Linear / Jump / Manual の各渡りの途中で Link のエンティティを消す、Link を動かす、Link を非アクティブにする。どれも完了後に Agent が目的地へ向かうか、正しい状態になる。途中で撮って復元しても連続実行と一致する。
8. **つながらない Link の警告**: 入口だけでなく出口がつながらない場合 (2 タイル以上離れている・出口に歩行面が無い) も、状態が変わった tick に WARN を 1 回出し、Inspector に警告を出す (`ConnectedLinkCount` かタイルごとの Off-Mesh の接続状態で判定)。SelfTest: 2 タイル以上離れた Link で警告の判定が立つ。
9. 親を持つ Agent の渡りの SelfTest (親を動かしたり回したりした状態で、渡り切った位置がワールドで正しいこと)。
10. (nit) 渡り開始の INFO ログは resim 中は出さない (`ts.resim` などの既存の判定を使う。無ければこの項は見送って理由を書く)。

## やらないこと (このサブでは)
- アニメーション連携 (後の BT / アニメで状態を読む)、ABI

## 触る場所 (planner の見立て)
- `Components.h/.cpp`、`NavBake`、`NavSystem`、`TickRunner.cpp` (物理の後の上書き)、`SimSnapshot`、`CreateMenu.cpp`、Inspector、ギズモ、`DemoContent.cpp`、Localization

## 受け入れ条件 (このサブ)
1. (spec 10) Linear / Jump / Manual それぞれで向こう側へ渡り目的地に着く。Manual は通知まで止まる。片方向は逆向きに使われない。— SelfTest
2. 渡りの途中の tick で撮って restore → 連続実行と一致。— SelfTest
3. (spec 4) Create → NavMesh Link が Undo / Redo できる。
4. replay_verify 全 PASS (`nav` 録り直し)、golden は `nav` 以外不変 (`nav` は差分が意図どおりか画像で確認して更新)、0 警告、check_rules 0。

## 検証コマンド
- 両構成ビルド、`--selftest` 両構成、`tools\replay_verify.bat`、`tools\shot_verify.bat`、`tools\check_rules.ps1`

## 実装メモ (coder が追記)

SELF_EVAL: sub-07 (round 1)
実装:
  - external\recastnavigation\DetourCrowd (Include/DetourCrowd.h, Source/DetourCrowd.cpp) + PATCHES.md 4 — パッチ 4: dtCrowd::update 末尾の Off-Mesh 補間ループを「OFFMESH の間は速度を 0 にするだけ」へ差し替え、getAgentAnimation(idx) を公開 (渡りは NavSystem が持つ)
  - Components.h/.cpp — NavMeshLinkComponent (TypeId 75、start/end/width/bidirectional/area/traversal/traversalSpeed/jumpHeight)、navlinktraversal、NavMeshAgent に linkComplete (書込) / linkStart / linkEnd (読取専用)、Surface.drawLinks (NoHash)
  - NavTileCacheSupport.h/.cpp — NavLinkSpec、NavMeshProcess::process が links を dtNavMeshCreateParams の offMeshCon* へ差し込む (入口 y をタイルの高さ範囲へ寄せる)、NavTileStore::ReplaceLinks / ConnectedLinkCount / HashLinks / LinkAt、SaveState/LoadState v4 (Link を保存・復元)、HashMeshTile に offMeshCons
  - NavSystem.h/.cpp — NavLinkKey / NavMakeLinkSpec / NavCollectLinkSpecs / NavFilterLinksToSurface、SyncObstacles が Link を同じ Commit にまとめて差し込み、NavAgentSlot に渡り状態 (linkPhase 以下) を追加し Nav 節と StateHash に入れた、BeginLink (OFFMESH に入った Agent の渡り開始)、PostPhysics (物理の後に位置と CC.velocity を上書きし、渡り終えたら WALKING へ戻す)、結果段で status = OnLink / moveInput = 0 / 進行方向へ旋回、Link の線描画 (Surface.drawLinks)
  - TickRunner.cpp — physicsSystem.Update の直後 (Transform の前) に navSystem->PostPhysics
  - SimSnapshot.h — kSimSnapshotVersion 28 -> 29
  - CreateMenu.cpp/.h, EditorComponentCatalog.cpp, SceneViewWindow.cpp/.h (DrawNavLinkGizmos: 入口・出口の球 + 線 + 矢尻)、InspectorWindow.cpp (Link のエリア名・歩行不可・片方向・Manual の注意書き、Agent の渡り中の入口/出口)、LocalizationTable.inl (en/ja)
  - DemoContent.cpp (BuildNavShowcaseScene) — 床から孤島 (3 m) の上へ跳ぶ双方向 Jump の Link を、Modifier の後・ベイクの前に追加。tests\golden\nav.png を手撮りで更新
  - NavAgentSelfTest.cpp — Link 項目 (11 / 11b / 11c)、Sim::Step に PostPhysics。NavEditorSelfTest.cpp — Create -> NavMesh Link の Undo/Redo。NavDeterminismSelfTest / AcousticAudioSelfTest — 期待値の追随 (下記)
仕様との差分:
  - [逸脱] .mnav に Link を入れない (sub-07 やること 2 の「ベイク時は Off-Mesh Connection として .mnav に入れる」と spec 4.2 の「Link のベイク時スナップ」) — Modifier (sub-06) と同じ「実行時のオーバーレイ」にした。.mnav にも入れると、同じ Link が二重の持ち主 (アセットとコンポーネント) になり、動かす・消すを元に戻せない。.mnav の形式と kNavBakeVersion は不変
  - [逸脱] 編集中 (非 Play) の NavDebugView の塗りには Link を反映しない — Off-Mesh ポリゴンは塗りに出ない (DebugUtils は flags 0 で描かない) ので見た目が変わらない。Link の位置は SceneView のギズモ (2 点 + 矢印) で見える
  - [追加] NavMeshAgent に linkComplete (書込) / linkStart / linkEnd (読取専用) — Manual の「完了の通知」と、渡り中の入口/出口の公開。ABI の関数は sub-08
  - [追加] NavMeshSurface に drawLinks (NoHash) — spec 4.3 の表示切り替え「Link」
  - [追加] Recast パッチ 4 (DetourCrowd) — 渡りを dtCrowd の外に出すため。PATCHES.md 4 に記録
  - [追加] 渡り開始時の INFO ログ 1 行 (どの Agent がどの種別で渡るか) — 観測可能性。resim では重複しうる
  - [追加] kSimSnapshotVersion 29 / NavTileStore の状態 v4 / NavDeterminismSelfTest の capture.A.store・B.store、NavAgentSelfTest の kExpectedYardHash、AcousticAudioSelfTest の版の値を追随 (いずれも Debug で採取し Release の一致を確認して焼いた。庭のハッシュは NavMeshAgent の新フィールドで変わる)
  - [追加] Link の動き: 入口へ近づく (Agent の speed で入口まで直線) -> 渡る (Linear = traversalSpeed の直線 / Jump = 直線 + jumpHeight x 4u(1-u) の弧 / Manual = 入口で待機) の 2〜3 フェーズ。1 フェーズの長さは ceil(距離 / (速さ x dt))。渡る間 CC.velocity は水平成分だけ (y = 0)、終了時は 0
  - [追加] 両端が許される位置: 入口はベイク範囲内、出口は入口のタイルと同じか隣のタイル (Detour の制約)。入口に歩行面が無い Link は dtNavMesh に残らず、WARN を 1 回出して無視する。出口側の制約は ADR に書く (sub-09)
検証:
  - Debug / Release ビルド (MSBuild、Components.h を触って全再コンパイル) → 警告 0 (vendor 含む)
  - Editor.exe --selftest (Debug / Release 直列) → 既知の Source control 2 件以外 ALL PASS。NavAgent self test ALL PASS (Link 項目 (約 40 チェック): Linear/Jump/Manual、片方向、areaMask、編集、渡りの途中の復元)。NavEditor ALL PASS。Debug 採取のハッシュ (庭 0x1AB952061BC96FF8、capture.A/B.store) が Release で一致
  - Server.exe --selftest (Debug / Release) → exit 0
  - tools\check_rules.ps1 → 0 error / 0 warning
  - tools\replay_verify.bat → 15 ジョブ全 PASS (nav は Link の跳び上がり・跳び降りを含む。`[nav] agent ... starts crossing a Jump link` のログで実際に渡ることを Runtime --nav-demo 700 frames で確認)
  - tools\shot_verify.bat → nav PASS (tol 3、更新した golden)。FAIL は着手前と同じ 5 枚・同じ値 (parts 198/3625、joints 208/137、acoustic_forward 83/596、acoustic_deferred 82/594、fracture_after 150/192)
  - 画像: Runtime --nav-demo (Release、1920x1080 のフレーム 120 の切り出し) で島の前面に Link の線と矢尻が出ることを目視 (cache\s07\nav_crop.png、git 管理外)
  - 未実行: Editor GUI の操作 (Create メニューのクリック、Inspector の表示、SceneView のギズモの見た目) は手で触っていない。Create の Undo/Redo は NavEditorSelfTest で検証。Link の長い距離 (タイル 2 枚以上離れた出口) の渡りは未検証
自己採点 (1-5):
  仕様適合: 4 — 3 種の渡り・片方向・areaMask・復元・Create/ギズモ/Inspector/Localization・demo・nav 録り直しまで満たす。.mnav に入れない逸脱と、編集中の塗りに Link を出さない点が残る
  正しさ: 4 — Link の SelfTest と replay 15 ジョブ、Debug/Release 一致で検証。出口が遠い Link・親付き Agent の渡り (親の逆行列でローカルへ戻す) は実走で未確認
  コード品質: 4 — Modifier と同じ差分適用の流儀で store/NavSystem に収めた。NavSystem.cpp が長く、渡りの状態機械は AdvanceLink/BeginLink/PostPhysics に分けたが 1 ファイルのまま
  テスト: 4 — Linear/Jump/Manual/片方向/mask/編集/復元/毎 tick の撮影-復元-再撮影。親付き Agent・タイル跨ぎの遠い出口・Link を渡っている最中の Link 削除は未テスト
不安・質問:
  1. .mnav に Link を入れない逸脱 (上) は planner の採否を待つ。入れるなら二重の持ち主をどう解消するか (例: ベイク時の Link は無視して常に World から) の方針が要る
  2. 親付き Agent (Body を子に持つ構造ではなく、Agent 自身が親を持つ場合) の渡りは、親の WorldMatrix (前 tick) の逆行列で位置を書く。実走テストは未作成。必要なら sub-07 の追加項目にしてほしい
  3. 出口が入口のタイルから 2 枚以上離れた Link は出口側がつながらない (Detour の仕様)。警告は「入口が外れた」場合しか出していない。出口側のつながりも警告するか
触ったファイル:
  - external/recastnavigation/DetourCrowd/Include/DetourCrowd.h
  - external/recastnavigation/DetourCrowd/Source/DetourCrowd.cpp
  - external/recastnavigation/PATCHES.md
  - src/Editor/Tools/NavEditorSelfTest.cpp
  - src/Editor/Widgets/CreateMenu.cpp
  - src/Editor/Widgets/CreateMenu.h
  - src/Editor/Widgets/EditorComponentCatalog.cpp
  - src/Editor/Windows/Scene/InspectorWindow.cpp
  - src/Editor/Windows/Scene/SceneViewWindow.cpp
  - src/Editor/Windows/Scene/SceneViewWindow.h
  - src/Engine/Core/Ecs/Components.cpp
  - src/Engine/Core/Ecs/Components.h
  - src/Engine/Core/Localization/LocalizationTable.inl
  - src/Engine/Engine/Audio/Spatial/AcousticAudioSelfTest.cpp
  - src/Engine/Engine/Demo/DemoContent.cpp
  - src/Engine/Engine/Loop/TickRunner.cpp
  - src/Engine/Engine/Navigation/NavAgentSelfTest.cpp
  - src/Engine/Engine/Navigation/NavDeterminismSelfTest.cpp
  - src/Engine/Engine/Navigation/NavSystem.cpp
  - src/Engine/Engine/Navigation/NavSystem.h
  - src/Engine/Engine/Navigation/NavTileCacheSupport.cpp
  - src/Engine/Engine/Navigation/NavTileCacheSupport.h
  - src/Engine/Engine/Replay/SimSnapshot.h
  - tests/golden/nav.png
申し送り:
  - sub-08 (ABI): Manual の完了通知は NavMeshAgent.linkComplete (Get/SetComponentField で書ける)。渡り中の入口/出口は linkStart/linkEnd。status の OnLink は既存の定数 (navagentstatus::kOnLink = 4)
  - sub-09 (ADR): Recast パッチ 4 (PATCHES.md)、Link は .mnav に入れず World から毎 tick 差し込む設計、出口は入口のタイルと同じか隣のタイルまで (Detour の connectExtOffMeshLinks が 8 近傍だけ)、渡りの状態は Nav 節の NavAgentSlot (kSimSnapshotVersion 29)
  - Link の入口の y は、層の高さ範囲 (地面を cellHeight で量子化) から 1 セル + 登れる段差以内なら範囲の端へ寄せてから dtCreateNavMeshData に渡す (そうしないと地面ぴったりの y の入口が分類から落ちる)
  - cache\s07\ に作業ファイル (ログ・スクショ・編集用 python) を置いた。git 管理外。削除は承認待ち

SELF_EVAL: sub-07 (round 2)
実装:
  - #1 NavSystem.cpp:FinishLink (新設) — 渡り終えた Agent を、渡り始めに保存した出口 (slot.linkEnd) で歩行へ戻す。corridor の古い polyRef (Off-Mesh のポリゴン) は使わず、出口の最寄り点 (Agent の filter) へ corridor.reset し、目的地を要求し直させる (slot.requested = 0)。最寄り点が無い・非有限なら crowd から外してスロットを空ける (次の Update がナビメッシュの外の Agent として扱う)。PostPhysics はこれを呼ぶだけに整理
  - #2 SyncObstacles — タイルを作り直した tick に全 Surface の「入口か出口がつながらない Link」の数を数え直し、数が変わったとき WARN を 1 回出す (文言を入口/出口/2 タイル離れに改めた)。NavSystemStats に linkDisconnected / linkWarnings。NavCheckLinkPlacement (静的検査: NoSurface / ExitTooFar) を新設し、Inspector の Link 節に警告 2 種 (en/ja) を出す
  - #3 NavAgentSelfTest 11f — 親が平行移動、および平行移動 + y 軸 90 度回転している子の Agent が Jump の Link を渡り、ワールドの目的地に着く
  - #4 NavSystem::SetCrossingLogEnabled を追加し、TickRunner が ts.resim の逆を渡す。渡り開始の INFO ログは resim 中は出ない
  - NavAgentSelfTest 11d (Linear / Jump / Manual x 削除 / 移動 / 非アクティブ化 / Surface の読み直し の 12 通り), 11e (つながらない Link の警告)
仕様との差分:
  - [逸脱] 「出口で NavMesh に載らず最寄り点も無ければ Inactive」は、crowd から外して既存のナビメッシュ外の扱い (目的地ありなら NoPath、無ければ Idle) にした。Inactive は Update の理由判定 (CC 無し・Surface 無し等) の結果で、渡りの終端では出していない。NaN・クラッシュは出ない
  - [追加] 渡り終わりのたびに corridor を置き直し経路を引き直す (正常な渡りでも 1 回の再探索が増える)。ハッシュに載る挙動なので replay の nav を再録して確認した (期待値の焼き直しは不要だった: 庭 / NavDeterminism は Link を使わない)
  - [追加] 出口の「歩行面が無い」の Inspector 警告は編集中は出せない (ナビメッシュを読み込まない)。静的に分かる 2 種 (入口が Surface の外 / 出口が 2 タイル以上離れる) だけ Inspector に出し、出口に床が無い場合は実行時の WARN が受け持つ
検証:
  - Debug / Release ビルド 警告 0 エラー 0
  - Editor.exe --selftest (Debug / Release 直列) → Source control 2 件以外 ALL PASS。NavAgent ALL PASS: 11d は 12 通りの結果と、読み直し以外 9 通りの「編集直後に撮った状態を空の NavSystem へ復元して 500 tick の毎 tick ハッシュが連続実行と一致」。11e は警告が 1 回ずつ増えること、静的検査 3 種。11f は 2 通りで世界座標 (10.2, 0.9, 0) に Arrived
  - Server.exe --selftest (Debug / Release) → exit 0
  - tools\check_rules.ps1 → 0 / 0
  - tools\replay_verify.bat → 15 ジョブ全 PASS (初回は ui ジョブが texture outofmem で FAIL (環境のメモリ不足、nav は PASS)。再実行で全 PASS)
  - tools\shot_verify.bat → nav PASS。FAIL は着手前と同じ 5 枚・同じ値。golden は撮り直していない
  - 未実行: Editor GUI の手操作 (Inspector の新しい警告の見た目)
自己採点 (1-5):
  仕様適合: 4 — #1〜#4 を満たす。Inactive の代わりに既存のナビメッシュ外の扱いにした逸脱が残る
  正しさ: 4 — 12 通りの途中変更 + 復元一致 + 親付き 2 通りで検証
  コード品質: 4 — 渡りの終端を FinishLink 1 か所に集めた
  テスト: 4 — 渡りの最中の Link 変更・親付き・警告を追加。Surface 読み直し後の復元一致は対象外 (読み直しで状態を捨てる設計)
不安・質問:
  1. 渡りの終端の「Inactive」を既存のナビメッシュ外の扱いで代替してよいか (逸脱の採否)
触ったファイル (round 2 で追加・変更):
  - src/Engine/Engine/Navigation/NavSystem.cpp, NavSystem.h, NavAgentSelfTest.cpp
  - src/Engine/Engine/Loop/TickRunner.cpp
  - src/Engine/Core/Localization/LocalizationTable.inl
  - src/Editor/Windows/Scene/InspectorWindow.cpp
  - plans/m82-navmesh/sub-07.md (実装メモ)
申し送り:
  - NavAgentSelfTest の Sim で階層付きのエンティティを使うときは、ベイクの前に Sim の TransformSystem を一度 Update する (ベイク側の TransformSystem が階層の dirty を消し、Sim 側が親子の WorldMatrix を組まなくなるため)

## フィードバック履歴
- round 1: VERDICT REWORK (planner、2026-10-04)。must: やること 7 (渡っている途中の Link の削除・移動)。should: 8 (出口がつながらないときの警告)、9 (親を持つ Agent)。nit: 10。採用した点: .mnav に入れないこと (spec 4.2)、編集中の塗りに Link を出さない (ギズモで見える)、パッチ 4、linkComplete / linkStart / linkEnd、drawLinks、渡りの動き、入口の y を寄せる処理、版 29 / 状態 v4。
- round 2: VERDICT OK (planner、2026-10-04)。must 1 (FinishLink、途中の変更 12 通りと復元の一致) と should 2・3、nit 4 はすべて解消。終端を NoPath / Idle にした逸脱を採用した (spec 4.1)。残り: Inspector の新しい Link の警告の見た目は手で見ていない → reviewer。
