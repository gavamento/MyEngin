# ソース管理・公開 API・C# スクリプト・Runtime・ツールの経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## src/Shared/EngineAPI.h — MYE_API_VERSION の版ごとの経緯
- (v3 / M19) gamepad / Raycast / PlaySound / StopSound / LoadScene をスロット予約で一括追加した (実装は M19.3 / M19.4 / M20)。
- (v4 / M28a) 剛体操作 (AddForce/AddImpulse/AddTorque/Get/SetVelocity) + 空間クエリ (OverlapSphere/OverlapBox/SphereCast) + OnCollision コールバックを一括で予約した。AddTorque は M28b、Overlap*/SphereCast と OnCollision の配信は M28c で実装。
- (v5 / M29b) キャラクターコントローラ操作 (CharacterMove/Jump/IsGrounded/GetVelocity) と SetTextMeshText を一括追加。SetTextMeshText はスロットだけ予約し、M29c の TextMesh で実装した。
- (v6 / M32f) エフェクト制御 (EmitterBurst/SetEmitterPlaying/RestartEffect/PlayEffect) を一括追加。
- (v7 / M37) Instantiate (fileId 予約方式) / FindByFileId / AnimatorParam / 動的 UI (SetUIText/Fill/Color/Focused + UIFocusNav) / DebugDrawLine / マスク付きクエリを追加。
- (v8 / M45) オーディオ操作一式をスロット予約で一括追加 (実装は M45g)。v3 の PlaySound/StopSound はシグネチャを変えずに残した — Interop.cs が位置ベースでミラーしているため、既存スロットをいじると C# 側が全てずれる。
- (v9 / M48h) 部位 (ソケット) クエリ FindPart / FindPartsByTag を追加。取り付けは既存の SetParent、位置取得は既存の Transform getter で足りるので、新スロットは 2 本だけにした。
- (v10 / M49) 部位ボリューム (PartBounds) へのレイキャスト RaycastParts を追加。root / tag のフィルタ込みで 1 本に収めた (root null = シーン全体、tag 0 = 全部位)。
- (v11 / M50d) 汎用フィールドアクセス GetComponentField / SetComponentField を追加。コンポーネント名 / フィールド名の FNV-1a 64bit ハッシュで任意コンポーネント (組込み / スキーマ / C++ スクリプト) の登録フィールドを値コピーで読み書きする。スキーマ codegen (SchemaComponents.gen.h / Schema.gen.cs) の呼び先で、型ごとのスロットを増やさずに済ませるための 2 本 (家風: 足す本数を減らす)。
- (v12 / M51h) M51 のエンジン内機能 (d 入力アクション / e UI / g ゲームフロー) を 14 本で一括開通した (M48h の「束ねて 1 回だけ bump」運用)。GetMouseWheel / UI 矩形・レイアウト・テクスチャ・ヒットテスト / アクションマップ 2 本 / TimeControl 2 本 / PersistSet・Get / SaveGame・LoadGame / SetPadVibration。ミラー照合を check_rules.ps1 規則 11 に載せたのもこの版。
- (v13 / M52i) ネット対戦 (2 人 P2P + 予測ロールバック) の状態参照 5 本 + 入力アクションのレーン指定版 2 本。Net* の 5 本は機種依存の値 (自分がどちら側か / ping / 巻き戻し回数) を返すので、描画レーンでだけ読む前提にした。誤用は M52i の desync 検出が毎 tick 見張り、静かに壊れるのではなく desync バンドルが出て止まる形で表面化する。レーン指定版は v12 の GetActionState / GetAxisValue と同じ評価結果を引くだけ (InputActions がレーン major で全 kMaxPlayers 本を持つ) で、既存 2 本は「レーン 0」の別名として 1 文字も変えずに残した。
- (v14 / M59k) 超リアル物理 (M59) の入口 8 本を束ねて 1 回で開通した (「ABI は各マイルストーン末に 1 回」の運用)。付け外し 2 本 / AddForceAtPosition / GetContactInfo / SampleWind / SampleTerrainHeight / スリープ 2 本。M59 の新機能はすべて「コンポーネントを付けたら効く」存在ゲートなので、構造的な ON/OFF は AddComponentByName / RemoveComponentByName、フィールド粒度の ON/OFF は v11 の SetComponentField が担当し、専用スロットは足さないと決めた (決定台帳 10)。
- (v15 / M64a) マウスルック 2 本。GameEngin_Demo のドッグフーディングで「一人称の視点をマウスで回せない」ことが分かって足した穴埋め。InputSnapshot は絶対座標しか持たず、カーソルを画面内へ留める手段も無かったので、窓の端で視点が止まっていた。GetMouseDelta (決定論レーン = InputSnapshot 由来) と SetCursorMode (出力レーン = SetPadVibration と同格) を対で開通し、この非対称に意味を持たせた: デルタは .rep に載る sim 入力、カーソルの掴みは載せてはいけない機種依存の副作用で、後者を sim から読み返す口は作らない。
- (v16 / M70c) UI の対話 (UIButtonState / UIGetFocused / UISetFocused / MouseCanvasPos / GetUIRect) + LoadPersist。版履歴のコメントにはこの版の行が抜けていた。
- (v17 / M71a) GetSceneName 1 本。シーン遷移 (v3 LoadScene) は M19.4 から動いていたが、スクリプトが「今どのシーンに居るか」を知る口が無く、遷移先を決める材料が常にスクリプト側の外部知識だった (三校のステージ進行で詰まった)。返すのをパスではなく sceneName にしたのは、SourcePath() が assets ルート込みの絶対パスでチェックアウト先ごとに変わるため。これに伴い Scene::name_ が sim の分岐に使う状態へ昇格したので、SimSnapshot v15 で撮る対象に加えた (載せないとタイムトラベルと .rep 埋め込みスナップショットで名前だけ古いまま復元される)。
- (v18 / 2026-09-15) IsDevelopmentRun 1 本。三校で、全体照明 (F3 / パッド Back) や視点の切り替えといったデバッグ操作が、書き出した exe でもそのまま効いていた。GameLogic.dll はエディタと配布物で同じ 1 本で、`#ifdef _DEBUG` でのロジック分岐は決定論の規則で禁止なので、実行時の値にした。判定は他の二経路と同じ projectRoot の有無 (Runtime だけが引数を読み終えてから決める。Editor は常に 1)。プロセスの定数で sim 状態ではないので .rep / SimSnapshot には載せていない。載せる案は .rep の版上げを伴うので見送り、配布物で記録した .rep は配布物の Runtime (--project 無し) で検証する、と注記した。M75 の計画が v18 を予約していたが M75h は未着手だったので、そちらを v19 へ繰り下げた。
- (v20 / 2026-09-17) 汎用タグ 4 本 (TagIndex / HasTag / SetTag / FindEntitiesWithTag)。RT を「特定のタグを付けた物にだけ」適用するために TagComponent (TypeId 62、タグ番号 0..63 のビット集合) を足し、同じタグをゲーム側の分類にも使えるようスクリプトへ開通した。名前ではなく番号を実体にしたのは物理レイヤーと同じ理由 (名前を変えてもシーンの参照が切れない)。HasTag / FindEntitiesWithTag は自分のタグだけを見る (Unity の Tag と同じ) が、描画の RT フィルタは祖先のタグも継承して見る — FBX のメッシュは子エンティティに分かれるので、ルートに付けたタグが子に効かないと使えないため。検索結果は EntityID の index 昇順に固定した (アーキタイプの行順は構造変更の履歴で変わるので、そのまま返すとスナップショット復元で答えがずれる)。v19 までの C# ラッパは使い手が無く足していなかったが、今回は Engine / MyeScript の両方に足した。

