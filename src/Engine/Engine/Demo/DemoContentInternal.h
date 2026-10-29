#pragma once
// DemoContent*.cpp (M93g で DemoContent.cpp を領域ごとの翻訳単位へ分けたもの) だけが include する内部ヘッダ。
// 複数のショーケースが共有する小さなヘルパーの宣言を置く。定義は DemoContent.cpp。
// 公開契約 (Editor / Runtime が呼ぶ Register* / Build*) は DemoContent.h のまま
#include "Engine/Core/Ecs/EntityID.h" // EntityID / AssetID

namespace mye {

class World;

// name のスクリプトコンポーネントが ComponentRegistry に登録されていれば e へ付ける。
// GameLogic.dll が焼けていない (型が無い) ときは何もしない = シーンの組み立ては止まらない
void AttachScriptIfRegistered(World& w, EntityID e, const char* name);

// 名前で .physmat を引く (プリセットは起動走査で登録済み)。未登録なら未割当の AssetID。
// 絶対パスのハッシュを組まない理由は定義側のコメント (M59a1 の申し送り 1)
AssetID FindPhysMat(const char* name);

} // namespace mye
