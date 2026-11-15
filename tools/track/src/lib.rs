//! mye_track — AI Change Tracking の追跡サービスのロジック。
//! 層: project / hash / ignore / content (純粋寄り) → schema / db / blob / journal → store → startup / writer
//!     → baseline / observe / debounce (監視の経験則。時刻は引数) / collector → watch_plan (監視の分け方) → watch (監視スレッドと Tracker)
//!     → declared_edit / gitref / attribution / session / operation (帰属と寿命)
//!     → review (レビューの状態) → restore (復元の記録) → protocol / pipe / ops / ops_record / notify → server (pipe のサービス) / client。
//! 設計: docs/ai-change-tracking/ (db-schema.md が保存形式の正本)。

pub mod attribution;
pub mod baseline;
pub mod bench;
pub mod blob;
pub mod client;
pub mod collector;
pub mod content;
pub mod db;
pub mod debounce;
pub mod declared_edit;
pub mod diff;
pub mod diff_worker;
pub mod document;
pub mod gitref;
pub mod hash;
pub mod hook;
pub mod ignore;
pub mod journal;
pub mod launch;
pub mod lexjson;
pub mod notify;
pub mod observe;
pub mod operation;
pub mod ops;
pub mod ops_record;
pub mod pipe;
pub mod process;
pub mod project;
pub mod protocol;
pub mod random;
pub mod rename;
pub mod restore;
pub mod review;
pub mod revision;
pub mod scene_diff;
pub mod scene_overlay;
pub mod schema;
pub mod server;
pub mod session;
pub mod startup;
pub mod store;
pub mod watch;
pub mod watch_plan;
pub mod writer;
