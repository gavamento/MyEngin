#pragma once

namespace mye {

// スキーマ由来の動的コンポーネント登録 (M48j) の回帰テスト。
// 一時ディレクトリに .component.schema.json を書いて登録し、
// 「登録順の固定 / 拒否すべきスキーマ / レイアウト / 既定値 / 保存往復 / ハッシュ被覆」を見る
bool RunSchemaSelfTest();

} // namespace mye
