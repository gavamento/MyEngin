//====================================================================================
//                          FractureLibrary.h
//  MyEngin/ 秋田蓮音                                                     09/25/2026
//                                          破片資産(.mfrac)の読込とメッシュ/凸包の登録
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Engine/Asset/FractureAsset.h"
#include "Engine/Engine/Physics/FractureBake.h"

namespace mye {

struct RenderResources;
class ConvexColliderLibrary;

// 破片 1 個の登録済み参照 (spec §2「破片メッシュの参照方法」)
struct FracturePieceRef {
    DirectX::XMFLOAT3 origin{ 0, 0, 0 };
    double volume = 0.0;
    AssetID outerMesh; // MeshLibrary "<prefix>#frag<i>"
    AssetID capMesh;   // MeshLibrary "<prefix>#frag<i>#cap"
    AssetID hull;      // ConvexColliderLibrary "<prefix>#frag<i>#hull"
    std::vector<FractureAsset::NeighborRecord> neighbors;
    std::string boneName; // スキン破壊 (M80j)。空 = 骨なし (通常の破片)
};

// 1 資産分の読み込み・登録済み状態。data は ConvexColliderLibrary::Clear() 後の
// 再登録 (ReregisterAll) に使うため保持しておく
struct FractureAssetHandle {
    std::string namePrefix;
    FractureAsset::FractureData data;
    std::vector<FracturePieceRef> pieces;
};

// `.mfrac` の読み込みと、MeshLibrary / ConvexColliderLibrary への登録 (M80c)。
// 登録名は "guid://<mfracGuid16hex>#frag<i>" 系 (ファイル由来) か、呼び出し側が渡す
// 任意の接頭辞 (メモリ上の焼き結果、--fracture-demo 用)。
//
// ConvexColliderLibrary::Clear() は登録済みの凸包を丸ごと捨てる。呼んだ後は必ず
// ReregisterAll() を呼ぶこと (忘れると shape=5 が null 解決され、破片がすり抜ける)
class FractureLibrary {
public:
    void Init(RenderResources* resources, ConvexColliderLibrary* colliders)
    {
        resources_ = resources;
        colliders_ = colliders;
    }

    // .mfrac をファイルから読み込む。読み込み済みならそのまま返す。
    // 見つからない/読めない/版違いはパスごとに ERROR を 1 回だけ出して nullptr
    const FractureAssetHandle* LoadFromFile(const std::wstring& path);

    // path が読み込み済みでもキャッシュを無視して読み直し、同じ登録名 (AssetID) のまま
    // メッシュ・凸包を差し替える。CommitFractureBake が書き出した直後に呼ぶ — 焼き方式の版
    // (kFractureBakeVersion) を上げ忘れて同じ保存名に別の中身を書いてしまった場合の保険。
    // 読めなければ ERROR を出して nullptr (登録済みの古いハンドルは残る)
    const FractureAssetHandle* ReloadFromFile(const std::wstring& path);

    // GUID の無いメモリ上の焼き結果を登録する (--fracture-demo 用)。
    // namePrefix の一意性は呼び出し側が保証する。同じ namePrefix は差し替え
    // boneNames (M80j): 非空なら bake.pieces と同じ並びで破片ごとの骨名を .mfrac へ書く
    // (AssignFractureBonesAndTransform の戻り値をそのまま渡す想定)。既定は非スキン
    const FractureAssetHandle* RegisterBaked(const std::string& namePrefix, const FractureBakeResult& bake,
                                              uint64_t sourceMeshHash, uint32_t seed, int32_t pieceCount,
                                              int32_t openMeshMode, int32_t voxelResolution,
                                              const std::vector<std::string>& boneNames = {});

    // 登録名 (ファイルなら "guid://<16hex>") で引く。未登録は nullptr
    const FractureAssetHandle* Find(const std::string& namePrefix) const;

    // 登録名の FNV-1a ハッシュ (= DestructibleComponent.fractureAsset がそのまま持つ値) で引く。
    // ファイル資産は LoadFromFile の prefix (SubAssetKeyPrefix) のハッシュ、メモリ登録
    // (--fracture-demo 等) は呼び出し側が namePrefix を HashStr したものと一致させて使う
    // (FractureSystem が実行時に Destructible.fractureAsset だけから資産を再解決するため)
    const FractureAssetHandle* FindByAssetId(AssetID id) const;

    // ConvexColliderLibrary::Clear() の後に呼ぶと、読み込み済みの全凸包を登録し直す
    void ReregisterAll();

    void Clear();

private:
    const FractureAssetHandle* RegisterInternal(const std::string& prefix, FractureAsset::FractureData data);

    RenderResources* resources_ = nullptr;
    ConvexColliderLibrary* colliders_ = nullptr;
    std::unordered_map<std::string, FractureAssetHandle> handles_;
    std::unordered_map<uint64_t, std::string> byHash_; // FindByAssetId 用の逆引き
    std::unordered_set<std::wstring> failedPaths_; // ERROR は 1 回だけ (パス単位)
};

// FractureBakeResult (Renderer 非依存の FractureVertex を使う分割コアの出力) を
// .mfrac の保存形式 (MeshVertex ベース) へ詰め替える
// boneNames (M80j): 非空なら bake.pieces[i] の骨名として pieces[i].boneName へ書く
// (i が範囲外なら空のまま = 非スキン扱い)
FractureAsset::FractureData BuildFractureAssetData(const FractureBakeResult& bake, uint64_t sourceMeshHash,
                                                    uint32_t seed, int32_t pieceCount, int32_t openMeshMode,
                                                    int32_t voxelResolution,
                                                    const std::vector<std::string>& boneNames = {});

// モジュール注入 (convexcol:: と同じ流儀)。EngineLoop が起動時に Install し終了時に外す
namespace fracturelib {
void Install(FractureLibrary* lib);
FractureLibrary* Library();
} // namespace fracturelib

} // namespace mye
