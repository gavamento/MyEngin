//! 変更の収集 (監視スレッドの中で動く)。パスの集まりを受け取り、ディスクの今の状態を
//! ベースラインと比べて観測を作り、Writer へ渡す。`notify` には依存しない。
//!
//! 判定はイベントの種別や件数ではなく、flush の時点のファイルの存在と内容のハッシュで決める
//! (prototype-results.md 試作 3 の結論 3、4、5、6)。

use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant};

use crate::baseline::{classify, Baseline, ContentKind};
use crate::content::{peek_len, read_stable, LenOutcome, ReadOutcome};
use crate::db::now_ms;
use crate::hash::sha256_hex;
use crate::ignore::IgnoreRules;
use crate::observe::{
    apply_observations, record_baseline, AfterContent, BaselineEntry, ChangeType, Completeness, Observation, Source,
};
use crate::writer::Writer;

/// 1 回の Writer への依頼に積む内容の上限。大量の変更でメモリを使い切らない。
const CHUNK_BYTES: usize = 64 * 1024 * 1024;
const CHUNK_COUNT: usize = 500;

/// 書き込みの塊を分けるイベントの間隔。1 回の保存のイベントは負荷で数十 ms 離れうるので広く取る (多めに付ける側)。
pub const DEFAULT_BURST_GAP: Duration = Duration::from_millis(200);

/// 追跡の設定。既定は architecture.md §4 の決定値。テストは短くできる。
#[derive(Clone, Debug)]
pub struct TrackerConfig {
    /// 最後のイベントからこれだけ静かになったら flush。
    pub debounce: Duration,
    /// 最初のイベントからこれを超えたら、続いていても flush。
    pub max_hold: Duration,
    /// これより離れたイベントは別の書き込みの塊として数える (`coalesced` の判定)。
    pub burst_gap: Duration,
    /// 他のプロセスが共有なしで開いていて読めないとき、やり直す間隔。
    pub busy_retry: Duration,
    /// 書き手が続いているかを見るサイズの確認の間隔。
    pub settle_wait: Duration,
    /// サイズが続けてこの回数同じなら、書き手は終わったとみなして読む (1 = 確認しない)。
    pub settle_samples: u32,
    /// 書き手が続いたままこれだけ待ったら、その時点の内容を読む (`coalesced`)。
    pub settle_give_up: Duration,
    /// 読み取りの前後でサイズ・更新時刻が違ったときの読み直しの上限。
    pub stable_attempts: u32,
    /// これを超える、または UTF-8 でないものは `binary`。
    pub max_text_bytes: u64,
}

impl Default for TrackerConfig {
    fn default() -> Self {
        TrackerConfig {
            debounce: Duration::from_millis(300),
            max_hold: Duration::from_secs(2),
            burst_gap: DEFAULT_BURST_GAP,
            busy_retry: Duration::from_millis(250),
            settle_wait: Duration::from_millis(100),
            settle_samples: 2,
            settle_give_up: Duration::from_secs(30),
            stable_attempts: 5,
            max_text_bytes: 8 * 1024 * 1024,
        }
    }
}

/// flush するパス 1 件。
pub struct DueItem {
    pub rel: String,
    /// 作成・削除・名前変更のイベントがあった (ディレクトリなら中身を突き合わせる)。
    pub structural: bool,
    /// 複数回の書き込みが 1 つにまとまった可能性が高い (`coalesced`)。
    pub multiple_writes: bool,
    /// 観測の `window`。近い時刻に届いたイベントのパスは同じ窓に入る。
    pub window: String,
    /// 同じディレクトリ内のリネームの旧パス (`Name(From)` の直後の `Name(To)`)。
    pub rename_from: Option<String>,
    /// 書き手が開いたままでも読む。
    pub give_up: bool,
}

#[derive(Clone, Default)]
struct Cand {
    multiple: bool,
    window: String,
    give_up: bool,
    rename_from: Option<String>,
}

enum Eval {
    Gone,
    Present { bytes: Vec<u8>, coalesced: bool },
}

/// 後でやり直すパスと、その待ち時間。
pub struct Deferred {
    pub rel: String,
    pub delay: Duration,
}

pub struct ScanStats {
    pub files: usize,
    pub bytes: u64,
    pub changes: usize,
}

