#pragma once
#include <string>

namespace mye {

// 「窓が Save で書き出すはずの中身」と「ディスクの現物」を比べる (M66d、spec §4.1 の S6)。
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
bool TextDiffersFromDisk(const std::wstring& path, const std::string& inMemory);

} // namespace mye
