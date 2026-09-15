# MyEngine 全機能ガイド

調査日: 2026-09-11。対象: `C:/HAKtokyo/My_Engin/MyEngin` の作業ツリー。

この文書は、MyEngine が何をできるエンジンなのかを、制作者・利用者・説明を聞く人に向けて整理したものです。実装の入口、コンポーネント、エディタ操作、検証手段を対応させています。詳細な列挙は [機能リファレンス](C:/HAKtokyo/My_Engin/MyEngin/docs/engine-feature-reference.md) を参照してください。

調査はソースコード、プロジェクト定義、シェーダ、既存仕様、テストコードの静的確認です。「実装あり」は実装と呼び出し経路を確認したという意味で、今回の実機動作試験の合格を意味しません。既存の未コミット変更も調査対象に含みます。ビルド、ゲーム起動、GPU・音声・通信試験は今回実行していません。既存文書にある性能値や過去のテスト合格数を、今回の測定結果としては扱いません。

## 1. エンジンの全体像

MyEngine は Windows 向けの C++20 / DirectX 11 製 3D ゲームエンジンです。オブジェクトを配置し、コンポーネントを加え、C++ または C# のスクリプトでゲームを作り、エディタを含まない Runtime として配布できます。

特徴は、ゲームの状態を決まった順番で更新する基盤と、その状態を記録・復元・比較する機能が一体になっていることです。リプレイ、タイムトラベル、予測ロールバック対戦、クラッシュ直前の再現に同じ基盤を使います。描画では通常のラスタライズに加え、DirectX 11 の Compute Shader による二次光線の追跡を持ちます。物理、音響伝播、音を聞く敵 AI もエンジン側にあります。

| 分野 | できること | 状態・主な境界 |
|---|---|---|
| ゲームの基盤 | ECS、階層、シーン、動的生成、入力、セーブ | 実装あり。セーブと sim スナップショットは別物 |
| スクリプト | C++ DLL リロード、C# ホスト、登録フィールド編集 | 実装あり。C# は決定論の検証対象外 |
| 描画 | Forward / Deferred、PBR、影、反射、GI、ポスト効果 | 実装あり。高度な効果には Deferred 限定・既定無効のものがある |
| 物理 | 剛体、衝突、関節、空力、浮力、車両、ラグドール | 実装あり。CC と剛体の相互作用には制約 |
| 変形体 | XPBD ロープと剛体への取り付け | 部分実装。布・ソフトボディ・粒子と世界の衝突は未実装 |
| 演出 | パーティクル、スプライト、トレイル、3D テキスト | 実装あり |
| 音響・AI | 3D 音声、残響、波面、音／光センサー、経路探索 | 実装あり。専用のグリッド・FSM モデル |
| 制作環境 | シーン編集、Inspector、地形、アニメーション、Git | 実装あり。Git と C# は追加ビルドが必要 |
| 再現デバッグ | リプレイ、巻き戻し、分岐、ゴースト、差分、クラッシュ記録 | 実装あり。保持予算と sim 状態の範囲に制約 |
| 配布・品質管理 | Runtime パッケージ、DDS、zip、自己テスト、画像回帰 | 実装あり。今回は検証コードの存在確認まで |

## 2. 構成と実行環境

| 成果物 | 役割 |
|---|---|
| Engine 静的ライブラリ | Platform / Core / Renderer / Engine の共通機能 |
| Editor.exe | ImGui の制作環境。共通エンジンをホストする |
| Runtime.exe | エディタ UI を持たないゲーム実行用ホスト |
| GameLogic.dll | C++ ゲームスクリプト。実行中の差し替え対象 |
| MyeScripting.dll | 別途ビルドする .NET スクリプトホスト |
| MyeCollab.dll / MyeCollabCli.exe | Rust 製の Git 連携サービスと検証用 CLI |

Visual Studio 2022、C++ デスクトップ開発環境、Windows SDK、x64 構成を使います。C++ 依存ソースは `external` に同梱されています。C# は .NET SDK、Git サービスのビルドは Rust ツールチェーンを別途必要とします。

Platform はウィンドウ・入力・時計・UDP・クラッシュ処理、Core は ECS・リフレクション・ジョブ・ログ、Renderer は GPU リソースと描画処理、Engine はそれらを使うゲーム機能を担当します。DLL 境界は C ABI の関数テーブルと POD 型で構成され、STL、例外、仮想関数テーブルを跨がせません。

根拠: [MyEngine.sln](C:/HAKtokyo/My_Engin/MyEngin/MyEngine.sln)、[Engine.vcxproj](C:/HAKtokyo/My_Engin/MyEngin/build/Engine.vcxproj)、[外部依存一覧](C:/HAKtokyo/My_Engin/MyEngin/external/VERSIONS.md)、[EngineAPI.h](C:/HAKtokyo/My_Engin/MyEngin/src/Shared/EngineAPI.h)。

## 3. オブジェクト・シーン・データ設計

