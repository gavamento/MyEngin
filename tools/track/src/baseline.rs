//! ベースライン: 「最後に確認できた内容」の表 (architecture.md §4)。
//!
//! 監視スレッドがメモリに持つ。起動時は、DB の `snapshots` / `change_revisions` から前回の終わりの状態を戻す。

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::time::SystemTime;

use rusqlite::Connection;

/// 内容の種類 (architecture.md §4 の「追跡する種類」)。
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ContentKind {
    Text,
    /// UTF-8 として読めない、または上限を超える。ファイル単位の記録だけ。
    Binary,
}

impl ContentKind {
    pub fn as_str(self) -> &'static str {
        match self {
            ContentKind::Text => "text",
            ContentKind::Binary => "binary",
        }
    }
}

impl ContentKind {
    pub fn parse(text: &str) -> Option<ContentKind> {
        match text {
            "text" => Some(ContentKind::Text),
            "binary" => Some(ContentKind::Binary),
            _ => None,
        }
    }
}

/// 8 MiB (設定可) 以下の UTF-8 ならテキスト。
pub fn classify(bytes: &[u8], max_text_bytes: u64) -> ContentKind {
    if bytes.len() as u64 > max_text_bytes || std::str::from_utf8(bytes).is_err() {
        ContentKind::Binary
    } else {
        ContentKind::Text
    }
}

/// 1 ファイルの最後に確認できた内容。
#[derive(Clone, Debug)]
pub struct FileState {
    /// 内容の SHA-256。除外パスでも、このプロセスが読んだものはメモリにだけ持つ。
    /// DB から戻した除外パスは None (DB に除外ファイルのハッシュは残さない)。
    pub hash: Option<String>,
    pub size: u64,
    pub snapshot_id: Option<i64>,
    /// Blob のハッシュ。除外パスと Blob の無いものは None。
    pub blob_hash: Option<String>,
    /// DB から戻したものは分からない。
    pub kind: Option<ContentKind>,
    /// 読んだときの更新時刻。検証の走査が、サイズと並べて「変わっていない」と判断するのに使う。DB から戻したものは None (必ず内容を読む)。
    pub modified: Option<SystemTime>,
}

impl FileState {
    /// 内容が同じか。ハッシュを持たない状態 (DB から戻した除外パス) は大きさで比べる。
    pub fn same_content(&self, hash: &str, size: u64) -> bool {
        match &self.hash {
            Some(h) => h == hash && self.size == size,
            None => self.size == size,
        }
    }
}

#[derive(Default)]
pub struct Baseline {
    /// ルートからの相対パス ('/' 区切り) → 状態。走査順を決定的にするため BTreeMap。
    pub files: BTreeMap<String, FileState>,
    /// 追跡対象だが内容を読めなかった (共有なしで開かれている等)。次に読めたとき `no_before`。
    pub unknown: BTreeSet<String>,
}

impl Baseline {
    /// `dir` の下 (再帰) にあるファイルのパス。
    pub fn under(&self, dir: &str) -> Vec<String> {
        let prefix = format!("{dir}/");
        self.files
            .range(prefix.clone()..)
            .take_while(|(k, _)| k.starts_with(&prefix))
            .map(|(k, _)| k.clone())
            .collect()
    }
}

/// DB から戻した前回の終わりの状態。
pub struct LoadedState {
    pub files: BTreeMap<String, FileState>,
    /// 前回までにベースラインを取り終えている (`baseline_complete` のイベントがある)。
    pub had_baseline: bool,
    /// 直前のイベントの時刻 (前回の `service_stop`、無ければ最後の観測)。
    pub last_event_at: Option<i64>,
}

pub fn load_from_db(conn: &Connection) -> rusqlite::Result<LoadedState> {
    let had_baseline: bool =
        conn.query_row("SELECT EXISTS(SELECT 1 FROM events WHERE kind = 'baseline_complete')", [], |r| r.get(0))?;
    let last_event_at: Option<i64> = conn
        .query_row("SELECT at FROM events ORDER BY event_id DESC LIMIT 1", [], |r| r.get(0))
        .ok();

    // 削除された (またはリネームで去った) パスと、そのとき Before だったスナップショット
    let mut gone_at: HashMap<String, i64> = HashMap::new();
    {
        let mut stmt = conn.prepare(
            "SELECT c.path, r.change_type, r.old_path, r.before_snapshot
               FROM change_revisions r JOIN changes c ON c.change_id = r.change_id
              WHERE r.change_type IN ('deleted', 'renamed') AND r.before_snapshot IS NOT NULL",
        )?;
        let rows = stmt.query_map([], |r| {
            Ok((r.get::<_, String>(0)?, r.get::<_, String>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, i64>(3)?))
        })?;
        for row in rows {
            let (path, change_type, old_path, before) = row?;
            let gone_path = if change_type == "renamed" { old_path.unwrap_or_default() } else { path };
            let slot = gone_at.entry(gone_path).or_insert(before);
            *slot = (*slot).max(before);
        }
    }

    let mut files = BTreeMap::new();
    let mut stmt = conn.prepare(
        "SELECT s.path, s.snapshot_id, s.blob_hash, s.size
           FROM snapshots s
           JOIN (SELECT path, MAX(snapshot_id) AS id FROM snapshots GROUP BY path) m ON s.snapshot_id = m.id",
    )?;
    let rows = stmt.query_map([], |r| {
        Ok((r.get::<_, String>(0)?, r.get::<_, i64>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, i64>(3)?))
    })?;
    for row in rows {
        let (path, snapshot_id, blob_hash, size) = row?;
        if gone_at.get(&path).is_some_and(|&b| b >= snapshot_id) {
            continue;
        }
        files.insert(
            path,
            FileState { hash: blob_hash.clone(), size: size as u64, snapshot_id: Some(snapshot_id), blob_hash, kind: None, modified: None },
        );
    }
    Ok(LoadedState { files, had_baseline, last_event_at })
}
