//====================================================================================
//                          ScenePreviewSelfTest.cpp
//  MyEngine/ 秋田蓮音                                                      11/16/2026
//                                          Before / After の 3D 比較 (プレビュー専用の Scene・本番を変えないこと・注記・失敗) の回帰テスト
//====================================================================================
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <process.h>
#include <set>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

#include "Editor/ChangeTracking/ChangeTrackingFakeIo.h"
#include "Editor/ChangeTracking/ChangeTrackingSelfTest.h"
#include "Editor/ChangeTracking/ChangeTrackingSession.h"
#include "Editor/ChangeTracking/PathGuard.h"
#include "Editor/ChangeTracking/ScenePreviewFormat.h"
#include "Editor/ChangeTracking/ScenePreviewSession.h"
#include "Editor/Scene/Selection.h"
#include "Editor/Undo/UndoStack.h"
#include "Editor/Windows/Project/AiChangesWindow.h"
#include "Editor/Windows/Project/ScenePreviewWindow.h"
#include "Engine/Core/Ecs/ComponentRegistry.h"
#include "Engine/Core/Ecs/Components.h"
#include "Engine/Core/Localization/Localization.h"
#include "Engine/Engine/Demo/ShowcaseScenes.h"
#include "Engine/Engine/HotReload/ReloadHub.h"
#include "Engine/Engine/Loop/HeadlessSim.h"
#include "Engine/Engine/Replay/WorldHasher.h"
#include "Engine/Engine/Scene/GameObject.h"
#include "Engine/Engine/Scene/Scene.h"
#include "Engine/Engine/Scene/SceneSerializer.h"
#include "Engine/Engine/Script/ManagedHost.h"
#include "Engine/Platform/Input.h"
#include "Engine/Platform/PathUtil.h"
#include "Engine/Platform/Sha256.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace mye {
namespace {

using namespace tracktest;
using Check = std::function<void(bool, const char*)>;

template <typename F>
void InBothLanguages(F&& body)
{
    const Lang previous = CurrentLanguage();
    for (const Lang lang : { Lang::En, Lang::Ja }) {
        SetLanguage(lang);
        body(lang);
    }
    SetLanguage(previous);
}

// ---- 道具: シーンの JSON ----

struct EntSpec {
    const char* name = "e";
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    int parent = -1;     // entities の添字
    uint64_t mesh = 0;   // MeshRenderer.mesh (AssetRef)。0 なら MeshRenderer を付けない
    uint64_t material = 0;
};

// 本物の Scene を組み、SaveToJson で書いた JSON を返す (fileId は 1 から順)。試験の Scene なので採番してよい
json MakeSceneDoc(const std::vector<EntSpec>& specs)
{
    Scene scene;
    std::vector<GameObject> objects;
    for (const EntSpec& spec : specs) {
        GameObject object = scene.CreateGameObjectTracked(spec.name);
        LocalTransform* transform = object.GetComponent<LocalTransform>();
        transform->position = { spec.x, spec.y, spec.z };
        if (spec.mesh != 0) {
            MeshRendererComponent* renderer = object.AddComponent<MeshRendererComponent>();
            renderer->mesh.value = spec.mesh;
            renderer->material.value = spec.material;
        }
        if (spec.parent >= 0) {
            object.SetParent(objects[static_cast<size_t>(spec.parent)]);
        }
        objects.push_back(object);
    }
    scene.GetWorld().ApplyStructuralChanges();
    return SceneSerializer::SaveToJson(scene);
}

// エンティティ数だけが要る大きい文書 (読み込みの上限の検査用)
json MakeBigDoc(int count)
{
    json entities = json::array();
    for (int i = 1; i <= count; ++i) {
        entities.push_back({ { "fileId", i }, { "name", "n" + std::to_string(i) } });
    }
    return { { "version", Scene::kDocVersion }, { "sceneName", "big" }, { "nextFileId", count + 1 }, { "entities", entities } };
}

std::string Hash(const std::string& content)
{
    return Sha256Hex(content);
}

std::string Base64Encode(const std::string& bytes)
{
    static const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8) | static_cast<uint8_t>(bytes[i + 2]);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += kAlphabet[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8);
        out += kAlphabet[(v >> 18) & 63];
        out += kAlphabet[(v >> 12) & 63];
        out += kAlphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

constexpr const char* kScenePath = "assets/level.scene.json";

// ---- 道具: 偽のサービス ----

struct PreviewRig {
    std::shared_ptr<FakeState> s = std::make_shared<FakeState>();
    ChangeTrackingSession session;
    size_t answered = 0;
    std::map<std::string, std::string> blobs; // hash -> 中身
    std::set<std::string> failingBlobs;       // blob_get が blob_missing 以外の失敗を返す hash
    std::map<std::string, int64_t> latest;    // path -> list が返す最新の revisionId
    bool listFails = false;
    std::vector<uint64_t> blobOffsets;        // blob_get の offset の履歴
    std::vector<std::string> blobHashes;      // blob_get の hash の履歴
    std::vector<std::string> listPaths;       // list の path の履歴

    PreviewRig()
    {
        session.StartWithIo(std::make_unique<FakeIo>(s), kRoot, true);
        session.Poll();
        ReplyHelloOk(*s);
        session.Poll();
    }

    TrackClient& Client() { return session.Client(); }

    void Put(const std::string& content) { blobs[Hash(content)] = content; }

