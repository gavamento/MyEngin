# エンジン中核 (src/Engine/Engine 直下) の経緯
コードのコメントから移した経緯。コードには今の事実と罠だけを残している。

## CollisionSystem.cpp / CollisionSystem.h — トリガーの境界と対象
- (M7 → M28a) 境界 (ちょうど接触 = 距離が厳密に一致) は M7 では「重なり含み」だった。M28a でソリッド判定と同じ「重なりのみ true」にそろえた。float が完全に一致するのは測度ゼロの事象なので、実挙動の差は無かった。
- (M28c) ソリッド同士のペアにもトリガーイベントを配信していたのを直した。ペアの少なくとも片方が isTrigger のものだけを配信し、ソリッド同士は OnCollision 系が扱う。

## FbxLoader.cpp — テクスチャ解決の失敗 (P2)
- (P2) 解決できないテクスチャは黙って White に落ちていた。空 ID を返し、試したパスを WARN に出すようにした。

## ModelLoader.h / DemoContent.cpp — 保存済みシーンのモデル登録 (M48g / M50a)
- (M48g) 保存済みシーンをロードする経路は `Load` を通らないので、SkinnedMesh.model が指す AssetID が誰にも登録されず、ポーズ評価 (骨追従・ボーンパレット) が丸ごと落ちていた。スケルトンだけを登録する RegisterSkinnedModels を足した。
- (M50a) M48g はスケルトンだけだったので、同じ経路で MeshRenderer.mesh / .material の実体も登録されず、モデルが描画されなかった。起動時のアセット走査 (RegisterAssetLibraries) で全モデルを RegisterAssets からヘッドレス登録するようにし、DemoContent の BoxTextured.glb の単発登録は消した。

## SchemaComponents.h / SchemaCodegen.h — スキーマ型のスクリプトアクセス
- (M48j) スキーマ由来コンポーネントの v1 は、スクリプトからフィールドを読み書きする汎用 ABI を持たず、用途は「オーサリング + 保存 + ハッシュ被覆」までだった。ADR-011 は型付きアクセサを「M49+」としていた。
- (M50d) SchemaCodegen がその型付きアクセサの回収にあたる。

## SceneSerializer.cpp — 文書 version v3 (M50c)
- (M50c) v3 は「キー不在 = ベース追随」を構造へ拡張した文書であることの宣言として上げた。値の書式は v2 と同一で、当時の旧エンジンは version を読まないので素通しでロードできた。版ごとの意味は Scene.h の kDocVersion にある。

## TickRunner.h / TickRunner.cpp — RunOneTick の抽出 (M52d)
- (M52d) RunOneTick は EngineLoop のフレームループの tick 本体をそのまま抜き出したもの。参照名は当時のまま TickServices から束ね直し、差分を「移動」に留めて、抽出が挙動を変えていないことを読めるようにした。合格条件は「3 ペアの replay_verify がビット一致すること」ただ 1 つだった。