## src/Shared/EngineAPI.h / ScriptAPI.h / src/GameLogic/Scripts/UIButtonDemo.cpp — UI の対話をエンジンへ寄せた (M70c)
- M70b までは「押されたか」がエンジン内 (UIRenderer のハイライト計算) にしか無く、ゲーム側は UIElement と同じ矩形をスクリプトに手書きして自前でヒットテストしていた。UIButtonDemo も btnX/btnY/btnW/btnH の 4 フィールドで矩形を二重に持っていた。
- この形は UIElement 側のレイアウトを動かすと絵と当たり判定が黙って食い違い、アンカーを 0 (左上) 以外にした瞬間に成立しなくなるうえ、M70b で数値がキャンバス単位になってからは実 px との対応も崩れていた。M70c で矩形の解決・押下判定・フォーカスをエンジンへ 1 本化し (状態は WorldHash 対象)、スクリプトは MyeUIClicked で結果を読むだけにした。
- M70b 時点のヒットテストのコメントは「キャンバス座標のマウス (MouseCanvasPos) は M70c で足す」だった。M70c で v16 として足した。
- 「UI は write-only。読み取りは今後も追加しない」(決定台帳 3) は、v16 で GetUIRect (解決済みの矩形) を足した時点で事実でなくなった。

## src/Shared/ScriptTypes.h — Inspector 用メタデータ (v16 / M70c・M70d)
- MyeScriptField の displayName / rangeMin / rangeMax は v16 (M70c) でレイアウトだけ予約し、中身を読むのは M70d からだった。先にレイアウトだけ置いたのは、ABI bump と同じコミットで固定するため — 後から足すと apiVersion が一致したまま別レイアウトの GameLogic.dll が受理される。

