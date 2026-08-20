#pragma once

namespace mye {

// 画像比較 (M52c: スクリーンショット回帰の判定本体) のヘッドレス回帰テスト。
// GPU 不要 — 一時ディレクトリへ PNG を書いて CompareImageFiles を通す。
// 「golden を 1 画素改竄したら赤になる」を機械で保証するのがこのテストの役目
bool RunImageDiffSelfTest();

} // namespace mye
