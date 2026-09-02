#pragma once

namespace mye {

// Source Control (M66) のヘッドレス回帰テスト。
//
// M66a で見るのは 2 点:
//   (a) CollabClient の配線 — 偽の応答行 / 通知行を流して、id 付きはコールバックへ、
//       event は購読者へ届くこと。**DLL が無くても回る**唯一の部分
//   (b) MyeCollab.dll との実往復 — LoadLibrary -> create -> hello -> poll -> destroy。
//       DLL が無ければ SKIP して true (rustup 未導入の環境で selftest 全体を
//       赤くしない)。ただし環境変数 MYE_COLLAB_REQUIRED=1 のときは失敗にする
//       — CI では「Rust をビルドし忘れて静かに素通り」が起きてはいけない
//
// M66b で足したもの (すべて DLL 不要 = 純関数の検査):
//   (c1) 偽の status トランスクリプト -> 対の束ね (.meta / .terrain.edit) と合成状態
//   (d)  フォルダ集約 (子の状態から親の状態)
//   (e1) repo_check の toplevel 不一致 -> ToplevelMismatch
//   (i)  ProjectManifest の canonicalRoot 往復 (一時ディレクトリで Save -> Load)
//   おまけ: op の待ち方の分類 (読み取り系だけがタイムアウトする / OpInFlight は書き込み系だけ)
bool RunSourceControlSelfTest();

} // namespace mye
