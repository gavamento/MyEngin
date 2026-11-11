//====================================================================================
//                          CullingSettings.h
//  MyEngin/ 秋田蓮音                                                       11/11/2026
//                                          カリング設定 (project_settings.json) の読み書きと CLI との合成
//====================================================================================
#pragma once
#include <string>

namespace mye {

// タグの数 (Components.h の kMaxTags と同じ。CullingSettings.cpp が static_assert で照合する。
// EngineLoop.h が取り込むので Components.h は持ち込まない)
inline constexpr int kCullingTagCount = 64;

// 画面サイズカリングの閾値の既定 (外接球の画面高さ比。1080p で約 5 px)
inline constexpr float kDefaultMinScreenSize = 0.005f;

// `assets\project_settings.json` の `"rendering"` 内のカリング設定 (描画専用。sim・ハッシュに触れない)。
// 正本の仕様は plans\m99-culling\spec.md 4.1.1
struct CullingSettings {
    bool frustum = true;
    bool occlusion = true;
    bool distance = true; // 距離が 1 つも無ければ何も落ちない
    bool size = false;
    float minScreenSize = kDefaultMinScreenSize; // 0 < x <= 1
    float defaultMaxDrawDistance = 0.0f;         // m。0 = 無制限
    float tagDistance[kCullingTagCount] = {};    // タグ番号別の最大描画距離 (m)。0 = 設定なし
};

// 起動時の CLI による上書き (ファイルへは書き戻さない)。bool は false = その種類を OFF にする
struct CullingCliOverrides {
    bool frustum = true;  // --no-frustum-culling
    bool occlusion = true; // --no-occlusion
    bool distance = true; // --no-distance-culling
    bool size = true;     // --no-size-culling
    float maxDrawDistance = -1.0f; // >= 0: 既定の最大描画距離をこの値にする (--cull-max-distance)
    float minScreenSize = 0.0f;    // > 0: 画面サイズカリングを ON にして閾値をこの値にする (--cull-min-screen-size)
};

// 画面サイズの閾値として有効か (0 < f <= 1)
bool IsValidMinScreenSize(float f);
// 距離として有効か (有限で 0 以上)
bool IsValidCullDistance(float d);

// ファイルから読む。ファイルが無い・読めない・キーが無い・型違い・範囲外は既定値 (範囲外と型違いはログへ 1 行)
CullingSettings LoadCullingSettings(const std::wstring& assetsRoot);

// 1 キーずつ差し替えて書く (他のキー・他の節は壊さない。読めないファイルは上書きせず false)
enum class CullingFlag { Frustum, Occlusion, Distance, Size };
bool SaveCullingFlag(const std::wstring& assetsRoot, CullingFlag flag, bool value);
bool SaveMinScreenSize(const std::wstring& assetsRoot, float value);        // 範囲外は書かず false
bool SaveDefaultMaxDrawDistance(const std::wstring& assetsRoot, float value); // 範囲外は書かず false
// 距離 > 0 のタグだけを tag 昇順の配列で書く
bool SaveTagCullDistances(const std::wstring& assetsRoot, const float (&distances)[kCullingTagCount]);

// 実効値 = ファイルの値 && CLI の OFF が無いこと。数値だけは CLI が上書きする。
// --no-size-culling は --cull-min-screen-size より強い (OFF が常に勝つ)。EngineLoop と selftest が同じ関数を使う
CullingSettings ApplyCullingCli(const CullingSettings& file, const CullingCliOverrides& cli);
CullingSettings ResolveCullingSettings(const std::wstring& assetsRoot, const CullingCliOverrides& cli);

} // namespace mye
