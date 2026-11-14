//! スキーマの移行スクリプト置き場。正本は同じディレクトリの `v*.sql` (db-schema.md §3、§8)。

/// 1 つの移行。`sql` は複数文を含んでよい。
#[derive(Clone, Copy, Debug)]
pub struct Migration {
    pub version: i64,
    pub note: &'static str,
    pub sql: &'static str,
}

/// 前へ進むだけの移行の一覧 (版の昇順)。
pub const MIGRATIONS: &[Migration] = &[Migration {
    version: 1,
    note: "初版: 14 表と索引",
    sql: include_str!("v1.sql"),
}];

/// このビルドが知っている最新のスキーマ版。
pub fn latest_version() -> i64 {
    MIGRATIONS.last().map_or(0, |m| m.version)
}
