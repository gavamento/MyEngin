#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye {
namespace TerrainAsset {

// 地形アセット (M58a、spec §6.5)。ソースは `.terrain.json`、クック結果は `.mterr`。
//
// クック結果 = ハイトマップ (R16) + スプラットマップ (RGBA8) + レイヤ定義。
// GPU も D3D も要らない純データなので、クック/往復/境界検査は全部 selftest で回せる。
//
// **描画専用レーン** — 地形は sim (ワールドハッシュ) に 1 バイトも触らない。
// ただし M59 の地形コリジョンがこの blob をハッシュレーンへ持ち込む予定なので、
// **クックはバイト決定論であること**が今から契約になっている
// (同じソースを 2 回焼いたら payload がビット一致すること。CookedCacheSelfTest が検査する)。
inline constexpr const wchar_t* kTerrainExt = L".mterr";
inline constexpr const wchar_t* kSourceSuffix = L".terrain.json";

// blob 形式の版。**形式を変えたら必ず上げる** (CookedCache::kCookVersion とは独立 —
// あちらはキャッシュ全体の無効化、こちらは .mterr の中身の互換性)
inline constexpr uint32_t kBlobVersion = 1;

// スプラットマップは RGBA8 の 4 チャンネル = レイヤ 4 枚が構造的な上限 (M58d)。
inline constexpr uint32_t kMaxLayers = 4;

// 解像度の上限。破損 blob の巨大な要素数をそのまま resize すると bad_alloc で即死するので、
// 「残量で検算」に加えて実用上の天井も置く (4097 = 4096 タイル + 継ぎ目の 1 列)
inline constexpr uint32_t kMaxResolution = 4097;

// スプラット 1 テクセルの重み合計。cook 側で必ずこの値へ量子化正規化する
// (シェーダ側で割らずに済ませるため。M58d のブレンドがこれを前提にする)
inline constexpr uint32_t kSplatWeightSum = 255;

// レイヤ 1 枚 = 地表マテリアル 1 種。テクスチャは `Material` に載せない
// (`Material` はテクスチャ 2 枚までで 4 レイヤ x (albedo+normal) = 8 枚が入らない。M58d)。
// パスは **`.terrain.json` からの相対のまま**保存する — 絶対パスを焼くと配布物を
// 移設した瞬間に全部空振りする (M51j の封印クックで踏んだのと同じ穴)。
struct TerrainLayer {
    std::string name;
    std::string albedo; // 相対パス (空 = 未設定)
    std::string normal;
    float tilingU = 8.0f; // ワールド全幅あたりの繰り返し回数
    float tilingV = 8.0f;
};

// クックで焼き込んだソース画像の同一性。
// ★CookedCache の deps は**存在しか見ない** (内容はリプレイ時に実ファイルを読み直す前提)。
// 地形はハイトマップ/スプラットマップの**中身を blob へ焼き込む**ので、それでは
// 「.terrain.json を触らずに PNG だけ差し替えた」を取りこぼす。ここに size + 内容ハッシュを
// 持たせて Load() 側で照合する (封印キャッシュ中は元画像が無いので照合しない)
struct TerrainSourceImage {
    std::string relPath; // 空 = 手続き生成 (照合対象なし)
    uint64_t byteSize = 0;
    uint64_t contentHash = 0;
};

// 手続き生成のパラメータ (ハイトマップ画像を指定しないときに使う)。
// 画像を 1 枚も同梱せずにデモ地形をリポジトリへ置けるようにするための経路で、
// 値ノイズ + fBm。整数ハッシュ由来なので `rand()` も実時間も混ざらない
struct TerrainProcedural {
    uint32_t seed = 1;
    uint32_t octaves = 4;
    float frequency = 3.0f;
    float lacunarity = 2.0f;
    float gain = 0.5f;
};

struct TerrainData {
    uint32_t heightW = 0; // ハイトマップの頂点数 (タイル数 + 1 が自然)
    uint32_t heightH = 0;
    uint32_t splatW = 0;
    uint32_t splatH = 0;
    float worldSizeX = 0.0f; // ワールド寸法 (m)
    float worldSizeZ = 0.0f;
    float heightBase = 0.0f;  // 正規化高さ 0 に対応するワールド Y
    float heightScale = 0.0f; // 正規化高さ 1 に対応する Y との差
    TerrainSourceImage heightSrc;
    TerrainSourceImage splatSrc;
    TerrainProcedural proc;
    std::vector<TerrainLayer> layers;
    std::vector<uint16_t> heights; // heightW * heightH、行優先 (Z が外側)
    std::vector<uint8_t> splat;    // splatW * splatH * 4、行優先。1 テクセルの和 = kSplatWeightSum

    // 構造的な整合 (要素数・上限・寸法)。Deserialize / CookFromSource の出口で必ず通す
    bool Valid() const;

    // 頂点 (x, z) のワールド高さ。範囲外はクランプ (M58b のメッシュ生成が使う)
    float HeightAtTexel(uint32_t x, uint32_t z) const;
};

// blob <-> 構造体。Deserialize は境界検査つき (破損 blob で false、絶対に落ちない)
void Serialize(const TerrainData& d, std::vector<uint8_t>& out);
bool Deserialize(const std::vector<uint8_t>& in, TerrainData& out);

// `.terrain.json` を読んでソース画像をデコードし、TerrainData を組む (フレッシュクック)。
// ハイトマップ未指定なら手続き生成。失敗で false (out は未定義値を残さない)
bool CookFromSource(const std::wstring& srcPath, TerrainData& out);

// クックキャッシュ優先のロード。ヒットすれば画像デコードを丸ごと省略する。
// ミスなら CookFromSource + キャッシュ書き出しまで面倒を見る
bool Load(const std::wstring& srcPath, TerrainData& out);

// path が `.terrain.json` か (大文字小文字を無視)
bool IsSourcePath(const std::wstring& path);

// 4 レイヤの重み (任意スケール) を合計 kSplatWeightSum の RGBA8 へ量子化する。
// 端数は最大剰余法 + レイヤ番号順のタイブレークで配る = 決定論。純関数なので selftest 可
void QuantizeSplatWeights(const float weights[4], uint8_t out[4]);

} // namespace TerrainAsset
} // namespace mye
