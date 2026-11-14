//! mye_track — AI Change Tracking の追跡サービスのロジック。
//! 層: project / hash (純粋) → schema / db / blob / journal → store → startup / writer。
//! 設計: docs/ai-change-tracking/ (db-schema.md が保存形式の正本)。

pub mod blob;
pub mod db;
pub mod hash;
pub mod journal;
pub mod process;
pub mod project;
pub mod schema;
pub mod startup;
pub mod store;
pub mod writer;
