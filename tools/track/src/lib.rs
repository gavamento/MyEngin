//! mye_track — AI Change Tracking の追跡サービスのロジック。
//! 層: project / hash / ignore / content (純粋寄り) → schema / db / blob / journal → store → startup / writer
//!     → baseline / observe / collector → watch (監視スレッドと Tracker)
//!     → protocol / pipe / ops → server (pipe のサービス) / client。
//! 設計: docs/ai-change-tracking/ (db-schema.md が保存形式の正本)。

pub mod baseline;
pub mod bench;
pub mod blob;
pub mod client;
pub mod collector;
pub mod content;
pub mod db;
pub mod diff;
pub mod diff_worker;
pub mod hash;
pub mod ignore;
pub mod journal;
pub mod observe;
pub mod ops;
pub mod pipe;
pub mod process;
pub mod project;
pub mod protocol;
pub mod rename;
pub mod revision;
pub mod schema;
pub mod server;
pub mod startup;
pub mod store;
pub mod watch;
pub mod writer;
