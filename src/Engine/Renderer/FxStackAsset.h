/*----
 FxStackAsset.h  fxstack.json のデータ型・ロード・保存
 作成者: 秋田蓮音                                09/22/2026
----*/
#pragma once
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "Engine/Renderer/ProjectEffectRunner.h"  // PostInsertionPoint / PropValue

namespace mye {

// fxstack パスの種別
enum class FxStackKind : int32_t
{
    Post    = 0, // *.post.hlsl — BeforeTonemap / AfterTonemap へ挿入
    Compute = 1, // *.cs.hlsl  — シーン駆動 Dispatch (sub-04 で実行)
};

// fxstack.json の 1 パスエントリ
struct FxStackEntry
{
    FxStackKind kind          = FxStackKind::Post;
    std::string shader;               // ShaderManager::Load() に渡す名前 (例: "MyTint.post")
    bool        enabled       = true;

    // kind==Post
    PostInsertionPoint insertion = PostInsertionPoint::BeforeTonemap;
    int                priority  = 100;

    // kind==Compute (sub-04 で使用)
    std::string dispatchPoint;        // "BeforePost" / "BeforeTonemap" / "AfterTonemap"

    // 両方: プロパティ値の辞書 (JSON "properties" オブジェクト → 名前→値)
    std::unordered_map<std::string, PropValue> properties;
};

// *.fxstack.json 全体
struct FxStackAsset
{
    int                      version = 1;
    std::vector<FxStackEntry> passes;
};

// *.fxstack.json を読み込む。ok=false のとき errorMsg に理由を書く (null 可)
bool LoadFxStack(const std::wstring& path, FxStackAsset& out, std::string* errorMsg = nullptr);

// *.fxstack.json へ書き出す。ok=false のとき errorMsg に理由を書く (null 可)
bool SaveFxStack(const std::wstring& path, const FxStackAsset& asset, std::string* errorMsg = nullptr);

// FxStackEntry.insertion と JSON 文字列の相互変換
const char*        InsertionToString(PostInsertionPoint ip);
PostInsertionPoint InsertionFromString(const std::string& s); // 未知は BeforeTonemap

} // namespace mye
