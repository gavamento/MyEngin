/*----
 ProjectShaderPropertiesSelfTest.cpp  Properties DSL パース・パックの回帰テスト
 作成者: 秋田蓮音                                09/22/2026
----*/
#include "Engine/Renderer/ProjectShaderPropertiesSelfTest.h"

#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "Engine/Core/Log.h"
#include "Engine/Renderer/ProjectShaderProperties.h"

namespace mye {
namespace {

int g_failCount = 0;

// テスト判定マクロ: 結果を MYE_LOG に書き出す
#define PROP_CHECK(cond)                                                            \
    do {                                                                            \
        if (cond) {                                                                 \
            MYE_LOG_INFO("  PASS: %s", #cond);                                     \
        } else {                                                                    \
            MYE_LOG_ERROR("  FAIL: %s (%s:%d)", #cond, __FILE__, __LINE__);        \
            ++g_failCount;                                                          \
        }                                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// テスト 1: 仕様 4.2 の DSL 例をパースできる
// ---------------------------------------------------------------------------
void TestParseSpecExample()
{
    MYE_LOG_INFO("[selftest] Properties DSL: 仕様 4.2 サンプルのパース");

    // 仕様書 §4.2 に記載された DSL 例
    const char* hlsl =
        "// HLSL header\n"
        "/*@MyEngineProperties\n"
        "[Range(0.0, 1.0)] _Intensity (\"Intensity\", Float) = 0.5\n"
        "_Tint (\"Tint\", Color) = (1, 1, 1, 1)\n"
        "_Mask (\"Mask\", 2D) = \"white\" {}\n"
        "[Header(Advanced)]\n"
        "_Direction (\"Direction\", Vector) = (0, 1, 0, 0)\n"
        "@*/\n"
        "struct VS_OUT { float4 pos : SV_POSITION; };\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    // Intensity / Tint / Mask / Header-only / Direction = 5 件
    PROP_CHECK(result.properties.size() == 5);

    if (result.properties.size() >= 5)
    {
        // --- _Intensity ---
        PROP_CHECK(result.properties[0].name        == "_Intensity");
        PROP_CHECK(result.properties[0].displayName == "Intensity");
        PROP_CHECK(result.properties[0].type        == PropType::Float);
        PROP_CHECK(result.properties[0].attr.hasRange);
        PROP_CHECK(std::fabs(result.properties[0].attr.rangeMin - 0.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[0].attr.rangeMax - 1.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[0].defaultFloat  - 0.5f) < 1e-6f);

        // --- _Tint ---
        PROP_CHECK(result.properties[1].name        == "_Tint");
        PROP_CHECK(result.properties[1].displayName == "Tint");
        PROP_CHECK(result.properties[1].type        == PropType::Color);
        PROP_CHECK(std::fabs(result.properties[1].defaultVec4[0] - 1.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[1].defaultVec4[3] - 1.0f) < 1e-6f);

        // --- _Mask ---
        PROP_CHECK(result.properties[2].name        == "_Mask");
        PROP_CHECK(result.properties[2].type        == PropType::Tex2D);
        PROP_CHECK(result.properties[2].defaultTex  == "white");
        PROP_CHECK(result.properties[2].cbOffset    == -1); // CB 対象外

        // --- [Header(Advanced)] ---
        PROP_CHECK(result.properties[3].name.empty());
        PROP_CHECK(result.properties[3].attr.header == "Advanced");
        PROP_CHECK(result.properties[3].cbOffset    == -1);

        // --- _Direction ---
        PROP_CHECK(result.properties[4].name        == "_Direction");
        PROP_CHECK(result.properties[4].type        == PropType::Vector);
        PROP_CHECK(std::fabs(result.properties[4].defaultVec4[0] - 0.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[4].defaultVec4[1] - 1.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[4].defaultVec4[2] - 0.0f) < 1e-6f);
    }
}

// ---------------------------------------------------------------------------
// テスト 2: HDR / HideInInspector / NoScaleOffset 属性
// ---------------------------------------------------------------------------
void TestParseAttributes()
{
    MYE_LOG_INFO("[selftest] Properties DSL: 各種属性 (HDR/HideInInspector/NoScaleOffset)");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "[HDR] _Emission (\"Emission\", Color) = (1, 1, 1, 1)\n"
        "[HideInInspector] _Hidden (\"Hidden\", Float) = 0.0\n"
        "[NoScaleOffset] _Tex (\"Tex\", 2D) = \"black\" {}\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    PROP_CHECK(result.properties.size() == 3);
    if (result.properties.size() >= 3)
    {
        PROP_CHECK(result.properties[0].attr.isHDR);
        PROP_CHECK(result.properties[1].attr.hideInInspector);
        PROP_CHECK(result.properties[2].attr.noScaleOffset);
    }
}

// ---------------------------------------------------------------------------
// テスト 3: Range を型として使う
// ---------------------------------------------------------------------------
void TestRangeAsType()
{
    MYE_LOG_INFO("[selftest] Properties DSL: Range 型");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "_Factor (\"Factor\", Range(0.0, 2.0)) = 1.0\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    PROP_CHECK(result.properties.size() == 1);
    if (!result.properties.empty())
    {
        PROP_CHECK(result.properties[0].type         == PropType::Range);
        PROP_CHECK(result.properties[0].attr.hasRange);
        PROP_CHECK(std::fabs(result.properties[0].attr.rangeMin - 0.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[0].attr.rangeMax - 2.0f) < 1e-6f);
        PROP_CHECK(std::fabs(result.properties[0].defaultFloat  - 1.0f) < 1e-6f);
    }
}

// ---------------------------------------------------------------------------
// テスト 4: ブロック無し → 空で成功
// ---------------------------------------------------------------------------
void TestNoBlock()
{
    MYE_LOG_INFO("[selftest] Properties DSL: ブロック無し (正常)");

    const char* hlsl = "// no properties\nvoid PSMain() {}";
    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    PROP_CHECK(result.properties.empty());
    PROP_CHECK(result.cbSizeBytes == 0);
}

// ---------------------------------------------------------------------------
// テスト 5: 不正ブロック — 閉じ漏れ (@*/ 無し)
// ---------------------------------------------------------------------------
void TestUnclosedBlock()
{
    MYE_LOG_INFO("[selftest] Properties DSL: 閉じ漏れエラー");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "_X (\"X\", Float) = 0.0\n";
    // @*/ が無い

    auto result = ParseProperties(hlsl);
    PROP_CHECK(!result.ok);
    PROP_CHECK(!result.errorMessage.empty());
}

// ---------------------------------------------------------------------------
// テスト 6: 不正ブロック — 未知の型
// ---------------------------------------------------------------------------
void TestUnknownType()
{
    MYE_LOG_INFO("[selftest] Properties DSL: 未知型エラー");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "_X (\"X\", UnknownType) = 0.0\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(!result.ok);
    PROP_CHECK(!result.errorMessage.empty());
}

// ---------------------------------------------------------------------------
// テスト 7: CB オフセット割り当てとサイズ
// (仕様 4.2 例: Intensity offset=0, Tint offset=16, Direction offset=32, 計 48 bytes)
// ---------------------------------------------------------------------------
void TestCbOffsets()
{
    MYE_LOG_INFO("[selftest] Properties DSL: CB オフセット (仕様 4.2 例)");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "[Range(0.0, 1.0)] _Intensity (\"Intensity\", Float) = 0.5\n"
        "_Tint (\"Tint\", Color) = (1, 1, 1, 1)\n"
        "_Mask (\"Mask\", 2D) = \"white\" {}\n"
        "[Header(Advanced)]\n"
        "_Direction (\"Direction\", Vector) = (0, 1, 0, 0)\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    PROP_CHECK(result.properties.size() == 5);

    if (result.properties.size() >= 5)
    {
        // float (4B) → offset 0
        PROP_CHECK(result.properties[0].cbOffset == 0);
        // Color (16B, 16B 境界) → offset 16
        PROP_CHECK(result.properties[1].cbOffset == 16);
        // Tex2D → CB 対象外
        PROP_CHECK(result.properties[2].cbOffset == -1);
        // Header-only → CB 対象外
        PROP_CHECK(result.properties[3].cbOffset == -1);
        // Vector (16B, 16B 境界) → offset 32
        PROP_CHECK(result.properties[4].cbOffset == 32);
    }
    // 合計 32 + 16 = 48 bytes
    PROP_CHECK(result.cbSizeBytes == 48);
}

// ---------------------------------------------------------------------------
// テスト 8: CB パック — 既定値
// ---------------------------------------------------------------------------
void TestPackDefaults()
{
    MYE_LOG_INFO("[selftest] Properties DSL: CB パック (既定値)");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "[Range(0.0, 1.0)] _Intensity (\"Intensity\", Float) = 0.5\n"
        "_Tint (\"Tint\", Color) = (1, 1, 1, 1)\n"
        "_Mask (\"Mask\", 2D) = \"white\" {}\n"
        "[Header(Advanced)]\n"
        "_Direction (\"Direction\", Vector) = (0, 1, 0, 0)\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);

    // 値辞書なし (全既定値) でパック
    std::unordered_map<std::string, PropValue> values;
    std::vector<uint8_t> cb;
    PROP_CHECK(PackProperties(result, values, cb));
    PROP_CHECK(cb.size() == 48);

    if (cb.size() == 48)
    {
        // _Intensity: offset 0, float = 0.5
        float f0;
        std::memcpy(&f0, cb.data() + 0, sizeof(float));
        PROP_CHECK(std::fabs(f0 - 0.5f) < 1e-6f);

        // _Tint: offset 16, float4 = (1,1,1,1)
        float tint[4];
        std::memcpy(tint, cb.data() + 16, 4 * sizeof(float));
        PROP_CHECK(std::fabs(tint[0] - 1.0f) < 1e-6f);
        PROP_CHECK(std::fabs(tint[3] - 1.0f) < 1e-6f);

        // _Direction: offset 32, float4 = (0,1,0,0)
        float dir[4];
        std::memcpy(dir, cb.data() + 32, 4 * sizeof(float));
        PROP_CHECK(dir[0] == 0.0f);
        PROP_CHECK(std::fabs(dir[1] - 1.0f) < 1e-6f);
        PROP_CHECK(dir[2] == 0.0f);
    }
}

// ---------------------------------------------------------------------------
// テスト 9: CB パック — 値辞書で上書き
// ---------------------------------------------------------------------------
void TestPackOverride()
{
    MYE_LOG_INFO("[selftest] Properties DSL: CB パック (値上書き)");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "_A (\"A\", Float) = 0.0\n"
        "_B (\"B\", Color) = (0, 0, 0, 0)\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);

    std::unordered_map<std::string, PropValue> values;
    values["_A"] = 3.14f;
    values["_B"] = std::array<float, 4>{ 0.1f, 0.2f, 0.3f, 0.4f };

    std::vector<uint8_t> cb;
    PROP_CHECK(PackProperties(result, values, cb));

    if (cb.size() >= 32)
    {
        // _A: offset 0
        float fa;
        std::memcpy(&fa, cb.data() + 0, sizeof(float));
        PROP_CHECK(std::fabs(fa - 3.14f) < 1e-5f);

        // _B: offset 16 (Color は 16B 境界)
        float fb[4];
        std::memcpy(fb, cb.data() + 16, 4 * sizeof(float));
        PROP_CHECK(std::fabs(fb[0] - 0.1f) < 1e-5f);
        PROP_CHECK(std::fabs(fb[1] - 0.2f) < 1e-5f);
        PROP_CHECK(std::fabs(fb[2] - 0.3f) < 1e-5f);
        PROP_CHECK(std::fabs(fb[3] - 0.4f) < 1e-5f);
    }
}

// ---------------------------------------------------------------------------
// テスト 10: float 4 個 + Color のパック (16B 境界の確認)
// ---------------------------------------------------------------------------
void TestCbFourFloatsThenColor()
{
    MYE_LOG_INFO("[selftest] Properties DSL: 4 スカラー + Color の CB オフセット");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "_A (\"A\", Float) = 1.0\n"
        "_B (\"B\", Float) = 2.0\n"
        "_C (\"C\", Float) = 3.0\n"
        "_D (\"D\", Float) = 4.0\n"
        "_E (\"E\", Color) = (0, 0, 0, 0)\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    PROP_CHECK(result.properties.size() == 5);
    if (result.properties.size() >= 5)
    {
        // 4 floats は 1 つの 16B レジスタに詰まる
        PROP_CHECK(result.properties[0].cbOffset == 0);
        PROP_CHECK(result.properties[1].cbOffset == 4);
        PROP_CHECK(result.properties[2].cbOffset == 8);
        PROP_CHECK(result.properties[3].cbOffset == 12);
        // 次の Color は次の 16B 境界
        PROP_CHECK(result.properties[4].cbOffset == 16);
    }
    // 16 + 16 = 32 bytes
    PROP_CHECK(result.cbSizeBytes == 32);
}

// ---------------------------------------------------------------------------
// テスト 11: float 3 個 + Color の境界確認
// (3 floats: 12B. 次の float 余地 4B あるが Color は 16B 境界が必要)
// ---------------------------------------------------------------------------
void TestCbThreeFloatsThenColor()
{
    MYE_LOG_INFO("[selftest] Properties DSL: 3 スカラー + Color + scalar の境界");

    const char* hlsl =
        "/*@MyEngineProperties\n"
        "_A (\"A\", Float) = 0.0\n"
        "_B (\"B\", Float) = 0.0\n"
        "_C (\"C\", Float) = 0.0\n"
        "_D (\"D\", Color) = (0,0,0,0)\n"
        "_E (\"E\", Float) = 0.0\n"
        "@*/\n";

    auto result = ParseProperties(hlsl);
    PROP_CHECK(result.ok);
    PROP_CHECK(result.properties.size() == 5);
    if (result.properties.size() >= 5)
    {
        PROP_CHECK(result.properties[0].cbOffset == 0);
        PROP_CHECK(result.properties[1].cbOffset == 4);
        PROP_CHECK(result.properties[2].cbOffset == 8);
        // Color は 16B 境界 → offset 16
        PROP_CHECK(result.properties[3].cbOffset == 16);
        // 次の Float は offset 32
        PROP_CHECK(result.properties[4].cbOffset == 32);
    }
    // 32 + 4 → AlignUp(36, 16) = 48
    PROP_CHECK(result.cbSizeBytes == 48);
}

// ---------------------------------------------------------------------------
// テスト 12: PackProperties が ok=false 時に false を返す (例外なし)
// ---------------------------------------------------------------------------
void TestPackOnError()
{
    MYE_LOG_INFO("[selftest] Properties DSL: パース失敗時の PackProperties");

    PropertyParseResult bad;
    bad.ok = false;
    std::unordered_map<std::string, PropValue> values;
    std::vector<uint8_t> cb;
    PROP_CHECK(!PackProperties(bad, values, cb));
    PROP_CHECK(cb.empty());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 公開エントリ
// ---------------------------------------------------------------------------
bool RunProjectShaderPropertiesSelfTest()
{
    g_failCount = 0;
    MYE_LOG_INFO("==== ProjectShaderProperties (M78a) self test ====");

    TestParseSpecExample();
    TestParseAttributes();
    TestRangeAsType();
    TestNoBlock();
    TestUnclosedBlock();
    TestUnknownType();
    TestCbOffsets();
    TestPackDefaults();
    TestPackOverride();
    TestCbFourFloatsThenColor();
    TestCbThreeFloatsThenColor();
    TestPackOnError();

    if (g_failCount == 0)
    {
        MYE_LOG_INFO("[selftest] ProjectShaderProperties: ALL PASS");
        return true;
    }
    MYE_LOG_ERROR("[selftest] ProjectShaderProperties: %d FAILED", g_failCount);
    return false;
}

} // namespace mye