### 3.1 ハイブリッド ECS

利用側は GameObject とコンポーネントの組み合わせで物体を扱います。内部は、同じコンポーネント構成のエンティティをまとめるアーキタイプと、型ごとの連続データ列で管理します。多数の物体を処理しやすくしながら、ゲーム側にはオブジェクト単位の API を提供します。

EntityID は index と generation を持ち、破棄済みオブジェクトを古い参照で操作する事故を検出します。生成・破棄・コンポーネントの構造変更はバッファに積んで決まった境界で適用し、走査中の配置変更を避けます。クエリキャッシュ、fileId 索引、ジョブ実行には無効化して比較する経路もあります。

### 3.2 階層と有効状態

LocalTransform が位置・回転・スケールを持ち、Hierarchy から WorldMatrix を計算します。親子付け、サブツリーの複製・生成、親の動きへの追従に使います。Active は階層伝播し、親が無効なら子も実効的に無効になります。

### 3.3 リフレクションとスキーマ

コンポーネントのフィールド名・型・表示名などを登録し、Inspector、JSON、状態移行、ハッシュ計算に利用します。すべてのフィールドが無条件に保存・ハッシュ対象になるわけではなく、フラグで描画専用や内部データを区別します。

`.component.schema.json` から動的なデータコンポーネントを定義でき、C++ / C# 用のアクセスコードを生成できます。C++ スクリプトには `MYE_F_JP`、`MYE_F_RANGE` による日本語表示名・調整範囲があります。

### 3.4 シーン保存と差分更新

`.scene.json` にエンティティとコンポーネントを保存します。fileId を使って保存上の識別と参照を維持し、外部で変更されたシーンを差分適用できます。

未登録のコンポーネントは現在の実装では JSON データを保留して再保存時に戻します。これは「意味を理解して実行する」機能ではなく、「未登録だからといって保存で消さない」機能です。型が利用可能かどうかは別途確認する必要があります。

根拠: [World.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Core/World.h)、[Components.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Core/Components.h)、[Reflection.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Core/Reflection.h)、[SchemaComponents.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/SchemaComponents.cpp)、[SceneSerializer.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/SceneSerializer.cpp)。

## 4. ゲーム更新とスクリプト

固定 tick の更新処理を `RunOneTick` に集約し、通常実行、タイムトラベルの再実行、ネットのロールバックで共用します。入力アクションと UI の押下状態を先に決め、その後にスクリプト、AI、アニメーション、物理などを処理します。構造変更や状態のハッシュ計算を決まった地点で行い、音声出力などを後段へ渡します。

| 機能 | 説明・使いどころ |
|---|---|
| C++ スクリプト | Start / Update / LateUpdate、衝突イベントなどでゲームを記述 |
| DLL ホットリロード | GameLogic.dll と PDB をコピーして差し替え。登録済み状態は名前・型の一致で移行 |
| C# スクリプト | .NET ホストからゲーム API を利用。制作時の別の記述手段 |
| 汎用フィールドアクセス | コンポーネント名・フィールド名をキーに、登録フィールドを読み書き |
| 空間・物理 API | Raycast、Overlap、SphereCast、力・速度操作、接触情報、CC 操作 |
| ゲーム API | 生成、親子付け、シーン遷移、UI、音、アニメーション、入力、永続値 |
| デバッグ API | ログ、線描画、状態の参照 |

現在の ABI は `MYE_API_VERSION = 18` です。v17 の `GetSceneName` は、環境依存の絶対パスをゲーム状態に持ち込まず、シーン名を取得するための入口です。v18 の `IsDevelopmentRun` は、エディタ / `--project` 付きの Runtime と配布物を見分けて、デバッグ操作を配布物で閉じるための入口です。

**利用時の制約:**

- C# は記録・検証・ネット対戦・再シミュレーション時に実行を止めます。C# の状態を C++ と同じように巻き戻せるとは説明できません。
- `Start()` では WorldMatrix に依存する空間クエリを前提にしません。
- `GetContactInfo` は物理処理後の LateUpdate / 衝突コールバックで利用します。Update では当該 tick の接触がまだありません。
- 非ハッシュの描画コンポーネントは汎用 API で書けますが、同 API の読み取りは制限されています。描画側の値が sim の判断に流入するのを防ぐ設計です。
- ホットリロードの対象はゲーム DLL です。エンジン本体の変更には再ビルドが必要です。

根拠: [TickRunner.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/TickRunner.cpp)、[ScriptAPI.h](C:/HAKtokyo/My_Engin/MyEngin/src/Shared/ScriptAPI.h)、[EngineApiTable.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Script/EngineApiTable.cpp)、[DllReloader.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/HotReload/DllReloader.cpp)、[ManagedHost.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Script/ManagedHost.cpp)。

## 5. アセットと再利用

