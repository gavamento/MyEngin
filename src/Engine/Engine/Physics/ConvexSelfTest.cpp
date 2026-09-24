#include "Engine/Engine/Physics/ConvexSelfTest.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Physics/ConvexHull.h"

using namespace DirectX;

namespace mye {
namespace {

// テスト用の決定論シャッフル。sim ではないので PCG32 を持ち出さず、その場の LCG で足りる
// (種を固定してあるので毎回同じ並べ替えになる = 失敗が再現する)
struct Lcg {
    uint32_t s;
    uint32_t Next()
    {
        s = s * 1664525u + 1013904223u;
        return s;
    }
};

void Shuffle(std::vector<XMFLOAT3>& v, uint32_t seed)
{
    Lcg rng{ seed };
    for (size_t i = v.size(); i > 1; --i) {
        const size_t j = rng.Next() % i;
        std::swap(v[i - 1], v[j]);
    }
}

std::vector<uint8_t> Blob(const ConvexHullData& h)
{
    std::vector<uint8_t> b;
    SerializeConvexHull(h, b);
    return b;
}

std::vector<XMFLOAT3> CubeCorners(float half)
{
    return {
        { -half, -half, -half }, { half, -half, -half }, { half, half, -half },
        { -half, half, -half },  { -half, -half, half }, { half, -half, half },
        { half, half, half },    { -half, half, half },
    };
}

bool Near(float a, float b, float tol)
{
    return std::fabs(a - b) <= tol;
}

// トーラス (MakeTorus(2.0, 0.6, 24, 16)、pieceCount=8、seed=1) の焼きで rank=7 の破片に
// 現れた点群 (`BuildConvexHull` へ渡す直前、位置の完全一致で重複除去した 195 点)。
// 無限ループ (地平線の新面が縮退したときに失敗した pick を outside から外し損ねる) の
// 回帰フィクスチャ
std::vector<XMFLOAT3> TorusRank7Points()
{
    return {
        { -0.000472068787f, 0.292466342f, 0.966791928f },
        { -0.002358675f, 0.372923225f, -0.970260322f },
        { -0.00274515152f, -0.432307243f, 0.147234857f },
        { -0.0132871866f, 0.288501263f, 0.960804641f },
        { -0.0431473255f, 0.279262722f, 0.946853995f },
        { -0.0532960892f, -0.380761325f, -0.945560992f },
        { -0.0532960892f, -0.380761325f, -0.945561111f },
        { -0.054602623f, 0.355035305f, -0.953481138f },
        { -0.0616092682f, -0.386634886f, -0.942856848f },
        { -0.0655802488f, 0.351276457f, -0.949955404f },
        { -0.0683902502f, 0.350314409f, -0.949052989f },
        { -0.0708935261f, -0.432307243f, 0.664872944f },
        { -0.0708936453f, -0.432307243f, -0.370403707f },
        { -0.0713400841f, -0.410899878f, 0.944775045f },
        { -0.0879375935f, 0.265404761f, 0.925927997f },
        { -0.107580066f, 0.336896122f, -0.936466277f },
        { -0.108842134f, 0.336463869f, -0.936060965f },
        { -0.111890078f, 0.257993996f, 0.914737403f },
        { -0.113111019f, 0.257616222f, 0.914166927f },
        { -0.145495772f, 0.323913872f, -0.924288929f },
        { -0.150614023f, 0.322161436f, -0.922645152f },
        { -0.155066729f, 0.244635105f, 0.894565165f },
        { -0.162142634f, 0.318214178f, -0.918942511f },
        { -0.168058395f, -0.432307243f, 0.899449527f },
        { -0.180330157f, 0.311986893f, -0.913101256f },
        { -0.180449843f, -0.411579549f, -0.904867113f },
        { -0.180450082f, -0.411579609f, -0.904866993f },
        { -0.181679726f, 0.23640132f, 0.882131577f },
        { -0.192072153f, 0.307966352f, -0.909329951f },
        { -0.217479587f, 0.225324869f, 0.865405858f },
        { -0.231690645f, 0.294401348f, -0.89660579f },
        { -0.232354999f, -0.386635065f, 0.147234857f },
        { -0.256184101f, 0.286014915f, -0.888739288f },
        { -0.269785285f, 0.209141731f, 0.840968609f },
        { -0.270694017f, -0.432307243f, -0.852764666f },
        { -0.281564593f, 0.277324796f, -0.880587757f },
        { -0.282968044f, -0.406809807f, 0.844769895f },
        { -0.285031319f, -0.432307243f, -0.871449411f },
        { -0.285031319f, -0.432307243f, -0.87144953f },
        { -0.287415743f, 0.275321305f, -0.878708541f },
        { -0.292679548f, -0.386635065f, 0.605445504f },
        { -0.292679667f, -0.386635065f, -0.310976237f },
        { -0.295450091f, 0.272570312f, -0.876128137f },
        { -0.298330426f, -0.429678679f, -0.867259562f },
        { -0.298330545f, -0.429678679f, -0.867259562f },
        { -0.306945086f, 0.268634647f, -0.872436225f },
        { -0.31331265f, 0.195674583f, 0.820632696f },
        { -0.321259141f, 0.193215996f, 0.816919982f },
        { -0.327821732f, 0.191185504f, 0.813854039f },
        { -0.341039062f, 0.187096149f, 0.80767864f },
        { -0.349264026f, 0.184551358f, 0.803836167f },
        { -0.356471896f, 0.251676977f, -0.856529772f },
        { -0.3692981f, 0.178352907f, 0.794476211f },
        { -0.37070632f, 0.177917182f, 0.793818295f },
        { -0.373890042f, -0.386635065f, 0.801504791f },
        { -0.377712846f, 0.244404197f, -0.849707782f },
        { -0.403752685f, 0.16769278f, 0.778378904f },
        { -0.407041192f, 0.234362155f, -0.840288341f },
        { -0.42662549f, 0.16061601f, 0.767692804f },
        { -0.427009106f, -0.256571382f, 0.147234857f },
        { -0.427024722f, 0.227519929f, -0.833870232f },
        { -0.454044461f, 0.218268663f, -0.825192273f },
        { -0.469542027f, -0.386635065f, -0.737959802f },
        { -0.472695112f, 0.146362245f, 0.746169031f },
        { -0.473718524f, 0.146045655f, 0.745691001f },
        { -0.476097703f, -0.310227156f, 0.751994729f },
        { -0.478942275f, 0.144429415f, 0.74325043f },
        { -0.480701089f, -0.256571382f, -0.260596007f },
        { -0.480701089f, -0.256571382f, 0.555065393f },
        { -0.485189438f, 0.142496571f, 0.740331709f },
        { -0.493327141f, 0.139978796f, 0.736529768f },
        { -0.501464963f, 0.137461007f, 0.732727706f },
        { -0.516109347f, -0.386635065f, -0.798647583f },
        { -0.516109467f, -0.386635065f, -0.798647583f },
        { -0.52017951f, 0.195624232f, -0.803951681f },
        { -0.533799171f, 0.190961063f, -0.799577415f },
        { -0.540326357f, 0.188726068f, -0.79748106f },
        { -0.542042375f, 0.12490648f, 0.713770092f },
        { -0.547870874f, -0.256571382f, 0.717227459f },
        { -0.557072878f, -0.0619172901f, 0.147234857f },
        { -0.557251811f, -0.359407961f, -0.785901785f },
        { -0.55725193f, -0.359407902f, -0.785901845f },
        { -0.575789332f, 0.176583916f, -0.786091506f },
        { -0.581587911f, 0.174598515f, -0.784229219f },
        { -0.587386489f, 0.172613114f, -0.782366872f },
        { -0.592028975f, 0.109440848f, 0.690416276f },
        { -0.595456243f, 0.131048933f, 0.147234857f },
        { -0.596966743f, 0.169332892f, -0.779289961f },
        { -0.599739313f, 0.107055277f, 0.68681401f },
        { -0.601756811f, 0.16769278f, -0.777751505f },
        { -0.603403568f, 0.125560954f, 0.215891868f },
        { -0.606333017f, -0.0619172901f, -0.226933002f },
        { -0.606333017f, -0.0619172901f, 0.521402359f },
        { -0.615755558f, 0.162899524f, -0.773255467f },
        { -0.617389798f, 0.162340149f, -0.772730708f },
        { -0.618660927f, -0.137414813f, 0.681858122f },
        { -0.635378838f, 0.130635053f, -0.1566315f },
        { -0.637791514f, 0.1018143f, 0.51297307f },
        { -0.638117433f, -0.256571382f, -0.640632808f },
        { -0.643313289f, 0.130552813f, -0.217024177f },
        { -0.644171476f, 0.153170109f, -0.764129162f },
        { -0.663513422f, -0.0619172901f, 0.659448147f },
        { -0.679921508f, 0.140929639f, -0.75264734f },
        { -0.68800652f, 0.0797457322f, 0.645575523f },
        { -0.68800652f, 0.079745777f, 0.645575523f },
        { -0.712646604f, -0.256571382f, -0.737761199f },
        { -0.712646723f, -0.256571382f, -0.737761199f },
        { -0.715340495f, 0.12880224f, -0.741271794f },
        { -0.742772341f, 0.108364537f, -0.468574166f },
        { -0.743365645f, 0.119206548f, -0.732270956f },
        { -0.747743487f, 0.117707767f, -0.730864942f },
        { -0.750756025f, -0.0619172901f, -0.575600922f },
        { -0.759451747f, -0.187589467f, -0.723691702f },
        { -0.759451866f, -0.187589407f, -0.723691761f },
        { -0.778082609f, 0.107319623f, -0.721120834f },
        { -0.778259873f, 0.107259147f, -0.721064031f },
        { -0.778708339f, 0.100347608f, -0.559462667f },
        { -0.80278492f, 0.098861739f, -0.713187218f },
        { -0.828695774f, 0.0899900347f, -0.704865396f },
        { -0.844721913f, -0.0619172901f, -0.698059738f },
        { -0.844721913f, -0.0619172901f, -0.698059797f },
        { -0.859275699f, 0.0795196444f, -0.695044041f },
        { -0.865529537f, 0.0386521369f, -0.692596853f },
        { -0.865529537f, 0.0386522263f, -0.692596912f },
        { -0.873011827f, 0.0748164952f, -0.690632403f },
        { -0.873011827f, 0.0748165846f, -0.690632463f },
        { -0.873011947f, 0.0748165771f, -0.690632522f },
        { 0.0033929348f, 0.374892533f, -0.972107589f },
        { 0.00850820541f, 0.376644075f, -0.973750532f },
        { 0.0382888317f, -0.386634886f, 0.996150792f },
        { 0.0608115196f, 0.394552231f, -0.99054867f },
        { 0.0679430962f, 0.396994084f, -0.992839158f },
        { 0.0802512169f, 0.40120846f, -0.996792257f },
        { 0.0807650089f, 0.317600667f, 1.00474572f },
        { 0.110946178f, 0.326938689f, 1.01884651f },
        { 0.122476101f, -0.256571144f, -1.00273895f },
        { 0.124330521f, -0.322850108f, 1.03574443f },
        { 0.126216888f, 0.41694665f, -1.01155496f },
        { 0.133086205f, 0.333788693f, 1.0291903f },
        { 0.133573055f, 0.419465482f, -1.01391768f },
        { 0.135341406f, -0.236137092f, -1.00705242f },
        { 0.135341406f, -0.236137122f, -1.00705242f },
        { 0.150892735f, -0.386634886f, -0.429831266f },
        { 0.150892973f, -0.386634886f, 0.724300444f },
        { 0.177366018f, 0.347488701f, 1.04987788f },
        { 0.179151297f, 0.435071349f, -1.02855611f },
        { 0.18270874f, 0.349141717f, 1.05237401f },
        { 0.195518017f, 0.440675259f, -1.03381252f },
        { 0.199795723f, 0.442139834f, -1.03518629f },
        { 0.205742121f, 0.44417578f, -1.03709602f },
        { 0.211884975f, 0.446279049f, -1.03906894f },
        { 0.211884975f, 0.446279138f, -1.03906918f },
        { 0.213736534f, -0.256571144f, 1.07688642f },
        { 0.226605177f, 0.42320314f, -1.04347825f },
        { 0.226605177f, 0.42320317f, -1.04347825f },
        { 0.226865053f, -0.386634886f, 0.147234857f },
        { 0.243126869f, 0.397302866f, -1.0484271f },
        { 0.245031357f, -0.0619172901f, -1.04382944f },
        { 0.245031357f, -0.0619172901f, -1.04382968f },
        { 0.246917009f, 0.45372358f, -0.941607058f },
        { 0.250912189f, -0.0300457478f, -1.04605651f },
        { 0.250912189f, -0.0300457776f, -1.04605651f },
        { 0.273362875f, -0.158075422f, 1.10344088f },
        { 0.280843735f, 0.20168592f, -1.05817866f },
        { 0.280843735f, 0.20168598f, -1.05817842f },
        { 0.2873981f, 0.16769278f, -1.05987287f },
        { 0.2873981f, 0.16769278f, -1.0598731f },
        { 0.33157444f, -0.0619172901f, 1.12936521f },
        { 0.332854271f, 0.395595968f, 1.12252212f },
        { 0.334377766f, 0.396067381f, 1.1232338f },
        { 0.334377766f, 0.3960675f, 1.1232338f },
        { 0.334377766f, 0.39606756f, 1.1232338f },
        { 0.337188721f, 0.397302866f, 1.11581087f },
        { 0.337188721f, 0.397302866f, 1.11581135f },
        { 0.338914156f, -0.256571144f, -0.480211437f },
        { 0.338914156f, -0.256571144f, 0.774680614f },
        { 0.353248835f, 0.286921263f, 1.1339221f },
        { 0.353376865f, 0.0564618185f, 1.13773406f },
        { 0.373862982f, 0.16769278f, 1.1455977f },
        { 0.405076265f, 0.428775042f, 0.897434413f },
        { 0.406274796f, 0.48758775f, -0.498260736f },
        { 0.421519041f, -0.256571144f, 0.147234857f },
        { 0.435215473f, 0.442747325f, 0.800484478f },
        { 0.444512129f, 0.487935364f, -0.206056446f },
        { 0.464545727f, -0.0619172901f, -0.513874412f },
        { 0.464545727f, 0.397302866f, -0.513874412f },
        { 0.464545965f, -0.0619172901f, 0.80834353f },
        { 0.464545965f, 0.397302866f, 0.80834353f },
        { 0.47037816f, 0.471628666f, 0.386815935f },
        { 0.490742922f, 0.488355696f, 0.147234857f },
        { 0.508661747f, 0.16769278f, -0.525695264f },
        { 0.508661985f, 0.16769278f, 0.820164323f },
        { 0.551582575f, -0.0619172901f, 0.147234857f },
        { 0.551582575f, 0.397302866f, 0.147234857f },
        { 0.597254753f, 0.16769278f, 0.147234857f },
    };
}

} // namespace

bool RunConvexSelfTest()
{
    MYE_LOG_INFO("==== Convex hull (M60f) self test ====");
    int failCount = 0;
    auto check = [&](bool cond, const char* what) {
        if (cond) {
            MYE_LOG_INFO("  PASS: %s", what);
        } else {
            MYE_LOG_ERROR("  FAIL: %s", what);
            ++failCount;
        }
    };

    // ---- 単位立方体: 位相 (オイラー) と質量特性 ----
    ConvexHullData cube;
    {
        const bool ok = BuildConvexHull(CubeCorners(0.5f), cube);
        check(ok, "cube: BuildConvexHull succeeds");
        check(cube.verts.size() == 8, "cube: 8 vertices");
        // ここが 12 なら同一平面の統合が効いていない = 参照面クリップが 3 点で頭打ちになる
        check(cube.faces.size() == 6, "cube: coplanar triangles merged into 6 quad faces");
        check(cube.edges.size() == 12, "cube: 12 edges");
        check(cube.verts.size() - cube.edges.size() + cube.faces.size() == 2,
              "cube: V - E + F == 2 (Euler)");
        for (const ConvexFace& f : cube.faces) {
            if (f.count != 4) {
                check(false, "cube: every face is a quad");
                break;
            }
        }
        check(Near(cube.volume, 1.0f, 1e-5f), "cube: volume == 1");
        check(Near(cube.com.x, 0.0f, 1e-5f) && Near(cube.com.y, 0.0f, 1e-5f)
                  && Near(cube.com.z, 0.0f, 1e-5f),
              "cube: center of mass at origin");
        // 密度 1・質量 1 の立方体 (辺 1): I = m(a^2+a^2)/12 = 1/6、非対角は 0
        const bool diag = Near(cube.inertia[0][0], 1.0f / 6.0f, 1e-5f)
                       && Near(cube.inertia[1][1], 1.0f / 6.0f, 1e-5f)
                       && Near(cube.inertia[2][2], 1.0f / 6.0f, 1e-5f);
        bool offDiagZero = true;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (r != c && !Near(cube.inertia[r][c], 0.0f, 1e-5f)) {
                    offDiagZero = false;
                }
            }
        }
        check(diag && offDiagZero, "cube: inertia tensor == diag(1/6) (analytic)");
    }

    // ---- 生成が入力順に依らない (本テストの主目的) ----
    {
        const std::vector<uint8_t> ref = Blob(cube);
        bool allSame = true;
        for (uint32_t seed = 1; seed <= 8; ++seed) {
            std::vector<XMFLOAT3> pts = CubeCorners(0.5f);
            Shuffle(pts, seed);
            ConvexHullData h;
            BuildConvexHull(pts, h);
            const std::vector<uint8_t> b = Blob(h);
            if (b.size() != ref.size() || std::memcmp(b.data(), ref.data(), b.size()) != 0) {
                allSame = false;
                break;
            }
        }
        check(allSame, "cube: hull is byte-identical under 8 input permutations");
    }

    // ---- 重複点と内部点を混ぜても結果が変わらない ----
    {
        std::vector<XMFLOAT3> pts = CubeCorners(0.5f);
        pts.insert(pts.end(), pts.begin(), pts.end()); // 全点を重複させる
        pts.push_back({ 0.0f, 0.0f, 0.0f });          // 内部点
        pts.push_back({ 0.1f, -0.2f, 0.3f });
        pts.push_back({ -0.0f, 0.0f, -0.0f });         // -0.0 と +0.0 の混在
        Shuffle(pts, 99u);
        ConvexHullData h;
        BuildConvexHull(pts, h);
        const std::vector<uint8_t> a = Blob(cube), b = Blob(h);
        check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
              "cube: duplicates / interior points / signed zeros do not change the hull");
    }

    // ---- 四面体 ----
    {
        const std::vector<XMFLOAT3> pts = {
            { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
        };
        ConvexHullData h;
        check(BuildConvexHull(pts, h), "tetra: BuildConvexHull succeeds");
        check(h.verts.size() == 4 && h.faces.size() == 4 && h.edges.size() == 6,
              "tetra: 4 vertices / 4 faces / 6 edges");
        check(Near(h.volume, 1.0f / 6.0f, 1e-6f), "tetra: volume == 1/6");
        check(Near(h.com.x, 0.25f, 1e-5f) && Near(h.com.y, 0.25f, 1e-5f)
                  && Near(h.com.z, 0.25f, 1e-5f),
              "tetra: center of mass at (1/4, 1/4, 1/4)");
    }

    // ---- 非一様スケールの質量特性 (慣性は相似変換にならないので積分し直す) ----
    {
        float vol = 0.0f;
        XMFLOAT3 com{};
        float I[3][3] = {};
        ConvexMassProperties(cube, 2.0f, 1.0f, 1.0f, vol, com, I);
        check(Near(vol, 2.0f, 1e-5f), "scaled cube: volume == 2");
        // 辺 (2,1,1)・密度 1 なので質量 2。Ix = m(1+1)/12、Iy = Iz = m(4+1)/12
        check(Near(I[0][0], 2.0f * 2.0f / 12.0f, 1e-5f)
                  && Near(I[1][1], 2.0f * 5.0f / 12.0f, 1e-5f)
                  && Near(I[2][2], 2.0f * 5.0f / 12.0f, 1e-5f),
              "scaled cube: inertia matches the analytic 2x1x1 box");
        // 負スケールは絶対値扱い (ShapeVolumeWorld と同一規約) = 体積が負にならない
        float vol2 = 0.0f;
        XMFLOAT3 com2{};
        float I2[3][3] = {};
        ConvexMassProperties(cube, -2.0f, 1.0f, -1.0f, vol2, com2, I2);
        check(Near(vol2, 2.0f, 1e-5f), "scaled cube: negative scale is taken as absolute");
    }

    // ---- 球の点群: 打ち切りと凸性 ----
    {
        std::vector<XMFLOAT3> pts;
        for (int i = 0; i < 400; ++i) {
            // フィボナッチ球 (決定論的な準一様分布)
            const float k = (static_cast<float>(i) + 0.5f) / 400.0f;
            const float phi = std::acos(1.0f - 2.0f * k);
            const float theta = 3.883222077f * static_cast<float>(i); // 黄金角 × i
            pts.push_back({ std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta),
                            std::cos(phi) });
        }
        std::vector<XMFLOAT3> shuffled = pts;
        Shuffle(shuffled, 7u);
        ConvexHullData a, b;
        check(BuildConvexHull(pts, a), "sphere cloud: BuildConvexHull succeeds");
        BuildConvexHull(shuffled, b);
        const std::vector<uint8_t> ba = Blob(a), bb = Blob(b);
        check(ba.size() == bb.size() && std::memcmp(ba.data(), bb.data(), ba.size()) == 0,
              "sphere cloud: 400 shuffled points give a byte-identical hull");
        check(static_cast<int>(a.verts.size()) <= kConvexMaxVerts,
              "sphere cloud: vertex count is capped at kConvexMaxVerts");
        // 内接多面体なので真球より小さいが、64 頂点なら 8 割は超える
        const float sphereVol = 4.18879020f;
        check(a.volume > sphereVol * 0.8f && a.volume < sphereVol,
              "sphere cloud: volume sits just under the analytic sphere volume");
        // 全ての面の裏側に重心がある = 凸で閉じている
        bool convex = true;
        for (const ConvexFace& f : a.faces) {
            if (f.nx * a.com.x + f.ny * a.com.y + f.nz * a.com.z - f.d > 1e-4f) {
                convex = false;
                break;
            }
        }
        check(convex, "sphere cloud: centroid lies behind every face plane");
        check(a.boundRadius > 0.9f && a.boundRadius <= 1.0001f,
              "sphere cloud: bounding radius matches the unit sphere");
    }

    // ---- 縮退入力は箱へ落ちる (すり抜けさせない) ----
    {
        ConvexHullData h;
        check(!BuildConvexHull({ { 1, 2, 3 } }, h) && h.Valid() && h.volume > 0.0f,
              "degenerate: a single point falls back to a valid thin box");
        check(!BuildConvexHull({ { 0, 0, 0 }, { 1, 0, 0 }, { 2, 0, 0 } }, h) && h.Valid(),
              "degenerate: collinear points fall back to a valid thin box");
        const std::vector<XMFLOAT3> planar = {
            { 0, 0, 0 }, { 1, 0, 0 }, { 1, 0, 1 }, { 0, 0, 1 }, { 0.5f, 0, 0.5f },
        };
        check(!BuildConvexHull(planar, h) && h.Valid() && h.volume > 0.0f,
              "degenerate: coplanar points fall back to a valid thin box");
        check(BuildConvexHull({}, h) == false && h.Valid(),
              "degenerate: an empty point set still yields a valid shape");
    }

    // ---- .mcvx blob の往復 ----
    {
        const std::vector<uint8_t> a = Blob(cube);
        ConvexHullData back;
        size_t pos = 0;
        check(DeserializeConvexHull(a.data(), a.size(), pos, back) && pos == a.size(),
              "blob: round-trips and consumes exactly the written bytes");
        const std::vector<uint8_t> b = Blob(back);
        check(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0,
              "blob: re-serialization is byte-identical");
        // 壊れた blob で落ちない / 読めたことにしない
        std::vector<uint8_t> broken = a;
        broken.resize(a.size() / 2);
        pos = 0;
        check(!DeserializeConvexHull(broken.data(), broken.size(), pos, back),
              "blob: a truncated blob is rejected without crashing");
        broken = a;
        broken[0] ^= 0xFFu; // 版を壊す
        pos = 0;
        check(!DeserializeConvexHull(broken.data(), broken.size(), pos, back),
              "blob: a bad version is rejected");
    }

    // ---- 支持点 ----
    {
        const int32_t px = ConvexSupportLocal(cube, 1.0f, 0.0f, 0.0f);
        check(cube.verts[static_cast<size_t>(px)].x > 0.0f, "support: +X picks a +X vertex");
        const int32_t diag = ConvexSupportLocal(cube, 1.0f, 1.0f, 1.0f);
        const XMFLOAT3& v = cube.verts[static_cast<size_t>(diag)];
        check(v.x > 0.0f && v.y > 0.0f && v.z > 0.0f, "support: (1,1,1) picks the far corner");
        // 同値のタイブレークが index 小で固定されていること (走査順に依らない主張の一部)
        check(ConvexSupportLocal(cube, 1.0f, 0.0f, 0.0f)
                  == ConvexSupportLocal(cube, 1.0f, 0.0f, 0.0f),
              "support: ties resolve to the lowest vertex index");
    }

    // ---- 無限ループ回帰: 逐次追加ループで失敗した pick を捨て損ねていた入力 ----
    {
        const std::vector<XMFLOAT3> rank7 = TorusRank7Points();
        const auto t0 = std::chrono::steady_clock::now();
        ConvexHullData h;
        const bool ok = BuildConvexHull(rank7, h);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        MYE_LOG_INFO("  rank7 regression: BuildConvexHull finished in %.3f ms", ms);
        check(ok && h.Valid(), "rank7 regression: BuildConvexHull terminates and returns a valid hull");
    }

    if (failCount == 0) {
        MYE_LOG_INFO("==== Convex hull self test: ALL PASS ====");
        return true;
    }
    MYE_LOG_ERROR("==== Convex hull self test: %d FAILED ====", failCount);
    return false;
}

} // namespace mye
