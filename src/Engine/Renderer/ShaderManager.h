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

// VS + PS (エントリ VSMain / PSMain) または CS (エントリ CSMain) のプログラム
struct ShaderProgram {
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> inputLayout;
    std::wstring path;                  // フルパス (正規化済み)
    std::vector<std::wstring> includes; // 依存 .hlsli (M3 のリロード依存グラフ用)
    bool isCompute = false;
    bool valid = false;
};

// assets/shaders/ からの実行時コンパイル (engine_spec.md 8.1 / 10 章)。
// コンパイルフラグは Debug/Release で同一 (描画結果の構成差を作らない)。
//
// シェーダルートは優先度順の複数持ちにする: [<project>\assets\shaders,
// <engineRepo>\assets\shaders]。プロジェクトに同名を置けばエンジン組込みを上書きでき、
// 置かなければエンジン側が使われる。これによりエンジンに機能を足しても
// 既存プロジェクトが古いシェーダのまま取り残されない (単一ルート時代の障害)
class ShaderManager {
public:
    // shaderDirs は優先度順 (先頭が最優先)。空要素・重複は呼び出し側で除いておくこと
    bool Init(GraphicsDevice& device, std::vector<std::wstring> shaderDirs);

    // "forward_lit" → 各ルートを順に見て最初に見つかった forward_lit.hlsl をコンパイル。
    // 失敗しても ID は返す (Get で valid=false のプログラムが得られる)
    AssetID Load(std::string_view name);
    // "particle_sim.cs" → particle_sim.cs.hlsl (エントリ CSMain)
    AssetID LoadCompute(std::string_view name);
    ShaderProgram* Get(AssetID id);

    // 同期再コンパイル。成功時のみ差し替え、失敗時は旧プログラム維持 + エラーログ
    bool Recompile(AssetID id);

    // ---- ホットリロード (engine_spec.md 8.1) ----
    // 変更ファイル (正規化パス) に依存する全プログラムの再コンパイルを
    // バックグラウンドで開始する。include 依存グラフ (ShaderProgram::includes) を辿る
    void RequestRecompileForFile(const std::wstring& normalizedPath);
    // フェーズ 2 で呼ぶ: 完了した非同期コンパイルを取り込み、成功分のみ差し替える
    void PollAsyncCompiles();

    const std::vector<std::wstring>& ShaderDirs() const { return dirs_; }

private:
    bool CompileProgram(const std::wstring& path, ShaderProgram& out); // out.isCompute を見て分岐
    // "<name>.hlsl" を各ルートで探す。見つからなければ最優先ルート上のパスを返す
    // (ホットリロード監視の照合キーになるので必ず非空を返す)
    std::wstring ResolvePath(std::string_view name) const;
    // 上位ルートが下位ルート (エンジン組込み) を隠している箇所を警告する
    void ReportShadowedBuiltins() const;

    GraphicsDevice* device_ = nullptr;
    std::vector<std::wstring> dirs_;
    std::unordered_map<uint64_t, ShaderProgram> programs_; // AssetID.value → program
    struct AsyncCompile {
        uint64_t id;
        std::future<ShaderProgram> future;
    };
    std::vector<AsyncCompile> async_;
};

} // namespace mye
