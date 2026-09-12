#pragma once
// ゲーム内 UI のプロジェクト設定 (M75c)。assets\project_settings.json の "ui" 節。
//   "ui": { "referenceW": 1920, "referenceH": 1080 }
// = Canvas の無い UI (暗黙の既定キャンバス) の基準解像度。明示 Canvas の referenceW/H <= 0 もこれに従う。
// 読むのは EngineLoop の起動時 1 回だけ (uilayout::SetDefaultCanvasReference へ流す)、
// 書くのは Project Settings 窓だけ。**.rep には載せない** (actions.json と同じ扱い) —
// ネットでは NetIdentity.referenceW/H の照合で食い違いを入口で弾く。
#include <string>

#include "Engine/Engine/UI/UILayout.h"

namespace mye {
namespace uilayout {

struct ProjectUiSettings {
    int referenceW = kCanvasRefW;
    int referenceH = kCanvasRefH;
    bool operator==(const ProjectUiSettings& o) const
    {
        return referenceW == o.referenceW && referenceH == o.referenceH;
    }
    bool operator!=(const ProjectUiSettings& o) const { return !(*this == o); }
};

// 許容範囲。外れた値は既定 (1920x1080) へ倒す — 0 や負は CanvasSize の除算を壊し、
// 巨大値は int の丸めで意味を失う
inline constexpr int kReferenceMin = 1;
inline constexpr int kReferenceMax = 16384;

// JSON 文字列から "ui" 節を読む (ファイル I/O 抜きの本体 = セルフテストの口)。
// 節や欄が無い / 型違い / 範囲外の欄は既定値のまま。戻り値は「有効な ui 節があったか」
bool ParseProjectUiSettings(const std::string& jsonText, ProjectUiSettings& out);

// assetsRoot\project_settings.json を読む。ファイルが無い / 壊れていれば既定値
ProjectUiSettings LoadProjectUiSettings(const std::wstring& assetsRoot);

// "ui" 節だけ書き換えるマージ保存 (ParticleSystem::SaveSettings と同じ流儀 — 他のキーは消さない)
bool SaveProjectUiSettings(const std::wstring& assetsRoot, const ProjectUiSettings& settings);

} // namespace uilayout
} // namespace mye
