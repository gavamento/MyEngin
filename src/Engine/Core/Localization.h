#pragma once
#include <cstdint>

// エディタ UI の言語切替 (M47)。
//
// 文字列の実体は LocalizationTable.inl に 1 行 1 文字列で並べ、そこから
//   - StrId          (この enum)
//   - 英語テーブル / 日本語テーブル  (Localization.cpp)
// を同じ X マクロで生成する。片方だけ書き足すことが構文的に不可能になるので、
// 訳抜けはコンパイルエラーとして出る。
//
// ImGui は毎フレームすべての文字列を再発行するため、SetLanguage() は次のフレームから
// 即座に反映される。ウィンドウ名は "表示名###安定ID" 形式なので、切り替えても
// ドッキング配置 (imgui.ini) とパネル開閉状態は保たれる。
//
// **Tr() の戻り値を printf 系の書式引数に渡さないこと** (詳細は .inl 冒頭の規約)。
namespace mye {

enum class Lang : uint8_t {
    Ja = 0, // 既定
    En = 1,
};

enum class StrId : uint16_t {
#define MYE_STR(id, en, ja) id,
#include "Engine/Core/LocalizationTable.inl"
#undef MYE_STR
    Count,
};

// 現在の言語での文字列。範囲外の id には "?" を返す (呼び出し側で分岐させない)。
const char* Tr(StrId id);

// 指定した言語での文字列 (言語に依存しない出力が要る箇所 — 自動化モードなど — 用)。
const char* TrIn(Lang lang, StrId id);

void SetLanguage(Lang lang);
Lang CurrentLanguage();

// 設定ファイル用の相互変換。未知の文字列は Lang::Ja になる。
const char* LangToString(Lang lang);
Lang LangFromString(const char* s);

} // namespace mye