## src/Shared/ScriptAPI.h / src/GameLogic/Scripts/AudioDemo.cpp — 登録フィールドの上限 16 → 32 (M70d)
- REGISTER_SCRIPT の登録フィールドは 16 本が上限だった。そのせいで AudioDemo が 9 個のキーのエッジ検出を int32_t のビットへ畳んでいた (= 上限が設計を歪めていた) ので、M70d で 32 に倍増した。AudioDemo のビット畳みは、1 語のほうが Inspector も snapshot も小さいのでそのまま残した。
- WatcherFpsCamera / WatcherThrowTool (M65g) のコメントは 16 本の頃に書かれ、上限 32 になった後も「16 本まで」のまま残っていた。

## src/Shared/ScriptAPI.h / src/Scripting/MyeScript.cs — M70d のドッグフーディングで直したもの
- MyePlaySoundHere / PlaySoundHere は v8 から**ローカル位置をワールド位置として**渡していたので、親を持つエンティティ (車輪・手に持った物・キャラの子ボーン) では鳴る場所が「親のワールド位置ぶん」ずれていた。原点付近の親では気づけない。M70d で GetWorldPosition を渡すよう直した。
- MyeGameObject の回転とスケールは書けるのに読めない非対称だった (dogfooding #18)。ABI スロットは v1 から 6 本とも埋まっていたので、足りなかったのは糖衣だけ。
- MyeNameHash は M70d で「スクリプト用ユーティリティ」節の先頭へ移した (MyeGameObject::GetWorldPosition が WorldMatrix を汎用フィールドアクセスで読むため、部位節より前で必要になった)。
- MyeSinRad / MyeCosRad は WatcherFpsCamera (M65g) が持っていた多項式を M70d で 1 命令も変えずに ScriptAPI.h へ引き上げたもの (視点角のビット列は M65g のまま)。角度を扱うスクリプトが増えるたびに多項式を書き写す形になっていた。
- C# の MyeScript.Tick: ネイティブは以前から Invoke で tick 番号を渡していたのに ScriptRuntime.cs が捨てていた。M70d でインスタンスへ渡すようにした。
- C# の AddComponent は v2 からエンジン内部にあったが C# へ露出しておらず、付けられるのに外せない / 確かめられない状態だったので、v14 で Remove / Has とまとめて開けた。

## src/GameLogic/Scripts/WatcherThrowTool.cpp — 囮をプレハブにしなかった (M65g)
- 計画本文は囮 (石 / 瓶) を prefab で作ると書いていたが、`.prefab.json` を経由すると PrefabInstance.prefabHash (ハッシュ対象) に正規化した絶対パスのハッシュが載り、sim 状態がチェックアウト先に依存する。Spawner.cpp の「CreateGameObject + SetMeshRenderer("builtin://...") + AddComponentByName」を採った。

## src/GameLogic/Scripts/WatcherFpsCamera.cpp — v15 の初の利用者 (M65g)
- GetMouseDelta / SetCursorMode は M64a で開通して以来、M65g の WatcherFpsCamera が初の利用者だった。replay の acoustic ペアに --synth-input を渡すことで、M64a の ABI に初めて実走の被覆が付いた。

## src/Editor/SourceControl/SourceControlSelfTest.cpp — staged 判定の空振り (M66 sub-12)
- 「None でなければ staged」で見る書き方だと、porcelain.rs が未追跡を '?' で返して indexState にも Untracked が入るため、未追跡が 1 個あるだけで検査ごと飛ぶ。fixture は必ず未追跡を持つので恒久検査が永久に空振りする。sub-12 round 1 の実行は実際に skip で通っており、round 2 の must として stagedForCommit (None と Untracked を除く) に直した。

## src/Editor/SourceControl/CollabClient.h — Shutdown の説明の食い違い
- ヘッダの設計要点は長く「Shutdown は destroy -> FreeLibrary の順。逆にすると走行中の worker のコードごとアンロードされる」と書いていたが、実装 (CollabClient.cpp の Shutdown) は FreeLibrary をわざと呼ばない。理由と実測 (checkout 直後の終了で 6 回に 1〜2 回 0xC0000005、外すと 12 回連続で落ちない) は .cpp 側にある。

## tools/collab_verify.ps1 — fixture をシナリオごとに作り直す (M66b)
- M66a では fixture を全シナリオで共用していた。N 本目の期待ファイルが 1..N-1 本目の実行結果に依存し、「新しいシナリオを足しただけで既存の期待が動く」「単体で 1 本流すと落ちる」壊れ方をしたので、M66b でシナリオ 1 本ごとに作り直す形へ変えた。

## tools/run_parallel.ps1 — chcp を戻し忘れた事故 (2026-08-27)
- ジョブの子 cmd を chcp 437 で起動する runner を経由した後、コードページが戻らないまま呼び出し元シェルに残り、同じコンソールを共有していた Claude Code の TUI 表示が文字化けした。手動で `chcp 65001` して復旧し、正常終了・異常終了どちらでも finally で必ず戻すようにした。

## tools/replay_verify.bat / tools/shot_verify.bat — parts シーンを毎回組み直す理由
- (M48g) parts シーンを保存ファイルにしなかった理由の 1 つは、モデル由来のサブアセット ID が絶対パスのハッシュで、保存した .scene.json がチェックアウト先に依存したことだった。M74a で .meta の GUID 由来になり、この理由は消えた。今も組み直すのは「版管理された唯一の正解は BuildPartsShowcaseScene」だから。

## tools/replay_verify.bat — 並列化と stress 統合
- (M52d) 素の Debug verify を回すのをやめ、snapshot stress 付き verify へ統合した。ハッシュ照合は素の verify と同一機構で毎 tick 走るので検出能力は同じ。
- 録画を --replay-fast で実時間から切り離す前は、600 tick = 実時間 10 秒 × 6 本を待っていた。導入時に遅い録画と fc /b で .rep のバイト一致を機械確認した。
- ヘッダの「並列プールで 9 ジョブ」「6 シーンチェーン」はシーンと What-if を足すうちに古くなった (今のジョブ一覧は -Jobs の引数が正本)。

## tools/shot_verify.bat — 撮影の追加と降格の経緯
- (M54a) 描画ショーケース 2 枚を足す前の 5 枚は平行光 1 本だけで組まれ、局所ライトの影 / デカール / SSR / プローブ / フロクセル / 地形はどれも既定でピクセル不変だった。この 2 枚が以降 27 サブの回帰の土台になった。
- (M56d) SSR の 1 枚は当初 CI 判定 (tol=3) に載せたが、CI run 32622063559 で降格した。実測は maxDiff=95 / tol=3 超えはわずか 30 画素 — 「広く薄く」ではなく「狭く極端に」違う形 (FXAA の 1 → 35 増幅と同型) で、差分ヒートマップも反射床と柱の輪郭に孤立した点が散る分岐反転の署名だった。tol を 95 まで上げると検出力がゼロになるので、FXAA / TAA / froxel と同じローカル限定 tol=0 の枠へ移した。ローカルでは maxDiff=0 なので検出力は落ちず、失ったのは「ランナー上でも SSR が同じ絵を出す」という主張だけ。
- (M57c / M57d) フロクセルの枠は M57c では撮らなかった。積分結果を読む者が 1 人も居ない段階で撮ると demo_render_deferred と tol=0 でビット一致する「同じ絵の 2 枚目」にしかならず、以後それが動いたときに原因が機能なのか撮影条件なのか切り分けられなくなるため。絵が初めて変わる M57d で撮った。
- (M57追補) 霧のショーケースを足すまで、GPU パーティクルのバックエンドは --screenshot で撮る手段が無く (エディタ GUI からしか選べなかった)、VFX 3 種 (Sprite / Trail / TextMesh) も当時の 14 枚のどれにも写っていなかった = どちらも壊れても全部緑のまま通っていた。M63a で CPU / GPU のパーティクルを 2 枚撮るのは、この穴を最初から開けないため。
- (M57追補) :shot の追加引数を 3 トークンから 4 トークンへ広げた。`--fog-demo --froxel --particle-backend gpu` で末尾の "gpu" が黙って落ち、CPU の絵が撮れていた。
- (M65e) M65a〜M65d は「存在ゲートの内側なので既存 17 枚が maxDiff=0」を主張し続けていたが、裏を返すと M65 の成果物は 4 サブぶん 1 画素も golden に写っていなかった。音響の 2 枚が波面伝播 / 床材 / 残光 / 転送 / 合成を初めて絵に固定した。
- (M67a) それまで 19 本の call :shot に --rt-* が 1 つも無く、rtReflEnabled を立てる口は --rt-refl とメニューだけ = RT 反射も RT GI も壊れて全 golden が緑のまま通る状態だった。「--rt-demo (コーネル箱) は WARP では重い」を RT レーン全体へ広げた結果で、M65 の「4 サブぶん golden に写っていなかった」と同根。M70b の UI キャンバス 2 枚も、ui_probe 1 枚だけでは非 1:1 スケールと非 16:9 の経路を 1 画素も通らないという同じ穴を塞いだもの。
- (M67d) 反射と GI が共有する rt_common.hlsli の放射輝度トレースを first-hit 版へ分解した (RtTraceRadianceFirstHit が本体、RtTraceRadianceLod はそれを呼ぶ薄いラッパ)。共有部分は今も 1 本なので GI 側の 1 枚が要る。
- (M67i / 2026-09-08) Hero の M 上限を 8 -> 16 にしたので demo_render_rtrefl_restir の 1 枚を差し替えた。このとき --update を使わず、比較 run が tests\actual\ に残した実物を 1 枚だけ tests\golden\ へコピーした。

## tools/check_rules.ps1 — 規則 9 の定数グループ
- (M55a) ライト配列長 (kMaxLights) とカスケード数は M17 / M38d 以来ずっと規則 9 に未登録の穴だったのを回収した。
- (M57c) フロクセル CS のスレッドグループの HLSL 側を froxel_common.hlsli 1 本にまとめた (clear / inject / temporal / integrate の 4 本が同じ割り方を要求するようになったため)。各 .cs.hlsl が #define を持たなくなったので照合先もそこへ移した。
- (M58d) 地形の cbuffer 宣言を terrain_common.hlsli へ移した (deferred / forward の 2 本が同じ地表を出すための共有点)。宣言が 1 箇所になったので照合先も 1 本になった。
- froxel::kSrvSlot のグループのコメントは長く「t13/t14 は M56 (SSR / 反射プローブ) の空席」と書いていたが、t14 は M56f の反射プローブ、t13 は M65e の音響の残光 (kGlowSrvSlot) で埋まっている (SRV スロットの経緯は renderer.md の DeferredPath.cpp の節)。
