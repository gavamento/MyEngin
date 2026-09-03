#pragma once
#include <string>

namespace mye {

// 保存直後の Source Control ヒント (M66i)。
//
// 「今このファイルを保存した / 作った / 消した」をサービスへ伝えて status を
// 取り直させるための、**受け口 1 個だけの中継**。監視 (notify + 300 ms デバウンス)
// でも同じ結果に行き着くので、これが無くてもバッジは遅れて正しくなる — 速さのためだけの口。
//
// ★なぜ関数 1 本のグローバルなのか:
//   保存の実体は `AssetOps` の自由関数と 4 つの窓に散っていて、どれも
//   `SourceControlSession` を知らない。全部に引数を足すと、この機能とは
//   無関係な 10 箇所以上のシグネチャが「Collab のために」変わる。
//   逆にバッジ (読み取り) 側は Content Browser 1 つだけなので、
//   そちらは EditorApp が `SourceControlSession*` を明示的に渡している。
//   **書き手が散っているものは受け口を 1 つに、読み手が 1 つのものは明示的に渡す**。
namespace scmhint {

// 受け口の登録 (EditorApp が起動時に 1 回。nullptr 相当で解除)。
// ★登録より前・解除より後の Changed() は**黙って捨てる**。ヘッドレスの
//   セルフテスト (AssetOpsSelfTest が実ファイルを作る) から呼ばれても
//   何も起きない形にしておくこと
using SinkFn = void (*)(void* user, const std::wstring& absPath);
void SetSink(SinkFn fn, void* user);

// 保存 / 生成 / 削除の直後に絶対パスを渡す。リポジトリ外かどうかの判定は
// 受け口 (SourceControlSession::HintSaved) がやるので、呼び出し側は考えなくてよい
void Changed(const std::wstring& absPath);

} // namespace scmhint
} // namespace mye
