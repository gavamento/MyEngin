// デカールの投影パス (M56a / M56b、spec §6.6)。
//
// GBuffer のジオメトリパスが終わった直後に、投影ボックス (単位立方体 [-0.5,0.5]^3 を
// デカールのワールド行列で変換したもの) を 1 個ずつ描いて **albedo (RT0)** と、
// M56b からは **法線 (RT1) / roughness (RT3 の g)** をアルファブレンドで上描きする。
// RT2 (ワールド座標) と RT4 (画面速度 = TAA の入力) には決して書かない。
//
// ★**深度の逆投影は要らない** — GBuffer RT2 にワールド座標がそのまま入っているので、
//   Load してデカールの逆行列を掛ければ即ローカル座標が出る。SSR (M56d) は
//   R16G16B16A16_FLOAT の精度が足りず深度から復元し直す必要があるが、
//   こちらは「箱の中か外か」の判定と UV しか使わないので 16F で十分。
//
// ★**ボックスは裏面 (CULL_FRONT) + 深度テスト無しで描く。** 表面を描くと
//   カメラが箱の中に入った瞬間にデカールが消える。深度を切ってあるので
//   「箱が完全に壁の裏」でもラスタライズはされるが、受け面のワールド座標が
//   箱の外に出るので下の判定で必ず捨てられる。
//
// ★頂点バッファを持たない。SV_VertexID から立方体を組む (ShaderManager の
//   入力レイアウト構築は SV_ 系を無視するので inputLayout は null になる)。

cbuffer DecalParams : register(b0)
{
    float4x4 gViewProj;      // transpose(view*proj)。ジッタ込み (ラスタライズ用)
    float4x4 gDecalWorld;    // transpose(ローカル → ワールド)
    float4x4 gDecalInvWorld; // transpose(ワールド → ローカル)
    float4   gDecalColor;    // rgb = リニア tint / a = 不透明度
    float4   gDecalUv;       // xy = UV スケール / zw = UV オフセット
    float4   gDecalProj;     // xyz = 投影方向 (ワールド、正規化) / w = 角度フェードの cos
    // ---- M56b (末尾 append) ----
    float4   gDecalAxisX;    // xyz = ローカル +X のワールド向き (正規化) / w = 法線の強さ
    float4   gDecalAxisY;    // xyz = ローカル +Y のワールド向き (正規化) / w = 上書き roughness
    float4   gDecalSurf;     // x = 粗さの強さ / y = 法線マップ有無 (0/1) / zw = 予約
};

Texture2D gPosition : register(t0); // GBuffer RT2 (ワールド座標。a = ジオメトリ有りマーク)
// GBuffer RT1 (ワールド法線 *0.5+0.5)。★M56b で RT1 を **RTV として bind する**フレームは
// ここに「RT1 のコピー」が張られる — 同一リソースの読み書き二重バインドは合法ではないため。
// コピーを作るかどうかは DeferredPath が「1 枚でも法線/粗さを書くか」で決める
Texture2D gNormal   : register(t1);
Texture2D gDecalTex : register(t2); // 貼る画像 (null のときは白 1x1 が張られる)
Texture2D gDecalNrm : register(t3); // M56b: 接線空間の法線マップ (null のときは白 1x1)
SamplerState gSampler : register(s0); // LINEAR/CLAMP (光パスの IBL 用サンプラを流用)

struct VSOut
{
    float4 pos : SV_Position;
};

// 単位立方体の 12 三角形。値は角の bit (bit0=x bit1=y bit2=z、0 = -0.5 / 1 = +0.5)。
// **巻きは「外向き面が D3D の表面 (cross(b-a,c-a) が外を向く)」**に揃えてある —
// ラスタライザ側が CULL_FRONT なので、実際に描かれるのはこの裏返しの内向き面
static const uint kCubeCorner[36] = {
    0, 2, 3,  0, 3, 1,  // -Z
    4, 5, 7,  4, 7, 6,  // +Z
    0, 4, 6,  0, 6, 2,  // -X
    1, 3, 7,  1, 7, 5,  // +X
    0, 1, 5,  0, 5, 4,  // -Y
    2, 6, 7,  2, 7, 3,  // +Y
};

VSOut VSMain(uint vid : SV_VertexID)
{
    const uint c = kCubeCorner[vid];
    const float3 lp = float3(((c & 1u) != 0u) ? 0.5f : -0.5f,
                             ((c & 2u) != 0u) ? 0.5f : -0.5f,
                             ((c & 4u) != 0u) ? 0.5f : -0.5f);
    VSOut o;
    o.pos = mul(mul(float4(lp, 1.0f), gDecalWorld), gViewProj);
    return o;
}