| 種類 | 内容 |
|---|---|
| テクスチャ | PNG / JPEG / TGA / BMP / DDS。インポート設定、GPU 登録、DDS クック |
| モデル | glTF / GLB、ufbx 経由の FBX / OBJ。階層、メッシュ、材質、スキンの読み込み経路 |
| マテリアル | `.mat.json`。物体の描画パラメータとテクスチャ参照 |
| 構成アセット | `.actor.json` と互換対象の `.prefab.json` |
| アニメーション | `.anim.json` と `.controller.json` |
| 音 | WAV / OGG、`.sound.json`、`.mixer.json` |
| 地形 | `.terrain.json` と編集データ `.terrain.edit` |
| 物理材料 | `.physmat.json` |
| データ型 | `.component.schema.json` |

AssetDatabase が GUID と実パスを対応付け、各アセットの `.meta` を管理します。本体と `.meta` を一緒に移動すれば参照を保てます。モデル内部のメッシュ・マテリアル・スキンの ID も、M74a からモデルの `.meta` の GUID から作るため、clone 先が違うマシン同士でも同じシーンが同じように表示されます。M74a より前に保存したシーンは `Editor.exe --migrate-subasset-ids` で新しい ID へ書き換えます。

クック済みモデルなどを `cache/cooked` に保存し、次回は再解析を減らします。破損や不一致の検査、キャッシュを無効にして元データから処理する経路があります。キャッシュの存在は、元データを不要にすることと同義ではありません。配布では封印マーカーを含む専用のパッケージ処理を使います。

### 構成アセット・部位

Actor / Prefab は物体のサブツリーを再利用する仕組みです。インスタンス生成、ベース更新、Apply / Revert、フィールド上書き、コンポーネント追加・削除の上書きを扱います。子エンティティ自体の増減を構造上書きとして追跡する機能は範囲外です。

Part は武器の持ち手や車体の接続点に相当するソケットです。名前・タグ検索、骨への追従、PartBounds の箱・球に対するレイ判定を使い、装備の取り付けや部位ヒット判定を作れます。

### 更新監視

ファイル監視からシェーダ、テクスチャ、モデル、シーン、構成アセットなどを更新します。シェーダは include 依存を追い、コンパイル失敗時は旧シェーダを維持します。リロードできることと、すべての編集中状態が無条件に移行されることは別です。

根拠: [AssetDatabase.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/AssetDatabase.h)、[FbxLoader.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/FbxLoader.cpp)、[CookedCache.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Asset/CookedCache.h)、[Prefab.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Prefab.h)、[Parts.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Parts.h)、[ReloadHub.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/HotReload/ReloadHub.cpp)。

## 6. レンダリング

### 6.1 通常描画と光

Forward と Deferred を実行時に切り替えられます。共通のライティング関数を使い、透明物は共通の後段で処理します。MeshRenderer と SkinnedMesh、カメラ、平行光・点光源・スポット光、材質、シャドウ、環境マップが基本要素です。

| 機能 | 何ができるか | 条件・境界 |
|---|---|---|
| PBR / IBL | 粗さ・金属感などに応じた材質と環境光 | 対応マテリアルと環境データを使用 |
| シャドウ | 平行光のシャドウと局所光の影処理 | 光源選択・アトラス容量に上限 |
| カリング / インスタンシング | 視界外の除外、同一メッシュ等のまとめ描画 | すべての描画が一括化されるわけではない |
| Skybox | 色のグラデーション、DDS cubemap、手続きの星空 (密度・明るさ・瞬き・細かさ) | Cubemap は実装済み。解決失敗時はフォールバック。星は gradient のみ。`lighting = 0` で空を環境光 (IBL) に使わない (暗いシーンで下地のアンビエントを残す) |
| Decal | 壁の汚れ・模様などを投影 | Deferred の G-Buffer を使う経路 |
| HZB | 深度の階層を生成 | SSR 等の基盤。単独で汎用遮蔽カリングの完成を意味しない |
| SSR | 画面内の深度を使う反射 | Deferred 限定。画面外の情報は取得できない |
| Reflection Probe | 周囲を cubemap に焼いて局所的な反射に使用 | 最大 8、軸平行ボックス。回転する影響箱は対象外 |
| 編集補助描画 | ピッキング、選択・デバッグ線、分岐ゴースト | ゲームの sim 状態とは分離 |

### 6.2 Compute Shader による光線追跡

一次可視面はラスタライズで作り、二次光線に BVH を使います。拡散 GI、平行光の影、スペキュラ反射を選択的に置き換え、時間蓄積・分散推定・A-Trous フィルタを組み合わせた SVGF でノイズを減らします。DirectX 11 / `cs_5_0` で動く自前の追跡であり、DXR の実装ではありません。

ReSTIR 反射は時間・空間方向にサンプルを再利用します。ReflectionClass は「反射面」ではなく「反射に映る物体」の分類で、主役・人型・乗り物・小物・既定に応じて再利用を調整します。これらは既定無効で、Deferred と対応オプションを必要とします。全面パストレーシングや、あらゆる透明体の屈折対応と同一視しないでください。

