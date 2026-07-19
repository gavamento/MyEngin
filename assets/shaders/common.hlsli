// 共通ライティングヘルパ。
// このファイルの変更は include 依存グラフ経由で全依存シェーダを再コンパイルさせる (spec 8.1)

float3 ApplyDirectionalLight(float3 albedo, float3 normal, float3 lightDir, float3 lightColor,
                             float intensity, float3 ambient)
{
    const float ndl = saturate(dot(normal, -lightDir));
    return albedo * (ambient + lightColor * intensity * ndl);
}

