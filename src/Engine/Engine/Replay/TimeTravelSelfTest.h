#pragma once

namespace mye {

// タイムトラベルのリング (M52e) の自己テスト。
// 検証するのは**リングの方針**: 分岐時の切り捨て / 追い出しと範囲の縮小 /
// 最寄りスナップショット探索 / ポーズ tick を撮影間隔に数えないこと。
// 「戻して再シムしたら元のハッシュに戻る」の実データ検証は --timetravel-selftest の担当
bool RunTimeTravelSelfTest();

} // namespace mye
