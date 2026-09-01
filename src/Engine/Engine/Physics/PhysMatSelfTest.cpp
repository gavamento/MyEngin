#include "Engine/Engine/Physics/PhysMatSelfTest.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

#include "Engine/Core/Log.h"
#include "Engine/Engine/AssetDatabase.h"
#include "Engine/Engine/Physics/PhysMatLibrary.h"

namespace fs = std::filesystem;

namespace mye {

bool RunPhysMatSelfTest()
{
    MYE_LOG_INFO("==== PhysMat (.physmat.json) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- ClassifyPath: 複合サフィックスの分類 ----
    {
        check(AssetDatabase::ClassifyPath(L"x\\steel.physmat.json") == AssetType::PhysMat,
              ".physmat.json classifies as PhysMat");
        check(AssetDatabase::ClassifyPath(L"x\\steel.PHYSMAT.JSON") == AssetType::PhysMat,
              "physmat classification ignores case");
        // ".physmat.json" と ".mat.json" は末尾 9 文字が違う ("smat.json" vs ".mat.json") ので
        // 現実装では衝突しないが、判定順が入れ替わっても壊れないことを両方向で固定する
        check(AssetDatabase::ClassifyPath(L"x\\steel.physmat.json") != AssetType::Material,
              ".physmat.json never classifies as Material");
        check(AssetDatabase::ClassifyPath(L"x\\steel.mat.json") == AssetType::Material,
              ".mat.json still classifies as Material");
        check(AssetDatabase::ParseTypeName(AssetDatabase::TypeName(AssetType::PhysMat))
                  == AssetType::PhysMat,
              "physmat type name round-trips through .meta");
    }

    // ---- FromJson: 種別キー必須 + 既定値の前方互換読み ----
    {
        PhysMat m;
        nlohmann::json notMat;
        notMat["engine"] = "MyEngine";
        notMat["shader"] = "forward_lit"; // .mat.json を装った別資産
        check(!PhysMatLibrary::FromJson(notMat, m), "FromJson rejects JSON without physmat key");

        nlohmann::json minimal;
        minimal["physmat"] = 1;
        PhysMat d; // 既定値
        check(PhysMatLibrary::FromJson(minimal, m), "FromJson accepts a minimal physmat");
        check(m.density == d.density && m.staticFriction == d.staticFriction
                  && m.dynamicFriction == d.dynamicFriction && m.restitution == d.restitution
                  && m.rollingResistance == d.rollingResistance
                  && m.dragCoefficient == d.dragCoefficient && m.adhesion == d.adhesion,
              "missing keys fall back to struct defaults (forward compat)");
        // ★M60d で足した adhesion が **旧ファイルで 0** になることが「粘着 0 = 従来と
        //   ビット同一」の入口。既定が 0 でなくなった瞬間に全既存資産の挙動が変わる
        check(d.adhesion == 0.0f, "a physmat written before M60d has no adhesion at all");
        // ★M65c で足した音響 3 本も同じ形。**旧ファイルは 3 本とも 0 = 完全に無音**で、
        //   足音も衝撃音も 1 本の波も出さない (存在ゲート)。既定が 0 でなくなった瞬間に
        //   既存 5 プリセットを使う全シーンが音を出し始める
        check(m.acousticLoudness == d.acousticLoudness && m.acousticRadiusM == d.acousticRadiusM
                  && m.acousticTone == d.acousticTone,
              "missing acoustic keys fall back to struct defaults (M65c forward compat)");
        check(d.acousticLoudness == 0.0f && d.acousticRadiusM == 0.0f && d.acousticTone == 0,
              "a physmat written before M65c is silent");

        // 音色は整数キー。float で書かれていても**読まない** (0..3 の意味が丸めに乗るため)
        nlohmann::json toned;
        toned["physmat"] = 1;
        toned["acousticTone"] = 2.7;
        PhysMat tm;
        check(PhysMatLibrary::FromJson(toned, tm) && tm.acousticTone == 0,
              "acousticTone ignores a non-integer JSON value");
    }

    // ---- Sanitize: NaN / 負値 / 範囲外の防波堤 ----
    {
        PhysMat m;
        const PhysMat d;
        m.density = std::numeric_limits<float>::quiet_NaN();
        m.staticFriction = -1.0f;
        m.dynamicFriction = std::numeric_limits<float>::infinity();
        m.restitution = 1.5f;
        m.rollingResistance = -0.25f;
        m.dragCoefficient = -std::numeric_limits<float>::infinity();
        m.adhesion = -50.0f;
        m.acousticLoudness = -1.0f;                                  // M65c
        m.acousticRadiusM = std::numeric_limits<float>::quiet_NaN(); // 同上
        m.acousticTone = 9;                                          // 同上
        PhysMatLibrary::Sanitize(m);
        check(m.density == d.density, "NaN density falls back to default (not 0)");
        check(m.staticFriction == 0.0f, "negative static friction clamps to 0");
        check(m.dynamicFriction == d.dynamicFriction,
              "+inf dynamic friction falls back to default (non-finite is garbage, not a bound)");
        check(m.restitution == 1.0f, "restitution clamps to [0,1]");
        check(m.rollingResistance == 0.0f, "negative rolling resistance clamps to 0");
        check(m.dragCoefficient == d.dragCoefficient, "-inf drag falls back to default");
        check(m.adhesion == 0.0f, "negative adhesion clamps to 0 (a contact never repels harder)");
        // M65c: 負の振幅は EnergyAt の単調減少を壊し、NaN の半径は maxRing の
        // 切り捨てで未定義になる。**音色の範囲外は色表の添字**になるので特に効く
        check(m.acousticLoudness == 0.0f, "negative acoustic loudness clamps to 0 (silent)");
        check(m.acousticRadiusM == d.acousticRadiusM, "NaN acoustic radius falls back to default");
        check(m.acousticTone == 3, "acoustic tone clamps into the 4 colour slots");
        m.density = 0.0f;
        m.dynamicFriction = 250.0f; // 有限の範囲外はクランプ (非有限との扱いの差を固定)
        PhysMatLibrary::Sanitize(m);
        check(m.density == 0.001f, "zero density clamps to the divide-safe floor");
        check(m.dynamicFriction == 100.0f, "finite out-of-range friction clamps to the bound");
    }

    // ---- ToJson → FromJson のビット同一往復 ----
    {
        PhysMat src;
        src.name = "roundtrip";
        src.density = 7850.0f;
        src.staticFriction = 0.74f;
        src.dynamicFriction = 0.45f;
        src.restitution = 0.6f;
        src.rollingResistance = 0.001f;
        src.dragCoefficient = 0.47f;
        src.adhesion = 25.0f; // M60d
        src.acousticLoudness = 0.55f; // M65c
        src.acousticRadiusM = 13.0f;
        src.acousticTone = 2;
        PhysMat dst;
        check(PhysMatLibrary::FromJson(PhysMatLibrary::ToJson(src), dst),
              "ToJson output parses back");
        check(dst.name == src.name && dst.density == src.density
                  && dst.staticFriction == src.staticFriction
                  && dst.dynamicFriction == src.dynamicFriction
                  && dst.restitution == src.restitution
                  && dst.rollingResistance == src.rollingResistance
                  && dst.dragCoefficient == src.dragCoefficient && dst.adhesion == src.adhesion
                  && dst.acousticLoudness == src.acousticLoudness
                  && dst.acousticRadiusM == src.acousticRadiusM
                  && dst.acousticTone == src.acousticTone,
              "ToJson/FromJson round-trip is bit-identical");
    }

    // ---- Register / Get / Enumerate (名前昇順) ----
    {
        PhysMatLibrary lib;
        PhysMat a;
        a.restitution = 0.8f;
        const uint64_t hb = lib.Register(L"x\\bbb.physmat.json", PhysMat{});
        const uint64_t ha = lib.Register(L"x\\aaa.physmat.json", a);
        check(ha != 0 && hb != 0 && ha != hb, "register yields distinct nonzero hashes");
        check(lib.Contains(ha) && lib.Get(ha) != nullptr && lib.Get(ha)->restitution == 0.8f,
              "Get returns the registered material");
        const auto entries = lib.Enumerate();
        check(entries.size() == 2 && entries[0].name == "aaa" && entries[1].name == "bbb",
              "Enumerate sorts by name ascending (rule 7)");
        check(entries[0].name == "aaa" && lib.Get(entries[0].hash) != nullptr,
              "compound suffix strips to a bare display name");
    }

    // ---- physmat:: 注入の null 安全 ----
    {
        physmat::Install(nullptr);
        check(physmat::Library() == nullptr && physmat::Resolve(AssetID{ 123 }) == nullptr,
              "Resolve without an installed library is a safe nullptr");
        PhysMatLibrary lib;
        const uint64_t h = lib.Register(L"x\\steel.physmat.json", PhysMat{});
        physmat::Install(&lib);
        check(physmat::Resolve(AssetID{}) == nullptr, "Resolve of a null AssetID is nullptr");
        check(physmat::Resolve(AssetID{ h }) == lib.Get(h), "Resolve finds a registered material");
        check(physmat::Resolve(AssetID{ h ^ 1 }) == nullptr, "Resolve of an unknown id is nullptr");
        physmat::Install(nullptr); // 後続テストに注入状態を漏らさない
    }

    // ---- LoadFromFile: 実ファイル経由 (temp) ----
    {
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / L"mye_physmat_selftest";
        fs::create_directories(dir, ec);
        const fs::path good = dir / L"steel.physmat.json";
        {
            PhysMat p;
            p.density = 7850.0f;
            p.restitution = 0.6f;
            std::ofstream f(good, std::ios::binary);
            f << PhysMatLibrary::ToJson(p).dump(2);
        }
        const fs::path bad = dir / L"broken.physmat.json";
        {
            std::ofstream f(bad, std::ios::binary);
            f << "{ this is not json";
        }
        PhysMatLibrary lib;
        const uint64_t h = lib.LoadFromFile(good.wstring());
        check(h != 0 && lib.Get(h) != nullptr && lib.Get(h)->density == 7850.0f,
              "LoadFromFile registers a well-formed physmat");
        check(lib.Get(h)->name == "steel", "asset name derives from the file stem");
        check(lib.LoadFromFile(good.wstring()) == h, "reload is idempotent (same hash)");
        check(lib.LoadFromFile(bad.wstring()) == 0, "parse failure returns 0 (not registered)");
        fs::remove_all(dir, ec);
    }

    MYE_LOG_INFO("==== PhysMat self test: %s ====", failCount == 0 ? "PASS" : "FAIL");
    return failCount == 0;
}

} // namespace mye