### 6.3 ポストプロセスと大気

HDR から ACES / Reinhard / passthrough のトーンマップ、露出、Bloom、FXAA を適用できます。追加で色収差、ビネット、彩度・コントラスト、カラーフィルタ、LUT、自動露出、被写界深度、モーションブラーがあります。

SSAO、TAA、SSR は Deferred に依存します。TAA は履歴を利用するため、滑らかさと残像の調整が必要です。CameraPostFx はシーンカメラ別の設定を持ち、SceneView は一部の効果を抑えて編集視界を保ちます。

距離 Fog は Linear / Exp / Exp2、高さ方向の減衰、太陽方向の散乱表現を持ちます。別に Froxel のボリュメトリック処理があり、空間を小区画に分けて光と霧を積分し、時間蓄積して合成します。Froxel 使用時はゴッドレイの重複計上を抑える処理があります。

根拠: [RenderSystem.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/RenderSystem.cpp)、[RenderTypes.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Renderer/RenderTypes.h)、[PostProcess.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Renderer/PostProcess.cpp)、[RtPasses.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Renderer/RayTracing/RtPasses.cpp)、[FroxelPass.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Renderer/FroxelPass.cpp)、[ADR-016](C:/HAKtokyo/My_Engin/MyEngin/docs/adr/ADR-016-restir-reflection.md)。

## 7. 地形・アニメーション・VFX

### 地形

ハイトマップ、スプラットマップ、レイヤ設定を使って地形を描画します。チャンク・LOD・継ぎ目のスカートに対応し、エディタのブラシで高さの上げ下げ、平滑化、レイヤ塗りを行えます。編集結果は `.terrain.edit` に保存し、解像度が合う場合に元レシピへ適用します。矩形の差分を Undo / Redo に使います。地形コライダーと高さサンプリングもあります。洞窟を自由に掘れるボクセル地形とは異なります。

### アニメーション

フィールド単位のキーフレームを tick で評価します。Linear / Step 補間、四元数補間、再生・ループを扱います。AnimatorController は状態・遷移・条件・Exit Time・遷移ブレンド・Any State を持ちます。現行のパラメータは 4 つの整数スロットを前提としています。任意のカーブ編集や高度なブレンドツリーがあるとは説明しません。

スキンメッシュではスケルトンと骨行列によってメッシュを変形します。PartFollow で骨に部位を追従させ、ラグドールでは剛体から骨を逆駆動する経路があります。

### パーティクルとエフェクト

CPU バックエンドは SoA と SSE、GPU バックエンドは Compute Shader、dead/alive リストと indirect draw を使います。CPU / GPU 切替、比較モード、スカラー参照経路を持ちます。生成用乱数の管理を共通化しますが、GPU の演算結果全般がすべての機種でビット一致するという意味ではありません。

ParticleEmitter には発生数・寿命・形状・速度・色・サイズなどの調整値があります。寿命カーブ、テクスチャシート、ソフトパーティクル、ソート、歪み、ライティングなどの処理を持ちます。全フィールドはリファレンスに列挙しています。

SpriteRenderer は板状画像、TrailRenderer は移動の軌跡、TextMesh は空間内文字を描きます。Effect はこれらを束ねた演出の再生・再始動・寿命管理に利用できます。

根拠: [TerrainEdit.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Asset/TerrainEdit.h)、[TerrainSystem.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/TerrainSystem.cpp)、[Animation.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Animation.h)、[AnimatorController.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/AnimatorController.h)、[ParticleSystem.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Particles/ParticleSystem.cpp)、[VfxRenderer.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Vfx/VfxRenderer.cpp)。

## 8. 物理

自作の衝突・剛体ソルバを使い、接触の蓄積インパルス、サブステップ、摩擦、回転、スリープを処理します。単純形状に加え、メッシュ、凸包、複合コライダー、地形を扱う実装があります。形状の組み合わせや高速移動の条件による制約があるため、「全形状に完全な CCD」とはしません。

| 機能 | 用途 |
|---|---|
| Collider / Rigidbody | 固体の接触、重力、速度・角速度、力・トルク |
| トリガー・衝突イベント | 接触をゲームロジックへ通知 |
| 空間クエリ | Raycast、SphereCast、OverlapSphere / Box、レイヤマスク |
| CharacterController | カプセル型キャラクターの移動・接地・ジャンプ |
| ConstantForce / SpringJoint | 継続的な力、ばねによる接続 |
| 物理材料 | 静動摩擦、反発、転がり抵抗などの資産化 |
| PhysicsEnvironment | 重力、空気、風、水面などの共通設定 |
| Aero / AeroSurface | 抗力、角抗力、マグヌス効果、翼面の空力 |
| Buoyancy | 水面を使った浮力 |
| Joint | Ball / Hinge / Fixed / Slider / Cone、制限、モータ、破断、粘着 |
| Ragdoll | スケルトンから物理構成を生成し、骨を剛体に追従させる |
| Wheel / Vehicle | レイによるサスペンション、タイヤ力、駆動と操舵 |
| XPBD Rope | 粒子と拘束によるロープ、剛体への双方向アタッチ |
| 診断 | 接触情報、コライダー・物理デバッグ線、自己テスト |

