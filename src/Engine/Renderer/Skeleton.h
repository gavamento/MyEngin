#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/EntityID.h"

namespace mye {

// スケルタルアニメ / GPU スキニング (M18)。
// 座標系は既存モデルと同じく Z 反転済み (右手→左手)、行列は行ベクトル規約。

// スケルトンの 1 ジョイント。行列はいずれも「頂点 * M」の行ベクトル規約。
struct SkeletonJoint {
    int32_t parent = -1; // 親ジョイントの index (joints 配列内)、-1=ルート
    // ジョイント名 (glTF: node 名 / FBX: ノード名)。部位ソケットの joint 指定が名前で引く
    // (M48a)。ECS 非登録の CPU 構造体なので可変長で持てる。無名は "" (検索対象外)
    std::string name;
    // inverse-bind: メッシュ空間 → ジョイントのバインド局所空間 (Z 反転済み)
    DirectX::XMFLOAT4X4 inverseBind = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    // バインドポーズのローカル TRS (該当アニメトラックが無いジョイントで使う)
    DirectX::XMFLOAT3 bindT = { 0, 0, 0 };
    DirectX::XMFLOAT4 bindR = { 0, 0, 0, 1 };
    DirectX::XMFLOAT3 bindS = { 1, 1, 1 };
};

// 1 ジョイントの TRS キーフレームトラック (時間は秒)。空のチャネルはバインド値を使う。
struct JointTrack {
    std::vector<float> tTimes;
    std::vector<DirectX::XMFLOAT3> tVals;
    std::vector<float> rTimes;
    std::vector<DirectX::XMFLOAT4> rVals; // クォータニオン (Z 反転済み)
    std::vector<float> sTimes;
    std::vector<DirectX::XMFLOAT3> sVals;
};

struct SkeletalClip {
    std::string name;
    float duration = 0.0f;          // 秒
    std::vector<JointTrack> tracks; // joints.size() 個
};

// glTF の 1 skin = 1 SkinnedModel (スケルトン + クリップ群)。SkinnedModelLibrary が保持。
struct SkinnedModel {
    std::vector<SkeletonJoint> joints;
    std::vector<SkeletalClip> clips;

    // name のジョイント index (先頭一致・大小区別)。見つからない / 空名は -1 (M48a)
    int32_t FindJointByName(std::string_view name) const;
};

class SkinnedModelLibrary {
public:
    AssetID Register(std::string_view name, SkinnedModel model); // 同名は差し替え
    const SkinnedModel* Get(AssetID id) const;

private:
    std::unordered_map<uint64_t, SkinnedModel> models_;
};

// clip を timeSec でサンプルして各ジョイントのローカル TRS を作り、階層を掛け合わせて
// ボーンパレット (= transpose(inverseBind * jointGlobal)、シェーダへ直接アップロード可能) を out へ。
// clip<0 か該当トラック無しはバインドポーズを使う (=恒等スキニング=バインドポーズ描画)。
// out は joints.size() 個 (呼び出し側で最大 kMaxBones に丸める)。
void ComputeBonePalette(const SkinnedModel& model, int clip, float timeSec,
                        std::vector<DirectX::XMFLOAT4X4>& out);

// clip を timeSec でサンプルした jointIndex のグローバル行列 (= ジョイント階層内の合成のみ。
// inverseBind もアップロード用転置も掛けない生の行ベクトル行列)。部位ソケットのワールドは
// jointGlobal * エンティティ WorldMatrix (M48a) — glTF は非ジョイント祖先をエンティティ側
// (gWorld) が担い、FBX は祖先閉包込みでエンティティ側が恒等という両ローダの規約に一致する。
// ComputeBonePalette と同一のサンプラ・同一の積列で評価する (ビット一致)。範囲外 index は恒等
DirectX::XMMATRIX ComputeJointGlobal(const SkinnedModel& model, int clip, float timeSec,
                                     int32_t jointIndex);

} // namespace mye
