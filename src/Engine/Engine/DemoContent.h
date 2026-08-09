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

// assets\ 以下の .prefab.json / .anim.json を各ライブラリへ登録する (Editor / Runtime 共用)。
// M48g からは .glb / .gltf / .fbx のスケルトンもここで (エンティティを作らずに) 登録する
void RegisterAssetLibraries(EngineContext& ctx);

} // namespace mye
