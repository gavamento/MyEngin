#pragma once

namespace mye {

struct EngineContext;

// デモ用リソース (メッシュ / マテリアル / モデル) を登録する。
// シーンファイルは AssetID しか持たないため実体登録は起動側の責務 — Editor / Runtime 共用。
void RegisterDemoContent(EngineContext& ctx);

// デモシーン (spinner / パーティクル / スクリプト) を構築する (シーンファイルが無い時のみ)。
// perfRate>0 でパーティクル放出数を上書き (性能計測モード)
void BuildDemoScene(EngineContext& ctx, float perfRate = 0.0f, int perfMax = 0);

// M46i: ハイブリッド パストレーシング (M46) のショーケースシーン。--rt-demo で選ぶ。
// コーネル箱 (赤/緑の側壁) + 天井の発光パネル + 鏡面/粗い金属球 + 遮蔽物という構成で、
// 「RT off = 定数アンビエントで平坦」「RT on = 間接光の色移り + 接触陰影 + 映り込み」を
// 1 枚の絵で対比できるようにしてある。**平行光は置かない** — 唯一の光源が発光面なので、
// 拡散 GI が無いと箱の中は光らない = emissive が GI 光源として機能していることの直接の証拠。
//
// ★BuildDemoScene とは別関数にしてあり、既定のデモシーンは 1 バイトも変えない。
//   デモシーンは golden.rep (リプレイ決定論検証) の入力そのものなので、
//   エンティティを足すと per-tick ワールドハッシュが変わり再録画が必要になる。
//   加えて M46f/g/h が積み上げた「RT off なら直前コミットとスクショがビット一致」という
//   受け入れ基準も、シーンが変わると比較対象を失う
void BuildRtShowcaseScene(EngineContext& ctx);

// 上のショーケースが参照するメッシュ/マテリアルの実体登録。**保存済みの
// rt_showcase.scene.json をロードする経路でも AssetID が解決できるように**、
// シーンを組まない場合でも呼ぶ (RegisterDemoContent と BuildDemoScene の関係と同じ)
void RegisterRtShowcaseContent(EngineContext& ctx);

// M48g: 部位 (ソケット) のボーン追従を **リプレイ検証で被覆する**ためのシーン。--parts-demo で選ぶ。
// CesiumMan (スキンメッシュ) + 手のジョイントに追従する部位 + 部位の子メッシュ +
// 部位の近くに落とす Rigidbody という構成で、`PartFollowSystem` が書く LocalTransform が
// 毎 tick のワールドハッシュに載る = **骨駆動の値が Debug/Release でビット一致することの機械証明**
// になる (既定デモシーンにはスキンメッシュが 1 体も無く、骨演算は一度もハッシュ被覆に
// 入ったことがなかった)。
//
// ★既定のデモシーンは 1 バイトも変えない (golden.rep の入力そのもの。BuildRtShowcaseScene と同じ理由)
void BuildPartsShowcaseScene(EngineContext& ctx);

// 上のショーケースが参照するメッシュ/マテリアルの実体登録 (RegisterRtShowcaseContent と同じ役割)
void RegisterPartsShowcaseContent(EngineContext& ctx);

// M51j: ゲームフロー統合デモ (--flow-demo) のシーン 2 本を確保する。
// タイトル (flow_title) → ゲーム (flow_game) → タイトル、を LoadScene で循環し、
// ポーズ/タイムスケール/PersistStore 持ち越し/セーブを 1 本の tick タイムラインで実走する
// (replay_verify 3 ペア目 = M51 決定論保証の総括)。
//
// **両シーンをファイルとして assets\scenes\ へ書く** (無いときだけ) — 遷移は
// LoadScene("scenes/flow_*.scene.json") = assets 相対解決なので、ファイルが無いと
// 遷移で詰む。builtin メッシュ + 名前キーのマテリアルしか使わないため内容は
// チェックアウト非依存だが、版管理された唯一の正解はコード側 (parts と同じ流儀) なので
// 生成物は gitignore にしてある。呼んだ後の ctx.scene は空 (呼び出し側がタイトルをロードする)
void EnsureFlowShowcaseScenes(EngineContext& ctx);

// 上のショーケースが参照するマテリアルの実体登録 (RegisterRtShowcaseContent と同じ役割)
void RegisterFlowShowcaseContent(EngineContext& ctx);

// M52g: ローカルマルチプレイ (入力レーン) のデモシーン。--local-demo で選ぶ。
// kMaxPlayers 体のキューブがそれぞれ `PlayerInputComponent` でレーンに結び付いていて、
// LocalPlayerDemo (C++ スクリプト) がそのミラーを読んで動かす。
//
// **replay_verify 4 ペア目の入力そのもの**。`--local-players 2 --synth-input` で回すと
//   ・レーン 0/1 は違う合成入力で違う動きをする
//   ・レーン 2/3 のミラーは恒常ゼロ (playerCount 超過)
// が毎 tick のワールドハッシュに載るので、「レーン n の入力がレーン n のエンティティに
// 届いたか」が Debug/Release のビット一致として機械検証される。
//
// builtin メッシュ + 名前キーのマテリアルだけで組むのでチェックアウト非依存。
// ★既定のデモシーンは 1 バイトも変えない (BuildRtShowcaseScene と同じ理由)
void BuildLocalPlayersScene(EngineContext& ctx);

// 上のデモが参照するマテリアルの実体登録 (RegisterRtShowcaseContent と同じ役割)
void RegisterLocalPlayersContent(EngineContext& ctx);

// M52i: 2 人ネット対戦のデモシーン。--net-demo で選ぶ。
// 中央のリングに出入りするたびに得点が入り、得点はキューブの大きさで見える。
//
// **入るのは 2 つの新しい経路だけ**:
//   ・プレイヤーの動きは ABI v13 の `GetAxisForPlayer` / `GetActionForPlayer`
//     (レーン指定のアクションマップ) で読む = 決定論の内側
//   ・HUD は ABI v13 の `NetLocalPlayer` / `NetPingMs` / `NetRollbackCount` を読む =
//     **機種依存の値なので UIElement (NoHash の描画レーン) にしか書かない**。
//     ここを sim 状態へ書いた瞬間に 2 台のワールドハッシュが割れる — その誤用を
//     見張っているのが M52i の desync 検出で、このデモは「正しい使い分け」の実例。
//
// ★C++ スクリプトで書く (C# はネット対戦中は止まっている)。
// ★builtin メッシュ + 名前キーのマテリアルだけ = チェックアウト非依存。
//   --local-players 2 --synth-input でも同じシーンがそのまま回る (1 台で挙動を見る用)
void BuildNetDuelScene(EngineContext& ctx);

// 上のデモが参照するマテリアルの実体登録
void RegisterNetDuelContent(EngineContext& ctx);

// M54a: 描画ロードマップ (M54〜M58) のショーケースシーン。--render-demo で選ぶ。
//
// ★これが無いと以降の描画サブは**回帰テストの被覆がゼロ**になる。既存の golden 5 枚は
//   全部 LightComponent の既定 (type=0 = 平行光 1 本) だけで組まれていて、点光源もスポットも
//   1 個も無い。つまり「局所ライトの影 / デカール / SSR / 反射プローブ / ボリュメトリック /
//   地形」はどれも**勝手にピクセル不変**になり、「golden が緑」が何も主張しなくなる。
//   絵の良し悪しではなく「M54〜M58 の全機能が画に出る被写体が揃っているか」で組んである:
//     ・床 200x200 (大スケール)    … CSM のカスケード分割 / M58 の地形スケール
//     ・柱 20 本                   … 影の落とし手と受け手 (座標は決定論的な固定表)
//     ・スポット 2 + 点光源 2      … M54c (透視シャドウ) / M54d (キューブ 6 面)
//     ・平行光 1 本                … 既存デモと同じ。CSM の被覆を落とさないため必ず残す
//     ・反射床パッチ (金属 0.9 / 粗さ 0.1) … M56d (SSR) / M56f (反射プローブ)
//     ・高さフォグ                 … M57d のフロクセルが置き換える対象
//     ・遠景オブジェクト           … DoF / フォグ / M58e の LOD
//     ・回転する物体               … M55c/M55e の velocity とモーションブラー
//
// ★builtin メッシュ + 名前キーのマテリアルだけ = チェックアウト非依存。
//   既定のデモシーンは 1 バイトも変えない (BuildRtShowcaseScene と同じ理由)
void BuildRenderShowcaseScene(EngineContext& ctx);

// 上のショーケースが参照するメッシュ/マテリアルの実体登録 (RegisterRtShowcaseContent と同じ役割)
void RegisterRenderShowcaseContent(EngineContext& ctx);

// M59d: 物理 (空力・浮力・材料) のショーケースシーン。--physics-demo で選ぶ。
//
// **replay_verify.bat の 5 ペア目**がこのシーン — M59 で足した数式 (重力ベクトル / 等方抗力 /
// マグヌス / 面サンプリング / 翼面 / 浮力 / 材料と密度 / CCD) が Debug と Release でビット一致
// することを 600 tick ぶん機械照合するのが唯一の存在理由。絵として見せるのは副次的。
//
// ★builtin メッシュ + 名前キーのマテリアル + **名前で引いた .physmat** だけ = チェックアウト
//   非依存。物理マテリアルの AssetID を絶対パスから組まないこと (M59a1 の申し送り 1)。
// ★床は x = 4 で切ってある — その先は水面 (waterPlaneY = 0) で、浮きが浮かぶ場所。
void BuildPhysicsShowcaseScene(EngineContext& ctx);

// 上のショーケースが参照するメッシュ/マテリアルの実体登録
void RegisterPhysicsShowcaseContent(EngineContext& ctx);

// M58c: 地形のショーケースシーン。--terrain-demo で選ぶ (golden `demo_terrain_deferred`)。
//
// ★**--render-demo に地形を足さない**のがこの別シーンの唯一の存在理由。既存の golden
//   `demo_render_forward` / `demo_render_deferred` を 1 ピクセルでも動かすと、同じ Wave で
//   走っている M54/M55 のブランチと **PNG (マージ不能なバイナリ) で衝突**する。
// ★被写体は assets\terrain\demo.terrain.json (M58a の手続き生成地形 = 画像非同梱)。
//   起伏が画面いっぱいに入る俯瞰カメラ + 平行光 1 本 + 参照用の箱だけ —
//   地形以外の要素を増やすほど「地形が壊れた」以外の理由で golden が割れる
//
// M58e: lodDistance > 0 で地形の LOD を有効化する (--terrain-lod)。**既定 0 = 無効** —
// golden `demo_terrain_deferred` は LOD 無しの絵のままで、LOD は A/B 撮影でだけ点ける。
// skirtDepth は TerrainComponent と同じ意味論 (0 = 自動 / < 0 = スカート無し)
void BuildTerrainShowcaseScene(EngineContext& ctx, float lodDistance = 0.0f,
                               float skirtDepth = 0.0f);

// 上のショーケースが参照するマテリアルの実体登録
void RegisterTerrainShowcaseContent(EngineContext& ctx);

// M60i: 関節と機構 (M60) のショーケースシーン。--joint-demo で選ぶ。
//
// **replay_verify.bat の 6 ペア目**がこのシーン — M60 で足した層 (拘束ブロック / ヒンジ /
// 固定 / スライダ / リミット / モータ / 破断 / 複合コライダー / 凸包 / ラグドールの逆駆動 /
// 車両) が Debug と Release でビット一致することを 600 tick ぶん機械照合するのが存在理由。
//
// ★被写体は「その機能が replay に**必ず**載る」ように誇張して置いてある。二重振り子は
//   軌道が初期値に鋭敏なので、機種差があれば 600 tick で必ず割れる。
// ★**substeps = 16** (env の上限)。ラグドールが要求する — 4 でも 8 でも「床に触れながら
//   関節に吊られている」骨が微振動を続け、島の全員が静まるまで誰も眠らないので
//   ラグドール全体が一生眠らない (M60g2 の実測)。車両が推奨する 8 も同時に満たす。
// ★builtin メッシュ + 名前キーのマテリアル + 名前引きの .physmat が基本だが、
//   **凸包 1 個とラグドールだけはモデル (.glb) 由来**なので、保存したシーン JSON は
//   チェックアウト先に依存する = コミットできない (parts と同じ。cache\ へ置いて毎回組む)。
//   凸包にモデル由来を 1 個混ぜてあるのは `.mcvx` クックを replay 被覆へ入れるため。
void BuildJointShowcaseScene(EngineContext& ctx);

// 上のショーケースが参照するメッシュ/マテリアルの実体登録 (RegisterRtShowcaseContent と同じ役割)。
// 車輪メッシュ (**軸が X 向き**の多角柱) もここで手続き生成する — builtin の Cylinder は
// 軸が Y で、車輪エンティティは回せない (回すとサスのレイ方向まで回る)
void RegisterJointShowcaseContent(EngineContext& ctx);

// assets\ 以下の .prefab.json / .anim.json を各ライブラリへ登録する (Editor / Runtime 共用)。
// M48g からは .glb / .gltf / .fbx のスケルトンもここで (エンティティを作らずに) 登録する
void RegisterAssetLibraries(EngineContext& ctx);

} // namespace mye
