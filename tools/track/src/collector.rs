//! 変更の収集 (監視スレッドの中で動く)。パスの集まりを受け取り、ディスクの今の状態を
//! ベースラインと比べて観測を作り、Writer へ渡す。`notify` には依存しない。
//!
//! 判定はイベントの種別や件数ではなく、flush の時点のファイルの存在と内容のハッシュで決める
//! (prototype-results.md 試作 3 の結論 3、4、5、6)。

use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};

use crate::baseline::{classify, Baseline, ContentKind};
use crate::blob::BlobStore;
use crate::content::{peek_len, peek_stat, read_stable, LenOutcome, ReadOutcome, StatOutcome};
use crate::db::now_ms;
use crate::diff::DiffLimits;
use crate::diff_worker::DiffWorker;
use crate::hash::sha256_hex;
use crate::ignore::IgnoreRules;
use crate::observe::{
    apply_observations, record_baseline, record_gap, AfterContent, BaselineEntry, ChangeType, Completeness, Observation,
    RecordFailure, Source, GAP_DISK_FULL, GAP_WRITE_FAILED,
};
use crate::rename::merge_renames;
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
    /// 検証の走査: 最後のイベントからこれだけ静かで、デバウンス中のパスが無いときに走らせる。
    pub verify_quiet: Duration,
    /// 検証の走査: 静かな時間が続いたとき、前回の走査からこれだけ空いたら走らせる。
    pub verify_interval: Duration,
    /// 検証の走査: 前回の走査の後に届いたイベントがこの件数に達したら、静かになった時点で走らせる。
    pub verify_burst_events: usize,
    /// 記録に失敗した観測がある間、書けるようになったかを確かめる間隔。
    pub unrecorded_retry: Duration,
    /// 試験用: ベースラインの塊 (500 件) ごとに待つ時間。途中で止める・途中で変更する試験が間に合うようにする。
    pub baseline_chunk_pause: Duration,
    /// 行 diff の計算量の上限 (超えたら Revision 全体を 1 Hunk にする)。
    pub diff_limits: DiffLimits,
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
            verify_quiet: Duration::from_secs(3),
            verify_interval: Duration::from_secs(5 * 60),
            verify_burst_events: 1000,
            unrecorded_retry: Duration::from_secs(1),
            baseline_chunk_pause: Duration::ZERO,
            diff_limits: DiffLimits::default(),
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
    Present { bytes: Vec<u8>, coalesced: bool, modified: Option<SystemTime> },
}

/// 後でやり直すパスと、その待ち時間。
pub struct Deferred {
    pub rel: String,
    pub delay: Duration,
    /// 元の観測の窓。待っている間、同じ窓の削除をリネームのまとめのために預かる。
    pub window: String,
}

/// ベースラインの所要時間の内訳 (ミリ秒)。`commit_ms` は Writer へ渡して戻るまで (Blob・ジャーナル・DB を含む)。
#[derive(Clone, Copy, Debug, Default)]
pub struct ScanTimings {
    pub walk_ms: u64,
    pub read_ms: u64,
    pub hash_ms: u64,
    pub commit_ms: u64,
}

pub struct ScanStats {
    pub files: usize,
    /// 読まずに済ませた数 (サイズと更新時刻が前回と同じ)。
    pub skipped: usize,
    pub bytes: u64,
    pub changes: usize,
    pub timings: ScanTimings,
    /// 止める合図で途中で打ち切ったら false。
    pub completed: bool,
}

impl ScanStats {
    fn new(files: usize) -> Self {
        ScanStats { files, skipped: 0, bytes: 0, changes: 0, timings: ScanTimings::default(), completed: true }
    }
}

/// 走査の進み具合。監視スレッドが書き、`Tracker::status` が読む。
#[derive(Default)]
pub struct ScanProgress {
    pub total: AtomicUsize,
    pub done: AtomicUsize,
}

/// 記録に失敗した観測の区間 (書けるようになったら `coverage_gaps` に残す)。
#[derive(Clone, Debug)]
pub struct Unrecorded {
    pub since_ms: i64,
    pub reason: &'static str,
    pub failures: u64,
    pub last_detail: String,
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
    stop: Arc<AtomicBool>,
    progress: Arc<ScanProgress>,
    unrecorded: Option<Unrecorded>,
    diff: DiffWorker,
    /// 同じ窓の作成を待っている削除 (`drain_holding`)。預かり始めた時刻つき。
    held: Vec<(Observation, Instant)>,
}

