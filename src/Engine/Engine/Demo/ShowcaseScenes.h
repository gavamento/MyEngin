#pragma once
#include <string>

namespace mye {

struct EngineContext;

// ショーケースの組み立てに渡す CLI の値 (--terrain-lod / --terrain-skirt。地形ショーケースだけが使う、M58e)
struct ShowcaseOptions {
    float terrainLodDistance = 0.0f; // 0 = LOD 無効 (既定 = golden の絵)
    float terrainSkirtDepth = 0.0f;  // 0 = 自動 / < 0 = スカート無し (クラックの A/B 撮影用)
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

} // namespace mye
