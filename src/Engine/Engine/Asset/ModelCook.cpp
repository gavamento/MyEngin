#include "Engine/Engine/Asset/ModelCook.h"

#include <cstring>
#include <filesystem>
#include <system_error>

#include "Engine/Core/Log.h"
#include "Engine/Engine/Asset/CookedCache.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Renderer/ShaderManager.h"

namespace mye::ModelCook {
namespace {

// blob は生バイト保存 (float ビットパターン維持)。struct を memcpy で書くため、
// レイアウトが変わったら kCookVersion を bump して全キャッシュを無効化すること
static_assert(sizeof(MeshVertex) == 52, "MeshVertex layout changed -- bump kCookVersion");
static_assert(sizeof(Material) == 56, "Material layout changed -- bump kCookVersion");
static_assert(sizeof(DirectX::XMFLOAT4X4) == 64, "XMFLOAT4X4 layout changed");

void Append(std::vector<uint8_t>& buf, const void* src, size_t n)
{
    const uint8_t* b = static_cast<const uint8_t*>(src);
    buf.insert(buf.end(), b, b + n);
}
template <typename T> void AppendPod(std::vector<uint8_t>& buf, const T& v)
{
    Append(buf, &v, sizeof(T));
}
void AppendStr(std::vector<uint8_t>& buf, const std::string& s)
{
    AppendPod(buf, static_cast<uint32_t>(s.size()));
    Append(buf, s.data(), s.size());
}
template <typename T> void AppendVec(std::vector<uint8_t>& buf, const std::vector<T>& v)
{
    AppendPod(buf, static_cast<uint32_t>(v.size()));
    Append(buf, v.data(), v.size() * sizeof(T));
}

// 境界検査つきリーダ。count は残量で必ず検算する (破損 blob の巨大 count で OOM しない)
struct Reader {
    const uint8_t* p = nullptr;
    size_t size = 0;
    size_t pos = 0;