#[derive(Default)]
struct Batch {
    obs: Vec<Observation>,
    bytes: usize,
    /// 上限で途中の塊を書き出した。この flush の削除と作成は全体を見られないので、リネームにまとめない。
    overflowed: bool,
}

impl Collector {
    /// `blob_reader` は Diff ワーカーが Before を読むためのハンドル (`BlobStore::reader`)。
    pub fn new(
        root: PathBuf,
        cfg: TrackerConfig,
        writer: Arc<Writer>,
        stop: Arc<AtomicBool>,
        progress: Arc<ScanProgress>,
        blob_reader: BlobStore,
    ) -> Self {
        Collector {
            ignore: IgnoreRules::new(root.clone()),
            root,
            diff: DiffWorker::spawn(blob_reader, cfg.diff_limits.clone()),
            held: Vec::new(),
            cfg,
            writer,
            baseline: Baseline::default(),
            run_id: now_ms(),
            window_seq: 0,
            size_seen: HashMap::new(),
            grew: HashSet::new(),
            stop,
            progress,
            unrecorded: None,
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
            Eval::Present { bytes, coalesced, modified } => {
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
                    after: Some(AfterContent { bytes, hash, kind, modified }),
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
    /// 記録できなかったものは `unrecorded` に区間として持つ (ベースラインは進めないので、次の検証の走査が拾う)。
    fn commit(&mut self, observations: Vec<Observation>) {
        if observations.is_empty() {
            return;
        }
        let first_at = observations.iter().map(|o| o.observed_at).min().unwrap_or_else(now_ms);
        let meta: Vec<(String, ChangeType, Option<String>, i64)> =
            observations.iter().map(|o| (o.path.clone(), o.change_type, o.old_path.clone(), o.observed_at)).collect();
        // 差分は Diff ワーカーで計算してから Writer へ渡す。Writer は計算で塞がらない
        let (observations, plans) = self.diff.plan(observations);
        if observations.len() != meta.len() {
            eprintln!("[track] Diff ワーカーから観測が戻らない");
            self.note_unrecorded(first_at, &RecordFailure { reason: GAP_WRITE_FAILED, detail: "Diff ワーカーが観測を返さなかった".to_string() });
            return;
        }
        let results = match self.writer.call(move |store| apply_observations(store, observations, plans)) {
            Ok(r) => r,
            Err(e) => {
                eprintln!("[track] 観測を Writer へ渡せない: {e}");
                self.note_unrecorded(first_at, &RecordFailure { reason: GAP_WRITE_FAILED, detail: e.to_string() });
                return;
            }
        };
        for ((path, change_type, old_path, observed_at), result) in meta.into_iter().zip(results) {
            match result {
                Err(failure) => {
                    eprintln!("[track] 観測を記録できない: {path}: {}", failure.detail);
                    self.note_unrecorded(observed_at, &failure);
                }
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

    fn note_unrecorded(&mut self, at: i64, failure: &RecordFailure) {
        let entry = self.unrecorded.get_or_insert_with(|| Unrecorded {
            since_ms: at,
            reason: failure.reason,
            failures: 0,
            last_detail: String::new(),
        });
        entry.since_ms = entry.since_ms.min(at);
        entry.failures += 1;
        entry.last_detail = failure.detail.clone();
        // 理由が混ざったら、直す手段が違う disk_full を残す
        if failure.reason == GAP_DISK_FULL {
            entry.reason = GAP_DISK_FULL;
        }
    }

    /// 記録に失敗した観測の区間 (まだ `coverage_gaps` に残せていない)。
    pub fn unrecorded(&self) -> Option<&Unrecorded> {
        self.unrecorded.as_ref()
    }

    /// 記録に失敗した区間があれば、`coverage_gaps` へ書けるか試す。書けたら true (呼び出し側が検証の走査を行う)。
    pub fn retry_unrecorded(&mut self) -> bool {
        let Some(open) = self.unrecorded.clone() else { return false };
        let to = now_ms().max(open.since_ms);
        let wrote = self
            .writer
            .call(move |store| record_gap(store, open.reason, open.since_ms, to, None).is_ok())
            .unwrap_or(false);
        if wrote {
            self.unrecorded = None;
        }
        wrote
    }

    fn push(&mut self, batch: &mut Batch, o: Observation) {
        batch.bytes += o.after.as_ref().map_or(0, |a| a.bytes.len());
        batch.obs.push(o);
        if batch.bytes >= CHUNK_BYTES || batch.obs.len() >= CHUNK_COUNT {
            batch.overflowed = true;
            self.drain(batch);
        }
    }

    /// 溜まった観測を書き出す。リネームのまとめ (削除 + 同じ内容の作成) は、塊の境界で切れていない
    /// (= 上限で途中に書き出していない) ときだけ行う。切れると、候補が 1 つに決まるかを全体で判断できない。
    fn drain(&mut self, batch: &mut Batch) {
        let mut obs = std::mem::take(&mut batch.obs);
        batch.bytes = 0;
        if !batch.overflowed {
            obs = merge_renames(obs);
        }
        self.commit(obs);
    }

    /// `flush` 用の書き出し。同じ窓にまだ待ちのパス (新しいファイルは書き手を待つので遅れる) があるとき、
    /// 内容ハッシュを持つ削除は書き出さずに預かり、あとの `flush` で届く同じ内容の作成と `renamed` にまとめる。
    /// 預かりは窓が閉じる (待ちのパスが無くなる) か `max_hold` で終わる。
    fn drain_holding(&mut self, batch: &mut Batch, open_windows: &HashSet<String>) {
        if batch.overflowed {
            return self.drain(batch);
        }
        let incoming = std::mem::take(&mut batch.obs);
        batch.bytes = 0;
        let since_by_path: HashMap<String, Instant> =
            self.held.iter().map(|(o, since)| (o.path.clone(), *since)).collect();
        let mut all: Vec<Observation> = self.held.drain(..).map(|(o, _)| o).collect();
        // 預かっている間はベースラインが進まないので、別の flush が同じ削除をもう一度作る。重ねない
        all.extend(
            incoming.into_iter().filter(|o| !(o.change_type == ChangeType::Deleted && since_by_path.contains_key(&o.path))),
        );
        let now = Instant::now();
        let mut ready = Vec::new();
        for o in merge_renames(all) {
            let holdable = o.change_type == ChangeType::Deleted && o.before.as_ref().is_some_and(|s| s.hash.is_some());
            if holdable {
                let since = since_by_path.get(&o.path).copied().unwrap_or(now);
                if open_windows.contains(&o.window) && now.duration_since(since) < self.cfg.max_hold {
                    self.held.push((o, since));
                    continue;
                }
                // 預かっている間に同じパスが作り直された: そのパスの観測が先に決めるので、削除は捨てる
                if since_by_path.contains_key(&o.path) && self.abs(&o.path).symlink_metadata().is_ok() {
                    continue;
                }
            }
            ready.push(o);
        }
        self.commit(ready);
    }

    /// 預かっている削除を全部書き出す (走査の前・停止の前など、窓を待てないとき)。
    pub fn release_held(&mut self) {
        let held = std::mem::take(&mut self.held);
        let ready: Vec<Observation> =
            held.into_iter().map(|(o, _)| o).filter(|o| self.abs(&o.path).symlink_metadata().is_err()).collect();
        self.commit(ready);
    }

    /// flush するパスを処理する。返り値は書き手がまだ開いていて読めなかったパス (後でやり直す)。
    /// `still_pending` はまだデバウンス中のパス。ディレクトリの突き合わせではそれを飛ばす (自分の番で処理する)。
    /// `pending_windows` はそのパスの窓 (同じ窓の作成を待つ削除を預かる判断に使う)。
    pub fn flush(
        &mut self,
        items: Vec<DueItem>,
        still_pending: &HashSet<String>,
        pending_windows: &HashSet<String>,
    ) -> Vec<Deferred> {
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
                ReadOutcome::Content { bytes, coalesced, modified } => {
                    let before = self.baseline.files.get(&from).cloned();
                    let hash = sha256_hex(&bytes);
                    let kind = classify(&bytes, self.cfg.max_text_bytes);
                    let completeness = if cand.multiple || coalesced { Completeness::Coalesced } else { Completeness::Complete };
                    let o = Observation {
                        path: to.clone(),
                        change_type: ChangeType::Renamed,
                        old_path: Some(from.clone()),
                        before,
                        after: Some(AfterContent { bytes, hash, kind, modified }),
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
                    deferred.push(Deferred { rel: to.clone(), delay: self.cfg.busy_retry, window: cand.window.clone() });
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
                            deferred.push(Deferred { rel, delay: self.cfg.settle_wait, window: cand.window.clone() });
                            continue;
                        }
                    }
                    LenOutcome::Busy => {
                        self.grew.insert(rel.clone());
                        deferred.push(Deferred { rel, delay: self.cfg.busy_retry, window: cand.window.clone() });
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
                ReadOutcome::Content { bytes, coalesced, modified } => {
                    let was_growing = self.grew.remove(&rel);
                    Eval::Present { bytes, coalesced: coalesced || was_growing || cand.give_up, modified }
                }
                ReadOutcome::Busy => {
                    self.grew.insert(rel.clone());
                    deferred.push(Deferred { rel, delay: self.cfg.busy_retry, window: cand.window.clone() });
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
        let mut open_windows = pending_windows.clone();
        open_windows.extend(deferred.iter().map(|d| d.window.clone()));
        self.drain_holding(&mut batch, &open_windows);
        deferred
    }

    /// ルート全体を走査してベースラインと比べ、違うファイルごとに `gap` の観測を作る。
    /// 起動時 (停止していた間)、監視のあふれの後、検証の走査で使う。
    /// 前回の読み取りとサイズ・更新時刻が同じファイルは読まない (内容のハッシュは取らない)。
    /// 監視スレッドの中で動くので、走査が読んだ後に書かれたファイルのイベントは、走査の後で処理される。
    pub fn rescan(&mut self, source: Source) -> ScanStats {
        self.release_held();
        let observed_at = now_ms();
        let window = self.new_window();
        let mut files = Vec::new();
        self.walk("", &mut files);
        let present: HashSet<String> = files.iter().cloned().collect();
        let mut stats = ScanStats::new(files.len());
        let mut batch = Batch::default();
        self.progress.total.store(files.len(), Ordering::Relaxed);
        self.progress.done.store(0, Ordering::Relaxed);

        for (index, rel) in files.iter().enumerate() {
            if self.stop.load(Ordering::SeqCst) {
                // 途中までの走査では「消えたファイル」を決められない
                stats.completed = false;
                self.drain(&mut batch);
                return stats;
            }
            self.progress.done.store(index, Ordering::Relaxed);
            if let Some(st) = self.baseline.files.get(rel) {
                if let Some(known) = st.modified {
                    if let StatOutcome::Stat { len, modified } = peek_stat(&self.abs(rel)) {
                        if len == st.size && modified == Some(known) {
                            stats.skipped += 1;
                            stats.bytes += len;
                            continue;
                        }
                    }
                }
            }
            let (eval, modified) = match self.read_item(rel) {
                ReadOutcome::Content { bytes, coalesced, modified } => {
                    stats.bytes += bytes.len() as u64;
                    (Eval::Present { bytes, coalesced, modified }, modified)
                }
                ReadOutcome::Missing | ReadOutcome::NotFile => continue,
                ReadOutcome::Busy | ReadOutcome::Failed(_) => {
                    if !self.baseline.files.contains_key(rel) {
                        self.baseline.unknown.insert(rel.clone());
                    }
                    continue;
                }
            };
            match self.decide(rel, eval, false, source, &window, observed_at) {
                Some(o) => {
                    stats.changes += 1;
                    self.push(&mut batch, o);
                }
                // 内容は同じ: 次回から読まずに済むよう、更新時刻だけ覚える
                None => {
                    if let Some(st) = self.baseline.files.get_mut(rel) {
                        st.modified = modified;
                    }
                }
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
        self.progress.done.store(files.len(), Ordering::Relaxed);
        stats
    }

    /// 検証の走査の前半: 実際のディレクトリの中身とベースラインを、安い順 (存在 → サイズと更新時刻) に突き合わせ、
    /// 違うパスを返す。内容は読まず、観測も作らない (後半の `observe_paths` が、イベントが遅れて届く時間を
    /// 待ってから読む)。戻り値の bool は、走査を取り終えたか。
    pub fn detect_changed(&mut self) -> (Vec<String>, bool) {
        let mut files = Vec::new();
        self.walk("", &mut files);
        let present: HashSet<&String> = files.iter().collect();
        self.progress.total.store(files.len(), Ordering::Relaxed);
        self.progress.done.store(0, Ordering::Relaxed);
        let mut changed: Vec<String> = Vec::new();
        for (index, rel) in files.iter().enumerate() {
            if self.stop.load(Ordering::SeqCst) {
                return (changed, false);
            }
            self.progress.done.store(index, Ordering::Relaxed);
            let Some(st) = self.baseline.files.get(rel) else {
                changed.push(rel.clone());
                continue;
            };
            let Some(known) = st.modified else {
                changed.push(rel.clone());
                continue;
            };
            match peek_stat(&self.abs(rel)) {
                StatOutcome::Stat { len, modified } if len == st.size && modified == Some(known) => {}
                StatOutcome::Busy | StatOutcome::Failed(_) => {}
                _ => changed.push(rel.clone()),
            }
        }
        for rel in self.baseline.files.keys() {
            if !present.contains(rel) && !self.ignore.is_ignored(rel, false) {
                changed.push(rel.clone());
            }
        }
        self.progress.done.store(files.len(), Ordering::Relaxed);
        (changed, true)
    }

    /// 検証の走査の後半: 指定したパスの今の状態をベースラインと比べ、違うものを `gap` の観測にする。
    /// 返り値は作った観測の数。
    pub fn observe_paths(&mut self, paths: Vec<String>, source: Source) -> usize {
        self.release_held();
        let observed_at = now_ms();
        let window = self.new_window();
        let mut batch = Batch::default();
        let mut changes = 0;
        for rel in paths {
            if self.ignore.is_ignored(&rel, false) {
                continue;
            }
            let (eval, modified) = match self.read_item(&rel) {
                ReadOutcome::Content { bytes, coalesced, modified } => (Eval::Present { bytes, coalesced, modified }, modified),
                ReadOutcome::Missing | ReadOutcome::NotFile => (Eval::Gone, None),
                ReadOutcome::Busy | ReadOutcome::Failed(_) => continue,
            };
            match self.decide(&rel, eval, false, source, &window, observed_at) {
                Some(o) => {
                    changes += 1;
                    self.push(&mut batch, o);
                }
                None => {
                    if let Some(st) = self.baseline.files.get_mut(&rel) {
                        st.modified = modified.or(st.modified);
                    }
                }
            }
        }
        self.drain(&mut batch);
        changes
    }

    /// 最初の起動: 追跡対象の全ファイルの内容ハッシュを取り、`baseline` のスナップショットに残す。
    /// 止める合図で途中で打ち切ったら `completed = false` (取り終えていないので、次の起動でやり直す)。
    pub fn scan_baseline(&mut self) -> ScanStats {
        let taken_at = now_ms();
        let started = Instant::now();
        let mut files = Vec::new();
        self.walk("", &mut files);
        let mut stats = ScanStats::new(files.len());
        stats.timings.walk_ms = Self::elapsed_since(started);
        self.progress.total.store(files.len(), Ordering::Relaxed);
        self.progress.done.store(0, Ordering::Relaxed);
        let mut entries: Vec<BaselineEntry> = Vec::new();
        let mut entry_bytes = 0usize;
        // 小さい区間の積み上げはマイクロ秒で足し、最後にミリ秒へ直す
        let (mut read_us, mut hash_us, mut commit_us) = (0u64, 0u64, 0u64);
        for (index, rel) in files.iter().enumerate() {
            if self.stop.load(Ordering::SeqCst) {
                stats.completed = false;
                break;
            }
            let read_started = Instant::now();
            let outcome = self.read_item(rel);
            read_us += Self::elapsed_since_micros(read_started);
            match outcome {
                ReadOutcome::Content { bytes, modified, .. } => {
                    stats.bytes += bytes.len() as u64;
                    entry_bytes += bytes.len();
                    let hash_started = Instant::now();
                    let hash = sha256_hex(&bytes);
                    let kind: ContentKind = classify(&bytes, self.cfg.max_text_bytes);
                    hash_us += Self::elapsed_since_micros(hash_started);
                    entries.push(BaselineEntry { path: rel.clone(), bytes, hash, kind, modified });
                }
                ReadOutcome::Missing | ReadOutcome::NotFile => {}
                ReadOutcome::Busy | ReadOutcome::Failed(_) => {
                    self.baseline.unknown.insert(rel.clone());
                }
            }
            if entry_bytes >= CHUNK_BYTES || entries.len() >= CHUNK_COUNT {
                let commit_started = Instant::now();
                self.commit_baseline(std::mem::take(&mut entries), taken_at);
                commit_us += Self::elapsed_since_micros(commit_started);
                entry_bytes = 0;
                self.progress.done.store(index + 1, Ordering::Relaxed);
                if !self.cfg.baseline_chunk_pause.is_zero() {
                    std::thread::sleep(self.cfg.baseline_chunk_pause);
                }
            }
        }
        if stats.completed {
            let commit_started = Instant::now();
            self.commit_baseline(entries, taken_at);
            commit_us += Self::elapsed_since_micros(commit_started);
            self.progress.done.store(files.len(), Ordering::Relaxed);
        }
        stats.timings.read_ms = read_us / 1000;
        stats.timings.hash_ms = hash_us / 1000;
        stats.timings.commit_ms = commit_us / 1000;
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

    fn elapsed_since_micros(start: Instant) -> u64 {
        start.elapsed().as_micros() as u64
    }
}
