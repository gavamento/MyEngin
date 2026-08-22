#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Engine/Asset/TerrainAsset.h"

namespace mye {
namespace TerrainEdit {

// 地形のブラシ編集 (M58f、spec §6.5)。
//
// **この層に GPU も World も ImGui も無い** — ブラシの効果もパッチもレイキャストも
// 全部 `TerrainData` だけを見る純関数にしてあるので、TerrainSelfTest が
// 「Undo/Redo の往復でハイトマップがバイト一致する」をヘッドレスで機械検証できる。
// SceneViewWindow はカーソル位置をローカル座標へ落として ApplyBrush を呼ぶだけ。
//
// **編集結果の置き場所 = サイドカー `.terrain.edit`** (`.terrain.json` の隣)。
// ソース JSON は「地形の作り方 (寸法・レイヤ・ノイズ種)」のレシピで、ブラシが変えるのは
// **画素**なので、両者を混ぜない。CookFromSource がレシピを解いた**後**にサイドカーを
// 被せる = 「元画像を差し替えたらブラシ結果は捨てる」でも「レシピを触ったらブラシ結果は
// 残る」でもなく、**解像度が一致する限りブラシ結果が勝つ**という 1 本の規則になる。
//
// ★M59 の地形コリジョンはこの画素をハッシュレーンへ持ち込むので、
//   サイドカーの直列化も**バイト決定論**であること (同じ TerrainData から同じバイト列)。

// サイドカーの拡張子と形式版。`.terrain.json` → `.terrain.edit`
inline constexpr const wchar_t* kEditExt = L".terrain.edit";
inline constexpr uint32_t kEditVersion = 1;

// Undo エントリに詰めるパッチの形式版 (UndoFileOp::bytes の中身)
inline constexpr uint32_t kPatchVersion = 1;

enum class BrushMode : uint32_t {
    Raise = 0,  // 高さを上げ下げ (strength の符号で反転)
    Smooth = 1, // 近傍平均へ寄せる
    Paint = 2,  // スプラットを 1 レイヤへ寄せる
};

// ブラシ 1 回 (= 1 ダブ) の指定。座標は**地形ローカル空間の XZ** (中心原点・メートル)。
// ワールド行列の逆変換は呼び出し側 (SceneViewWindow) の仕事 — ここは地形の中だけを見る
struct Brush {
    BrushMode mode = BrushMode::Raise;
    float centerX = 0.0f;
    float centerZ = 0.0f;
    float radius = 8.0f;   // m。0 以下は何も塗らない
    float strength = 1.0f; // Raise: ワールド Y の増分 (m、負で掘る) / Smooth・Paint: 0..1 の寄せ率
    uint32_t layer = 0;    // Paint の対象レイヤ (0..kMaxLayers-1)
};

// 編集 1 ストロークぶんの差分 (Undo エントリの中身)。
// **矩形パッチ**にしてあるのは、地形全体のスナップショットを 2 枚持つと 1 ストローク
// あたり数十 MB になるため (4097^2 の R16 だけで 33MB)。矩形は「変わった texel の
// バウンディングボックス」なので、往復が厳密であることは MakeDiffPatch の
// 「差分の外は 1 バイトも違わない」という不変量に依存する — selftest がそこを突く。
struct TerrainPatch {
    uint32_t hx0 = 0, hz0 = 0, hw = 0, hh = 0; // ハイトマップの矩形 (hw==0 = 高さ変化なし)
    std::vector<uint16_t> heightBefore, heightAfter;
    uint32_t sx0 = 0, sz0 = 0, sw = 0, sh = 0; // スプラットの矩形 (sw==0 = 塗り変化なし)
    std::vector<uint8_t> splatBefore, splatAfter; // RGBA
    bool Empty() const { return hw == 0 && sw == 0; }
};

// ---- ブラシ ----

// ブラシ 1 回ぶんを data へ適用する。**純関数** (data と b だけで結果が決まる)。
// 1 バイトでも変われば true。data が壊れている / 効果ゼロなら false (data は無傷)
bool ApplyBrush(TerrainAsset::TerrainData& d, const Brush& b);

// ---- パッチ ----

// 同じ解像度の 2 つの TerrainData の画素差分を矩形パッチにする。
// 解像度違い / 壊れたデータで false。差が無ければ true + Empty() のパッチ
bool MakeDiffPatch(const TerrainAsset::TerrainData& before,
                   const TerrainAsset::TerrainData& after, TerrainPatch& out);

// パッチを適用する (redo = after を書く / undo = before を書く)。
// **矩形と要素数を全部検算してから書く** — 途中で弾くと半分だけ巻き戻った地形が残る
bool ApplyPatch(TerrainAsset::TerrainData& d, const TerrainPatch& p, bool redo);

// パッチ ⇔ バイト列 (UndoFileOp::bytes)。Deserialize は境界検査つき (壊れた入力で false)
void SerializePatch(const TerrainPatch& p, std::string& out);
bool DeserializePatch(const std::string& in, TerrainPatch& out);

// ---- サイドカー ----

// `<x>.terrain.json` → `<x>.terrain.edit`。ソースでなければ空文字列
std::wstring EditPathFor(const std::wstring& srcPath);

// 画素 (heights + splat) をサイドカーのバイト列へ。同じ TerrainData なら常に同じバイト列
void SerializeSidecar(const TerrainAsset::TerrainData& d, std::vector<uint8_t>& out);

// サイドカーの画素を d へ被せる。**解像度が d と一致するときだけ**受理する
// (JSON の heightRes を変えたら古いブラシ結果は自動的に無効になる)。
// 壊れている / 解像度違いで false (d は無傷)
bool ApplySidecarBlob(const std::vector<uint8_t>& blob, TerrainAsset::TerrainData& d);

// 編集結果を永続化する: サイドカー書き出し → d.editSrc の刻印 → クックキャッシュの更新。
// **ブラシも Undo/Redo もこの 1 本を通る** (2 経路に分けると「塗ったときだけ / 戻したときだけ
// キャッシュが腐る」という直し方の分からないバグになる)
bool SaveEdits(const std::wstring& srcPath, TerrainAsset::TerrainData& d);

// ---- 高さ場の問い合わせ (エディタ専用) ----
//
// ★**sim から呼ばないこと。** 地形コリジョンは engine_spec §6.5 で M59 送りと決めてあり、
//   ここを tick から触った瞬間に地形がワールドハッシュのレーンに入る
//   (`replay_verify.bat` に 5 ペア目が必要になる)。今の唯一の呼び出し元は
//   SceneViewWindow のブラシ = カーソル下の地表を求めるためだけ。

// 地形ローカル XZ の高さ (双一次補間)。範囲外は端の値へクランプ
float SampleHeightLocal(const TerrainAsset::TerrainData& d, float lx, float lz);

// 地形ローカル空間のレイと高さ場の交点。dir は正規化されていなくてよい。
// 地形の XZ 範囲の外での交差は採らない (クランプした「延長平面」に当たってしまうため)。
// 起点が既に地面より下なら false
bool RaycastLocal(const TerrainAsset::TerrainData& d, const DirectX::XMFLOAT3& origin,
                  const DirectX::XMFLOAT3& dir, float maxDist, DirectX::XMFLOAT3& outHit);

} // namespace TerrainEdit
} // namespace mye
