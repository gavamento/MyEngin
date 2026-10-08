#pragma once
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <DirectXMath.h>

#include "Engine/Core/Ecs/EntityID.h"

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

// 読み込み元のキーの補間 (glTF の sampler.interpolation)。JointTrack は線形しか持たないので、
// 読み込み時に ExpandToLinearKeys で線形のキー列へ直す (M89p)
enum class KeyInterpolation : int32_t {
    Linear = 0,
    Step,        // 次のキーまで前の値を保つ
    CubicSpline, // エルミート。値は 1 キー 3 要素 (入り接線・値・出接線)
};

// times (秒、昇順) と raw (成分 comps 個ずつ) を、線形補間すると元の補間になる (近似する) キー列にする。
//   - Linear: そのまま写す (従来の読み込みとビット一致)
//   - Step: 2 つ目以降のキーの時刻に「前の値」と「その値」を並べる。FindSpan は同時刻なら後ろを取るので、
//     キーの時刻ちょうどで新しい値に切り替わる (glTF の STEP と同じ)
//   - CubicSpline: 区間ごとに 1/sampleHz 秒以下の刻みでエルミート補間を評価する (キーの時刻は必ず含む)。
//     normalize なら各点を正規化する (回転。glTF 仕様の「補間後に正規化」)
// raw の要素数が足りなければ false を返し、out は空にする
bool ExpandToLinearKeys(KeyInterpolation interp, const std::vector<float>& times, const std::vector<float>& raw,
                        int32_t comps, bool normalize, float sampleHz, std::vector<float>& outTimes,
                        std::vector<float>& outVals);

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
    // HashStr(クリップ名) == nameHash のクリップ index (先頭一致)。見つからない / 無名のクリップは -1 (M89b)。
    // コントローラは骨クリップを名前で参照する — 同じ名前のクリップが、モデルごとに違う index に居てよい
    int32_t FindClipByHash(uint64_t nameHash) const;
};

// 列挙の 1 件 (参照ピッカー用)。GpuResources.h の AssetEntry は **このヘッダより下流**
// (GpuResources.h が Skeleton.h を include する) なので使えない。PhysMatEntry /
// AnimClipEntry と同じく自前の型を返し、呼び出し側で AssetEntry へ詰め替える
struct SkinnedModelEntry {
    uint64_t hash = 0;
    std::string name;
};

