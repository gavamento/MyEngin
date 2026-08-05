#pragma once
#include <string>
#include <string_view>

#include "Engine/Core/EntityID.h"

namespace mye {

class World;

// NameComponent.value に収まる最大バイト数 (終端 NUL を除く)。
inline constexpr size_t kMaxEntityNameBytes = 63;

// エンティティ名を書き込む。**末尾までゼロ埋めしてから設定する** —
// NameComponent.value[64] は WorldHasher が 64 バイトを生で読むため、NUL 以降のバイトも
// 決定論的でなければならない (ImGui の InputText は残骸バイトを消さないので、リネーム確定時は
// 文字列が変わっていなくても必ずここを通すこと)。63 バイトを超える分は切り捨てる。
void SetEntityName(World& world, EntityID e, std::string_view name);

// parent の子 (parent == kNullEntity ならルート兄弟) の中で desired と衝突しない名前を返す。
// 規則は nameutil::MakeUniqueNumbered (アセットのファイル名連番と共通)。
// exclude は衝突判定から除外する — 経路によって意味が逆転するので**必ず明示的に渡すこと**:
//   - 生成前に名前を決める経路 (CreateMenu) は対象がまだ存在しないので kNullEntity
//   - 生成後に改名する経路 (複製 / 貼り付け / アセット配置 / リネーム) は対象自身を渡す
//     (渡さないと自分自身と衝突して、名前を変えていないのに " (1)" が付く)
//
// ★**エディタ操作からのみ呼ぶこと**。シーンロード / ランタイム生成 / Undo 復元の経路で呼ぶと
//   NameComponent が変わる = WorldHash が変わり、既存シーンとリプレイの決定論が壊れる。
//   「兄弟名の一意」は不変条件ではなく、エディタでの生成・改名時点の保証にすぎない
//   (D&D による再親子化や Undo/Redo では重複が生じうる。それは仕様)。
std::string MakeUniqueSiblingName(World& world, EntityID parent, std::string_view desired,
                                  EntityID exclude);

// エンティティ名として不正な文字を落とし、前後の空白を除いた名前を返す。
// 現在の禁止文字は '/' (部位パス "a/b/c" の区切り、M48f の FindPart が使う)。
// 結果が空になった場合は fallback を返す。
std::string SanitizeEntityName(std::string_view desired, std::string_view fallback);

// リネーム UI の確定処理 (M48b): 禁止文字除去 + 前後空白トリム + 兄弟内一意化 + ゼロ埋め書き戻し。
// **編集中ではなく確定時 (IsItemDeactivated / IsItemDeactivatedAfterEdit) に 1 回だけ呼ぶこと** —
// 毎フレーム呼ぶと "Cube" を打つ途中の "C"/"Cu" が衝突判定されて目的の名前を打ち切れなくなる。
//
// ★original は編集開始時点の名前。edited == original なら**正規化も一意化も行わない**
//   (ゼロ埋めの書き直しだけする)。ImGui は Esc キャンセル時にバッファを元の名前へ戻すが
//   確定判定は真になるため、比較しないと「Esc で取り消したのに兄弟の同名と衝突して
//   ' (1)' が付く」「昔から '/' を含む名前が F2+Esc だけで改名される」事故になる。
//   edited / original は NameComponent のバッファ自身を指していてもよい (先にコピーする)。
void FinishRename(World& world, EntityID e, std::string_view edited, std::string_view original);

} // namespace mye
