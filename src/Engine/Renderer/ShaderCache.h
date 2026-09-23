//====================================================================================
//                          ShaderCache.h
//  MyEngin/ 秋田蓮音                                                       09/17/2026
//                                          シェーダのバイトコードキャッシュ (形式と鮮度判定)
//====================================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mye {

// コンパイル済みシェーダ 1 プログラム分のキャッシュ項目。
// ★鮮度は「タイムスタンプ」ではなく**中身のハッシュ**で判定する — git の checkout や
//   ZIP 展開で mtime は平気で巻き戻るので、mtime で見ると古いバイトコードを掴む。
// ★include は**要求名と解決先の両方**を持つ。シェーダは 2 ルート解決 (プロジェクト →
//   エンジン) なので、後からプロジェクト側に同名 .hlsli を置くと、中身が 1 バイトも
//   変わっていなくても「別のファイルを読むべき」状態になる。解決先を照合しないと
//   上書きが黙って無視される
struct ShaderCacheEntry {
    struct Dependency {
        std::string requestedName; // #include "xxx" の xxx (UTF-8)
        std::string resolvedPath;  // 解決先の正規化パス (UTF-8)
        uint64_t contentHash = 0;  // 解決先ファイルの中身の HashBytes
    };

    uint64_t configKey = 0;  // ターゲット / エントリ / フラグ / コンパイラ版 (ShaderCacheConfigKey)
    bool isCompute = false;
    // M79 sub-02: 色VS/PS・影VS・速度VS/PSの5本 (SurfaceProgram) を表す。isComputeと排他
    bool isSurface = false;
    uint64_t sourceHash = 0; // 本体 .hlsl の中身の HashBytes
    std::vector<Dependency> deps;
    // CS なら 1 本 (cs)、VS+PS なら 2 本 (vs, ps)、サーフェスなら 5 本
    // (色vs, 色ps, 影vs, 速度vs, 速度ps) の順
    std::vector<std::vector<uint8_t>> blobs;
};

// コンパイル条件を 1 つのキーへ畳む。フラグやコンパイラの版を変えたら自然に全項目が無効になる
uint64_t ShaderCacheConfigKey(bool isCompute, uint32_t compileFlags);
// M79 sub-02: サーフェスの 5 エントリ版 (ShaderCacheConfigKey とはエントリ名の集合が違うため別関数)
uint64_t SurfaceShaderCacheConfigKey(uint32_t compileFlags);

// キャッシュファイル名 (拡張子込み)。キーは解決済みのシェーダパスと CS/VS+PS の別
std::wstring ShaderCacheFileName(const std::wstring& normalizedShaderPath, bool isCompute);
// M79 sub-02: サーフェス版 (通常の VS+PS と同じパスでも衝突しないよう別の salt を使う)
std::wstring SurfaceShaderCacheFileName(const std::wstring& normalizedShaderPath);

// バイト列へ直列化する。末尾に全体のチェックサムを付ける (書きかけ・破損を読み手が弾けるように)
std::vector<uint8_t> EncodeShaderCacheEntry(const ShaderCacheEntry& entry);

// 直列化を読む。魔法数 / 版 / 長さ / チェックサムのどれかが合わなければ false
bool DecodeShaderCacheEntry(const std::vector<uint8_t>& bytes, ShaderCacheEntry& out);

} // namespace mye
