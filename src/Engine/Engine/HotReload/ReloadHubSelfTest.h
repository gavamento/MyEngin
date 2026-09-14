#pragma once

namespace mye {

// ReloadHub の資産の種類表 (ReloadKindOf / ReloadRank) の回帰テスト。
// パスごとに「どの読み直しへ振り分けるか」と「一括適用の順位」を固定する
// (ファイルもライブラリも要らない)
bool RunReloadHubSelfTest();

} // namespace mye