CC のジャンプ API 自体は接地条件を判断しないため、呼び出す側が接地を確認します。CC は Transform のスケールの影響を受けます。CC と Rigidbody は汎用的な双方向の押し合いが完成しているわけではありません。

車両は PhysicsEnvironment の有無に影響されます。欠けていても期待する設定になると仮定しないでください。XPBD はロープまでで、布、ソフトボディ、塑性変形、粒子と世界の衝突、破砕、熱・流体・電気シミュレーションは実装済み機能に含めません。浮力があることは流体ソルバの存在を意味しません。

根拠: [PhysicsSystem.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Physics/PhysicsSystem.h)、[PhysicsSystem.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Physics/PhysicsSystem.cpp)、[PhysicsQueries.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Physics/PhysicsQueries.cpp)、[RagdollBuilder.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/RagdollBuilder.cpp)、[XpbdBackend.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Physics/XpbdBackend.cpp)。

## 9. 音声・音響伝播・敵 AI

### 9.1 音声出力

XAudio2 と X3DAudio を使い、WAV / OGG、ワンショット、ループ、音量・ピッチ、フェード、BGM ストリーミング、3D 定位、距離減衰、ドップラー、残響を扱います。AudioSource が発音元、AudioListener が聴取位置です。

Sound アセットは音のバリエーションと再生条件、Mixer はバスごとの音量・mute・solo・残響送りを管理します。エディタにはミキサーと SoundGen があり、手続き的な音の生成・試聴に使えます。音声システムの定数は最大 64 voice、バスの最大チャンネル数 8 です。これは無制限の同時発音や無制限の出力構成ではありません。

### 9.2 波面をゲーム機能に使う

AcousticVolume のグリッドで、整数の移動コストを使って波を伝播します。AcousticEmitter が波を出し、AcousticListener が到来方向や刺激を取得します。同じ場を可視化、敵の聴覚、経路探索に利用します。

AcousticAudio はこの伝播を実際の音へ接続します。経路長から遮蔽を整形し、曲がり角を回る音は仮想音源の位置とローパスで回折を表現します。空間の開放度から残響プリセットを補間し、波の到達範囲と音の減衰範囲を対応させます。これはグリッドに基づくゲーム向け近似で、波動方程式の厳密な音響解析ではありません。

### 9.3 音・光に反応する AI

AgentBrain は Patrol / Alert / Search / Chase / Return の 5 状態です。AcousticListener による聴覚と LightSeeker による光センサーを組み合わせ、AcousticNav を使って移動します。同じエンティティの移動入力をスクリプトと AI の両方が書くと、後に走る AI が優先される設計です。一般目的の Behavior Tree エディタや NavMesh ベイクとは区別します。

根拠: [AudioSystem.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Audio/AudioSystem.h)、[SynthCore.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Audio/SynthCore.h)、[AcousticField.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Acoustic/AcousticField.h)、[AcousticAudio.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Audio/AcousticAudio.cpp)、[AgentSystem.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Acoustic/AgentSystem.h)、[ADR-017](C:/HAKtokyo/My_Engin/MyEngin/docs/adr/ADR-017-acoustic-audio.md)。

## 10. 入力・ゲーム内 UI・ゲーム進行

キーボード、マウス、ホイール、生マウスデルタ、ゲームパッド、振動、カーソルロックを扱います。InputActions でボタンと軸を名前付きアクションへ対応付け、held / pressed / released を固定 tick で評価します。複数の入力レーンを持ち、ローカルプレイヤーとネット対戦の入力を扱います。

UIElement はアンカー、矩形、画像、文字、色、fill、レイアウトなどのデータを持ちます。基準 1920×1080 のキャンバス単位から `min(width/1920, height/1080)` で一様拡大縮小し、アスペクト比に応じたキャンバス寸法で端アンカーを扱います。任意の UI スケール方式を選ぶ機能ではありません。

hovered / pressed / clicked / focused をスクリプトより前に評価します。クリックは「押した要素の上で離す」で成立し、フォーカス中の決定操作も clicked に合流します。方向アクションでフォーカスを移動できます。UI の表示用コンポーネントと、ハッシュ対象の対話状態を分離しています。

| ゲーム進行機能 | 内容・境界 |
|---|---|
| シーン遷移 | 安全な境界で LoadScene を処理。GetSceneName で現在名を取得 |
| ポーズ・速度変更 | sim を進める tick のゲートで制御。解除用 UI のスクリプト等は継続 |
| PersistSet / Get | シーンを跨ぐ値を保持。1 エントリ最大 65,536 バイト |
| SaveGame | シーン情報と PersistStore をスロットの JSON に書く |
| LoadGame | 保存したシーンへ遷移し、永続値を戻す |
| LoadPersist | シーンを変えず、永続値だけを読み込む |

