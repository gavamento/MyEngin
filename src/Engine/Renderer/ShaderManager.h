#pragma once
#include <future>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"

namespace mye {

class GraphicsDevice;

// VS + PS を 1 つの .hlsl (エントリ VSMain / PSMain) からコンパイルしたプログラム
struct ShaderProgram {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    std::wstring path;                  // フルパス
    std::vector<std::wstring> includes; // 依存 .hlsli (M3 のリロード依存グラフ用)
    bool valid = false;
};

// assets/shaders/ からの実行時コンパイル (engine_spec.md 8.1 / 10 章)。
// コンパイルフラグは Debug/Release で同一 (描画結果の構成差を作らない)。
class ShaderManager {
public:
    bool Init(GraphicsDevice& device, std::wstring shaderDir);

    // "forward_lit" → {shaderDir}\forward_lit.hlsl をコンパイルしてキャッシュ。
    // 失敗しても ID は返す (Get で valid=false のプログラムが得られる)
    AssetID Load(std::string_view name);
    ShaderProgram* Get(AssetID id);

    // 同期再コンパイル。成功時のみ差し替え、失敗時は旧プログラム維持 + エラーログ
    bool Recompile(AssetID id);

    // ---- ホットリロード (engine_spec.md 8.1) ----
    // 変更ファイル (正規化パス) に依存する全プログラムの再コンパイルを
    // バックグラウンドで開始する。include 依存グラフ (ShaderProgram::includes) を辿る
    void RequestRecompileForFile(const std::wstring& normalizedPath);
    // フェーズ 2 で呼ぶ: 完了した非同期コンパイルを取り込み、成功分のみ差し替える
    void PollAsyncCompiles();

    const std::wstring& ShaderDir() const { return dir_; }

private:
    bool CompileProgram(const std::wstring& path, ShaderProgram& out);

    GraphicsDevice* device_ = nullptr;
    std::wstring dir_;
    std::unordered_map<uint64_t, ShaderProgram> programs_; // AssetID.value → program
    struct AsyncCompile {
        uint64_t id;
        std::future<ShaderProgram> future;
    };
    std::vector<AsyncCompile> async_;
};

} // namespace mye
