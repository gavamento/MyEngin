#pragma once
#include <string>
#include <vector>

namespace mye {

struct EngineContext;

// ショーケースの組み立てに渡す CLI の値 (--terrain-lod / --terrain-skirt = 地形、M58e / --2d-night = 2D デモの夜、M94)
struct ShowcaseOptions {
    float terrainLodDistance = 0.0f; // 0 = LOD 無効 (既定 = golden の絵)
    float terrainSkirtDepth = 0.0f;  // 0 = 自動 / < 0 = スカート無し (クラックの A/B 撮影用)
    bool twoDNight = false;          // --2d-demo に Light2D (Global 暗 + スポット 2 灯) を足す
    bool twoDStress = false;         // --2d-demo に負荷計測用の Sprite2D 2000 枚とタイル 15000 セルを足す
};

// デモ / ショーケースのシーン 1 本 = --*-demo フラグ 1 本 (表は ShowcaseScenes.cpp の kShowcases)。
// Editor と Runtime は同じ行から保存先と組み立て関数を引く
struct ShowcaseDef {
    const wchar_t* flag;      // "--rt-demo" など
    bool inAssets;            // true = <assets>\ 相対。false = 作業ディレクトリ相対 (cache\)
    const wchar_t* scenePath; // 保存先 = 在ればロードする先
    void (*prepare)(EngineContext& ctx);                              // ロードの前に呼ぶ (nullptr = 何もしない)
    void (*build)(EngineContext& ctx, const ShowcaseOptions& options); // ファイルが無ければ組む (nullptr = 組まない)
    bool editorOnly;          // Runtime には無いフラグ
};

// flag の行 (無ければ nullptr)。editor = false なら editorOnly の行は引かない
const ShowcaseDef* FindShowcase(const std::wstring& flag, bool editor);
// --*-demo を複数渡したときにどちらを使うか。**表の上の行が勝つ** (渡した順ではない)
const ShowcaseDef* PickShowcase(const ShowcaseDef* current, const ShowcaseDef* candidate);
// 保存先の実パス
std::wstring ShowcaseScenePath(const ShowcaseDef& showcase, const std::wstring& assetsRoot);

// kShowcases[] を表の順に返す (--dump-index が索引へ書く読み取り専用の列挙)
std::vector<const ShowcaseDef*> ListShowcases();

} // namespace mye