    void AnswerAll()
    {
        std::string batch;
        for (; answered < s->written.size(); ++answered) {
            const json req = json::parse(s->written[answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kHello) {
                continue;
            }
            json result = json::object();
            if (op == trackop::kBlobGet) {
                const json& args = req["args"];
                const std::string hash = args.value("hash", std::string());
                blobOffsets.push_back(args.value("offset", static_cast<uint64_t>(0)));
                blobHashes.push_back(hash);
                const auto it = blobs.find(hash);
                if (failingBlobs.count(hash) != 0) {
                    batch += ErrReply(id, "internal").dump() + "\n";
                    continue;
                }
                if (it == blobs.end()) {
                    batch += ErrReply(id, "blob_missing").dump() + "\n";
                    continue;
                }
                const size_t offset = (std::min)(args.value("offset", static_cast<size_t>(0)), it->second.size());
                const size_t length = (std::min)(args.value("length", static_cast<size_t>(524288)), it->second.size() - offset);
                result = { { "data", Base64Encode(it->second.substr(offset, length)) }, { "size", it->second.size() }, { "offset", offset }, { "length", length } };
            } else if (op == trackop::kList) {
                const std::string path = req["args"].value("path", std::string());
                listPaths.push_back(path);
                if (listFails) {
                    batch += ErrReply(id, "internal").dump() + "\n";
                    continue;
                }
                const auto it = latest.find(path);
                result = { { "items", it == latest.end() ? json::array() : json::array({ { { "revisionId", it->second }, { "path", path } } }) }, { "nextCursor", nullptr } };
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            s->chunks.push_back(batch);
        }
    }

    // サービスの応答を返し、プレビューを進める。settled か上限まで
    void Drive(ScenePreviewSession& preview, int maxRounds = 40)
    {
        for (int i = 0; i < maxRounds; ++i) {
            s->nowMs += 100;
            session.Poll();
            AnswerAll();
            session.Poll();
            preview.Tick(session.Client());
            if (preview.Settled() && (preview.Assets().GetState() != PreviewAssetProbe::State::Probing)) {
                return;
            }
        }
    }

    // 書き込みの op が 1 つも無いこと (取得の op だけ)
    bool OnlyReadOps() const
    {
        for (const std::string& w : s->written) {
            const json req = json::parse(w, nullptr, false);
            const std::string op = req.value("op", std::string());
            if (op != trackop::kHello && op != trackop::kBlobGet && op != trackop::kList && op != trackop::kSubscribe && op != trackop::kStatus) {
                return false;
            }
        }
        return true;
    }
};

// 偽のディスク。読み取りの回数を数え、書く口を持たない
class FakeDiskIo final : public RestoreFileIo {
public:
    std::map<std::wstring, std::string> files;
    int reads = 0;

    void Put(const std::string& rel, const std::string& content) { files[JoinProjectPath(kRoot, rel)] = content; }

    uint32_t Attributes(const std::wstring& absPath) override
    {
        if (absPath == kRoot) {
            return kRestoreAttrDirectory; // プロジェクトのルート (書き先の検査が、ルートがディレクトリであることを見る)
        }
        return files.count(absPath) != 0 ? 0x80u : kRestoreAttrAbsent;
    }
    ReadStatus Read(const std::wstring& absPath, uint64_t offset, size_t maxBytes, std::string& out, uint64_t& totalSize) override
    {
        ++reads;
        const auto it = files.find(absPath);
        if (it == files.end()) {
            return ReadStatus::Absent;
        }
        totalSize = it->second.size();
        const size_t start = static_cast<size_t>((std::min)(offset, static_cast<uint64_t>(it->second.size())));
        out += it->second.substr(start, maxBytes);
        return ReadStatus::Ok;
    }
};

RestoreSide SideOf(const std::string& content)
{
    RestoreSide side;
    side.state = RestoreBlob::Present;
    side.hash = Hash(content);
    side.size = static_cast<int64_t>(content.size());
    return side;
}

ScenePreviewOpenArgs ArgsFor(const std::string& before, const std::string& after, std::vector<TrackSceneChange> changes = {})
{
    ScenePreviewOpenArgs args;
    args.revision.revisionId = 5;
    args.revision.path = kScenePath;
    args.revision.changeType = "modified";
    args.revision.before = SideOf(before);
    args.revision.after = SideOf(after);
    args.sceneChanges = std::move(changes);
    args.projectRoot = kRoot;
    return args;
}

TrackSceneChange ChangeRow(int64_t fileId, const char* type)
{
    TrackSceneChange row;
    row.sceneChangeId = fileId;
    row.hasFileId = true;
    row.fileId = fileId;
    row.changeType = type;
    return row;
}

bool Near(float a, float b, float eps = 0.01f)
{
    return std::abs(a - b) <= eps;
}

// ---- 本番の状態の写し ----

struct ProdState {
    uint64_t hash = 0;
    uint32_t alive = 0;
    size_t archetypes = 0;
    uint64_t serial = 0;
    uint64_t primary = 0;
    size_t selected = 0;
    std::wstring assetPath;
    bool hubPending = false;
    bool operator==(const ProdState&) const = default;
};

struct ProdRig {
    Scene scene;
    UndoStack undo;
    Selection selection;
    ReloadHub hub;

    ProdRig()
    {
        for (int i = 0; i < 4; ++i) {
            GameObject object = scene.CreateGameObjectTracked("prod" + std::to_string(i));
            object.GetComponent<LocalTransform>()->position = { static_cast<float>(i), 0.0f, 0.0f };
        }
        scene.GetWorld().ApplyStructuralChanges();
        selection.ids = { 2 };
        selection.primary = 2;
        hub.SetActiveScenePath(L"C:\\prod\\main.scene.json");
    }

    ProdState Capture()
    {
        ProdState state;
        state.hash = HashWorld(scene.GetWorld());
        state.alive = scene.GetWorld().AliveCount();
        state.archetypes = scene.GetWorld().Archetypes().size();
        state.serial = undo.StateSerial();
        state.primary = selection.primary;
        state.selected = selection.ids.size();
        state.assetPath = selection.assetPath;
        state.hubPending = hub.HasPendingScene();
        return state;
    }
};

// ---- 基本: 読み込み・印・カメラ・数 ----

std::string DumpDoc(const json& doc)
{
    return doc.dump(2);
}

struct Docs {
    std::string before;
    std::string after;
    std::vector<TrackSceneChange> changes;
};

// Before: A(1) 原点, B(2) x=5, C(3) x=10 (削除される)。After: A が y=2 へ動く, B はそのまま, D(4) x=-5 (追加)
Docs MakeDocs()
{
    Docs docs;
    json before = MakeSceneDoc({ { "A", 0, 0, 0 }, { "B", 5, 0, 0 }, { "C", 10, 0, 0 } });
    json after = before;
    after["entities"][0]["components"]["LocalTransform"]["position"] = json::array({ 0.0, 2.0, 0.0 });
    after["entities"].erase(after["entities"].begin() + 2);
    json added = before["entities"][0];
    added["fileId"] = 4;
    added["name"] = "D";
    added["components"]["LocalTransform"]["position"] = json::array({ -5.0, 0.0, 0.0 });
    after["entities"].push_back(added);
    docs.before = DumpDoc(before);
    docs.after = DumpDoc(after);
    docs.changes = { ChangeRow(1, "FieldModified"), ChangeRow(3, "EntityRemoved"), ChangeRow(4, "EntityAdded") };
    return docs;
}

void TestBasic(const Check& check)
{
    const int baseScenes = PreviewScene::LiveCount();
    const Docs docs = MakeDocs();
    PreviewRig r;
    r.Put(docs.before);
    r.Put(docs.after);
    FakeDiskIo disk;
    ScenePreviewSession preview;
    check(preview.Open(r.Client(), disk, ArgsFor(docs.before, docs.after, docs.changes)) && preview.IsOpen(), "basic: a scene revision opens");
    check(preview.Before().state == ScenePreviewSession::SideState::Fetching, "basic: the content is fetched over blob_get (not available at once)");
    r.Drive(preview);
    check(preview.Settled() && preview.Before().state == ScenePreviewSession::SideState::Loaded && preview.After().state == ScenePreviewSession::SideState::Loaded,
          "basic: Before and After are both loaded from their Blobs");
    check(preview.Before().load.entityCount == 3 && preview.After().load.entityCount == 3, "basic: entity counts of both sides");
    check(PreviewScene::LiveCount() == baseScenes + 2, "basic: exactly two preview scenes are alive (Before and After)");
    check(r.OnlyReadOps(), "basic: only read ops were sent to the service (no review / restore / saved notices)");

    // 印: 更新 (After の位置)、削除 (Before の位置)、追加 (After の位置)。fileId の昇順
    const std::vector<PreviewMarker>& markers = preview.Markers();
    check(markers.size() == 3 && markers[0].fileId == 1 && markers[1].fileId == 3 && markers[2].fileId == 4, "basic: one marker per changed entity, in fileId order");
    if (markers.size() == 3) {
        check(markers[0].color == OverlayColorId::Modified && Near(markers[0].position.y, 2.0f) && !markers[0].fromBefore && markers[0].name == "A",
              "basic: a modified entity is marked at its After position");
        check(markers[1].color == OverlayColorId::Removed && markers[1].fromBefore && Near(markers[1].position.x, 10.0f) && markers[1].name == "C",
              "basic: a removed entity is marked (ghost) at its Before position");
        check(markers[2].color == OverlayColorId::Added && !markers[2].fromBefore && Near(markers[2].position.x, -5.0f) && markers[2].name == "D",
              "basic: an added entity is marked at its After position");
    }
    // カメラ: 変わったエンティティの範囲 (x -5..10, y 0..2) が収まる
    const PreviewCamera camera = preview.Camera();
    check(Near(camera.center.x, 2.5f, 0.05f) && Near(camera.center.y, 1.0f, 0.05f) && camera.distance > 6.0f, "basic: the camera starts on the changed entities' bounds");
    // カメラの操作 (左右でも同じ 1 つのカメラ)
    const float yaw = camera.yaw;
    preview.OrbitCamera(10.0f, 0.0f);
    preview.ZoomCamera(1.0f);
    preview.PanCamera(5.0f, 5.0f);
    check(preview.Camera().yaw != yaw && preview.Camera().distance < camera.distance && preview.Camera().center.x != camera.center.x, "basic: orbit, zoom and pan move the one shared camera");
    preview.OrbitCamera(0.0f, 100000.0f);
    check(std::abs(preview.Camera().pitch) <= 1.5f + 1e-4f, "basic: the pitch is clamped");
    for (int i = 0; i < 200; ++i) {
        preview.ZoomCamera(-1.0f);
    }
    check(preview.Camera().distance <= kPreviewMaxDistance, "basic: the zoom is clamped");
    preview.FitCamera();
    check(Near(preview.Camera().center.x, 2.5f, 0.05f), "basic: Fit camera returns to the changed entities' bounds");
    for (const PreviewMode mode : { PreviewMode::Before, PreviewMode::After, PreviewMode::Overlay, PreviewMode::SideBySide }) {
        preview.SetMode(mode);
        check(preview.Mode() == mode, "basic: the mode can be switched");
    }
    preview.Close();
    check(!preview.IsOpen() && PreviewScene::LiveCount() == baseScenes && preview.Markers().empty() && preview.BeforeScene() == nullptr, "basic: closing destroys both preview scenes");
    // 2 回開いて閉じても数が増えない
    for (int i = 0; i < 2; ++i) {
        preview.Open(r.Client(), disk, ArgsFor(docs.before, docs.after, docs.changes));
        r.Drive(preview);
        preview.Close();
    }
    check(PreviewScene::LiveCount() == baseScenes, "basic: opening and closing twice leaves no preview scene behind");
    // 開いたまま開き直しても 2 つのまま
    preview.Open(r.Client(), disk, ArgsFor(docs.before, docs.after, docs.changes));
    r.Drive(preview);
    preview.Open(r.Client(), disk, ArgsFor(docs.before, docs.after, docs.changes));
    r.Drive(preview);
    check(PreviewScene::LiveCount() == baseScenes + 2, "basic: opening again replaces the scenes (still two)");
    preview.Close();
    // シーンでない文書は開かない
    ScenePreviewOpenArgs prefab = ArgsFor(docs.before, docs.after);
    prefab.revision.path = "assets/a.prefab.json";
    check(!preview.Open(r.Client(), disk, std::move(prefab)) && !preview.IsOpen() && PreviewScene::LiveCount() == baseScenes, "basic: a prefab document is not previewed");
}

// ---- 文面 ----

void TestScreen(const Check& check)
{
    const Docs docs = MakeDocs();
    PreviewRig r;
    r.Put(docs.before);
    r.Put(docs.after);
    FakeDiskIo disk;
    ScenePreviewSession preview;
    preview.Open(r.Client(), disk, ArgsFor(docs.before, docs.after, docs.changes));
    r.Drive(preview);
    InBothLanguages([&](Lang) {
        const ScenePreviewScreen screen = BuildScenePreviewScreen(preview);
        bool texts = !screen.target.empty() && !screen.beforeLine.empty() && !screen.afterLine.empty() && !screen.notes.empty();
        for (const std::string& note : screen.notes) {
            texts = texts && !note.empty();
        }
        check(texts && screen.target.find("#5") != std::string::npos, "screen: the target, both sides and the notes have a text in this language");
        // 常に出す注記: アセットは今の内容・未保存のエディタの状態は比べない
        check(screen.notes.size() >= 3 && screen.notes.front() == Tr(StrId::ScPv_NoteAssets) && screen.notes.back() == Tr(StrId::ScPv_NoteUnsaved),
              "screen: 'assets are drawn with their current content' and 'unsaved editor edits are not compared' are always shown");
    });
    for (const PreviewFailure failure : { PreviewFailure::NoContent, PreviewFailure::Unavailable, PreviewFailure::TooLarge, PreviewFailure::NotConnected,
                                          PreviewFailure::ServiceError, PreviewFailure::Mismatch, PreviewFailure::DiskAbsent, PreviewFailure::DiskError }) {
        InBothLanguages([&](Lang) { check(std::string(PreviewFailureText(failure)).size() > 4, "screen: every failure reason has a text in both languages"); });
    }
}

// ---- 本番を変えない・プロジェクトのファイルを増やさない ----

std::map<std::string, std::string> SnapshotFiles(const fs::path& root)
{
    std::map<std::string, std::string> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            std::string hex;
            Sha256FileHex(it->path().wstring(), hex);
            files[fs::relative(it->path(), root, ec).generic_string()] = hex;
        }
    }
    return files;
}

void TestProductionUntouched(const Check& check)
{
    const Docs docs = MakeDocs();
    ProdRig prod;
    const ProdState before = prod.Capture();
    PreviewRig r;
    r.Put(docs.before);
    r.Put(docs.after);
    // 一時プロジェクト (本物のファイルの読み取り口と、本物のパスの検査)
    const fs::path root = fs::temp_directory_path() / (L"mye_scene_preview_" + std::to_wstring(_getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"assets", ec);
    std::ofstream(root / L"assets" / L"level.scene.json", std::ios::binary) << docs.after;
    std::ofstream(root / L"assets" / L"cube.obj", std::ios::binary) << "v 0 0 0\n";
    const std::map<std::string, std::string> filesBefore = SnapshotFiles(root);

    ScenePreviewOpenArgs args = ArgsFor(docs.before, docs.after, docs.changes);
    args.projectRoot = root.wstring();
    args.assetPath = [](uint64_t) { return std::string("assets/cube.obj"); };
    ScenePreviewWindow window;
    window.open = true;
    check(window.Open(r.session, args), "untouched: the window opens the preview");
    for (int i = 0; i < 6; ++i) {
        r.Drive(window.Session(), 2);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    }
    for (const PreviewMode mode : { PreviewMode::Before, PreviewMode::After, PreviewMode::Overlay, PreviewMode::SideBySide }) {
        window.Session().SetMode(mode);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    }
    window.Session().OrbitCamera(30.0f, 10.0f);
    window.Session().PanCamera(10.0f, 10.0f);
    window.Session().ZoomCamera(1.0f);
    // After を今のディスクのファイルに切り替える (本物の読み取り口)
    window.Session().SetAfterSource(PreviewAfterSource::Disk, r.Client());
    for (int i = 0; i < 6; ++i) {
        r.Drive(window.Session(), 2);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    }
    check(window.Session().After().state == ScenePreviewSession::SideState::Loaded && window.Session().After().load.entityCount == 3,
          "untouched: the real file on disk is read as the After side");
    check(prod.Capture() == before, "untouched: open, switch modes, move the camera, switch the After source: the live Scene (hash, entities, archetypes), Undo serial, Selection and the hub are unchanged");
    window.Close();
    check(prod.Capture() == before && PreviewScene::LiveCount() == 0, "untouched: the same after closing");
    check(SnapshotFiles(root) == filesBefore, "untouched: no project file was created, changed or removed (no .meta)");
    fs::remove_all(root, ec);
}

// ---- Play 中でも sim の結果が変わらない ----

uint64_t RunSim(const Docs* previewDocs, uint32_t ticks, uint32_t openAt, bool* ok)
{
    HeadlessSimSetup setup;
    setup.config.title = L"MyEngine ScenePreviewSelfTest";
    setup.saveDirOverride = (fs::temp_directory_path() / (L"mye_selftest_save_pv" + std::to_wstring(_getpid()))).wstring();
    setup.scene.showcase = FindShowcase(L"--local-demo", /*editor=*/true);
    HeadlessSim sim;
    if (setup.scene.showcase == nullptr || !sim.Init(setup)) {
        *ok = false;
        return 0;
    }
    InputSnapshot lanes[kMaxPlayers] = {};
    std::unique_ptr<PreviewRig> rig;
    std::unique_ptr<ScenePreviewSession> preview;
    FakeDiskIo disk;
    for (uint32_t tick = 0; tick < ticks; ++tick) {
        if (previewDocs != nullptr && tick == openAt) {
            rig = std::make_unique<PreviewRig>();
            rig->Put(previewDocs->before);
            rig->Put(previewDocs->after);
            preview = std::make_unique<ScenePreviewSession>();
            preview->Open(rig->Client(), disk, ArgsFor(previewDocs->before, previewDocs->after, previewDocs->changes));
        }
        if (preview != nullptr && preview->IsOpen()) {
            rig->Drive(*preview, 1); // tick の合間に、サービスの応答とプレビューの読み込みを進める
            if (tick % 3 == 0) {
                preview->SetMode(static_cast<PreviewMode>(tick % 4));
                preview->OrbitCamera(3.0f, 1.0f);
            }
        }
        sim.RunTick(lanes, nullptr, false);
    }
    if (preview != nullptr) {
        preview->Close();
    }
    *ok = true;
    return sim.WorldHash();
}

void TestPlayUnaffected(const Check& check)
{
    const Docs docs = MakeDocs();
    bool okA = false;
    bool okB = false;
    const uint64_t plain = RunSim(nullptr, 24, 0, &okA);
    const uint64_t withPreview = RunSim(&docs, 24, 8, &okB);
    if (!okA || !okB) {
        check(false, "play: the headless sim starts (run from the repository root)");
        return;
    }
    check(plain != 0 && plain == withPreview, "play: with a preview opened and driven during the run, the world hash after the same number of ticks is identical to the run without it");
}

// ---- 失敗の分け方 ----

void TestFailures(const Check& check)
{
    const Docs docs = MakeDocs();
    const int baseScenes = PreviewScene::LiveCount();
    using Pv = ScenePreviewSession;
    // 一方を差し替えて開き、Before の側の結果を返す
    auto openWithBefore = [&](PreviewRig& r, const std::string& beforeContent, RestoreSide beforeSide, Pv& preview, FakeDiskIo& disk) {
        ScenePreviewOpenArgs args = ArgsFor(beforeContent, docs.after, docs.changes);
        args.revision.before = std::move(beforeSide);
        preview.Open(r.Client(), disk, std::move(args));
        r.Drive(preview);
    };
    FakeDiskIo disk;
    {   // (a) Blob の欠け
        PreviewRig r;
        r.Put(docs.after);
        Pv preview;
        openWithBefore(r, docs.before, SideOf(docs.before), preview, disk); // Before の Blob を置いていない
        check(preview.Before().state == Pv::SideState::Failed && preview.Before().failure == PreviewFailure::Unavailable && preview.After().state == Pv::SideState::Loaded,
              "failure: a missing Blob fails only that side (the other side is still shown)");
        InBothLanguages([&](Lang) { check(!PreviewSideText(preview.Before()).empty(), "failure: the reason is shown in this language"); });
        check(preview.Markers().size() == 2, "failure: markers come from the side that loaded (the removed entity cannot be placed)");
        preview.Close();
    }
    {   // (b) 内容を持たない面 (追加の Before)
        PreviewRig r;
        r.Put(docs.after);
        Pv preview;
        openWithBefore(r, docs.before, RestoreSide(), preview, disk);
        check(preview.Before().state == Pv::SideState::Failed && preview.Before().failure == PreviewFailure::NoContent, "failure: a side with no snapshot is 'no content'");
        preview.Close();
    }
    {   // (c) 版が新しい・壊れた JSON・シーンの形でない
        struct Bad {
            std::string text;
            PreviewLoadStatus status;
        };
        const json newer = [&] {
            json d = json::parse(docs.before);
            d["version"] = Scene::kDocVersion + 1;
            return d;
        }();
        const std::vector<Bad> bads = { { newer.dump(), PreviewLoadStatus::UnsupportedVersion },
                                        { "{ not json", PreviewLoadStatus::BadJson },
                                        { "{\"version\":4,\"entities\":5}", PreviewLoadStatus::NotAScene },
                                        { "[]", PreviewLoadStatus::NotAScene },
                                        { "{\"version\":\"x\",\"entities\":[]}", PreviewLoadStatus::NotAScene },
                                        { "{\"version\":4,\"entities\":[{\"fileId\":0}]}", PreviewLoadStatus::NotAScene } };
        for (const Bad& bad : bads) {
            PreviewRig r;
            r.Put(bad.text);
            r.Put(docs.after);
            Pv preview;
            openWithBefore(r, bad.text, SideOf(bad.text), preview, disk);
            check(preview.Before().state == Pv::SideState::Failed && preview.Before().load.status == bad.status && preview.Before().scene == nullptr,
                  "failure: a document that cannot be shown (newer version / broken / not a scene) is reported and no scene is kept");
            InBothLanguages([&](Lang) { check(!PreviewSideText(preview.Before()).empty(), "failure: the load failure has a reason text"); });
            preview.Close();
        }
    }
    {   // (d) ハッシュが合わない・サービスの失敗・大きすぎる
        PreviewRig r;
        r.Put(docs.after);
        RestoreSide lying = SideOf(docs.before);
        r.blobs[lying.hash] = docs.before + " "; // 中身が記録と違う (大きさも)
        Pv preview;
        openWithBefore(r, docs.before, lying, preview, disk);
        check(preview.Before().state == Pv::SideState::Failed && preview.Before().failure == PreviewFailure::Mismatch, "failure: content that does not match its recorded hash or size is not shown");
        preview.Close();

        PreviewRig r2;
        r2.Put(docs.after);
        r2.Put(docs.before);
        r2.failingBlobs.insert(Hash(docs.before));
        Pv preview2;
        openWithBefore(r2, docs.before, SideOf(docs.before), preview2, disk);
        check(preview2.Before().failure == PreviewFailure::ServiceError && preview2.Before().state == Pv::SideState::Failed, "failure: a service error fails that side");
        preview2.Close();

        PreviewRig r3;
        r3.Put(docs.after);
        RestoreSide huge = SideOf(docs.before);
        huge.size = static_cast<int64_t>(kPreviewMaxDocumentBytes) + 1;
        Pv preview3;
        openWithBefore(r3, docs.before, huge, preview3, disk);
        check(preview3.Before().failure == PreviewFailure::TooLarge && std::count(r3.blobHashes.begin(), r3.blobHashes.end(), Hash(docs.before)) == 0, "failure: a document over the byte limit is refused without fetching it");
        preview3.Close();
    }
    {   // (e) エンティティ数の上限: ちょうど上限は読める、1 つ超えると読まない (一部だけは読まない)
        const std::string atLimit = MakeBigDoc(kPreviewMaxEntities).dump();
        const std::string overLimit = MakeBigDoc(kPreviewMaxEntities + 1).dump();
        PreviewRig r;
        r.Put(atLimit);
        r.Put(overLimit);
        Pv preview;
        ScenePreviewOpenArgs args = ArgsFor(atLimit, overLimit);
        preview.Open(r.Client(), disk, std::move(args));
        r.Drive(preview, 60);
        check(preview.Before().state == Pv::SideState::Loaded && preview.Before().load.entityCount == kPreviewMaxEntities, "limit: exactly the entity limit is loaded");
        check(preview.After().state == Pv::SideState::Failed && preview.After().load.status == PreviewLoadStatus::TooManyEntities && preview.After().scene == nullptr,
              "limit: one entity over the limit is not loaded at all (no partial scene)");
        InBothLanguages([&](Lang) { check(PreviewSideText(preview.After()).find(std::to_string(kPreviewMaxEntities)) != std::string::npos, "limit: the screen names the limit"); });
        preview.Close();
    }
    {   // (f) 追跡サービスと話せない
        auto io = std::make_shared<FakeState>();
        ChangeTrackingSession offline;
        offline.StartWithIo(std::make_unique<FakeIo>(io), kRoot, false); // 無効
        Pv preview;
        preview.Open(offline.Client(), disk, ArgsFor(docs.before, docs.after));
        offline.Client().Poll();
        preview.Tick(offline.Client());
        check(preview.Before().failure == PreviewFailure::NotConnected && preview.After().failure == PreviewFailure::NotConnected && io->c.Total() == 0,
              "failure: without a service connection both sides say so and no I/O happens");
        preview.Close();
    }
    {   // (g) 今のファイルが無い・大きすぎる
        PreviewRig r;
        r.Put(docs.before);
        r.Put(docs.after);
        FakeDiskIo empty;
        Pv preview;
        preview.Open(r.Client(), empty, ArgsFor(docs.before, docs.after));
        r.Drive(preview);
        preview.SetAfterSource(PreviewAfterSource::Disk, r.Client());
        check(preview.After().failure == PreviewFailure::DiskAbsent && preview.Before().state == Pv::SideState::Loaded, "failure: no file on disk now fails only the After side");
        FakeDiskIo withFile;
        withFile.Put(kScenePath, std::string(kPreviewMaxDocumentBytes + 1, ' '));
        Pv tooBig;
        tooBig.Open(r.Client(), withFile, ArgsFor(docs.before, docs.after));
        tooBig.SetAfterSource(PreviewAfterSource::Disk, r.Client());
        for (int i = 0; i < 40; ++i) {
            tooBig.Tick(r.Client());
        }
        check(tooBig.After().failure == PreviewFailure::TooLarge, "failure: a file on disk over the byte limit is refused");
        tooBig.Close();
        preview.Close();
    }
    check(PreviewScene::LiveCount() == baseScenes, "failure: no preview scene is left behind by any failure");
}

// ---- 大きい内容は数回に分けて取る・古い応答 ----

void TestChunksAndStale(const Check& check)
{
    // 1.3 MiB の文書 (長い sceneName で膨らませる)。512 KiB ずつ 3 回
    json big = MakeSceneDoc({ { "A", 0, 0, 0 } });
    big["sceneName"] = std::string(1300u * 1024u, 'x');
    const std::string bigText = big.dump();
    const std::string tinyDoc = DumpDoc(MakeSceneDoc({ { "A", 0, 0, 0 } }));
    PreviewRig r;
    r.Put(bigText);
    r.Put(tinyDoc);
    FakeDiskIo disk;
    ScenePreviewSession preview;
    preview.Open(r.Client(), disk, ArgsFor(bigText, tinyDoc));
    int rounds = 0;
    while (!preview.Settled() && rounds < 40) {
        r.Drive(preview, 1);
        ++rounds;
    }
    const std::vector<uint64_t> offsets = r.blobOffsets;
    const bool chunked = std::count(offsets.begin(), offsets.end(), static_cast<uint64_t>(0)) == 2 && std::find(offsets.begin(), offsets.end(), static_cast<uint64_t>(kPreviewBlobChunkBytes)) != offsets.end()
                         && std::find(offsets.begin(), offsets.end(), static_cast<uint64_t>(kPreviewBlobChunkBytes * 2)) != offsets.end();
    check(chunked && preview.Before().state == ScenePreviewSession::SideState::Loaded, "chunks: a 1.3 MiB document is fetched in 512 KiB pieces, over several Polls, and loads");
    preview.Close();
    // 取っている途中の進行
    PreviewRig slow;
    slow.Put(bigText);
    slow.Put(tinyDoc);
    ScenePreviewSession progress;
    progress.Open(slow.Client(), disk, ArgsFor(bigText, tinyDoc));
    slow.s->nowMs += 100;
    slow.session.Poll();
    slow.AnswerAll();
    slow.session.Poll(); // 最初の 1 塊だけ届く
    check(progress.Before().state == ScenePreviewSession::SideState::Fetching && progress.Before().fetch.Received() == kPreviewBlobChunkBytes && progress.Before().fetch.Total() > kPreviewBlobChunkBytes,
          "chunks: the progress (received / total) is visible between pieces");
    InBothLanguages([&](Lang) { check(PreviewSideText(progress.Before()).find("KiB") != std::string::npos, "chunks: the progress is shown while loading"); });
    progress.Close();
    // 閉じたあとに届く応答は無視する
    PreviewRig stale;
    stale.Put(tinyDoc);
    ScenePreviewSession pending;
    pending.Open(stale.Client(), disk, ArgsFor(tinyDoc, tinyDoc));
    pending.Close();
    stale.s->nowMs += 100;
    stale.session.Poll();
    stale.AnswerAll();
    stale.session.Poll();
    pending.Tick(stale.Client());
    check(!pending.IsOpen() && pending.BeforeScene() == nullptr && PreviewScene::LiveCount() == 0, "stale: a response that arrives after Close is ignored");
    // 開き直したあとに、前の応答が届いても新しい内容になる
    const std::string other = DumpDoc(MakeSceneDoc({ { "A", 0, 0, 0 }, { "B", 1, 0, 0 }, { "C", 2, 0, 0 } }));
    PreviewRig reopen;
    reopen.Put(tinyDoc);
    reopen.Put(other);
    ScenePreviewSession again;
    again.Open(reopen.Client(), disk, ArgsFor(tinyDoc, tinyDoc));
    again.Open(reopen.Client(), disk, ArgsFor(other, other));
    reopen.Drive(again);
    check(again.Before().state == ScenePreviewSession::SideState::Loaded && again.Before().load.entityCount == 3, "stale: after opening again, only the new revision's content is used");
    again.Close();
}

// ---- アセットの注記 ----

void TestAssetNotes(const Check& check)
{
    const json doc = MakeSceneDoc({ { "A", 0, 0, 0, -1, 777, 888 }, { "B", 1, 0, 0, -1, 999, 0 } });
    const std::string text = DumpDoc(doc);
    auto resolver = [](uint64_t guid) -> std::string {
        switch (guid) {
        case 777: return "assets/mesh.obj";
        case 888: return "assets/mat.mat.json";
        case 999: return "assets/old.obj";
        default: return std::string();
        }
    };
    FakeDiskIo disk;
    auto run = [&](PreviewRig& r, ScenePreviewSession& preview) {
        ScenePreviewOpenArgs args = ArgsFor(text, text);
        args.assetPath = resolver;
        preview.Open(r.Client(), disk, std::move(args));
        r.Drive(preview);
    };
    {   // 参照のアセットのうち、Before のあとに記録があるものだけ数える
        PreviewRig r;
        r.Put(text);
        r.latest["assets/mesh.obj"] = 99;        // この変更 (5) より後
        r.latest["assets/mat.mat.json"] = 3;     // この変更より前
        ScenePreviewSession preview;
        run(r, preview);
        const PreviewAssetProbe& assets = preview.Assets();
        check(assets.GetState() == PreviewAssetProbe::State::Done && assets.ChangedCount() == 1 && assets.ChangedPaths() == std::vector<std::string>{ "assets/mesh.obj" },
              "assets: an asset with a record after this change is counted and named");
        check(r.listPaths.size() == 3 && std::set<std::string>(r.listPaths.begin(), r.listPaths.end()).size() == 3, "assets: each referenced asset is asked about once (3 distinct paths)");
        InBothLanguages([&](Lang) {
            const ScenePreviewScreen screen = BuildScenePreviewScreen(preview);
            bool changedNote = false;
            for (const std::string& note : screen.notes) {
                changedNote = changedNote || (note.find("assets/mesh.obj") != std::string::npos && note.find(" 1") != std::string::npos);
            }
            check(changedNote && screen.notes.front() == Tr(StrId::ScPv_NoteAssets), "assets: the screen shows 'drawn with current content' plus the count and path");
        });
        preview.Close();
    }
    {   // 変更が無ければ「今の内容で描いています」だけ
        PreviewRig r;
        r.Put(text);
        r.latest["assets/mesh.obj"] = 4;
        ScenePreviewSession preview;
        run(r, preview);
        check(preview.Assets().GetState() == PreviewAssetProbe::State::Done && preview.Assets().ChangedCount() == 0, "assets: no later record -> nothing counted");
        const ScenePreviewScreen screen = BuildScenePreviewScreen(preview);
        bool onlyBase = true;
        for (const std::string& note : screen.notes) {
            onlyBase = onlyBase && note.find("assets/") == std::string::npos;
        }
        check(onlyBase && screen.notes.front() == Tr(StrId::ScPv_NoteAssets), "assets: with no later record only the 'current content' note is shown");
        preview.Close();
    }
    {   // 確かめられない
        PreviewRig r;
        r.Put(text);
        r.listFails = true;
        ScenePreviewSession preview;
        run(r, preview);
        check(preview.Assets().GetState() == PreviewAssetProbe::State::Unknown, "assets: a failed history query is 'cannot check' (never 'no change')");
        InBothLanguages([&](Lang) {
            bool unknown = false;
            for (const std::string& note : BuildScenePreviewScreen(preview).notes) {
                unknown = unknown || note == Tr(StrId::ScPv_NoteAssetsUnknown);
            }
            check(unknown, "assets: the screen says the change could not be checked");
        });
        preview.Close();
    }
    {   // 参照が多い: 先頭 64 件だけ調べ、残りを数える
        std::vector<EntSpec> specs;
        for (int i = 0; i < 70; ++i) {
            specs.push_back({ "m", 0, 0, 0, -1, static_cast<uint64_t>(1000 + i), 0 });
        }
        const std::string many = DumpDoc(MakeSceneDoc(specs));
        PreviewRig r;
        r.Put(many);
        ScenePreviewSession preview;
        ScenePreviewOpenArgs args = ArgsFor(many, many);
        args.assetPath = [](uint64_t guid) { return "assets/m" + std::to_string(guid) + ".obj"; };
        preview.Open(r.Client(), disk, std::move(args));
        r.Drive(preview);
        check(r.listPaths.size() == static_cast<size_t>(kPreviewMaxAssetProbes) && preview.Assets().Skipped() == 6, "assets: only the first 64 are asked about; the rest are counted as not checked");
        preview.Close();
    }
    {   // 参照を解決できなければ (組み込みなど) 数えない。解決の口が無ければ何も聞かない
        PreviewRig r;
        r.Put(text);
        ScenePreviewSession preview;
        preview.Open(r.Client(), disk, ArgsFor(text, text));
        r.Drive(preview);
        check(preview.Assets().GetState() == PreviewAssetProbe::State::Idle && r.listPaths.empty(), "assets: without a path resolver nothing is asked and nothing is claimed");
        preview.Close();
    }
}

// ---- C# コンポーネントを読み込まない ----

void TestManagedSkipped(const Check& check)
{
    json doc = MakeSceneDoc({ { "A", 0, 0, 0 }, { "B", 1, 0, 0 } });
    doc["entities"][0]["components"]["FakeScript"] = { { "speed", 1.5 } };
    doc["entities"][1]["components"]["FakeScript"] = { { "speed", 2.5 } };
    doc["entities"][1]["components"]["OtherScript"] = json::object();
    const std::string text = DumpDoc(doc);
    int asked = 0;
    const ManagedComponentFilter filter = [&](const std::string& name) {
        ++asked;
        return name == "FakeScript";
    };
    // 読み込みの部品: C# の名前のコンポーネントは取り除いてから渡す
    PreviewScene preview;
    const PreviewLoadInfo info = LoadPreviewScene(preview.scene, text, filter);
    check(info.status == PreviewLoadStatus::Ok && info.managedSkipped == 2 && info.entityCount == 2, "managed: C# components are skipped (counted per instance)");
    check(preview.scene.GetUnknownComponents(1) == nullptr && preview.scene.GetUnknownComponents(2) != nullptr && preview.scene.GetUnknownComponents(2)->count("OtherScript") == 1
              && preview.scene.GetUnknownComponents(2)->count("FakeScript") == 0,
          "managed: a skipped component is not even kept as raw data; other unknown components stay as the loader keeps them");
    // 画面の注記
    PreviewRig r;
    r.Put(text);
    FakeDiskIo disk;
    ScenePreviewSession session;
    ScenePreviewOpenArgs args = ArgsFor(text, text);
    args.isManaged = filter;
    session.Open(r.Client(), disk, std::move(args));
    r.Drive(session);
    check(session.ManagedSkipped() == 4, "managed: both sides' skipped components are added up");
    InBothLanguages([&](Lang) {
        bool noted = false;
        for (const std::string& note : BuildScenePreviewScreen(session).notes) {
            noted = noted || (note.find(Tr(StrId::ScPv_NoteManaged)) == 0 && note.find(" 4") != std::string::npos);
        }
        check(noted, "managed: the screen says C# components were not loaded, with the count");
    });
    session.Close();
}

// ---- 窓: ヘッドレスの ImGui フレーム・デバイス消失 ----

void TestWindow(const Check& check)
{
    const Docs docs = MakeDocs();
    PreviewRig r;
    r.Put(docs.before);
    r.Put(docs.after);
    ScenePreviewWindow window;
    window.open = true;
    WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    check(window.ImageCount() == 0 && !window.Session().IsOpen(), "window: opened with nothing selected it only explains how to open a preview");
    check(window.Open(r.session, ArgsFor(docs.before, docs.after, docs.changes)), "window: Open starts a preview");
    for (int i = 0; i < 8; ++i) {
        r.Drive(window.Session(), 2);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    }
    check(window.Session().Settled() && window.ImageCount() == 1, "window: Overlay draws one image");
    window.Session().SetMode(PreviewMode::SideBySide);
    WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    check(window.ImageCount() == 2, "window: side by side draws two images (one shared camera)");
    window.Session().SetMode(PreviewMode::Before);
    WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    check(window.ImageCount() == 1, "window: Before draws one image");
    // デバイスの消失: GPU の資源だけ手放す。Scene は残り、次の描画で作り直せる (ここでは GPU が無いので、残ることを確かめる)
    window.ReleaseGpu();
    check(window.GpuTargetCount() == 0 && window.Session().IsOpen() && window.Session().BeforeScene() != nullptr && window.Session().AfterScene() != nullptr,
          "window: after the device is lost only the GPU targets are released; the preview scenes survive for re-creation");
    WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    check(window.open && window.Session().IsOpen(), "window: a frame after ReleaseGpu is safe");
    window.ReleaseGpu();
    window.Close();
    check(!window.open && !window.Session().IsOpen() && PreviewScene::LiveCount() == 0 && window.GpuTargetCount() == 0, "window: Close destroys the scenes and the targets");
    // x で閉じた: open を偽にすると、次のフレームで破棄する
    window.Open(r.session, ArgsFor(docs.before, docs.after, docs.changes));
    r.Drive(window.Session(), 6);
    window.open = false;
    WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
    check(!window.Session().IsOpen() && PreviewScene::LiveCount() == 0, "window: closing the window (x) destroys the scenes");
    // 2 回開いて閉じても数が増えない
    for (int i = 0; i < 2; ++i) {
        window.Open(r.session, ArgsFor(docs.before, docs.after, docs.changes));
        r.Drive(window.Session(), 6);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
        window.Close();
    }
    check(PreviewScene::LiveCount() == 0 && window.GpuTargetCount() == 0, "window: opening and closing twice leaves neither scenes nor targets");
    // 日本語でも描ける
    {
        const Lang previous = CurrentLanguage();
        SetLanguage(Lang::Ja);
        window.Open(r.session, ArgsFor(docs.before, docs.after, docs.changes));
        r.Drive(window.Session(), 6);
        WithHeadlessImGuiFrame([&] { window.OnImGui(r.session); });
        check(window.ImageCount() == 1, "window: draws in Japanese too");
        window.Close();
        SetLanguage(previous);
    }
}

// ---- AI Changes 窓の入口 ----

void TestEntrance(const Check& check)
{
    const Docs docs = MakeDocs();
    PreviewRig r;
    r.Put(docs.before);
    r.Put(docs.after);
    AiChangesWindow window;
    AiChangesHost host;
    host.projectRoot = kRoot;
    // 詳細が無い・口が無いときは開かない
    check(!window.OpenScenePreview(r.session, host), "entrance: without a selected revision nothing opens");
    ScenePreviewOpenArgs received;
    int opened = 0;
    host.openScenePreview = [&](ScenePreviewOpenArgs args) {
        received = std::move(args);
        ++opened;
        return true;
    };
    check(!window.OpenScenePreview(r.session, host) && opened == 0, "entrance: no detail yet -> nothing opens");
    ChangeModel& model = r.session.Model();
    model.SetInterest(kTrackAllParts);
    model.SelectRevision(5);
    for (int i = 0; i < 6; ++i) {
        r.s->nowMs += 1000;
        r.session.Poll();
        // describe に Before / After の Blob を答える
        std::string batch;
        for (; r.answered < r.s->written.size(); ++r.answered) {
            const json req = json::parse(r.s->written[r.answered], nullptr, false);
            const std::string op = req.value("op", std::string());
            const uint64_t id = req["id"].get<uint64_t>();
            if (op == trackop::kHello) {
                continue;
            }
            json result = json::object();
            if (op == trackop::kDescribe) {
                result = { { "revisionId", 5 }, { "path", kScenePath }, { "changeType", "modified" }, { "actor", "ai" },
                           { "before", { { "hash", Hash(docs.before) }, { "size", docs.before.size() }, { "blobState", "present" } } },
                           { "after", { { "hash", Hash(docs.after) }, { "size", docs.after.size() }, { "blobState", "present" } } } };
            } else if (op == trackop::kList || op == trackop::kSceneDiff || op == trackop::kHunks || op == trackop::kSessions) {
                result = { { "items", json::array() }, { "nextCursor", nullptr } };
            }
            batch += OkReply(id, result).dump() + "\n";
        }
        if (!batch.empty()) {
            r.s->chunks.push_back(batch);
        }
        r.session.Poll();
    }
    check(window.OpenScenePreview(r.session, host) && opened == 1, "entrance: a scene revision with its detail opens the preview through the host");
    check(received.revision.revisionId == 5 && received.revision.path == kScenePath && received.revision.before.hash == Hash(docs.before) && received.revision.after.hash == Hash(docs.after)
              && received.projectRoot == kRoot,
          "entrance: the revision's Before / After hashes and the project root are passed");
}

} // namespace

void RunScenePreviewSelfTests(const std::function<void(bool, const char*)>& check)
{
    TestBasic(check);
    TestScreen(check);
    TestFailures(check);
    TestChunksAndStale(check);
    TestAssetNotes(check);
    TestManagedSkipped(check);
    TestWindow(check);
    TestEntrance(check);
    TestProductionUntouched(check);
    TestPlayUnaffected(check);
}

// SceneRestoreSelfTest が起動した本物の ManagedHost で確かめる (CoreCLR は 1 プロセスで 1 回しか起動できないので、同じホストを借りる)
void RunScenePreviewManagedChecks(const std::function<void(bool, const char*)>& check, ManagedHost& host, Scene& liveScene, ComponentTypeId probeType)
{
    if (probeType == kInvalidComponentType || !host.IsManagedComponent(probeType)) {
        check(false, "preview managed: the C# probe component is registered");
        return;
    }
    World& liveWorld = liveScene.GetWorld();
    GameObject live = liveScene.CreateGameObjectTracked("PreviewLiveProbe");
    liveWorld.AddComponentRaw(live.Id(), probeType);
    liveWorld.ApplyStructuralChanges();
    const uint64_t liveFid = liveScene.EnsureFileId(live.Id());
    const json liveBefore = SceneSerializer::EntityToJson(liveScene, live.Id());
    const uint64_t hashBefore = HashWorld(liveWorld);

    // C# の型を含むシーンの JSON (本物の ManagedHost の判定で取り除く)
    json doc = MakeSceneDoc({ { "A", 0, 0, 0 }, { "B", 1, 0, 0 } });
    doc["entities"][0]["components"]["RestoreProbe"] = { { "speed", 9.0 }, { "count", 7 } };
    doc["entities"][1]["components"]["RestoreProbe"] = { { "speed", 3.0 } };
    const ManagedComponentFilter filter = [&host](const std::string& name) {
        const ComponentTypeId type = ComponentRegistry::Get().FindByName(name);
        return type != kInvalidComponentType && host.IsManagedComponent(type);
    };
    {
        PreviewScene preview;
        const PreviewLoadInfo info = LoadPreviewScene(preview.scene, doc.dump(), filter);
        World& previewWorld = preview.scene.GetWorld();
        bool anyManaged = false;
        const ComponentTypeId required[] = { probeType };
        previewWorld.ForEachArchetype(required, [&](Archetype&) { anyManaged = true; });
        check(info.status == PreviewLoadStatus::Ok && info.managedSkipped == 2 && !anyManaged,
              "preview managed: with a real C# host, C# components in the document are skipped and no entity of the preview scene has one");
    }
    const json liveAfter = SceneSerializer::EntityToJson(liveScene, liveScene.FindByFileId(liveFid).Id());
    check(liveAfter == liveBefore && HashWorld(liveWorld) == hashBefore,
          "preview managed: the live C# component (its serialized fields) and the live world hash are unchanged by loading a preview");
}

} // namespace mye