pub struct Collector {
    root: PathBuf,
    cfg: TrackerConfig,
    writer: Arc<Writer>,
    ignore: IgnoreRules,
    pub baseline: Baseline,
    run_id: i64,
    window_seq: u64,
    /// 書き手が続いているか見ている最中のパス → (最後に見た大きさ, 続けて同じだった回数)。
    size_seen: HashMap<String, (u64, u32)>,
    /// 待っている間に大きさが変わった、または開かれていて読めなかったパス。途中の状態を見ていない。
    grew: HashSet<String>,
}

#[derive(Default)]
struct Batch {
    obs: Vec<Observation>,
    bytes: usize,
}

impl Collector {
    pub fn new(root: PathBuf, cfg: TrackerConfig, writer: Arc<Writer>) -> Self {
        Collector {
            ignore: IgnoreRules::new(root.clone()),
            root,
            cfg,
            writer,
            baseline: Baseline::default(),
            run_id: now_ms(),
            window_seq: 0,
            size_seen: HashMap::new(),
            grew: HashSet::new(),
        }
    }

    fn abs(&self, rel: &str) -> PathBuf {
        if rel.is_empty() { self.root.clone() } else { self.root.join(rel) }
    }

    /// 新しい観測の窓の印。
    pub fn new_window(&mut self) -> String {
        self.window_seq += 1;
        format!("{}:{}", self.run_id, self.window_seq)
    }

    fn read_item(&self, rel: &str) -> ReadOutcome {
        read_stable(&self.abs(rel), self.cfg.stable_attempts)
    }

    /// 書き手が続いていないか。サイズが `settle_samples` 回続けて同じになれば真 (読んでよい)。
    /// 変わったパスは `grew` に入れ、そのあとの Revision を `coalesced` にする。
    fn settle(&mut self, rel: &str, len: u64) -> bool {
        match self.size_seen.get_mut(rel) {
            Some((seen, count)) if *seen == len => *count += 1,
            Some(entry) => {
                *entry = (len, 1);
                self.grew.insert(rel.to_string());
            }
            None => {
                self.size_seen.insert(rel.to_string(), (len, 1));
            }
        }
        let stable = self.size_seen[rel].1 >= self.cfg.settle_samples.max(1);
        if stable {
            self.size_seen.remove(rel);
        }
        stable
    }

    /// `dir_rel` の下のファイルを集める (無視を除く、シンボリックリンクは辿らない)。
    fn walk(&mut self, dir_rel: &str, out: &mut Vec<String>) {
        let Ok(entries) = std::fs::read_dir(self.abs(dir_rel)) else { return };
        let mut names: Vec<(String, bool)> = Vec::new();
        for entry in entries.flatten() {
            let Ok(file_type) = entry.file_type() else { continue };
            if file_type.is_symlink() {
                continue;
            }
            let Some(name) = entry.file_name().to_str().map(str::to_string) else { continue };
            names.push((name, file_type.is_dir()));
        }
        names.sort();
        for (name, is_dir) in names {
            let rel = if dir_rel.is_empty() { name } else { format!("{dir_rel}/{name}") };
            if self.ignore.is_ignored(&rel, is_dir) {
                continue;
            }
            if is_dir {
                self.walk(&rel, out);
            } else {
                out.push(rel);
            }
        }
    }

    fn decide(
        &self,
        rel: &str,
        eval: Eval,
        multiple: bool,
        source: Source,
        window: &str,
        observed_at: i64,
    ) -> Option<Observation> {
        let completeness = |no_before: bool, coalesced: bool| {
            if source.is_gap() {
                Completeness::Gap
            } else if no_before {
                Completeness::NoBefore
            } else if multiple || coalesced {
                Completeness::Coalesced
            } else {
                Completeness::Complete
            }
        };
        let known = self.baseline.files.get(rel);
        match eval {
            Eval::Present { bytes, coalesced } => {
                let hash = sha256_hex(&bytes);
                if known.is_some_and(|st| st.same_content(&hash, bytes.len() as u64)) {
                    return None;
                }
                let no_before = known.is_none() && self.baseline.unknown.contains(rel);
                let change_type = if known.is_some() || no_before { ChangeType::Modified } else { ChangeType::Added };
                let kind = classify(&bytes, self.cfg.max_text_bytes);
                Some(Observation {
                    path: rel.to_string(),
                    change_type,
                    old_path: None,
                    before: known.cloned(),
                    after: Some(AfterContent { bytes, hash, kind }),
                    completeness: completeness(no_before, coalesced),
                    source,
                    window: window.to_string(),
                    observed_at,
                })
            }
            Eval::Gone => known.map(|st| Observation {
                path: rel.to_string(),
                change_type: ChangeType::Deleted,
                old_path: None,
                before: Some(st.clone()),
                after: None,
                completeness: completeness(false, false),
                source,
                window: window.to_string(),
                observed_at,
            }),
        }
    }