通常のセーブは、全エンティティ・物理の途中状態をそのまま保存するスナップショットではありません。ロードは記録・検証・ネット中には制限されます。ゲーム内ポーズと、エディタで tick 自体を止める Pause / Hold は異なる制御です。

根拠: [InputActions.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Platform/InputActions.h)、[UILayout.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/UI/UILayout.cpp)、[UIInteraction.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/UI/UIInteraction.cpp)、[GameFlow.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/GameFlow.h)、[SaveGame.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/SaveGame.h)。

## 11. エディタの機能

| 画面・機構 | 主な用途 |
|---|---|
| Project Manager | 外部プロジェクトの作成・登録・選択、テンプレート利用 |
| Hierarchy | エンティティ階層、選択、親子関係の編集 |
| Inspector | 登録フィールド編集、コンポーネント追加、参照選択、コピー |
| Scene View | 制作用カメラ、選択・ピッキング、移動・回転・拡縮ギズモ、地形ブラシ |
| Game View | シーンカメラからのゲーム表示 |
| Console | ログ・警告・エラーの確認 |
| Profiler | CPU / GPU 等の計測結果・統計の表示 |
| Asset / Content Browser | アセット検索、プレビュー、作成、移動、リネーム、再ビルドの入口 |
| Search | 名前・コンポーネント型・アセット検索、EntityRef の参照逆引き |
| Animation | キーフレームの制作・確認 |
| Animator Controller | 状態と遷移の編集 |
| Particle Settings | バックエンドとパーティクルの設定・比較 |
| Audio Mixer / SoundGen | 音量経路、試聴、音の生成 |
| Timeline | 過去 tick のシーク、分岐、差分、ゴースト、入力上書き |
| Net | ネットワーク状態の確認・接続操作 |
| Project Settings | プロジェクト設定、入力等の設定入口 |
| Build Settings | ゲームのパッケージ作成 |
| Source Control | Git の変更一覧、stage、commit、push、fetch、pull、ブランチ・競合操作 |

そのほか、Undo / Redo、ショートカット、ドッキング配置の保存、テーマ、ステータスバー、通知、カメラの pilot、保存済み内容との比較があります。Play / Pause / Step / Stop で制作中に実行を制御します。日本語が既定で英語へ切替可能です。文字列を集約し、表示言語変更で ImGui ID や保存キーが変わらないようにしています。

Search は現在コードにある名前・型・参照検索です。`plans` にある自然言語検索やエンジン MCP サーバの案を、この検索機能に含めません。

### Git 連携の境界

Rust サービスは JSON と 6 つの C ABI 関数でエディタに接続します。ファイルを書き換える操作前に未保存や実行中処理を検査し、更新されたファイルから、ホットリロード・シーン再読込・再起動を振り分けます。`.meta` や地形編集データを本体と対応させます。

サービスがない場合は Source Control が利用不可になります。認証の初期設定と Git 自体の制約は残ります。モデルのサブアセット ID に関する配置先の制約は M74a で無くなりました。これはゲーム内ネットワーク機能とは独立です。

根拠: [EditorApp.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/EditorApp.cpp)、[EditorApp.h](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/EditorApp.h)、[SearchWindow.h](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/Windows/SearchWindow.h)、[PlayModeController.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/PlayModeController.cpp)、[GitTransaction.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/SourceControl/GitTransaction.cpp)、[ADR-015](C:/HAKtokyo/My_Engin/MyEngin/docs/adr/ADR-015-in-process-rust-collab.md)。

## 12. リプレイ・タイムトラベル・クラッシュ再現

### リプレイと決定論

`.rep` に tick の入力と検証用ハッシュを記録し、再生時のワールド状態と比較します。Debug / Release 間の比較、入力の合成、ハッシュダンプ、リプレイ差分、フィールド差分の検証口があります。

保証の対象は定められた sim 状態です。GPU の描画結果、音声デバイスの出力、C# の任意の状態まで同じハッシュで保証するものではありません。描画は別途 PNG の比較を使います。

### タイムトラベルと What-if

スナップショットと入力列から過去 tick へ戻り、同じ更新関数で再実行してハッシュを照合します。現在は過去へ戻って変更した際に、以前の未来を分岐として保持します。枝の切替、最初の乖離 tick の検索、エンティティ／コンポーネント／フィールド差分、別分岐の動きを重ねるゴースト、入力上書きがあります。

Pause / Hold は tick を止め、Step は指定本数だけ進めて再び止めます。ポーズ中の Inspector 編集は分岐点の状態として再取得する実装です。

現行の既定予算はスナップショット間隔 30 sim tick、最大 120 枚、全レーン 64 MiB、分岐最大 8。ゴーストは分岐あたり 8 MiB、最大 1,800 tick です。容量や枝の追い出しがあるので、無限の履歴や永続的なバージョン管理ではありません。ゴーストの採取対象にも制約があります。

