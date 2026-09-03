#include "Editor/SourceControl/StageClassifier.h"

#include <algorithm>

namespace mye {

namespace {

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool StartsWith(const std::string& s, const char* prefix)
{
    const size_t n = std::char_traits<char>::length(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

// パスの比較は**小文字化してから**。git は大小を保って返すが Windows のファイル系は
// 大小を区別しない = "Assets/Foo.PNG" と "assets/foo.png" が同じものを指す。
// ここで揃えないと「拡張子が大文字のテクスチャだけ段階判定から漏れる」
std::string Lower(const std::string& s)
{
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c == '\\') {
            c = '/'; // 呼び手が '\' 区切りを渡してきても壊れないように
        }
    }
    return out;
}

} // namespace

bool IsMetaSidecar(const std::string& path)
{
    return EndsWith(Lower(path), ".meta");
}

bool IsInsideAssets(const std::string& path)
{
    return StartsWith(Lower(path), "assets/");
}

bool IsSchemaPath(const std::string& path)
{
    return StartsWith(Lower(path), "assets/schemas/");
}

bool IsCppScript(const std::string& path)
{
    const std::string p = Lower(path);
    if (!StartsWith(p, "src/gamelogic/scripts/")) {
        return false;
    }
    // ★.h も含める。ヘッダだけ変わったときに「Rebuild Scripts」を案内しないと、
    //   ソースは新しいのに DLL は古い、という一番分かりにくい食い違いが残る
    return EndsWith(p, ".cpp") || EndsWith(p, ".h") || EndsWith(p, ".hpp")
        || EndsWith(p, ".inl");
}

bool IsCsScript(const std::string& path)
{
    return EndsWith(Lower(path), ".cs");
}

bool IsReloadableAsset(const std::string& path)
{
    const std::string p = Lower(path);
    // spec §4.1 の A の一覧。**ReloadHub::HandleChange が実際に分岐している綴りと
    // 1 対 1**。片方だけ足すと「A と判定したのに何も起きない」= 変更が
    // 見えないまま作業が進む
    static const char* const kExact[] = {
        ".hlsl", ".hlsli", ".png",  ".tga",  ".jpg",         ".jpeg",
        ".dds",  ".wav",   ".ogg",  ".glb",  ".gltf",        ".fbx",
        ".mat.json", ".anim.json", ".sound.json", ".mixer.json", ".physmat.json",
        ".actor.json", ".prefab.json",
    };
    for (const char* suffix : kExact) {
        if (EndsWith(p, suffix)) {
            return true;
        }
    }
    return false;
}

ApplyStage CombineStage(ApplyStage a, ApplyStage b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

ApplyStage ClassifyChange(const StageChange& change, const std::string& activeScene)
{
    const std::string p = Lower(change.path);
    if (p.empty()) {
        return ApplyStage::A;
    }

    // ---- C: 作り直さないと辻褄が合わないもの ----
    // スキーマ = 動的コンポーネントの定義。TypeId の並びが変わるので、
    // 生きている World を持ったまま読み直す手段が無い
    if (IsSchemaPath(p)) {
        return ApplyStage::C;
    }
    if (IsMetaSidecar(p)) {
        // guid が変わった / .meta がリネームされた = シーンが持つ参照が全部別物を指す。
        // 差し替えでは直せないので再起動 (spec §4.1 の C)
        if (change.metaGuidChanged || change.kind == BatchChange::Kind::Renamed) {
            return ApplyStage::C;
        }
        // それ以外の `.meta` は本体に付いて動くだけ = 単体では何もしない
        return ApplyStage::A;
    }

    // ---- B: 開いている文書を読み直す ----
    if (!activeScene.empty() && p == Lower(activeScene)) {
        return ApplyStage::B;
    }
    if (IsCppScript(p) || IsCsScript(p)) {
        return ApplyStage::B;
    }
    if (EndsWith(p, ".controller.json") || EndsWith(p, ".terrain.json")
        || EndsWith(p, ".terrain.edit")) {
        return ApplyStage::B;
    }
    if (p == "assets/input/actions.json" || p == "assets/project_settings.json") {
        return ApplyStage::B;
    }
    if (IsReloadableAsset(p) && change.kind == BatchChange::Kind::Deleted) {
        // ★消えた資産は ReloadHub では扱えない (読むファイルが無い)。
        //   参照している側 (シーン) を丸ごと読み直すしかない (spec S4)
        return ApplyStage::B;
    }

    // ---- A: その場で差し替える / 何もしない ----
    // 非アクティブな .scene.json / project.mye.json / 未知拡張子 /
    // assets\ と src\GameLogic\Scripts\ の外は no-op = A のまま
    return ApplyStage::A;
}

ApplyStage Classify(const StageInputs& in)
{
    // 件数が多すぎる = ブランチごと移った。1 件ずつ差し替えるより再起動が確実
    if (in.maxBatchApply > 0 && static_cast<int>(in.changes.size()) > in.maxBatchApply) {
        return ApplyStage::C;
    }
    ApplyStage stage = ApplyStage::A;
    bool hasActor = false;
    bool hasScene = false;
    for (const StageChange& c : in.changes) {
        stage = CombineStage(stage, ClassifyChange(c, in.activeScene));
        const std::string p = Lower(c.path);
        if (EndsWith(p, ".actor.json") || EndsWith(p, ".prefab.json")) {
            hasActor = true;
        }
        if (EndsWith(p, ".scene.json")) {
            hasScene = true;
        }
    }
    if (hasActor && hasScene) {
        // ★アクターとシーンが同時に動いたら開き直す。アクターの再合成
        //   (Prefab::PropagateBaseChange) は**今メモリにあるシーン**へ差分を撒くので、
        //   そのシーンのファイル自体も入れ替わっていると「どちらの世代へ撒いたか」が
        //   適用順で決まってしまう
        stage = CombineStage(stage, ApplyStage::B);
    }
    return stage;
}

} // namespace mye
