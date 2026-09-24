/*----
 FxStackAsset.cpp  fxstack.json のロード・保存実装
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/FxStackAsset.h"

#include <array>
#include <fstream>
#include <string>

#include "Engine/Core/Log.h"
#include "nlohmann/json.hpp"

using nlohmann::json;

namespace mye {
namespace {

// JSON の "properties" オブジェクトをパースして PropValue map へ変換
void ParseProperties(const json& j, std::unordered_map<std::string, PropValue>& out)
{
    if (!j.is_object()) {
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        const json& val = it.value();

        // スカラー: float
        if (val.is_number()) {
            out[key] = val.get<float>();
        }
        // 配列: float4
        else if (val.is_array() && val.size() == 4) {
            std::array<float, 4> v = {};
            for (size_t i = 0; i < 4; ++i) {
                v[i] = val[i].is_number() ? val[i].get<float>() : 0.0f;
            }
            out[key] = v;
        }
        // 文字列: Tex2D のビルトイン名 or アセット GUID hex
        else if (val.is_string()) {
            out[key] = val.get<std::string>();
        }
        // それ以外は無視 (警告は出さない — 旧形式との互換)
    }
}

// PropValue map を JSON "properties" オブジェクトへ変換
json SerializeProperties(const std::unordered_map<std::string, PropValue>& props)
{
    json j = json::object();
    for (const auto& [key, val] : props) {
        if (std::holds_alternative<float>(val)) {
            j[key] = std::get<float>(val);
        } else if (std::holds_alternative<std::array<float, 4>>(val)) {
            const auto& arr = std::get<std::array<float, 4>>(val);
            j[key] = json::array({ arr[0], arr[1], arr[2], arr[3] });
        } else if (std::holds_alternative<std::string>(val)) {
            j[key] = std::get<std::string>(val);
        }
    }
    return j;
}

} // namespace

// ---------------------------------------------------------------------------
// 文字列変換
// ---------------------------------------------------------------------------

const char* InsertionToString(PostInsertionPoint ip)
{
    switch (ip) {
    case PostInsertionPoint::BeforeTonemap: return "BeforeTonemap";
    case PostInsertionPoint::AfterTonemap:  return "AfterTonemap";
    default:                                return "BeforeTonemap";
    }
}

PostInsertionPoint InsertionFromString(const std::string& s)
{
    if (s == "AfterTonemap") {
        return PostInsertionPoint::AfterTonemap;
    }
    return PostInsertionPoint::BeforeTonemap;
}

// ---------------------------------------------------------------------------
// ロード
// ---------------------------------------------------------------------------

bool LoadFxStack(const std::wstring& path, FxStackAsset& out, std::string* errorMsg)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        if (errorMsg) {
            *errorMsg = "ファイルを開けません";
        }
        return false;
    }

    json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        if (errorMsg) {
            *errorMsg = std::string("JSON パース失敗: ") + e.what();
        }
        return false;
    }

    if (!j.is_object()) {
        if (errorMsg) {
            *errorMsg = "ルートが JSON オブジェクトではありません";
        }
        return false;
    }

    // 型の崩れた値 (数値欄に文字列など) は json::value / get が type_error を投げる。
    // RenderSystem は描画中に読むので、例外を漏らさずロード失敗として返す
    // (ProjectShaderProperties の DecodeMaterialProperties と同じ作法)
    FxStackAsset parsed;
    try {
        parsed.version = j.value("version", 1);

        const json passes = j.value("passes", json::array());
        if (!passes.is_array()) {
            // passes がなければ空スタック扱い (ok=true)
            out = std::move(parsed);
            return true;
        }

        for (const json& ep : passes) {
            if (!ep.is_object()) {
                // 要素 1 件の破損でスタック全体を捨てない。その要素だけ飛ばす
                MYE_LOG_WARN("fxstack: passes の要素がオブジェクトではないため飛ばします");
                continue;
            }
            FxStackEntry entry;

            // kind
            const std::string kind = ep.value("kind", "post");
            if (kind == "compute") {
                entry.kind = FxStackKind::Compute;
            } else {
                entry.kind = FxStackKind::Post;
            }

            entry.shader  = ep.value("shader", "");
            entry.enabled = ep.value("enabled", true);

            if (entry.kind == FxStackKind::Post) {
                entry.insertion = InsertionFromString(ep.value("insertion", "BeforeTonemap"));
                entry.priority  = ep.value("priority", 100);
            } else {
                entry.dispatchPoint = ep.value("dispatchPoint", "BeforePost");
                entry.priority      = ep.value("priority", 50);
            }

            // プロパティ値
            if (ep.contains("properties") && ep["properties"].is_object()) {
                ParseProperties(ep["properties"], entry.properties);
            }

            parsed.passes.push_back(std::move(entry));
        }
    } catch (const json::exception& e) {
        if (errorMsg) {
            *errorMsg = std::string("fxstack の型が不正: ") + e.what();
        }
        return false;
    }

    out = std::move(parsed);
    return true;
}

// ---------------------------------------------------------------------------
// 保存
// ---------------------------------------------------------------------------

bool SaveFxStack(const std::wstring& path, const FxStackAsset& asset, std::string* errorMsg)
{
    json j;
    j["version"] = asset.version;
    j["passes"]  = json::array();

    for (const FxStackEntry& entry : asset.passes) {
        json ep;
        ep["kind"]       = (entry.kind == FxStackKind::Compute) ? "compute" : "post";
        ep["shader"]     = entry.shader;
        ep["enabled"]    = entry.enabled;

        if (entry.kind == FxStackKind::Post) {
            ep["insertion"] = InsertionToString(entry.insertion);
            ep["priority"]  = entry.priority;
        } else {
            ep["dispatchPoint"] = entry.dispatchPoint;
            ep["priority"]      = entry.priority;
        }

        ep["properties"] = SerializeProperties(entry.properties);
        j["passes"].push_back(std::move(ep));
    }

    std::ofstream f(path);
    if (!f.is_open()) {
        if (errorMsg) {
            *errorMsg = "ファイルに書き込めません";
        }
        return false;
    }

    try {
        f << j.dump(2);
    } catch (const std::exception& e) {
        if (errorMsg) {
            *errorMsg = std::string("JSON シリアライズ失敗: ") + e.what();
        }
        return false;
    }

    return f.good();
}

} // namespace mye