// M56b: 書き先は albedo (RT0) / 法線 (RT1) / material (RT3) の 3 枚。
// ★**RT2 (ワールド座標) と RT4 (画面速度) は意図的に飛ばしてある** — RT2 はこのパスが
//   SRV として読んでいる入力そのもので、RT4 は TAA の入力。DeferredPath 側でも
//   スロット 2 / 4 に RTV を張らない (SV_Target2 を宣言しないことで「書きようがない」に
//   してある = D3D の「PS が書かなかった RT の内容は未定義」規則ごと消す)
struct PSOut
{
    float4 albedo   : SV_Target0;
    float4 normal   : SV_Target1;
    float4 material : SV_Target3;
};

PSOut PSMain(VSOut i)
{
    const int3 pixel = int3(int2(i.pos.xy), 0);
    const float4 gpos = gPosition.Load(pixel);
    // ★ジオメトリの無い画素 (空) を必ず先に捨てる。GBuffer はゼロクリアなので、
    //   ワールド原点を含む箱に対しては「空がちょうど箱の中に居る」ことになってしまう
    if (gpos.a < 0.5f) {
        discard;
    }
    const float3 lp = mul(float4(gpos.xyz, 1.0f), gDecalInvWorld).xyz;
    if (any(abs(lp) > 0.5f)) {
        discard; // 受け面が箱の外
    }
    // 角度フェード: 受け面が投影方向に正対しているほど濃い。
    // ★投影パスでは common.hlsli の PerturbNormal 系 (posW の ddx/ddy から TBN を作る)
    //   が使えない — 微分が「投影ボックスの面」のものになるため。ここは GBuffer に
    //   既に書かれている受け面の法線をそのまま読む (M56b の法線書き込みも同じ理由で
    //   デカール自身の OBB 基底から TBN を作ることになる)
    const float3 n = normalize(gNormal.Load(pixel).xyz * 2.0f - 1.0f);
    const float ndl = dot(n, -gDecalProj.xyz);
    const float fade = saturate((ndl - gDecalProj.w) / max(1.0f - gDecalProj.w, 1e-4f));
    if (fade <= 0.0f) {
        discard;
    }
    // ローカル xy → UV。y を反転しているのは「画像の上が +Y」に見えるようにするため
    const float2 uv = float2(lp.x + 0.5f, 0.5f - lp.y) * gDecalUv.xy + gDecalUv.zw;
    const float4 tex = gDecalTex.Sample(gSampler, uv);
    const float alpha = saturate(gDecalColor.a * fade * tex.a);

    PSOut o;
    // 非プリマルチプライ (ブレンドは SRC_ALPHA / INV_SRC_ALPHA)。
    // RT0 のアルファは「ジオメトリ有り」マークなので、DestBlendAlpha=INV_SRC_ALPHA と
    // SrcBlendAlpha=ONE の組み合わせで a + 1*(1-a) = 1 のまま保たれる
    o.albedo = float4(gDecalColor.rgb * tex.rgb, alpha);

    // ---- M56b: 法線 (RT1) ----
    // TBN は**デカール自身の OBB 基底**から作る。T = ローカル +X / B = ローカル -Y
    // (UV の v を反転しているぶん符号が入れ替わる) / N = -投影方向 (デカール面は
    // プロジェクタの方を向く)。3 本とも CPU 側で正規化済みなのでここでは正規化しない。
    // ★common.hlsli の PerturbNormal は使えない — posW の ddx/ddy が「投影ボックスの面」の
    //   ものになり、受け面の微分にならないため (これがこのサブの中心的な制約)
    float3 tsN = float3(0.0f, 0.0f, 1.0f);
    if (gDecalSurf.y > 0.5f) {
        tsN = gDecalNrm.Sample(gSampler, uv).xyz * 2.0f - 1.0f;
    }
    const float3 nDecal = normalize(tsN.x * gDecalAxisX.xyz - tsN.y * gDecalAxisY.xyz
                                    + tsN.z * (-gDecalProj.xyz));
    // ★**アルファがそのままブレンド係数**。強度 0 → src*0 + dst*1 = dst を**厳密に**
    //   維持する (係数 0 の乗算は IEEE で厳密) = 法線を使わないデカールは RT1 を
    //   1 ビットも動かさない。encode が affine なので、ブレンドは
    //   encode(lerp(受け面, デカール)) と等価 = 符号化空間で混ぜてよい
    o.normal = float4(nDecal * 0.5f + 0.5f, alpha * gDecalAxisX.w);

    // ---- M56b: roughness (RT3 の g だけ) ----
    // r = metallic / b = emissive / a はブレンドステートの書込マスクで守る (GREEN のみ)。
    // ここでも係数は alpha * 粗さの強さ なので、強度 0 は厳密に恒等
    o.material = float4(0.0f, gDecalAxisY.w, 0.0f, alpha * gDecalSurf.x);
    return o;
}