class SkinnedModelLibrary {
public:
    AssetID Register(std::string_view name, SkinnedModel model); // 同名は差し替え
    const SkinnedModel* Get(AssetID id) const;
    // ★名前は Register でしか手に入らない (models_ はハッシュしか持たない)。これが無いと
    //   Inspector の AssetRef ピッカーが候補を 1 件も作れず、SkinnedMesh.model が
    //   「メッシュ + マテリアル + テクスチャの混合リスト」へ落ちる
    std::vector<SkinnedModelEntry> Enumerate() const;

private:
    std::unordered_map<uint64_t, SkinnedModel> models_;
    std::unordered_map<uint64_t, std::string> names_; // 列挙用 (MeshLibrary と同じ流儀)
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

// ---- 上の 2 つを組み立てている素材 (M48g で公開) ----
// **`ComputeJointGlobal` は 1 回ごとに全ジョイントの局所行列を作り直す**ので、同じモデル・
// 同じ時刻で複数のジョイントを引く用途 (部位追従) では O(部位数 × ジョイント数) になる。
// 局所行列は (model, clip, timeSec) だけの関数なのでキャッシュ単位として切り出してある。
// 評価順は ComputeBonePalette と 1 命令も変えていない = 結果はビット一致

// clip を timeSec でサンプルした全ジョイントのローカル行列 (joints.size() 個)
void ComputeJointLocals(const SkinnedModel& model, int clip, float timeSec,
                        std::vector<DirectX::XMMATRIX>& outLocals);

// ---- クロスフェード (M18 追補) ----
// 2 クリップをそれぞれサンプルし、ジョイントごとに T / S を線形・R を slerp で混ぜた局所行列
// (joints.size() 個)。weightB = 0 で A、1 で B。
// ★行列同士を線形に混ぜない — 回転成分が縮んで、切り替えの途中だけ皮膚が痩せる。
// ★ComputeJointLocals とは別関数にしてある。あちらは M18 からのビット不変が selftest の
//   対象 (pose checksum) なので、フェードしていない経路は 1 命令も変えない
void ComputeJointLocalsBlended(const SkinnedModel& model, int clipA, float timeSecA, int clipB,
                               float timeSecB, float weightB,
                               std::vector<DirectX::XMMATRIX>& outLocals);

// ---- 多層ブレンド (M89a) ----
// ポーズプログラムの 1 層。重みは整数 (Q16 など、単位は呼び出し側が揃える) のまま渡す —
// 層を畳むときの比 w_i / (w_0 + ... + w_i) を整数の和から作るため (float の累積を持たない)
struct SkeletalLayer {
    int32_t clip = -1;     // 範囲外はバインドポーズ
    float timeSec = 0.0f;
    int32_t weight = 0;    // 0 以下の層は評価しない
};

// 層を先頭から順に畳んだ局所行列 (joints.size() 個)。層 i は「ここまでの結果」と
// w_i / (w_0 + ... + w_i) の比で混ぜる (T / S は線形、R は slerp)。2 層なら
// ComputeJointLocalsBlended と同じ混ぜ方になる。順序で結果が変わるので層の並びも入力の一部。
// ★重みが正の層が 1 枚だけなら ComputeJointLocals(clip, timeSec) をそのまま呼ぶ (ビット一致)。
//   単一クリップのステートが旧経路と同じ絵になることを、この分岐で固定している。
// 重みが正の層が無ければバインドポーズ。重みが正の層は先頭から kMaxSkeletalLayers 枚まで使う
inline constexpr int32_t kMaxSkeletalLayers = 8;
void ComputeJointLocalsLayered(const SkinnedModel& model, const SkeletalLayer* layers,
                               int32_t layerCount, std::vector<DirectX::XMMATRIX>& outLocals);

// ---- ルートモーション (M89j) ----
// clip を timeSec でサンプルした jointIndex の局所の平行移動 (上の関数群と同じサンプラ、トラックが無ければ bindT)。
// clip が範囲外は bindT、jointIndex が範囲外は 0
DirectX::XMFLOAT3 SampleJointTranslation(const SkinnedModel& model, int clip, int32_t jointIndex, float timeSec);
// ルートモーションを測るジョイント = 親を持たない最初のジョイント。無ければ -1。
// ★FBX は非ジョイントの祖先もジョイントに含める (M48a) ので、そちらでは動かない祖先が選ばれる
int32_t FindRootJoint(const SkinnedModel& model);
// ---- ルートモーションのヨー (M89k) ----
// clip を timeSec でサンプルした jointIndex の回転の、クリップの先頭 (時刻 0) からの変化 R(t)·R(0)⁻¹ のうち、
// up (ジョイントの親空間の単位ベクトル) まわりのひねり (swing-twist の twist)。返り値は (cos(θ/2), sin(θ/2)) で
// 四元数 (sin·up, cos) に当たり、cos >= 0 にそろえる (θ は -π..π)。sqrt と四則だけで求める。
// 回らない (先頭と同じ回転・回転トラックが無い・範囲外) ときは (1, 0) ちょうど
DirectX::XMFLOAT2 SampleJointYaw(const SkinnedModel& model, int clip, int32_t jointIndex, float timeSec,
                                 const float (&up)[3]);

// locals (上の出力) から 1 ジョイントのグローバル行列。範囲外 index は恒等
DirectX::XMMATRIX JointGlobalFromLocals(const SkinnedModel& model,
                                        const std::vector<DirectX::XMMATRIX>& locals,
                                        int32_t jointIndex);

// ---- 2 ボーン IK (M89l) ----
// 1 本の鎖の目標。座標はすべてモデルのエンティティ空間 (= jointGlobal の空間)
struct TwoBoneIkGoal {
    int32_t endJoint = -1;                               // 先端。中間 = その親、根 = 中間の親
    DirectX::XMFLOAT3 target = { 0.0f, 0.0f, 0.0f };     // 先端を置く位置
    bool useRotation = false;                            // true: 先端の回転を targetRotation にする
    DirectX::XMFLOAT4 targetRotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    bool hasPole = false;                                // true: 中間を pole の側へ曲げる。false: 今の曲げ面のまま
    DirectX::XMFLOAT3 pole = { 0.0f, 0.0f, 0.0f };
    float weight = 1.0f;                                 // 0..1。0 は何もしない
};
// locals の 3 ジョイント (根・中間・先端) を、先端が goal.target に届くように回す (純関数、sqrt と四則のみ)。
// 骨の長さは変えず、届かない距離は伸び切り / 縮み切りで止める (根から目標への直線上)。
// 根と中間は「今の向き → 新しい向き」の最短の弧で回す (回転の作り方は半角公式。acos / atan2 は使わない)。
// weight は目標を今の先端の位置から、曲げ面を今の面から pole の面へ、先端の回転を今の回転から、それぞれ線形に寄せる
// (0 の近くで姿勢が飛ばない)。鎖が組めない (先端・中間に親が無い) / 骨の長さが 0 のときは何もしない
void SolveTwoBoneIk(const SkinnedModel& model, const TwoBoneIkGoal& goal, std::vector<DirectX::XMMATRIX>& locals);
// joint のグローバル位置を offset (エンティティ空間) だけずらす。子孫も一緒に動く (足の接地の骨盤、M89m)。範囲外は何もしない
void OffsetJointGlobal(const SkinnedModel& model, int32_t joint, const DirectX::XMFLOAT3& offset,
                       std::vector<DirectX::XMMATRIX>& locals);

// ---- ラグドール用のパレット構築 (M60g1) ----
// `hasOverride[j]` が非 0 のジョイントは **`overrides[j]` をそのまま jointGlobal として使う**
// (剛体が骨の代わりに姿勢を決めている)。残りは locals から階層合成で埋める。
//
// ★合成は `JointGlobalFromLocals` と**同じ「親チェーンを上へ」の形**で書いてある。
//   途中で override 済みの祖先に当たったらそこで打ち切ってその行列を掛ける。
//   おかげで **joints 配列が親→子順に並んでいる必要が無い** (glTF / FBX のどちらでも
//   前提を置かずに済む) し、override が 1 つも無いときは `ComputeBonePalette` と
//   1 命令も違わない = ビット一致する。
// ★配列長が joints.size() と食い違うときは override 無しとして扱う (壊れた入力から
//   黙って別のポーズを作らない)。
void ComputeBonePaletteWithOverrides(const SkinnedModel& model,
                                     const std::vector<DirectX::XMMATRIX>& locals,
                                     const std::vector<uint8_t>& hasOverride,
                                     const std::vector<DirectX::XMMATRIX>& overrides,
                                     std::vector<DirectX::XMFLOAT4X4>& out);

} // namespace mye
