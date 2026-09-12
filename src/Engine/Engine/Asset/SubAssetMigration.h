#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mye {

struct RenderResources;

// ---- サブアセット ID の移行 (M74b) ----
// M74a でモデル由来サブアセットの登録名が "<正規化絶対パス>#mesh3#part0" から
// "guid://<16hex>#mesh3#part0" になり、AssetID (= 登録名の HashStr) も変わった。
// 旧 ID を持つ .scene.json / .prefab.json / .actor.json を新 ID へ書き換えるのがこのモジュール。
//
// 旧 ID は「そのファイルを保存したマシンの clone 先」が分からないと再計算できない (ハッシュは
// 逆算できない)。だから旧 clone 先のプロジェクトルートを明示的に受け取り、全モデルの新キーから
// 「そこに置かれていた場合の旧キー」を組み立てて対応表を作る。2 台が別のパスで同じシーンを
// 触っていた場合は、両方のルートを渡せば両方の旧 ID が 1 回の実行で揃う。
namespace subasset {

using IdMap = std::unordered_map<uint64_t, uint64_t>; // 旧 ID → 新 ID

// 新形式の登録名から旧形式の登録名を作る。key の先頭の "guid://<16hex>" と同じ綴りの出現を
// **すべて** legacyPrefix (正規化絶対パスの UTF-8) に置き換える — glTF の "#img:" は名前の無い
// 画像で材質キーを丸ごと埋め込むので、接頭辞が 1 回とは限らない。
// key が新形式でなければ空文字列
std::string LegacyKeyOf(std::string_view key, std::string_view legacyPrefix);

// resources に登録済みの新形式キー (メッシュ / マテリアル / スキン / テクスチャ) それぞれについて、
// そのモデルが legacyAssetsRoots のどれかの下の同じ相対位置に置かれていたときの旧 ID → 新 ID を
// out に足す。GUID → 現在パスは assetguid::ResolvePath (呼び出し側が resolver を Install 済み)。
// 現在パスが assetsRoot の外にあるモデルは相対位置が決まらないので飛ばす。戻り値 = 足した対応数
size_t AddLegacyMappings(const RenderResources& resources, const std::wstring& assetsRoot,
                         const std::vector<std::wstring>& legacyAssetsRoots, IdMap& out);

// JSON テキストの数値トークンのうち、map のキーに一致する非負整数だけを置き換える。
// ★テキストのまま置換する (パースして書き直さない) — エンジンの dump と外部ツール (Python) の
//   書き出しで浮動小数の綴りが違っても差分がその ID の桁だけに収まり、git の差分が読める。
// 文字列リテラルの中は触らない (名前に長い数字が入っていても誤爆しない)。戻り値 = 置換数
size_t RewriteIds(std::string& jsonText, const IdMap& map);

// --migrate-subasset-ids の本体。assetsRoot を走査して .meta を揃え、resolver を Install し、
// 全モデル (.fbx / .glb / .gltf) をヘッドレス登録して対応表を作り、*.scene.json /
// *.prefab.json / *.actor.json を書き換える (dryRun は数えるだけ)。
// legacyProjectRoots = 旧 clone 先のプロジェクトルート (assets の親)。**現在の clone 先は
// 自動で含める** (このマシンが保存した旧 ID)。戻り値 = プロセスの終了コード
// (0 = 成功 / 1 = 書き込み失敗 / 2 = assets ルートが無い)
int RunMigration(const std::wstring& assetsRoot, const std::vector<std::wstring>& legacyProjectRoots,
                 bool dryRun);

} // namespace subasset
} // namespace mye
