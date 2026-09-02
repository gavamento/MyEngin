#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Editor/SourceControl/SourceControlState.h"

namespace mye {

// 対の規則 (M66c、spec §4.1「対の規則 (決定 7 の確定版)」)。
//
// 何を解いているか: `.meta` は本体を保存すると必ず一緒に動く。片方だけ commit すると
// **他人のマシンで GUID が変わる** (`.meta` が無いアセットは path ハッシュから
// 作り直されるため) = シーンの参照が静かに壊れる。だから「本体を stage する」は
// 常に「本体 + 実在するサイドカー」を意味しなければならない。
//
// 一覧の束ね (どの行にどのサイドカーがぶら下がるか) は SourceControlState の
// PrimaryPathFor / BuildModel が既にやっている。ここはその**逆写像**で、
// 「この行を stage するとき git に何を渡すか」だけを決める。
// ★束ね方を 2 箇所で書かない — 食い違うと「一覧では 1 行なのに commit されるのは
//   片方だけ」という、画面を見ても気付けない壊れ方をする。

namespace pairrule {

// Collect の結果。
struct PairPlan {
    // git へ渡すパス (toplevel 相対 '/' 区切り、昇順・重複なし)。
    // ★昇順に固定するのは、同じ選択が常に同じコマンドラインになるようにするため
    //   (期待 NDJSON と実機の再現性の両方が順序に依存する)
    std::vector<std::string> toStage;
    // stage する前に AssetDatabase::EnsureMeta を呼ぶべき**本体**のパス。
    // 「資産なのに .meta がまだ無い」= エディタ外で置かれた新規アセット。
    // ★呼び出し側は必ず toEnsureMeta を先に処理すること。toStage には
    //   生成後の `<path>.meta` が既に入っている (無いまま git へ渡すと
    //   pathspec エラーになる)
    std::vector<std::string> toEnsureMeta;
};

// パスが実在するか (toplevel 相対)。**注入するのはテストのため** —
// 実ファイルを置かないと検査できない形にすると、`.meta` 欠落や
// `.terrain.edit` 同居のような条件が永久に未検査で残る
using PathExistsFn = std::function<bool(const std::string&)>;

// 本体パス -> サイドカーの綴り候補 (実在は見ない)。**純関数**。
//   "x.png"           -> { "x.png.meta" }
//   "x.terrain.json"  -> { "x.terrain.json.meta", "x.terrain.edit", "x.terrain.edit.meta" }
// ★PrimaryPathFor (.terrain.edit -> .terrain.json) の逆。".json" を ".edit" に
//   差し替える形であって、末尾に足すのではない (TerrainEdit.cpp の EditPathFor が正本)
std::vector<std::string> SidecarCandidates(const std::string& primary);

// 拡張子から見て「エンジンが .meta を持たせる資産」か (AssetDatabase::ClassifyPath)。
// `.terrain.edit` や `.txt` は Unknown = false
bool IsAssetPath(const std::string& path);

// 選択された行 -> stage 計画。
// 入れるのは「status に出ている」か「ディスクに実在する」パスだけ
// (どちらでもないものを渡すと git が pathspec エラーで**選択ごと**失敗する)
PairPlan Collect(const std::vector<PairedEntry>& rows, const PathExistsFn& exists);

// unstage 用: status に出ているパスだけを集める。
// ★ディスクを見ない / EnsureMeta もしない — unstage は index を戻すだけの操作で、
//   その途中で**ファイルを新しく作る**のは明らかに筋が違う
std::vector<std::string> ListedPaths(const std::vector<PairedEntry>& rows);

} // namespace pairrule

} // namespace mye