    bool Bytes(void* dst, size_t n)
    {
        if (pos + n > size || pos + n < pos) {
            return false;
        }
        memcpy(dst, p + pos, n);
        pos += n;
        return true;
    }
    template <typename T> bool Pod(T& v) { return Bytes(&v, sizeof(T)); }
    bool Str(std::string& s)
    {
        uint32_t len = 0;
        if (!Pod(len) || pos + len > size) {
            return false;
        }
        s.assign(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        return true;
    }
    template <typename T> bool Vec(std::vector<T>& v)
    {
        uint32_t count = 0;
        if (!Pod(count) || static_cast<uint64_t>(count) * sizeof(T) > size - pos) {
            return false;
        }
        v.resize(count);
        return Bytes(v.data(), static_cast<size_t>(count) * sizeof(T));
    }
    // セクションの要素数を「最小直列化サイズ × 個数 ≤ 残量」で検算してから返す。
    // 破損 blob の巨大 count をそのまま resize すると bad_alloc で落ちる (selftest が突く穴)
    bool Count(uint32_t& n, size_t minElemBytes)
    {
        if (!Pod(n) || static_cast<uint64_t>(n) * minElemBytes > size - pos) {
            return false;
        }
        return true;
    }
};

} // namespace

void ModelCookData::AddTexture(uint8_t kind, bool srgb, std::string key,
                               const std::wstring& path, const void* bytes, size_t size)
{
    for (const CookedTexture& t : textures) {
        if ((t.kind == 1 ? t.path == path : t.key == key) && t.kind == kind) {
            return;
        }
    }
    CookedTexture t;
    t.kind = kind;
    t.srgb = srgb ? 1 : 0;
    t.key = std::move(key);
    t.path = path;
    if (bytes != nullptr && size > 0) {
        const uint8_t* b = static_cast<const uint8_t*>(bytes);
        t.bytes.assign(b, b + size);
    }
    textures.push_back(std::move(t));
}

void ModelCookData::AddMesh(std::string key, const std::vector<MeshVertex>& vertices,
                            const std::vector<uint32_t>& indices)
{
    for (const CookedMesh& m : meshes) {
        if (m.key == key) {
            return;
        }
    }
    meshes.push_back(CookedMesh{ std::move(key), vertices, indices });
}

void ModelCookData::AddMaterial(std::string key, const Material& mat)
{
    for (const CookedMaterial& m : materials) {
        if (m.key == key) {
            return;
        }
    }
    materials.push_back(CookedMaterial{ std::move(key), mat });
}

void ModelCookData::AddSkin(std::string key, const SkinnedModel& model)
{
    for (const CookedSkin& s : skins) {
        if (s.key == key) {
            return;
        }
    }
    skins.push_back(CookedSkin{ std::move(key), model });
}

std::vector<std::wstring> ModelCookData::ExternalDeps() const
{
    std::vector<std::wstring> deps;
    for (const CookedTexture& t : textures) {
        if (t.kind == 1) {
            deps.push_back(t.path);
        }
    }
    return deps;
}

void Serialize(const ModelCookData& d, std::vector<uint8_t>& out)
{
    out.clear();
    AppendPod(out, static_cast<uint32_t>(d.textures.size()));
    for (const CookedTexture& t : d.textures) {
        AppendPod(out, t.kind);
        AppendPod(out, t.srgb);
        AppendStr(out, t.key);
        AppendStr(out, WideToUtf8(t.path));
        AppendVec(out, t.bytes);
    }
    AppendPod(out, static_cast<uint32_t>(d.meshes.size()));
    for (const CookedMesh& m : d.meshes) {
        AppendStr(out, m.key);
        AppendVec(out, m.vertices);
        AppendVec(out, m.indices);
    }
    AppendPod(out, static_cast<uint32_t>(d.materials.size()));
    for (const CookedMaterial& m : d.materials) {
        AppendStr(out, m.key);
        AppendPod(out, m.mat);
    }
    AppendPod(out, static_cast<uint32_t>(d.skins.size()));
    for (const CookedSkin& s : d.skins) {
        AppendStr(out, s.key);
        AppendPod(out, static_cast<uint32_t>(s.model.joints.size()));
        for (const SkeletonJoint& j : s.model.joints) {
            AppendPod(out, j.parent);
            AppendStr(out, j.name);
            AppendPod(out, j.inverseBind);
            AppendPod(out, j.bindT);
            AppendPod(out, j.bindR);
            AppendPod(out, j.bindS);
        }
        AppendPod(out, static_cast<uint32_t>(s.model.clips.size()));
        for (const SkeletalClip& c : s.model.clips) {
            AppendStr(out, c.name);
            AppendPod(out, c.duration);
            AppendPod(out, static_cast<uint32_t>(c.tracks.size()));
            for (const JointTrack& tr : c.tracks) {
                AppendVec(out, tr.tTimes);
                AppendVec(out, tr.tVals);
                AppendVec(out, tr.rTimes);
                AppendVec(out, tr.rVals);
                AppendVec(out, tr.sTimes);
                AppendVec(out, tr.sVals);
            }
        }
    }
}

bool Deserialize(const std::vector<uint8_t>& in, ModelCookData& out)
{
    out = ModelCookData{};
    Reader r{ in.data(), in.size(), 0 };

    // 最小直列化サイズ: texture = kind1+srgb1+key4+path4+bytes4 / mesh = key4+verts4+idx4 /
    // material = key4+Material / skin = key4+joints4+clips4 / joint = parent4+name4+行列+TRS /
    // clip = name4+dur4+tracks4 / track = 6 配列の長さ 4×6
    uint32_t n = 0;
    if (!r.Count(n, 14)) {
        return false;
    }
    out.textures.resize(n);
    for (CookedTexture& t : out.textures) {
        std::string pathUtf8;
        if (!r.Pod(t.kind) || !r.Pod(t.srgb) || !r.Str(t.key) || !r.Str(pathUtf8)
            || !r.Vec(t.bytes)) {
            return false;
        }
        t.path = Utf8ToWide(pathUtf8);
    }
    if (!r.Count(n, 12)) {
        return false;
    }
    out.meshes.resize(n);
    for (CookedMesh& m : out.meshes) {
        if (!r.Str(m.key) || !r.Vec(m.vertices) || !r.Vec(m.indices)) {
            return false;
        }
    }
    if (!r.Count(n, 4 + sizeof(Material))) {
        return false;
    }
    out.materials.resize(n);
    for (CookedMaterial& m : out.materials) {
        if (!r.Str(m.key) || !r.Pod(m.mat)) {
            return false;
        }
    }
    if (!r.Count(n, 12)) {
        return false;
    }
    out.skins.resize(n);
    for (CookedSkin& s : out.skins) {
        uint32_t jn = 0;
        if (!r.Str(s.key) || !r.Count(jn, 112)) {
            return false;
        }
        s.model.joints.resize(jn);
        for (SkeletonJoint& j : s.model.joints) {
            if (!r.Pod(j.parent) || !r.Str(j.name) || !r.Pod(j.inverseBind) || !r.Pod(j.bindT)
                || !r.Pod(j.bindR) || !r.Pod(j.bindS)) {
                return false;
            }
        }
        uint32_t cn = 0;
        if (!r.Count(cn, 12)) {
            return false;
        }
        s.model.clips.resize(cn);
        for (SkeletalClip& c : s.model.clips) {
            uint32_t tn = 0;
            if (!r.Str(c.name) || !r.Pod(c.duration) || !r.Count(tn, 24)) {
                return false;
            }
            c.tracks.resize(tn);
            for (JointTrack& tr : c.tracks) {
                if (!r.Vec(tr.tTimes) || !r.Vec(tr.tVals) || !r.Vec(tr.rTimes) || !r.Vec(tr.rVals)
                    || !r.Vec(tr.sTimes) || !r.Vec(tr.sVals)) {
                    return false;
                }
            }
        }
    }
    return r.pos == r.size; // 末尾ゴミも破損扱い
}

void Replay(RenderResources& resources, ShaderManager& shaders, const ModelCookData& d)
{
    // フレッシュパースが暗黙に行う登録を先に揃える: forward_lit のロード (Material::shader の
    // 実体) と White() の遅延生成 (テクスチャ無しマテリアルの Material::texture が指す)
    shaders.Load("forward_lit");
    resources.textures.White();

    for (const CookedTexture& t : d.textures) {
        if (t.kind == 0) {
            resources.textures.CreateFromEncoded(t.key, t.bytes.data(), t.bytes.size(),
                                                 t.srgb != 0);
        } else {
            std::wstring p = t.path;
            // M51j: 封印キャッシュ (配布ビルド) ではクック元の絶対パスが移設先に存在しない。
            // "\assets\" 以降のテールを現行の assets ルートへ付け替えて読む。.meta も同伴
            // コピーされているので、解決される AssetID はクック時に Material.texture へ
            // 記録された値 (= .meta GUID) と一致する — 参照は壊れない
            std::error_code ec;
            if (CookedCache::Sealed() && !std::filesystem::exists(p, ec)) {
                const std::wstring key = NormalizePathKey(p);
                const size_t pos = key.rfind(L"\\assets\\");
                if (pos != std::wstring::npos) {
                    p = FindAssetsRoot() + key.substr(pos + 7); // "\assets" 直後の '\' から
                }
            }
            resources.textures.LoadFile(p, t.srgb != 0);
        }
    }
    for (const CookedMesh& m : d.meshes) {
        resources.meshes.Register(m.key, m.vertices, m.indices);
    }
    for (const CookedMaterial& m : d.materials) {
        resources.materials.Register(m.key, m.mat);
    }
    for (const CookedSkin& s : d.skins) {
        resources.skinnedModels.Register(s.key, s.model); // 値渡し = コピーが登録される
    }
}

bool TryReplayFromCache(RenderResources& resources, ShaderManager& shaders,
                        const std::wstring& srcPath)
{
    if (!CookedCache::Enabled()) {
        return false;
    }
    std::vector<uint8_t> payload;
    if (!CookedCache::ReadValidated(srcPath, kModelExt, payload)) {
        return false;
    }
    ModelCookData d;
    if (!Deserialize(payload, d)) {
        MYE_LOG_WARN("[cook] corrupt model blob, recooking: %s", WideToUtf8(srcPath).c_str());
        return false;
    }
    Replay(resources, shaders, d);
    MYE_LOG_INFO("[cook] model cache hit: %s (%zu meshes, %zu materials, %zu skins)",
                 WideToUtf8(srcPath).c_str(), d.meshes.size(), d.materials.size(),
                 d.skins.size());
    return true;
}

void SaveToCache(const std::wstring& srcPath, const ModelCookData& d)
{
    if (!CookedCache::Enabled()) {
        return;
    }
    std::vector<uint8_t> payload;
    Serialize(d, payload);
    if (CookedCache::Write(srcPath, kModelExt, payload.data(), payload.size(),
                           d.ExternalDeps())) {
        MYE_LOG_INFO("[cook] model cooked: %s (%zu KB)", WideToUtf8(srcPath).c_str(),
                     payload.size() / 1024);
    }
}

} // namespace mye::ModelCook
