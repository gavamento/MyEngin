// シャドウマップ深度パス スキニング版。VS だけが shadow_depth と異なる (GPU スキニング)。
// gMVP = transpose(world * lightViewProj) (CPU 側で合成済み)。PS は深度専用のためダミー。
// エントリ: VSMain / PSMain (ShaderManager の規約)
//
// ★これが無いと、スキンメッシュは**バインドポーズの生ジオメトリ**のまま影に焼かれる。
//   本描画 (forward_skinned / deferred_gbuffer_skinned) は geometry 空間 → world を
//   ボーンパレットだけで完結させ、メッシュのエンティティは恒等 transform に固定されている
//   (FbxLoader の P4-5) ので、パレットを掛けない影は world = 恒等のまま生の頂点を描く =
//   FBX の元単位がそのまま world 単位になる。cm 単位で作られた素材 (Mixamo 等。ufbx は
//   cm→m をジオメトリではなくノード鎖へ入れる) では、これが 100 倍の塊として影に出る。

// RenderTypes.h の mye::kMaxBones と必ず一致させること (check_rules.ps1 規則 9 が検査)
#define MYE_MAX_BONES 128

cbuffer ShadowObject : register(b0)
{
    float4x4 gMVP;
};

// ボーンパレット (M18)。各行列 = transpose(inverseBind * jointWorld) (行ベクトル規約)。
// スロットは本描画のスキニング版と同じ b3 — パレットの中身も同一のものを流用する。
cbuffer BonePalette : register(b3)
{
    float4x4 gBones[MYE_MAX_BONES];
};

// ★入力レイアウトは VS リフレクション + APPEND_ALIGNED で組まれる (ShaderManager) ので、
//   使わない normal / uv も MeshVertex (GpuResources.h) の並び順どおりに宣言すること。
//   間を抜くと後続のオフセットがずれてボーン index / weight が化ける。
struct VSIn
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD0;
    uint4  bones   : BLENDINDICES;
    float4 weights : BLENDWEIGHT;
};

float4 VSMain(VSIn v) : SV_Position
{
    float3 localPos = v.pos;
    const float wsum = v.weights.x + v.weights.y + v.weights.z + v.weights.w;
    if (wsum > 1e-4f) {
        // 行ベクトル規約: skinnedPos = sum_i weight_i * (pos * gBones[idx_i])
        // (forward_skinned.hlsl / deferred_gbuffer_skinned.hlsl と同一式。法線は深度のみの
        //  このパスでは要らないので位置だけ回す)
        const float4 p = float4(v.pos, 1.0f);
        float3 sp = mul(p, gBones[v.bones.x]).xyz * v.weights.x;
        sp += mul(p, gBones[v.bones.y]).xyz * v.weights.y;
        sp += mul(p, gBones[v.bones.z]).xyz * v.weights.z;
        sp += mul(p, gBones[v.bones.w]).xyz * v.weights.w;
        localPos = sp;
    }
    return mul(float4(localPos, 1.0f), gMVP);
}

// 深度のみ描画のため実際には bind しない (PSSetShader(nullptr))。コンパイル成立用の最小 PS。
float4 PSMain() : SV_Target
{
    return 0.0f;
}
