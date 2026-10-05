//====================================================================================
//                          NavAgentTypes.h
//  MyEngin/ 秋田蓮音                                                     10/05/2026
//                                          NavMesh の Agent Type の表 (project_settings.json)
//====================================================================================
#pragma once
#include <string>
#include <vector>

namespace mye {

struct NavMeshSurfaceComponent;

// Agent Type 1 件 (Unity の Agent Types / UE の Supported Agents)。id は Surface / Agent の agentTypeId に入る値で、
// 名前を変えても変わらない。寸法は Surface のベイク寸法 (agentRadius / agentHeight / maxClimb / maxSlopeDeg) の元
struct NavAgentType {
    int id = 0;
    char name[32] = {};
    float radius = 0.3f;
    float height = 1.8f;
    float maxClimb = 0.3f;
    float maxSlopeDeg = 45.0f;
    float dropHeight = 2.0f;   // Link の自動生成 (M84e) の飛び降りの高さ
    float jumpDistance = 1.0f; // 同じく飛び越えの隙間の幅 (縁から縁)
};

// Agent Type の表 (M84a)。assets\project_settings.json の "navAgentTypes" 配列を読む。
// エディタ専用 — project_settings はリプレイの外にあるので、sim は Surface に写した寸法しか見ない。
// 写すのは型を選んだときと Bake のとき (NavApplyAgentType)。id 0 は常にあり、消せない (Unity の Humanoid と同じ)。
// 他キーを保存時に壊さない (NavAreaNames と同じ流儀)
class NavAgentTypes {
public:
    static constexpr int kMaxTypes = 16;

    NavAgentTypes(); // 既定 (id 0 = Humanoid だけ) で初期化
    static NavAgentTypes& Get(); // エディタ内シングルトン

    // 冪等 (同じ assetsRoot なら再読込しない)。保存後は force で
    void Load(const std::wstring& assetsRoot, bool force = false);
    bool Save(const std::wstring& assetsRoot) const;

    // 編集中の表が保存済みの内容と食い違うか (ディスクを読み直した表と比べる)。一度も Load していなければ false
    bool DiffersFromDisk() const;

    int Count() const { return static_cast<int>(types_.size()); }
    const NavAgentType& At(int index) const { return types_[index]; }
    NavAgentType& EditAt(int index) { return types_[index]; }
    const NavAgentType* Find(int id) const; // 無ければ null
    int IndexOf(int id) const;              // 無ければ -1

    // 末尾に追加する (id は既存の最大 + 1)。満杯なら -1、成功なら追加した添字
    int Add();
    // 添字の型を消す。id 0 は消せない (false)
    bool Remove(int index);

private:
    std::vector<NavAgentType> types_;
    std::wstring loadedRoot_;
};

// Surface の寸法が型の寸法と一致するか
bool NavSurfaceMatchesAgentType(const NavMeshSurfaceComponent& surface, const NavAgentType& type);
// 型の寸法を Surface へ写す
void NavApplyAgentType(NavMeshSurfaceComponent& surface, const NavAgentType& type);

} // namespace mye