## TickRunner.cpp — プレハブ生成の親判定 (M70d)
- (M70d, dogfooding #4) 親の有無を「index も generation も 0 でなければ親あり」で判定していた。(a) 自然に書ける MyeEntityId{} (null id、index 0xFFFFFFFF) が「親あり」に分類され、EnsureFileId が死んだエンティティに 0 を返すという 2 段の偶然でルート生成になっていた。(b) 最初に作られた実在エンティティ {0,0} を親に渡すと黙ってルート生成になっていた (こちらは本物のバグ)。null id そのもので判定するようにした。

## EngineLoop.cpp — 撮影モードの入力中立化 (M68c / M70c)
- (M68b) 撮影中に机のマウスが動くと acoustic の golden が割れた。WatcherFpsCamera (M65g) がマウスデルタを yaw に積分して MeshRenderer 付きのプレイヤーの箱を回すため。実測: acoustic_deferred が maxDiff=125 / 520 px、worst pixel は部屋 A の隅の箱。
- (M68c) 生デバイス由来のマウスデルタだけを 0 にした。キーボードとマウス位置は、位置に依存する golden が無いことを確認していなかったので、効く範囲を最小に留めて触らなかった。合成入力 (--synth-input) と .rep の記録入力は後段でレーンごと置換されるので、replay 7 ペア目 (記録側 --synth-input) の視点角の被覆は失っていない。
- (M70c) hovered / pressed がボタンのハイライトを決めるようになり、撮影中にカーソルが窓の上にあるだけで golden が割れるようになった (ui_probe と flow_title はボタンを含む)。マウスの位置とボタンも中立化した。位置は 0,0 (左上の正当な座標) ではなく、負の座標に置かれた要素にも当たらない値にした。

## EngineLoop.cpp — tick 末ハッシュの共有 (M52i)
- (M52f 申し送り 6 → M52i) M52f までは tick 末のワールドハッシュを消費者 (クラッシュリング / タイムトラベル) ごとに撮っていて、両方 on だと同じ tick で 2 回走っていた (実測 約 0.2ms/回)。ロールバックが毎 tick ハッシュを要求するようになったので、3 者が同じ 1 個を使う形に畳んだ。

## EngineLoop.cpp — ゲーム面のキャンバス換算 (M75b)
- (M75b) それまでは EngineLoop が CanvasSize を解いて正規化済みの値を sim へ渡していた。換算は sim 側の uilayout::CanvasOfInput へ移した。

## LightSelection.h / RenderSystem.cpp — ライト選別 (M54b)
- (M54b) それまでのライト収集はカリングもソートも無い「登録順の先着 16 本」で、点光源が 16 本並んだシーンで太陽が落ちえた。type 優先の決定論キーでソートする純関数として RenderSystem から LightSelection へ切り出した。理由は 2 つ: M54c のシャドウアトラスが影を投げるライト列の frame 間安定を要求すること (安定性を保証する場所は 1 箇所でないと守れない)、描画ロードマップ (M54〜M58) で唯一まともに selftest 化できる論理だったこと。視錐台の計算も収集ブロックの中から CollectLights の頭へ引き上げた。
- (M54b) ambient を選別後の先頭ではなく走査順の先頭のライトから取るのは、M54b の前の挙動を 1 ビットも変えないため。
- (M54b) kMaxShadowLights = 4 は、M54d の実測で見直す前提の暫定値として置いた。

## RenderSystem.cpp / RenderSystem.h / EngineLoop.h — 消費者より先に置いた配線と目視口
- (M55c) velocity バッファの可視化 (--velocity-debug) は、velocity を読む本番の消費者 (M55d TAA / M55e / M55f) が入るまでの唯一の目視口として足した。
- (M56c) HZB の可視化 (--hzb-debug) も、HZB を読む SSR (M56d) が入るまでの唯一の目視口だった。
- (M57b / M57c) フロクセルは積分結果を読む者が居ないまま配線した (on にしても絵は変わらなかった)。GPU コストを実シーンで測り、値を読み戻して検査できるようにするため。M57c は「積分結果を読む者が居ない段階で golden を撮らない」とした。最終画像への合成は M57d (Deferred) / M57e (Forward)。
- (M57d) ゴッドレイの自動 off に path.AppliesFroxel() を条件として入れたのは、Forward がまだ積分結果を読まなかった (M57e で合成が入った) ため。SRV の有無だけで判定すると「ゴッドレイだけ消えて霧が増えない」になる。
- (M65d) 音響の残光ボリュームは、消費者 (M65e のライティング) が居ない段階で配線した。転送の正しさは `--acoustic-dump` の読み戻し (CPU 側の残光配列とバイト比較) で数値として主張した (M57c と同じ流儀)。

## DemoContent.cpp / DemoContent.h — 音響ショーケース (M65b〜M65f)
- (M65b) シーンは絵に出ない (デバッグ線でしか見えない) 段階で先に置いた。replay 7 ペア目の被写体 = 伝播のハッシュ被覆を確保するため。ライティングへの差し込みは M65e。聴者も M65b では鏡が空のまま、デバッグ線の被写体として置いた。「いつ・どこから聞こえたか」が入ったのは M65f。
- (M65e) golden をこのシーンで初めて撮るのに合わせて、カメラを (26,-17) → (20,-13) へ寄せた。
- (M65c → M65f) 床材タイルは M65c で天面 0.45 に置いていたが、M65f で敵が廊下へ入れない原因になった (probe で発見。0.45m の段差は collide-and-slide では登れない)。天面を 0.05 へ下げた。
- (M65c) 歩行者の初期高さ 1.35 は「天面 0.45 + カプセル半長 0.9」で決めた値で、落として馴染ませるより初期値で載せる意図だった。タイルを 0.05 へ下げた後も 1.35 のままなので、今は 0.4m 上から始まる。

## DemoContent.cpp — ジョイントショーケースの食い込み対策 (M60i / M60j)
- (M60i) 関節で繋がった相手と食い込むと永久に押し合う (溶接の相手なら接触ソルバが毎 tick 押し返して静止しない) ので、ロープの鎖は 0.4、溶接の腕は 0.45 へコライダーを縮めて幾何で逃げていた。
- (M60j) `Joint.disableCollision` が入ったので、繋がったペアだけ候補から落とす形に替え、コライダーを見た目どおりの 0.5 に戻した。M60k までは disableCollision を踏むのが selftest だけだった (ロープがその replay / golden 被覆)。

## RagdollBuilder.h — 置き場所 (M60i)
- (M60i) ラグドール生成器を `src\Editor\` から Engine 層へ移した。--joint-demo のラグドールを DemoContent が組むため (Editor 層のままでは Runtime.exe が同じシーンを組めず、golden は Runtime で撮る)。中身は元から Engine 層の型しか触っていなかったので、移動は include の付け替えだけで済んだ。これで生成器が吐く階層が replay 6 ペア目の被写体になった。

## DemoContent.cpp / DemoContent.h — golden 被写体の変遷
- (M54e) レンダーショーケースのスポットで初めて局所影 (castShadow = 1) を立てた。M54b〜M54d の間は既定 0 のままにしてあったので、golden 8 枚は機能を足しても絵が動かずビット一致し続けた。M54e のコミットで動いたのは demo_render_* の 2 枚だけ。
- (M57追補) M57e まで、GPU パーティクル描画経路と VfxRenderer (Sprite / Trail / TextMesh) はどちらも golden に 1 枚も写っておらず、壊れても 14 枚が全部緑のまま通っていた。GPU バックエンドは GUI と設定ファイルからしか選べず、--screenshot で撮る手段も無かった。--fog-demo と --particle-backend の CLI 上書きはその穴埋め。
- (M42追補) GPU パーティクルは長らく begin→end の 2 点線形しか持っておらず、中間キーを丸ごと無視していた。煙エミッタの中間キーはその CPU/GPU 一致をピクセルで担保する被覆として置いた。
- (M63d) パーティクルショーケースの左端に mode=1 のライティングエミッタを足したとき、既存 5 本は 1 つも動かさなかった (元から x_px 230 より左は空白だった)。
- (M70b) フローデモの UI の x/y/w/h/fontScale を、960x540 相当の実 px からキャンバス単位 (基準 1920x1080) へ 2 倍した。960x540 では canvasScale がちょうど 0.5 なので、絵は IEEE754 でビット一致した。
- (M70c) タイトルのヒント (TitleHint) は anchor 4 + y=240 で、真下の TitleBest と文字が重なっていた (golden にもそのまま写っていた)。ボタンを足して画面の下半分が埋まったのを機に anchor 7 (下中央) へ移した。
- (M74a) モデル由来のサブアセット ID は M74a の前は絶対パス由来で、保存物がチェックアウト先に依存した。ジョイントショーケースを cache\ へ置いて毎回組む理由の 1 つだった。
- (M75c) --ui-demo は M75c の時点では Canvas Scaler の 3 モードと Canvas の sortOrder だけで、M75e (自動レイアウト) / M75f (ウィジェット) を末尾に積み増した。

## Prefab.cpp — プレハブ読み込みの検証 (M48d)
- (M48d) 宣言キー (actor:1 / prefab:1) を一切見ずに entities だけ拾っていたので、別種の .json (シーンやマテリアル) を渡しても「エンティティ 0 件のプレハブ」として静かに登録されていた。
- (M48d) ベースに複数のルートがあると、最後のルートだけをタグ付けして残りが野良になっていた。ラッパーで包むようにした。
