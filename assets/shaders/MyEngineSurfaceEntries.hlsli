// M79: サーフェスシェーダーの生成エントリ本体 (作者には見えない)。
// ShaderManager::CompileSurfaceProgram が「作者ソース + このファイル」を 1 つの翻訳単位として
// 3 系統 5 エントリ (色 VS/PS・影 VS・速度 VS/PS) を個別にコンパイルする。
// VSIn / VSOut / VSMain / PSMain は作者側の宣言 (MyEngineSurface.hlsli の下、この #include の前)
// を指す — 型名・メンバ名が規約と違うとここでコンパイルエラーになる (「サーフェス規約」の実体)。
//
// 位置に効く static (gViewProj/gWorld/gTime/gWaterTime, MyEngineSurface.hlsli) へ
// MyEngineSurfaceFrame/MyEnginePerObject の値を代入してから VSMain を呼ぶ。
// 速度エントリだけ前後 2 回評価し、2 つの VSOut から求めた clip 座標の差を velocity にする
// (spec §4.1「エンジン生成エントリ」)。

// ---- 色: Forward 不透明・透明、Deferred フォワード段の共通 ----
VSOut MyeVSColor(VSIn v)
{
    gViewProj = gMyeCurViewProj;
    gWorld = gMyeWorld;
    gTime = gMyeCurTime;
    gWaterTime = gMyeCurWaterTime;
    return VSMain(v);
}

float4 MyePSColor(VSOut i) : SV_Target
{
    return PSMain(i);
}

// ---- 影: CSM (ライトの ViewProj で評価。PS なし = 深度のみ) ----
VSOut MyeVSShadow(VSIn v)
{
    gViewProj = gMyeShadowViewProj;
    gWorld = gMyeWorld;
    gTime = gMyeCurTime;
    gWaterTime = gMyeCurWaterTime;
    return VSMain(v);
}

// ---- 速度: Deferred 不透明のみ。VSMain を前後 2 回評価する ----
// ★SV_Position は PS 側ではピクセル座標に化ける (deferred_gbuffer.hlsl と同じ注意)。
//   velocity の計算にはクリップ座標の複製 (myeCurClip/myePrevClip) を使うこと
struct MyeVelocityOut
{
    VSOut myeInner; // pos (SV_Position) を含む作者の可変を丸ごと運ぶ
    float4 myeCurClip : MYECURCLIP;
    float4 myePrevClip : MYEPREVCLIP;
};

MyeVelocityOut MyeVSVelocity(VSIn v)
{
    gViewProj = gMyeCurViewProj;
    gWorld = gMyeWorld;
    gTime = gMyeCurTime;
    gWaterTime = gMyeCurWaterTime;
    const VSOut curOut = VSMain(v);

    gViewProj = gMyePrevViewProj;
    gWorld = gMyePrevWorld;
    gTime = gMyePrevTime;
    gWaterTime = gMyePrevWaterTime;
    const VSOut prevOut = VSMain(v);

    MyeVelocityOut o;
    o.myeInner = curOut;
    o.myeCurClip = curOut.pos;
    o.myePrevClip = prevOut.pos;
    return o;
}

void MyePSVelocity(MyeVelocityOut i, out float4 myeColorOut : SV_Target0,
                   out float2 myeVelocityOut : SV_Target1)
{
    myeColorOut = PSMain(i.myeInner);
    myeVelocityOut = (gMyeHistoryValid != 0)
        ? ComputeVelocityUv(i.myeCurClip, i.myePrevClip, gMyeJitterNdc)
        : float2(0.0f, 0.0f);
}
