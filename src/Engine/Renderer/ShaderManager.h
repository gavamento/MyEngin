#pragma once
#include <atomic>
#include <future>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

#include "Engine/Core/EntityID.h"
#include "Engine/Renderer/SurfaceProgram.h"

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
    // v21 (M78e): CS バイトコード保持 — ComputeAbiRunner の D3DReflect ベース名前バインドに使用
    std::vector<uint8_t> csBytecode;
    bool isCompute = false;
    bool valid = false;
};

// assets/shaders/ および assets 全域の *.post.hlsl / *.cs.hlsl からの実行時コンパイル (engine_spec.md 8.1 / 10 章)。
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

    // M79: "Foo.surface" → Foo.surface.hlsl。作者の VSMain/PSMain と
    // MyEngineSurfaceEntries.hlsli の生成エントリ (色/速度/影) を束ねてコンパイルする。
    // Init 前 (ヘッドレス) は ID だけ予約する (Load/LoadCompute と同じ規則)
    AssetID LoadSurface(std::string_view name);
    SurfaceProgram* GetSurface(AssetID id);

    // 同期再コンパイル。成功時のみ差し替え、失敗時は旧プログラム維持 + エラーログ
    bool Recompile(AssetID id);

    // ---- ホットリロード (engine_spec.md 8.1) ----
    // 変更ファイル (正規化パス) に依存する全プログラムの再コンパイルを
    // バックグラウンドで開始する。include 依存グラフ (ShaderProgram::includes) を辿る
    void RequestRecompileForFile(const std::wstring& normalizedPath);
    // フェーズ 2 で呼ぶ: 完了した非同期コンパイルを取り込み、成功分のみ差し替える
    void PollAsyncCompiles();

    const std::vector<std::wstring>& ShaderDirs() const { return dirs_; }

    // プロジェクト assets 内の *.post.hlsl / *.cs.hlsl 索引 (M78: 短名は assets 内で一意)
    void SetAssetsRoot(std::wstring root);
    void RebuildProjectShaderIndex();
    // Load 解決と同じ規則 (索引 → shaderDirs)。SelfTest / 診断用
    std::wstring ResolveShaderPath(std::string_view name) const;

    // ---- バイトコードキャッシュ ----
    // dir にコンパイル済みバイトコードを置き、次回は中身のハッシュが一致すれば D3DCompile を
    // 飛ばす (RT の CS 9 本で起動が 6.6 秒止まっていたため)。enabled=false または dir 空で
    // 無効 = 毎回コンパイル (--no-shader-cache)。**バイトコードはコンパイル結果そのもの**なので
    // キャッシュの有無で描画結果は変わらない。Load/LoadCompute より前に呼ぶこと
    void SetCacheDir(std::wstring dir, bool enabled);
    int CacheHits() const { return cacheHits_.load(); }
    int CacheMisses() const { return cacheMisses_.load(); }

private:
    bool CompileProgram(const std::wstring& path, ShaderProgram& out); // out.isCompute を見て分岐
    // M79: 作者ソース + 生成エントリ (MyEngineSurfaceEntries.hlsli) を 5 エントリ
    // (色 VS/PS・影 VS・速度 VS/PS) 個別コンパイルし、リフレクション表と入力レイアウトまで作る
    bool CompileSurfaceProgram(const std::wstring& path, SurfaceProgram& out);
    // キャッシュから読めたら out を完成させて true。鮮度が合わない / 無い / 壊れていれば false
    bool TryLoadCached(const std::wstring& path, const std::vector<char>& source,
                       ShaderProgram& out);
    // バイトコード (CS 1 本 or VS+PS 2 本) から D3D オブジェクトを作る。キャッシュと
    // コンパイル直後で同じ関数を通す = 「キャッシュ経由だけ入力レイアウトが違う」を作らない
    bool Instantiate(const std::string& pathUtf8,
                     const std::vector<std::vector<uint8_t>>& blobs, ShaderProgram& out);
    // #include "name" を優先度順のルートで解決する (IncludeRecorder と同じ規則)。
    // 見つからなければ空文字列
    std::wstring ResolveInclude(const char* name, std::vector<char>* outData) const;
    // "<name>.hlsl" を各ルートで探す。見つからなければ最優先ルート上のパスを返す
    // (ホットリロード監視の照合キーになるので必ず非空を返す)
    std::wstring ResolvePath(std::string_view name) const;
    // 上位ルートが下位ルート (エンジン組込み) を隠している箇所を警告する
    void ReportShadowedBuiltins() const;

    GraphicsDevice* device_ = nullptr;
    std::vector<std::wstring> dirs_;
    std::wstring assetsRoot_;
    std::unordered_map<std::string, std::wstring> projectShaders_; // 短名 → 正規化パス
    std::unordered_map<uint64_t, ShaderProgram> programs_; // AssetID.value → program
    std::unordered_map<uint64_t, SurfaceProgram> surfacePrograms_; // M79: AssetID.value → program
    struct AsyncCompile {
        uint64_t id;
        std::future<ShaderProgram> future;
    };
    std::vector<AsyncCompile> async_;
    std::wstring cacheDir_; // 空 = キャッシュ無効
    // ホットリロードの非同期コンパイルからも数えるので atomic
    std::atomic<int> cacheHits_{ 0 };
    std::atomic<int> cacheMisses_{ 0 };
};

} // namespace mye
