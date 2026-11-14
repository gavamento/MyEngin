//! mye_track — AI Change Tracking の追跡サービスのロジック。
//! 層: project / hash / ignore / content (純粋寄り) → schema / db / blob / journal → store → startup / writer
//!     → baseline / observe / collector → watch (監視スレッドと Tracker)。
//! 設計: docs/ai-change-tracking/ (db-schema.md が保存形式の正本)。

pub mod baseline;
pub mod bench;
pub mod blob;
pub mod collector;
pub mod content;
pub mod db;
pub mod hash;
pub mod ignore;
pub mod journal;
pub mod observe;
pub mod process;
pub mod project;
pub mod schema;
pub mod startup;
pub mod store;
pub mod watch;
pub mod writer;