### クラッシュ記録

CrashRing は直近スナップショットと、そこからの全 tick 入力を `.rep` のバイト列として事前に保持します。クラッシュの瞬間には複雑な再構築を避けて書き出します。落ちた tick の入力も残すため、tick に入る前に入力を記録します。

調査時の作業ツリーでは、CrashRing 自身が要求するハッシュ間隔は既定 60 tick、スナップショット間隔は 600 tick です。入力は間引きません。ハッシュ 0 は期待値なしを意味します。通常リプレイの照合と、間引き可能なクラッシュ用チェックポイントを区別する必要があります。この箇所は既存の未コミット変更を含みます。

根拠: [Replay.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Replay/Replay.h)、[WorldHasher.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Replay/WorldHasher.cpp)、[SimSnapshot.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Replay/SimSnapshot.h)、[TimeTravel.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Replay/TimeTravel.h)、[CrashRing.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Replay/CrashRing.h)、[ADR-018](C:/HAKtokyo/My_Engin/MyEngin/docs/adr/ADR-018-whatif-branches.md)。

## 13. ネット対戦

UDP の 2 人 P2P を対象とし、相手の入力が揃うまで待つ遅延ロックステップと、予測入力で先に進めて差があれば戻す予測ロールバックを持ちます。直近入力を重複送信してパケット損失に備え、確定 tick のハッシュで不一致を検出します。不一致時の診断バンドル、人工的な遅延・損失、自己検証用の不一致注入があります。

入力基盤が複数レーンを持つことから、多人数インターネット対戦まで完成しているとは判断できません。マッチメイキング、NAT 越えサービス、専用サーバ、アカウント基盤は本機能に含めません。C# はネット中に止め、ping や自分の peer 番号など機種・接続依存の値は sim の分岐に戻さない設計です。

UI キャンバス寸法を接続時に照合するため、同一アスペクトで解像度だけが異なる場合と、アスペクトそのものが異なる場合では互換条件が違います。

根拠: [NetSession.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Net/NetSession.h)、[NetRollback.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Net/NetRollback.cpp)、[net_verify.bat](C:/HAKtokyo/My_Engin/MyEngin/tools/net_verify.bat)、[ADR-013](C:/HAKtokyo/My_Engin/MyEngin/docs/adr/ADR-013-predictive-rollback-netcode.md)。

## 14. プロジェクト・配布・検証

`project.mye.json` が名前、エンジン版、起動シーン、canonicalRoot を持ちます。`--project` で外部プロジェクトを開けます。プロジェクトローカルなエディタ設定は `.mye` に分離します。Project.h の表示用バージョンは `0.66` で、ABI v18 や開発マイルストーン番号とは別の値です。

Build Settings は、スクリプト再ビルド、クックの準備、Runtime / DLL / assets / 起動シーン / cache のコピー、任意の DDS 変換、任意の zip 化を段階実行します。.NET ランタイムの同梱設定もあります。C++ エンジン本体の Release ビルドそのものは Visual Studio / MSBuild の役割です。

| 検証入口 | 確認対象 |
|---|---|
| `Editor.exe --selftest` | 登録されたエンジン／エディタ自己テスト |
| `tools/check_rules.ps1` | 決定論ルール、ローカライズ、C++ / HLSL 定数、ABI ミラー等 |
| `tools/replay_verify.bat` | 対象シーンの Debug / Release リプレイ比較 |
| `tools/shot_verify.bat` | 基準 PNG との描画回帰 |
| `tools/net_verify.bat` | 通信・ロックステップ・ロールバックの検証 |
| `tools/crash_verify.bat` | クラッシュと保存された再現記録の検証 |
| `tools/collab_verify.bat` | Git サービス・連携処理 |
| `tools/watcher_rules_verify.bat` | 更新監視とルールの検証 |
| `tools/verify_sanko_replay.ps1` | 三校向けのリプレイ検証 |
| `tools/bisect_replay.bat` | リプレイ不一致を使う変更点の絞り込み |
| `.github/workflows/ci.yml` | 自動ビルド・検証の構成 |

スクリーンショットの固定条件、WARP、内蔵フォント、画像差分の許容値、sim / cook キャッシュの切替など、原因を分けるための CLI があります。テストの存在は対象範囲の検証手段がある証拠であり、全機能の正しさを一括保証するものではありません。

根拠: [Project.h](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/Project.h)、[BuildSettingsWindow.h](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/Windows/BuildSettingsWindow.h)、[EditorMain.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Editor/EditorMain.cpp)、[RuntimeMain.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Runtime/RuntimeMain.cpp)、[CI](C:/HAKtokyo/My_Engin/MyEngin/.github/workflows/ci.yml)。

## 15. 実装と既存文書の差・残る制約

