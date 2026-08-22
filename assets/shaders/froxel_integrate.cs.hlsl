// M57c: フロクセルの前方積分 (手前から奥へレイマーチして参加媒質の方程式を解く)。
//
// 1 スレッド = 1 本の Z 列 (だから XY だけをタイルにしてある。Z は割れない)。
// 入力セルは「単位長あたり」の量:  rgb = 内向き散乱 σ_s·L / a = 消散係数 σ_t。
// 出力セルは「カメラからそのスライスの奥端まで」の積分値:
//   rgb = 積算した内向き散乱 (透過率で重み付け済み) / a = そこまでの透過率 T。
// 消費者 (M57d/M57e) は「Lo = scene * T + inscatter」で合成するだけでよい。
//
// ★1 スライスを均質と見なした**解析積分**を使う。厚みをそのまま掛けると、
//   1 スライス内の自己遮蔽が消えて濃い霧ほど明るくなる (Hillaire 2015)。
// ★格納規約: テクセル z = 「スライス z の**奥端**までの積分」。テクセル中心は
//   sliceCoord z+0.5 にあるので、サンプル側は半テクセル手前を指すこと
//   (C++ の froxel::IntegratedSampleW が正本)。ここを揃えないと霧が 1 スライスずれる。
#include "froxel_common.hlsli"

// froxel_temporal.cs.hlsl と共有する CB (C++ の FroxelPostCB、176 バイト)。
// 積分はグリッドの深度分布しか要らないので、行列も feedback も読まない
cbuffer FroxelPostCB : register(b0)
{
    float4x4 gFroxelInvView;
    float4x4 gFroxelPrevViewProj;
    uint3 gFroxelGridSize;
    uint gFroxelHistValid;
    float gFroxelNearZ;
    float gFroxelFarZ;
    float gFroxelInvProj00;
    float gFroxelInvProj11;
    float gFroxelSliceJitter;
    float gFroxelFeedback;
    float2 gFroxelPostPad;
};

Texture3D<float4> gFroxelSrc : register(t0); // 注入 (またはテンポラル) の出力
RWTexture3D<float4> gFroxelOut : register(u0);

[numthreads(MYE_FROXEL_GROUP, MYE_FROXEL_GROUP, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gFroxelGridSize.xy)) {
        return;
    }
    const float sliceCount = (float)gFroxelGridSize.z;
    float3 accum = float3(0.0f, 0.0f, 0.0f);
    float transmittance = 1.0f;
    // 手前の境界は必ず nearZ から始める。**ジッタしたセル中心から始めてはいけない** —
    // 厚みの総和が (far - near) からずれ、一様な霧の全体透過率が e^{-σ_t·(f-n)} に
    // ならなくなる (テンポラルでフレーム毎に霧の濃さが脈打つ形で出る)
    float prevDepth = gFroxelNearZ;

    for (uint z = 0; z < gFroxelGridSize.z; ++z) {
        const float4 cell = gFroxelSrc[uint3(id.xy, z)];
        const float depth = FroxelSliceDepth((float)z + 1.0f, sliceCount, gFroxelNearZ,
                                             gFroxelFarZ);
        const float thickness = depth - prevDepth;
        prevDepth = depth;

        const float sigmaT = max(cell.a, 0.0f);
        // 透過率で重み付けした「このスライスぶんの寄与」。cell.rgb は単位長あたりなので
        // 解析積分係数を掛けると 1 スライスぶんの放射輝度になる
        accum += transmittance * cell.rgb * FroxelIntegratedSliceScatter(sigmaT, thickness);
        transmittance *= FroxelSliceTransmittance(sigmaT, thickness);

        gFroxelOut[uint3(id.xy, z)] = float4(accum, transmittance);
    }
}
