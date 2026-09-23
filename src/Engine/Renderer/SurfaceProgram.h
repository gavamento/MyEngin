//====================================================================================
//                          SurfaceProgram.h
//  MyEngin/ 秋田蓮音                                                     09/24/2026
//                                          プロジェクト側サーフェスシェーダーの束
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <d3dcommon.h>
#include <wrl/client.h>

namespace mye {

// D3DReflect で引いたリソース (cbuffer / Texture / Sampler) 1 件の位置
struct SurfaceReflectedResource {
    uint32_t bindSlot = 0;
    D3D_SHADER_INPUT_TYPE type = D3D_SIT_CBUFFER;
};

// cbuffer 内の変数 1 件 (MyEnginePerMaterial の各プロパティ、予約 CB の各フィールド)
struct SurfaceReflectedVar {
    uint32_t cbufBindSlot = 0;
    uint32_t cbufSize = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

// 生成エントリ 1 本 (VS 単体 or PS 単体) のリフレクション結果。名前は D3DReflect が返す
// ものそのまま (予約 CB・予約テクスチャ/サンプラ・作者の MyEnginePerMaterial・作者の Texture2D
// を区別せず同じ表に積む — 解決は名前だけで行う契約のため)
struct SurfaceEntryReflection {
    std::unordered_map<std::string, SurfaceReflectedResource> resources;
    std::unordered_map<std::string, SurfaceReflectedVar> vars;
};

// *.surface.hlsl 1 本から生成した「色・速度・影」3 系統のプログラム。
// 作者は VSMain/PSMain だけを書き、この構造体は MyEngineSurfaceEntries.hlsli が包んだ
// 生成エントリ (MyeVSColor 等) をコンパイルした結果を持つ。
// speed (velocity) は Deferred 不透明専用、shadow は CSM 専用 (PS を持たない、深度のみ)
struct SurfaceProgram {
    // 色: Forward 不透明・透明、Deferred フォワード段の共通
    Microsoft::WRL::ComPtr<ID3D11VertexShader> colorVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> colorPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> colorInputLayout;
    SurfaceEntryReflection colorVSReflect;
    SurfaceEntryReflection colorPSReflect;

    // 速度: Deferred 不透明のみ (前後 2 回評価。VS 出力は MyeVelocityOut)
    Microsoft::WRL::ComPtr<ID3D11VertexShader> velocityVS;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> velocityPS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> velocityInputLayout;
    SurfaceEntryReflection velocityVSReflect;
    SurfaceEntryReflection velocityPSReflect;

    // 影: CSM 専用 (PS なし。深度のみ)
    Microsoft::WRL::ComPtr<ID3D11VertexShader> shadowVS;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> shadowInputLayout;
    SurfaceEntryReflection shadowVSReflect;

    std::wstring path;                  // フルパス (正規化済み)
    std::vector<std::wstring> includes; // #include 依存 (ホットリロード用)
    std::string errorMessage;           // 失敗時のエラー文 ("サーフェス規約: ..." を含む)
    bool valid = false;
};

} // namespace mye