| 項目 | 調査時の判定 |
|---|---|
| ABI v16 / 110 スロットという README 記述 | 現行コードは v18。公開関数一覧はリファレンス参照 |
| 未登録コンポーネントの保存消失 | 現行は未登録 JSON を保持する実装あり。古い未解決扱いを転記しない |
| ハイスコアだけの読込ができない | `LoadPersist` が実装済み |
| Skybox cubemap が未実装 | 実装済み。旧記録側の誤り |
| ドッグフーディング未解決 16 件という README 記述 | 現行台帳では 5 件。過去の概要より台帳を優先 |
| タイムトラベル | 単純な巻き戻しに加え M72 の分岐、M73 の Hold / Step 周辺の実装あり |
| クラッシュハッシュ | 作業ツリーは間隔指定に対応。全 tick に必ず期待ハッシュがあるとは限らない |
| エンジン版の数字 | 表示用 `0.66`、ABI、マイルストーンは別管理。数字だけで実装範囲を判断しない |
| XPBD | ロープまで。世界衝突・布・ソフトボディは未完成 |
| 構成アセット | コンポーネント構造上書きあり。子エンティティ増減の追跡は範囲外 |
| C# | sim の巻き戻し・リプレイ・ネットの対象外 |
| セーブ | 永続値とシーン情報。全ワールドの途中状態を保存する形式ではない |

現行ドッグフーディング台帳の未解決項目は、ヘッドレスのデバッグ線ログ、汎用の組込み車輪メッシュ、CC と Rigidbody の双方向性、PhysicsEnvironment 欠落時の警告、スクリプト間メッセージングの 5 件です。これは全コードの不具合総数ではなく、同台帳に残った項目です。

また、`plans` の将来案は、対応する実装・登録・呼び出しがあることを確認できない限り実装済みへ分類しません。ゲーム企画や外部プロジェクトの個別ルールもエンジン共通機能と区別します。

根拠: [dogfooding.md](C:/HAKtokyo/My_Engin/MyEngin/docs/dogfooding.md)、[engine_spec.md](C:/HAKtokyo/My_Engin/MyEngin/engine_spec.md)、[EngineAPI.h](C:/HAKtokyo/My_Engin/MyEngin/src/Shared/EngineAPI.h)、[SceneSerializer.cpp](C:/HAKtokyo/My_Engin/MyEngin/src/Engine/Engine/SceneSerializer.cpp)。

## 16. 説明・デモの組み立て方

### 30 秒での紹介例

「MyEngine は、C++20 と DirectX 11 で開発した Windows 向け 3D ゲームエンジンです。エディタでシーンや物理、アニメーション、音を組み立て、C++ と C# でゲームを作れます。特徴は再現性を重視した更新基盤で、コードのホットリロード、リプレイ、時間の巻き戻しと分岐比較、予測ロールバック対戦へつなげています。描画では Compute Shader による GI・影・反射、ゲーム機能では音響伝播と音に反応する AI も備えています。」

### 5〜10 分の説明順

1. Hierarchy / Inspector / Scene / Game を見せ、コンポーネントで制作する流れを説明する。
2. GameLogic の登録フィールドとホットリロードを見せ、調整しながら開発できることを示す。
3. `--render-demo` と Deferred、必要に応じて RT オプションで描画機能を示す。
4. `--physics-demo` / `--joint-demo` で剛体、機構、車両などを示す。
5. `--acoustic-demo` で波の可視化、敵の反応、音声の関係を説明する。
6. Timeline の巻き戻し・分岐・差分で、見た目だけでなく原因を追えることを示す。
7. Build Settings と Runtime を見せ、制作から配布までの経路を締めくくる。

これらは説明用の手順案です。今回デモを起動して成功を確認したものではありません。発表前には対象ビルドと同じマシンで動作確認してください。

### 最低限の操作例

以下はビルド済み出力ディレクトリから実行します。オプションの全抽出一覧と実装箇所は機能リファレンスにあります。

```powershell
.\Editor.exe --selftest
.\Editor.exe --render-demo --deferred
.\Editor.exe --rt-demo --deferred --rt-gi --rt-shadow --rt-refl
.\Editor.exe --physics-demo
.\Editor.exe --joint-demo
.\Editor.exe --acoustic-demo
.\Runtime.exe --timetravel-selftest
.\Runtime.exe --whatif-selftest
```

## 17. 調査の追跡と更新

機能リファレンスには、組込みコンポーネントの宣言フィールド、C ABI の公開関数、Editor / Runtime の CLI、実装ファイル・テスト・シェーダの索引を収録します。列挙は静的な宣言・ファイルに基づくため、Inspector 公開可否、組合せ制約、動作確認の判定には本ガイドと各実装を合わせて参照してください。

新機能追加時は「目的、操作入口、実装箇所、既定値と制約、検証入口」を一緒に更新します。既存仕様より実装が進んだ箇所もあるため、マイルストーン名だけで完了と判定しないことが重要です。
