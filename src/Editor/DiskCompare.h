#pragma once
#include <string>

namespace mye {

// 「窓が Save で書き出すはずの中身」と「ディスクの現物」を比べる (M66d、spec §4.1 の S6)。
//
// ★M66h で DocumentDirty.h から改名した。`DocumentDirty` は GitTransaction.h 側にある
//   **別の型** (ゲートが集める未保存の集計) で、同じ名前のファイルが隣にあると
//   「この関数はあの構造体を作るのだろう」と読まれてしまう。ここはもっと狭い —
//   与えられた文字列とファイルを比べるだけ。
//
// なぜ差分比較か: Animation / Animator / Audio Mixer の 3 窓は編集のたびに
// **メモリ上のアセットを直接書き換え**、Save ボタンでだけ書き出す。dirty フラグを
// 後から足すには編集点が多すぎる (どれか 1 箇所を漏らすと「未保存なのに保存済みに
// 見える」= git の書き込みで黙って消える) ので、フラグを持たずに毎回照合する。
// 評価が高い代わりに呼ぶのは GitTransaction のゲートだけ + 500 ms キャッシュ。
//
// 比較は**改行と末尾の空白を無視する**。ControllerLibrary::SaveToFile は
// ofstream をテキストモードで開くので、同じ JSON でもディスク側だけ CRLF になる
// (ここを厳密比較にすると .controller.json が常に未保存扱いになる)。
//
// ファイルが無いときは「メモリ側が非空なら未保存」。これは「Save で初めて
// ファイルができる」窓 (Animation / Animator / Mixer = アセットが実在する側) では
// 正しいが、**直列化器が常に非空を返す**種類の文書では必ず未保存になってしまう
// (review-1 #2)。そういう相手は下の 3 引数版を使う。
bool TextDiffersFromDisk(const std::wstring& path, const std::string& inMemory);

// ファイルが無いときの比較相手 (`whenMissing`) を明示する版 (M66k、spec §4.1)。
//
// ★未保存とは「今 git に working tree を書き換えられたら失われる**ユーザーの編集**が
//   ある」ことであって「ディスクとバイト一致しない」ことではない。ファイルを一度も
//   保存していない文書は、「何も読み込まなかったときの状態」を同じ直列化器へ通した
//   文字列と一致する限り**未保存ではない**。
//   実害: `InputActions::ToJsonText()` は定義 0 件でも `{"actions": [], "axes": []}` を
//   返すので、2 引数版だと `assets\input\actions.json` を持たないプロジェクト
//   (= 新規作成したものは全部) で Project Settings 窓を 1 度開くだけで
//   `GateBlocker::ProjectSettingsDirty` が立ちっぱなしになり、revert / checkout /
//   pull / abort / continue が恒久的に塞がる。
//   `PhysicsLayerNames::DiffersFromDisk` が同じ罠を「ディスクを読み直した表と
//   比べる」ことで避けているのと同じ考え方 (`PhysicsLayerNames.h`)。
bool TextDiffersFromDisk(const std::wstring& path, const std::string& inMemory,
                         const std::string& whenMissing);

} // namespace mye
