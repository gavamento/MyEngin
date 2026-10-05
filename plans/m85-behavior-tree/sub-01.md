# sub-01: BT の核 — アセット・コンポーネント・実行器・Composite 3 種・Wait・BT 節とハッシュ

- 依存: なし
- 状態: OK (commit a5f7ab1)
- 往復: 0

## やること
M85 で最も荷重のかかる未知 = 「BT の実行状態をシステムの表で持ち、BT 節で保存 → 復元して連続実行と一致させる」(spec 2. #1、7.) を、
後続サブがそのまま乗れる形で決め切る。

1. **アセット** (spec 4.2): `BehaviorTreeLibrary` / `BlackboardLibrary` (`src\Engine\Engine\AI\` に新設。手本は `src\Engine\Engine\Navigation\NavFilterLibrary.{h,cpp}`)。
   `.bt.json` / `.bb.json` の読み書き・Sanitize (未知の type、壊れた子参照、循環、子の数違反は読み込み失敗)・`Enumerate` は名前 → GUID 順。
   ノードの種類は「種類 → パラメータ記述子 (名前・型・既定値・範囲)」の表で持ち、後続サブとエディタ (sub-08) がこの表を引いて汎用に扱えるようにする。
   このサブで登録するのは Selector / Sequence / SimpleParallel / Wait だけ。
2. **アセット種別の配線** (spec 4.2 の 24 か所のうちエディタ窓以外): AssetType 末尾追加 (`BehaviorTree`, `Blackboard`)、ClassifyPath、TypeName / ParseTypeName、`kCompound`、SimLibraries、EngineLoop / HeadlessSim の install、起動走査 (`DemoContent.cpp:3183-3256`)、ReloadHub (行・HandleChange・関数、`.json` の総取り行より上)、StageClassifier の `kExact`、DrawAssetRef の分岐 (フィールド名は `tree` → 既存のキーワードに当たらないことを確認)、Create メニュー (New Behavior Tree / New Blackboard)、アイコン、タイルの語、文字列。ダブルクリックで窓を開くのは sub-08。
3. **`BehaviorTreeComponent`** (TypeId = 登録時点の末尾 + 1。現在 77 なので 78 の見込み。違えば SELF_EVAL に書く): spec 4.2 のフィールド。Inspector に BT と AgentBrain の同居警告 (spec 2. #14、`InspectorWindow.cpp:1874` の流儀)。
4. **`BehaviorTreeSystem`** (`src\Engine\Engine\AI\BehaviorTreeSystem.{h,cpp}`): spec 4.1.1 の実行モデル (Composite 3 種、Wait、根の終了は次の tick にやり直し、ライフサイクル 4.1.9、BB の set ビット)。Decorator の評価・Abort の枠 (OnAbort を深い方から呼ぶ経路) はこのサブで**枠だけ**作る (sub-02 で中身)。`kBtMaxStepsPerTick` の数え方もここで入れる。
5. **TickRunner** フェーズ 3.4a2 (`TickRunner.cpp:389-404` の間)、`TickServices` へポインタ、EngineLoop と HeadlessSim の両方で生成・配線、シーン遷移で Reset (`TickRunner.cpp:830-835` の流儀)。
6. **SimSnapshot の BT 節** (`'BT01'`): Nav 節 (`SimSnapshot.cpp:493-523`) と同じ形で Write / Read / Validate / Apply、`SimRefs` に追加、`SimSourcesOf` / `SimSources` (末尾追加) / `WorldHasher.cpp` に `HasHashableState` ゲート付きで畳む。`kSimSnapshotVersion` 34 + 変更ログ行 + `AcousticAudioSelfTest.cpp:129`。
   表の形は後続サブ (Decorator の計時、MoveTo の状態、Patrol の次の点、C++ タスクの可変長状態、イベントの配送待ち) が足せるようにする。決めた形は ADR-025 の下書き (`docs\adr\ADR-025-behavior-tree.md`) に書く (却下した形と理由も)。
7. **`BehaviorTreeSelfTest`** (`src\Engine\Engine\AI\BehaviorTreeSelfTest.{h,cpp}`、`EditorMain.cpp` の selftest 列と `ServerSelfTest.cpp` に登録): アセットの往復、Composite の表、保存 → 新システムへ復元 → 連続実行一致、BT 無しで RNG とハッシュが動かないこと、100 体 × 30 ノードの計測 (Release の値を ADR 下書きへ)。

## やらないこと (このサブでは)
- Decorator の中身、Wait 以外の Task、イベント、SubTree、エディタ窓、ABI

## 触る場所 (planner の見立て)
- 新規: `src\Engine\Engine\AI\BehaviorTree{Asset,Library,System,SelfTest}.*` (分け方は任意)、`docs\adr\ADR-025-behavior-tree.md` (下書き)
- 変更: `Components.h` / `Components.cpp` (末尾 append)、`AssetDatabase.{h,cpp}`、`AssetOps.cpp:215-219`、`SimInit.{h,cpp}`、`EngineLoop.cpp`、`HeadlessSim.cpp`、`DemoContent.cpp:3183-3256`、`ReloadHub.{h,cpp}`、`StageClassifier.cpp:78-83`、`InspectorWindow.cpp` (DrawAssetRef・同居警告)、`AssetBrowserWindow.cpp` (Create / タイル / フィルタ)、`EditorWidgets.cpp:206-227`、`TickRunner.{h,cpp}`、`SimSnapshot.{h,cpp}`、`WorldHasher.{h,cpp}`、`LocalizationTable.inl`、`AssetDatabaseSelfTest.cpp:90-97`、`AssetOpsSelfTest.cpp`、`ReloadHubSelfTest.cpp:48`、`EditorMain.cpp`、`ServerSelfTest.cpp`、`build\Engine.vcxproj` (+filters、`tools\gen_project_files.ps1`)
- 手本: `NavFilterLibrary.cpp:58-187`、`NavSystem.h:211-223` (Save / Validate / Apply / HasHashableState)、`FractureSelfTest.cpp:2276-2324` (新システムへ restore して照合)、`PerceptionSystem.cpp:500-526` (存在ゲート)

## 受け入れ条件 (このサブ)
1. (spec 1) 2 種のアセットが Editor / Runtime / Server の起動で読まれ、ReloadHub が読み直し、Create で作れて、改名で拡張子が保たれる — 既存 3 SelfTest の追加項目が PASS。
2. (spec 2) Composite 3 種 + Wait の結果が spec 4.1.1 どおり — `BehaviorTreeSelfTest`。
3. (spec 3) tick 50 保存 → 新システムへ復元 → tick 100 で連続実行とハッシュ一致 (BB の値・実行中ノード・Wait の残り・Parallel の状態)。BT の無いシーンで RNG とハッシュが不変 — `BehaviorTreeSelfTest`。replay_verify の既存ジョブがすべて PASS (撮り直しなし)。
4. ADR-025 下書きに表の形・却下案・計測値 (100 体 × 30 ノード、Release)。
5. (spec 16) Debug / Release `/p:MyeWarnAsError=true` 0 警告、Editor `--selftest` 両構成・Server `--selftest` で新しい FAIL 0、`check_rules.ps1` 0。

## 検証コマンド
- `msbuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` と Release
- `bin\x64\Debug\Editor.exe --selftest` / `bin\x64\Release\Editor.exe --selftest` / `bin\x64\Release\Server.exe --selftest`
- `tools\check_rules.ps1`
- `tools\replay_verify.bat` (既存ジョブの不変確認)

## 実装メモ (coder が追記)

### SELF_EVAL: sub-01 (round 1)

実装:
- `src\Engine\Engine\AI\BlackboardLibrary.{h,cpp}`: `.bb.json` (Bool / Int / Float / Vector / Entity、初期値、eventName、64 キー・名前 63 バイト・重複禁止)。`blackboard::` 注入、`shared_ptr` で登録。
- `src\Engine\Engine\AI\BehaviorTreeLibrary.{h,cpp}`: `.bt.json` の読み書きと `BtLinkAsset` (id 重複・子参照・子の数・親 2 つ・循環・深さ 64・根の親・ノード 1024 を失敗)。ノード種類表 `BtNodeTypeInfo` (名前・分類・子の数・パラメータ記述子)。Selector / Sequence / SimpleParallel / Wait。`behaviortree::` 注入。
- `src\Engine\Engine\AI\BehaviorTreeSystem.{h,cpp}`: `Update` (キー順、1 体ずつ、UE 方式の再訪)、Composite 3 種 + Wait、`AbortNode` (深い方から)、`kBtMaxStepsPerTick`、ライフサイクル (無効化・読み直し・外れた / 消えた・AssetMissing)、`SaveSnapshot` / `ReadSnapshot` / `ApplySnapshot` / `StateHash`。
- `Components.{h,cpp}`: `BehaviorTreeComponent` (TypeId 78、末尾 append) と `btstatus`。`EditorComponentCatalog.cpp` に登録。
- `SimSnapshot.{h,cpp}`: `'BT01'` 節、`SimRefs::behaviorTree`、`SimSourcesOf` に引数追加、`kSimSnapshotVersion` 34。`WorldHasher.{h,cpp}`: `SimSources::behaviorTree` (内容ゲート)。
- `TickRunner.{h,cpp}`: フェーズ 3.4a2 (知覚の後・Nav の前)、`TickServices::behaviorTree`、シーン遷移で `Reset`。`EngineLoop.cpp` / `HeadlessSim.cpp`: 実体・両ライブラリ・配線。`SimInit.{h,cpp}`: `SimLibraries` へ `behaviorTrees` / `blackboards` を末尾追加。
- アセット種別の配線: `AssetDatabase.{h,cpp}` (AssetType 末尾 2 つ・分類・名前)、`AssetOps.{h,cpp}` (Create 2 種・kCompound)、`DemoContent.cpp` (起動走査)、`ReloadHub.{h,cpp}` (行・種別・読み直し)、`StageClassifier.cpp`、`EditorWidgets.cpp` (アイコン)、`AssetBrowserWindow.cpp` (Create メニュー・型フィルタ・タイルの語)、`InspectorWindow.{h,cpp}` (`tree` 欄のピッカー、BehaviorTree の注意表示)、`LocalizationTable.inl` (en / ja)。
- SelfTest: `BehaviorTreeSelfTest.{h,cpp}` を新設して `EditorMain.cpp` と `ServerSelfTest.cpp` に登録。`AssetDatabaseSelfTest` / `AssetOpsSelfTest` / `ReloadHubSelfTest` に追加項目。`AcousticAudioSelfTest` の版を 34 へ。
- `docs\adr\ADR-025-behavior-tree.md` (下書き): 表の形・却下案・実行モデル・ライフサイクル・計測値。

仕様との差分:
- [未実装] Decorator の評価・Abort の「枠」のうち、Decorator を評価する呼び出し位置は作っていない。Decorator の種類が 1 つも無い今は空の関数になる (AGENTS.md の「未使用コードを足さない」)。作ったのは `AbortNode` (子孫を先に抜ける経路。SimpleParallel の Immediate・無効化・読み直しが実際に通る) と手数の上限。`.bt.json` の `decorators` は空配列だけを許し、中身があれば未知の type と同じく読み込み失敗。sub-02 が `BtNodeDef` に Decorator を足し、`stateSlotCount` に Decorator の欄を足す。
- [追加] 数値の上限: ノード 1024 (`kBtMaxNodes`)、根からの深さ 64 (`kBtMaxDepth`、実行器が再帰で降りるのでスタックを守る)、tick のパラメータ 216000 (`kBtMaxTicksParam`、60 Hz で 1 時間)。spec に数値が無いため。
- [追加] 読み込みの厳しさ: パラメータの型違い・未知の列挙名は読み込み失敗 (手書きの綴り間違いを既定値にしない)、範囲外の数値は範囲へ丸める、未知のパラメータ名は無視。SimpleParallel は子がちょうど 2 つ、葉は 0 個、Selector / Sequence は 0 個以上。
- [追加] Wait の数え方: 入った tick から ticks tick 後に Success (ticks <= 0 は入った tick に Success)。spec の「ticks 待って」の起点を決めた。
- [追加] 木が引けない・未設定の扱い: tree が 0 は Idle (警告なし、表を作らない)、未登録の GUID または木が指す BB が未登録は AssetMissing (1 GUID につき 1 回警告)。一度も有効になっていないコンポーネントは表を作らない (有効になった tick に作る)。木の `root` が -1 (空の木) は Idle で表だけ残る。
- [追加] 木と BB を `shared_ptr` で保持し、「登録が置き換わった」を同一性で検出する。ReloadHub が同じ GUID を読み直したとき、古い木の形のまま実行中のノードを Abort してから新しい形に作り直すため (新しい形で古い状態を Abort すると添字がずれる)。BT 節には入れない導出値。
- [追加] `lastAbortTick` は Int32 (-1 = まだ無い)。`UInt64` は「無し」を表す値が無く、Inspector で見づらいため。2^31 tick (約 414 日) で飽和する。
- [追加] `BtNodeState` は全ノード共通の固定長 (active / phase / child / counter、10 バイト) にした。spec の「固定長の小さな POD + C++ タスク用の可変長」の前者。可変長は sub-11 で足す (そのとき版を上げる)。
- [追加] Inspector: BehaviorTree の「木が未設定 / 登録されていない」と AgentBrain 同居の警告 (spec 2. #14 の指定)、ピッカーの `tree` 欄 (`fname == "tree"`)。AssetBrowser の Create メニューに 2 項目。新規の木は根に Selector を 1 つ置いた形。
- [追加] SelfTest: 起動走査 (`RegisterAssetLibraries`) が両アセットを登録することを `BehaviorTreeSelfTest` で確認。`Scene::Clear` が世代を進めるので、Play の開始・終了で同じ index に作り直したエンティティが前の表を引き継がないこと (専用の検出は不要と判断。いったん「Idle なのに実行中のノードがある表を捨てる」検出を入れたが、`World::Clear` を読んで不要と分かり消した) もテスト。

検証:
- `msbuild MyEngine.sln /p:Configuration=Debug /p:Platform=x64 /p:MyeWarnAsError=true` → 警告 0・エラー 0。同 Release → 警告 0・エラー 0。
- `bin\x64\Debug\Editor.exe --selftest` → 最終版で exit 0 (BehaviorTree ALL PASS、TypeId = 78)。**同じ実装での 1 回目は exit 1** で、Fracture editor の weight cache 3 項目と Server/client net の V1 (LoadPersist / LoadGame) 2 項目が FAIL した。同じバイナリでの 2 回目と、検査を足した後の再ビルドでの 3 回目は FAIL 0。plans の台帳 (m81 / m82) に既知の flake として同じ 2 件がある。再現しないので BT との関係は無いと判断したが、原因は調べていない。
- `bin\x64\Release\Editor.exe --selftest` → exit 0。
- `bin\x64\Release\Server.exe --selftest` → exit 0 (BehaviorTree の項目 PASS)。`bin\x64\Debug\Server.exe --selftest` → exit 0。
- `tools\check_rules.ps1` → 0 error / 0 warning。
- `tools\replay_verify.bat` → 16 ジョブ全 PASS (11 シーン、snapshot 往復・巻き戻し・What-if・規則検査を含む)。golden の撮り直しなし。
- 計測 (`BehaviorTreeSelfTest`、100 体 × 30 ノード、200 tick 平均): Release の `Update` = 17 µs / tick (基準 500 µs)、`StateHash` = 109 µs。Debug は 794 µs / 1705 µs。ADR-025 下書きに記載。
- 未実行: shot_verify (描画に触れていない。golden `bt` は sub-13)。エディタ GUI の実操作 (Create メニュー・型フィルタ・Inspector の注意表示・ピッカー) は未確認で、コードが通ることと SelfTest の範囲 (作成 → 改名 → 読み直し) だけ。Runtime.exe の実起動も未確認 (ビルドと replay_verify の Server.exe / Debug / Release の再生は通っている)。
- UE の公式ドキュメントとの照合: 試したが不可 (dev.epicgames.com は JS 描画で本文が取れず、docs.unrealengine.com は 403)。照合できていない。SimpleParallel の Immediate / Delayed と「背景が先に終わったら背景だけやり直す」は私の記憶でも spec 4.1.1 と同じ。

自己採点 (1-5):
  仕様適合: 4 — Decorator を評価する呼び出し位置だけ意図的に未作成 (差分に明記)。それ以外の受け入れ条件 1〜5 は SelfTest と replay_verify で確認した。
  正しさ: 4 — 復元の連続実行一致 (BB・Wait の残り・Parallel の状態・RNG、毎 tick のハッシュ)、存在ゲート、ライフサイクルを実走で確認。UE の規則は記憶ベース、GUI 未確認。
  コード品質: 4 — NavFilter / Nav の前例に合わせた構成。`BtNodeState` の共通欄は後続サブで種類が増えると窮屈になる可能性がある (不安に記載)。
  テスト: 4 — Composite・Wait・手数上限・ライフサイクル・BT 節・壊れた節の拒否・往復・防波堤を網羅。GUI の経路 (メニュー・Inspector) は未確認。

不安・質問:
- `BtNodeState` を固定 10 バイトの共通欄にした。sub-03 (MoveTo の目的地・observeTarget の前回位置 = float 3 つ)、sub-07 (Patrol の次の点と向き)、sub-04 (SearchArea の点の列) は欄が足りないので、各サブで名前付きの欄を足す (BT 節の書式と版が動く) 前提。sub-04 の「点の列」は固定長の配列 (pointCount の上限) になりそう。planner が上限を決めるか、可変長を sub-04 で入れるかを sub-04 の仕様で決めてほしい。
- spec 4.2 の「Decorator は 1 つにつき 1 欄」を `stateSlotCount` で表す想定にしたので、sub-02 は `BtLinkAsset` の `stateSlotCount` と `BtNodeDef` に Decorator の欄を足す。Cooldown は「ノードが終わった後も残る」状態なので、ノード側ではなく Decorator 側の欄に置く前提。
- SimpleParallel の左が Task でなくても実行器は動く (検査エラーはエディタの検査 = sub-09)。読み込みでは拒否していない。
- 台帳の既知 flake (Fracture weight cache / Server-client net V1) が今回の Debug の 1 回目に出た。BT とは無関係に見えるが、確認できたのは「再実行で消える」ことだけ。

触ったファイル:
- 新規: `src\Engine\Engine\AI\BlackboardLibrary.h`, `BlackboardLibrary.cpp`, `BehaviorTreeLibrary.h`, `BehaviorTreeLibrary.cpp`, `BehaviorTreeSystem.h`, `BehaviorTreeSystem.cpp`, `BehaviorTreeSelfTest.h`, `BehaviorTreeSelfTest.cpp`、`docs\adr\ADR-025-behavior-tree.md`
- 変更: `src\Engine\Core\Ecs\Components.h`, `Components.cpp`, `src\Engine\Core\Localization\LocalizationTable.inl`, `src\Engine\Engine\Asset\AssetDatabase.h`, `AssetDatabase.cpp`, `AssetDatabaseSelfTest.cpp`, `src\Engine\Engine\Audio\Spatial\AcousticAudioSelfTest.cpp`, `src\Engine\Engine\Demo\DemoContent.cpp`, `src\Engine\Engine\HotReload\ReloadHub.h`, `ReloadHub.cpp`, `ReloadHubSelfTest.cpp`, `src\Engine\Engine\Loop\EngineLoop.cpp`, `HeadlessSim.cpp`, `SimInit.h`, `SimInit.cpp`, `TickRunner.h`, `TickRunner.cpp`, `src\Engine\Engine\Replay\SimSnapshot.h`, `SimSnapshot.cpp`, `WorldHasher.h`, `WorldHasher.cpp`, `src\Editor\App\EditorMain.cpp`, `src\Editor\Asset\AssetOps.h`, `AssetOps.cpp`, `AssetOpsSelfTest.cpp`, `src\Editor\SourceControl\StageClassifier.cpp`, `src\Editor\Widgets\EditorComponentCatalog.cpp`, `EditorWidgets.cpp`, `src\Editor\Windows\Asset\AssetBrowserWindow.cpp`, `src\Editor\Windows\Scene\InspectorWindow.h`, `InspectorWindow.cpp`, `src\Server\ServerSelfTest.cpp`
- 生成物 (`tools\gen_project_files.ps1` を `pwsh` で実行して更新。M84c も同じ 2 つをコミットしている): `build\Engine.vcxproj`, `build\Engine.vcxproj.filters`

申し送り:
- sub-02 へ: Decorator は `BtNodeDef` に足し、`BtLinkAsset` が `stateSlotCount` を数える。評価は `VisitSequenceOrSelector` など各 `Visit*` の入口 (子へ入る前) と、毎 tick の監視 (`StepOwner` の `Visit(root)` の前) に置く。`AbortNode` は子孫が先で、ノード固有の後始末 (Cooldown の計時開始など) はその関数の `Finish(state)` の前に足す。`ActiveLeafId` は active な子を左から探す。
- `gen_project_files.ps1` は Windows PowerShell 5.1 では構文エラー。`pwsh` で実行する。
- `Scene::Clear` / `World::Clear` は使った index の世代を進める。表のキーが (index, generation) なのでエディタの Play の開始・終了で前の表は自然に落ちる (Nav の slots も同じ前提で守られているはず)。
- 台帳 / ロードマップの TypeId の予定は合っていた (78)。`kSimSnapshotVersion` 34 で、次にコンポーネントか BT 節を変えるサブが 35 にする。

## フィードバック履歴
- round 1: VERDICT OK (planner)。Decorator の評価位置を sub-02 へ回したのは spec の「枠だけ」の範囲内として承認 (未使用コードを置かない判断は AGENTS.md 5 章に合う)。追加の上限 (ノード 1024・深さ 64・tick パラメータ 216000) と Wait の起点・AssetMissing の扱いは承認し、ADR-025 で確定させる。不安 1 (状態欄の窮屈さ) は sub-03 / sub-04 に方針を書いた。Debug selftest 1 回目の FAIL 5 件は台帳の既知 flake (Fracture weight cache / net V1) として申し送り
