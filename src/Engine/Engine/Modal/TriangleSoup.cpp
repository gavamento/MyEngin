//====================================================================================
//                          TriangleSoup.cpp
//  MyEngine/ 秋田蓮音                                                      09/16/2026
//                                          OFF/OBJ の最小テキストリーダの実装
//====================================================================================
#include "Engine/Engine/Modal/TriangleSoup.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <sstream>

namespace mye {
namespace modal {
namespace {

bool IsSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

// 空白区切りのトークン列にする。'#' 以降は行末までコメントとして捨てる
// (OFF/OBJ 共通の規則。位置は行頭に限らない)
std::vector<std::string> Tokenize(const std::string& text)
{
    std::vector<std::string> tokens;
    std::string cur;
    bool inComment = false;
    for (char c : text) {
        if (c == '\n') {
            inComment = false;
        }
        if (inComment) {
            continue;
        }
        if (c == '#') {
            inComment = true;
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (IsSpace(c)) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        tokens.push_back(cur);
    }
    return tokens;
}

bool ParseDouble(const std::string& s, double& out)
{
    // std::from_chars の double 対応は環境によりまちまちなので strtod を使う
    // (このファイルは決定論の対象外 — オフラインツールの入力パースであり sim には無関係)
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0';
}

bool ParseLong(const std::string& s, long& out)
{
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

// "f" 行の 1 頂点参照 ("i" / "i/j" / "i/j/k") から先頭成分だけを取り出す
bool ParseFaceRef(const std::string& tok, long& indexOut)
{
    const size_t slash = tok.find('/');
    const std::string head = (slash == std::string::npos) ? tok : tok.substr(0, slash);
    return ParseLong(head, indexOut);
}

// ファン三角形分割: (poly[0], poly[k], poly[k+1]) を k=1..size-2 で積む
void FanTriangulate(const std::vector<uint32_t>& poly, std::vector<uint32_t>& indices)
{
    if (poly.size() < 3) {
        return;
    }
    for (size_t k = 1; k + 1 < poly.size(); ++k) {
        indices.push_back(poly[0]);
        indices.push_back(poly[k]);
        indices.push_back(poly[k + 1]);
    }
}

} // namespace

bool LoadOffText(const std::string& text, TriangleSoup& out, std::string* error)
{
    out.positions.clear();
    out.indices.clear();

    std::vector<std::string> tokens = Tokenize(text);
    if (tokens.empty()) {
        if (error) {
            *error = "empty OFF text";
        }
        return false;
    }

    size_t ti = 0;
    const std::string& first = tokens[0];
    if (first == "OFF" || first == "off") {
        ti = 1;
    } else if (first.size() > 3 && first.compare(0, 3, "OFF") == 0) {
        // ModelNet のクセ: "OFF8 12 0" のようにヘッダと最初の数字がくっついている。
        // ヘッダを剥がした残りをその数字トークンとして使い回す (ti は動かさない)
        tokens[0] = first.substr(3);
        ti = 0;
    } else {
        if (error) {
            *error = "missing OFF header";
        }
        return false;
    }

    if (ti + 3 > tokens.size()) {
        if (error) {
            *error = "truncated OFF header";
        }
        return false;
    }
    long nVerts = 0;
    long nFaces = 0;
    long nEdges = 0; // OFF の宣言に含まれるが未使用 (幾何には要らない)
    if (!ParseLong(tokens[ti], nVerts) || !ParseLong(tokens[ti + 1], nFaces)
        || !ParseLong(tokens[ti + 2], nEdges) || nVerts < 0 || nFaces < 0) {
        if (error) {
            *error = "invalid OFF counts";
        }
        return false;
    }
    ti += 3;

    out.positions.reserve(static_cast<size_t>(nVerts));
    for (long v = 0; v < nVerts; ++v) {
        if (ti + 3 > tokens.size()) {
            if (error) {
                *error = "truncated OFF vertex list";
            }
            return false;
        }
        double x = 0.0, y = 0.0, z = 0.0;
        if (!ParseDouble(tokens[ti], x) || !ParseDouble(tokens[ti + 1], y)
            || !ParseDouble(tokens[ti + 2], z)) {
            if (error) {
                *error = "invalid OFF vertex";
            }
            return false;
        }
        out.positions.push_back({ static_cast<float>(x), static_cast<float>(y),
                                  static_cast<float>(z) });
        ti += 3;
    }

    out.indices.reserve(static_cast<size_t>(nFaces) * 3);
    std::vector<uint32_t> poly;
    for (long f = 0; f < nFaces; ++f) {
        if (ti >= tokens.size()) {
            if (error) {
                *error = "truncated OFF face list";
            }
            return false;
        }
        long count = 0;
        if (!ParseLong(tokens[ti], count) || count < 3) {
            if (error) {
                *error = "invalid OFF face vertex count";
            }
            return false;
        }
        ++ti;
        if (ti + static_cast<size_t>(count) > tokens.size()) {
            if (error) {
                *error = "truncated OFF face";
            }
            return false;
        }
        poly.clear();
        poly.reserve(static_cast<size_t>(count));
        for (long k = 0; k < count; ++k) {
            long idx = 0;
            if (!ParseLong(tokens[ti], idx) || idx < 0
                || static_cast<size_t>(idx) >= out.positions.size()) {
                if (error) {
                    *error = "invalid OFF face index";
                }
                return false;
            }
            poly.push_back(static_cast<uint32_t>(idx));
            ++ti;
        }
        FanTriangulate(poly, out.indices);
    }

    return !out.positions.empty() && !out.indices.empty();
}

bool LoadObjText(const std::string& text, TriangleSoup& out, std::string* error)
{
    out.positions.clear();
    out.indices.clear();

    std::istringstream stream(text);
    std::string line;
    std::vector<uint32_t> poly;
    size_t lineNo = 0;
    while (std::getline(stream, line)) {
        ++lineNo;
        // 行コメント ('#' 以降) を落としてからトークン化
        const size_t hash = line.find('#');
        const std::string body = (hash == std::string::npos) ? line : line.substr(0, hash);
        std::istringstream ls(body);
        std::string tag;
        if (!(ls >> tag)) {
            continue; // 空行
        }
        if (tag == "v") {
            double x = 0.0, y = 0.0, z = 0.0;
            if (!(ls >> x >> y >> z)) {
                if (error) {
                    *error = "invalid OBJ vertex at line " + std::to_string(lineNo);
                }
                return false;
            }
            out.positions.push_back({ static_cast<float>(x), static_cast<float>(y),
                                      static_cast<float>(z) });
        } else if (tag == "f") {
            poly.clear();
            std::string tok;
            while (ls >> tok) {
                long raw = 0;
                if (!ParseFaceRef(tok, raw) || raw == 0) {
                    if (error) {
                        *error = "invalid OBJ face reference at line " + std::to_string(lineNo);
                    }
                    return false;
                }
                // OBJ は 1-based。負値はその時点の頂点数からの相対参照 (末尾から数える)
                const long resolved =
                    (raw < 0) ? static_cast<long>(out.positions.size()) + raw : raw - 1;
                if (resolved < 0 || static_cast<size_t>(resolved) >= out.positions.size()) {
                    if (error) {
                        *error = "OBJ face index out of range at line " + std::to_string(lineNo);
                    }
                    return false;
                }
                poly.push_back(static_cast<uint32_t>(resolved));
            }
            if (poly.size() < 3) {
                if (error) {
                    *error = "OBJ face needs at least 3 vertices at line " + std::to_string(lineNo);
                }
                return false;
            }
            FanTriangulate(poly, out.indices);
        }
        // v/f 以外 (vt, vn, o, g, mtllib, usemtl, s, ...) は無視 (最小リーダ)
    }

    return !out.positions.empty() && !out.indices.empty();
}

} // namespace modal
} // namespace mye
