//====================================================================================
//                          KernelLint.h
//  MyEngin/ 秋田蓮音                                                       10/12/2026
//                                          カーネル (HLSL サブセット) の検査器
//====================================================================================
#pragma once
// *.kernel.hlsl が spec 4.1.2 のサブセットに収まっているかを検査する。違反は (ファイル名, 行, 規則 ID, 説明)。
// 違反のあるカーネルはどの実行先 (GPU / C++ / C#) でも使わない。サブセットの外は解析を広げず、
// 構文を読み切れない違反はその場で打ち切る (最初の 1 件だけ返す)。
//
// ★C++ コンパイラが黙って通す書き方 (1.0 の double リテラル、縮小変換、int と uint の混在、
//   符号付き整数の 0 除算など) を弾くのがこの検査器の役目。RW の添字が KernelId 以外なのは C++ 側でも C2280 になる。
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mye::kernel {

namespace lint {
struct KernelProgram;
}

enum class KernelRule : uint8_t {
    Syntax,          // KL001 サブセット外の構文
    Preprocessor,    // KL002 #include "kernel_common.hlsli" 以外の前処理
    UnsupportedType, // KL003 bool / double / half / 行列 / テクスチャなど
    Array,           // KL004 配列
    Swizzle,         // KL005 スウィズル・存在しないメンバ
    FloatLiteral,    // KL006 f の無い小数リテラル
    IntLiteral,      // KL007 範囲外・8 進の整数リテラル
    TypeMismatch,    // KL008 型の不一致・暗黙の縮小変換 (float->int, int<->uint)
    IntDivision,     // KL009 整数の / % は右辺が 0 でない整数リテラルのときだけ
    VectorCompare,   // KL010 ベクトルの比較・ベクトルの三項演算子
    Builtin,         // KL011 ホワイトリスト外の組込み関数・引数の型
    Attribute,       // KL012 属性 ([unroll] など)
    OutParam,        // KL013 out / inout / in 引数
    AssignInExpr,    // KL014 式の中の代入・++ --
    Reserved,        // KL015 予約語・組込み名の再利用
    RwIndex,         // KL016 RW の添字が id 以外
    ScalarMix,       // KL017 スカラーとベクトルの混在が許される 6 関数以外
    Shadow,          // KL018 遮蔽・再宣言
    UnknownName,     // KL019 未宣言の名前 (自分自身の呼び出し = 再帰を含む)
    KernelDecl,      // KL020 KERNEL / KERNEL_PARAMS の形・ファイル名との不一致
    Keyword,         // KL021 groupshared / barrier / Interlocked / switch など
    Shift,           // KL022 シフト量が 0..31 の整数リテラルではない・ベクトルのシフト
    MissingReturn,   // KL023 戻り値を返さない経路
    ReadOnly,        // KL024 RO / params / const への書き込み
    Condition,       // KL025 条件式が bool ではない
    Global,          // KL026 グローバル変数
    StructShape,     // KL027 struct の形 (入れ子・空・bool メンバ)
    Uninitialized,   // KL028 初期化子のない宣言
    Count
};

const char* KernelRuleId(KernelRule rule);   // "KL005"
const char* KernelRuleName(KernelRule rule); // "swizzle"

struct KernelDiagnostic {
    std::string file;
    int line = 0;
    KernelRule rule = KernelRule::Syntax;
    std::string message;
};

// "file(12): error KL005 [swizzle]: ..." (MSVC 風。エディタのログからジャンプできる形)
std::string FormatKernelDiagnostic(const KernelDiagnostic& d);

struct KernelLintResult {
    std::string kernelName; // ファイル名から ".kernel.hlsl" を除いたもの
    uint64_t sourceHash = 0; // HashKernelSource(text)
    std::vector<KernelDiagnostic> diagnostics;
    std::shared_ptr<const lint::KernelProgram> program; // 違反が無いときだけ入る

    bool Ok() const { return diagnostics.empty() && program != nullptr; }
};

// 1 本のカーネルを検査する。fileName は葉の名前 (例 "Foo.kernel.hlsl")
KernelLintResult LintKernelSource(std::string_view fileName, std::string_view text);

// カーネルごとの検査結果のキャッシュ。ソースのハッシュが変われば作り直す (ファイル変更で更新)
class KernelLintCache {
public:
    std::shared_ptr<const KernelLintResult> Get(const std::string& fileName, std::string_view text);
    void Clear();

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<const KernelLintResult>> entries_;
};

} // namespace mye::kernel