    /// 溜まった観測を Writer へ渡し、記録できたものだけベースラインを進める。
    fn commit(&mut self, observations: Vec<Observation>) {
        if observations.is_empty() {
            return;
        }
        let meta: Vec<(String, ChangeType, Option<String>)> =
            observations.iter().map(|o| (o.path.clone(), o.change_type, o.old_path.clone())).collect();
        let results = match self.writer.call(move |store| apply_observations(store, observations)) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("[track] 観測を Writer へ渡せない: {e}");
                return;
            }
        };
        for ((path, change_type, old_path), result) in meta.into_iter().zip(results) {
            match result {
                Err(e) => eprintln!("[track] 観測を記録できない: {path}: {e}"),
                Ok(state) => {
                    self.baseline.unknown.remove(&path);
                    match change_type {
                        ChangeType::Deleted => {
                            self.baseline.files.remove(&path);
                        }
                        ChangeType::Added | ChangeType::Modified | ChangeType::Renamed => {
                            if let Some(old) = old_path {
                                self.baseline.files.remove(&old);
                            }
                            if let Some(state) = state {
                                self.baseline.files.insert(path, state);
                            }
                        }
                    }
                }
            }
        }
    }

    fn push(&mut self, batch: &mut Batch, o: Observation) {
        batch.bytes += o.after.as_ref().map_or(0, |a| a.bytes.len());
        batch.obs.push(o);
        if batch.bytes >= CHUNK_BYTES || batch.obs.len() >= CHUNK_COUNT {
            self.drain(batch);
        }
    }

    fn drain(&mut self, batch: &mut Batch) {
        let obs = std::mem::take(&mut batch.obs);
        batch.bytes = 0;
        self.commit(obs);
    }

    /// flush するパスを処理する。返り値は書き手がまだ開いていて読めなかったパス (後でやり直す)。
    /// `still_pending` はまだデバウンス中のパス。ディレクトリの突き合わせではそれを飛ばす (自分の番で処理する)。
    pub fn flush(&mut self, items: Vec<DueItem>, still_pending: &HashSet<String>) -> Vec<Deferred> {
        let observed_at = now_ms();
        let mut deferred: Vec<Deferred> = Vec::new();

        // `.gitignore` の変更を、同じ flush の判定より先に反映する
        for item in &items {
            if let Some(parent) = item.rel.strip_suffix(".gitignore") {
                if parent.is_empty() || parent.ends_with('/') {
                    self.ignore.reload(parent.trim_end_matches('/'));
                }
            }
        }

        let mut cands: BTreeMap<String, Cand> = BTreeMap::new();
        let mut dir_items: Vec<&DueItem> = Vec::new();
        for item in &items {
            let is_dir = std::fs::symlink_metadata(self.abs(&item.rel)).map(|m| m.is_dir()).unwrap_or(false);
            if is_dir {
                dir_items.push(item);
                continue;
            }
            cands.insert(
                item.rel.clone(),
                Cand {
                    multiple: item.multiple_writes,
                    window: item.window.clone(),
                    give_up: item.give_up,
                    rename_from: item.rename_from.clone(),
                },
            );
            // ディレクトリが消えた / ファイルに置き換わった: 下にあったファイルはなくなった
            for key in self.baseline.under(&item.rel) {
                if !still_pending.contains(&key) {
                    cands.entry(key).or_insert_with(|| Cand { window: item.window.clone(), ..Cand::default() });
                }
            }
            if let Some(from) = &item.rename_from {
                cands.entry(from.clone()).or_insert_with(|| Cand { window: item.window.clone(), ..Cand::default() });
            }
        }
        for item in dir_items {
            // ディレクトリ自身の Modify は捨てる。中身を突き合わせるのは作成・名前変更・削除のとき
            if !item.structural || self.ignore.is_ignored(&item.rel, true) {
                continue;
            }
            let mut files = Vec::new();
            self.walk(&item.rel, &mut files);
            files.extend(self.baseline.under(&item.rel));
            // ファイルがディレクトリに置き換わった
            if self.baseline.files.contains_key(&item.rel) {
                files.push(item.rel.clone());
            }
            for file in files {
                if !still_pending.contains(&file) {
                    cands.entry(file).or_insert_with(|| Cand { window: item.window.clone(), ..Cand::default() });
                }
            }
        }

        let mut batch = Batch::default();

        // 同じディレクトリ内のリネーム: 旧パスが消え、新パスが新しいファイルのときだけ 1 件にまとめる
        let pairs: Vec<(String, String)> =
            cands.iter().filter_map(|(to, c)| c.rename_from.clone().map(|from| (from, to.clone()))).collect();
        for (from, to) in pairs {
            let from_known = self.baseline.files.contains_key(&from);
            let to_known = self.baseline.files.contains_key(&to);
            let from_exists = std::fs::symlink_metadata(self.abs(&from)).is_ok();
            if !from_known || to_known || from_exists || self.ignore.is_ignored(&to, false) {
                continue;
            }
            let cand = cands.get(&to).cloned().unwrap_or_default();
            match self.read_item(&to) {
                ReadOutcome::Content { bytes, coalesced } => {
                    let before = self.baseline.files.get(&from).cloned();
                    let hash = sha256_hex(&bytes);
                    let kind = classify(&bytes, self.cfg.max_text_bytes);
                    let completeness = if cand.multiple || coalesced { Completeness::Coalesced } else { Completeness::Complete };
                    let o = Observation {
                        path: to.clone(),
                        change_type: ChangeType::Renamed,
                        old_path: Some(from.clone()),
                        before,
                        after: Some(AfterContent { bytes, hash, kind }),
                        completeness,
                        source: Source::Watch,
                        window: cand.window.clone(),
                        observed_at,
                    };
                    self.push(&mut batch, o);
                    cands.remove(&from);
                    cands.remove(&to);
                }
                ReadOutcome::Busy => {
                    self.grew.insert(to.clone());
                    deferred.push(Deferred { rel: to.clone(), delay: self.cfg.busy_retry });
                    cands.remove(&from);
                    cands.remove(&to);
                }
                _ => {}
            }
        }

        for (rel, cand) in cands {
            // 今は無視対象になったパス (.gitignore の変更など) は、記録せず見ない
            if self.ignore.is_ignored(&rel, false) {
                continue;
            }
            // 書き手が続いているかもしれない (開いたままのハンドルへ書き続けるファイル)。
            // サイズが続けて同じになるまで読まない。待ちの上限を超えたら、その時点の内容を coalesced で読む
            if !cand.give_up {
                match peek_len(&self.abs(&rel)) {
                    LenOutcome::Len(len) => {
                        if !self.settle(&rel, len) {
                            deferred.push(Deferred { rel, delay: self.cfg.settle_wait });
                            continue;
                        }
                    }
                    LenOutcome::Busy => {
                        self.grew.insert(rel.clone());
                        deferred.push(Deferred { rel, delay: self.cfg.busy_retry });
                        continue;
                    }
                    LenOutcome::Missing | LenOutcome::NotFile => {
                        self.size_seen.remove(&rel);
                    }
                    LenOutcome::Failed(e) => {
                        eprintln!("[track] 読めない: {rel}: {e}");
                        continue;
                    }
                }
            } else {
                self.size_seen.remove(&rel);
            }
            let eval = match self.read_item(&rel) {
                ReadOutcome::Missing | ReadOutcome::NotFile => Eval::Gone,
                ReadOutcome::Content { bytes, coalesced } => {
                    let was_growing = self.grew.remove(&rel);
                    Eval::Present { bytes, coalesced: coalesced || was_growing || cand.give_up }
                }
                ReadOutcome::Busy => {
                    self.grew.insert(rel.clone());
                    deferred.push(Deferred { rel, delay: self.cfg.busy_retry });
                    continue;
                }
                ReadOutcome::Failed(e) => {
                    eprintln!("[track] 読めない: {rel}: {e}");
                    continue;
                }
            };
            if let Some(o) = self.decide(&rel, eval, cand.multiple, Source::Watch, &cand.window, observed_at) {
                self.push(&mut batch, o);
            }
        }
        self.drain(&mut batch);
        deferred
    }

    /// ルート全体を走査してベースラインと比べ、違うファイルごとに `gap` の観測を作る。
    /// 起動時 (停止していた間) と、監視のあふれの後に使う。
    pub fn rescan(&mut self, source: Source) -> ScanStats {
        let observed_at = now_ms();
        let window = self.new_window();
        let mut files = Vec::new();
        self.walk("", &mut files);
        let present: HashSet<String> = files.iter().cloned().collect();
        let mut stats = ScanStats { files: files.len(), bytes: 0, changes: 0 };
        let mut batch = Batch::default();

        for rel in &files {
            let eval = match self.read_item(rel) {
                ReadOutcome::Content { bytes, coalesced } => {
                    stats.bytes += bytes.len() as u64;
                    Eval::Present { bytes, coalesced }
                }
                ReadOutcome::Missing | ReadOutcome::NotFile => continue,
                ReadOutcome::Busy | ReadOutcome::Failed(_) => {
                    if !self.baseline.files.contains_key(rel) {
                        self.baseline.unknown.insert(rel.clone());
                    }
                    continue;
                }
            };
            if let Some(o) = self.decide(rel, eval, false, source, &window, observed_at) {
                stats.changes += 1;
                self.push(&mut batch, o);
            }
        }
        let vanished: Vec<String> = self.baseline.files.keys().filter(|k| !present.contains(*k)).cloned().collect();
        for rel in vanished {
            if self.ignore.is_ignored(&rel, false) {
                // 今は無視対象 (.gitignore が変わった)。変更ではなく、追跡の外へ出ただけ
                self.baseline.files.remove(&rel);
                continue;
            }
            if let Some(o) = self.decide(&rel, Eval::Gone, false, source, &window, observed_at) {
                stats.changes += 1;
                self.push(&mut batch, o);
            }
        }
        self.drain(&mut batch);
        stats
    }

    /// 最初の起動: 追跡対象の全ファイルの内容ハッシュを取り、`baseline` のスナップショットに残す。
    pub fn scan_baseline(&mut self) -> ScanStats {
        let taken_at = now_ms();
        let mut files = Vec::new();
        self.walk("", &mut files);
        let mut stats = ScanStats { files: files.len(), bytes: 0, changes: 0 };
        let mut entries: Vec<BaselineEntry> = Vec::new();
        let mut entry_bytes = 0usize;
        for rel in &files {
            match self.read_item(rel) {
                ReadOutcome::Content { bytes, .. } => {
                    stats.bytes += bytes.len() as u64;
                    entry_bytes += bytes.len();
                    let hash = sha256_hex(&bytes);
                    let kind: ContentKind = classify(&bytes, self.cfg.max_text_bytes);
                    entries.push(BaselineEntry { path: rel.clone(), bytes, hash, kind });
                }
                ReadOutcome::Missing | ReadOutcome::NotFile => {}
                ReadOutcome::Busy | ReadOutcome::Failed(_) => {
                    self.baseline.unknown.insert(rel.clone());
                }
            }
            if entry_bytes >= CHUNK_BYTES || entries.len() >= CHUNK_COUNT {
                self.commit_baseline(std::mem::take(&mut entries), taken_at);
                entry_bytes = 0;
            }
        }
        self.commit_baseline(entries, taken_at);
        stats
    }

    fn commit_baseline(&mut self, entries: Vec<BaselineEntry>, taken_at: i64) {
        if entries.is_empty() {
            return;
        }
        let paths: Vec<String> = entries.iter().map(|e| e.path.clone()).collect();
        let results = match self.writer.call(move |store| record_baseline(store, entries, taken_at)) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("[track] ベースラインを Writer へ渡せない: {e}");
                return;
            }
        };
        for (path, result) in paths.into_iter().zip(results) {
            match result {
                Ok(state) => {
                    self.baseline.files.insert(path, state);
                }
                Err(e) => {
                    eprintln!("[track] ベースラインを記録できない: {path}: {e}");
                    self.baseline.unknown.insert(path);
                }
            }
        }
    }

    pub fn root(&self) -> &Path {
        &self.root
    }

    pub fn elapsed_since(start: Instant) -> u64 {
        start.elapsed().as_millis() as u64
    }
}
