//! 監視の経験則 (デバウンス、書き込みの塊、書き手の待ちの打ち切り、検証の走査の間隔) の判定。
//!
//! 時刻は `Instant` の引数で受け取り、ここでは読まない。監視スレッドは実時間を渡し、試験は
//! 基準の `Instant` に `Duration` を足した値を渡す (architecture.md §4「時刻の口」)。
//! 負荷で 1 回の保存の間隔が開けば判定が変わるのは仕様なので、その境界はここの単体試験で決定的に確かめる。

use std::time::{Duration, Instant};

use crate::collector::TrackerConfig;

/// デバウンス中の 1 パス。
pub struct Pending {
    pub first: Instant,
    pub last_event: Instant,
    /// 書き込みの塊の数 (`burst_gap` より離れたイベントで区切る)。
    pub bursts: u32,
    /// 内容の変更を表す `Modify` の件数。1 回の書き込みは最大 2 件 (切り詰めと書き込み)。
    pub writes: u32,
    pub structural: bool,
    /// 待ち直す前のイベントで、複数回の書き込みと判断していた。
    pub carried_multiple: bool,
    /// 書き手が続いているかもしれない (または共有なしで開かれている) ので、この時刻にやり直す。
    pub retry_at: Option<Instant>,
    pub busy_since: Option<Instant>,
    pub rename_from: Option<String>,
    pub rename_to: Option<String>,
    pub window: String,
}

impl Pending {
    pub fn new(now: Instant, window: String) -> Self {
        Pending {
            window,
            first: now,
            last_event: now,
            bursts: 1,
            writes: 0,
            structural: false,
            carried_multiple: false,
            retry_at: None,
            busy_since: None,
            rename_from: None,
            rename_to: None,
        }
    }

    /// 待ち直すパス: 判断 (`multiple`) と待ちの始まり (`busy_since`) を引き継ぎ、`retry_at` にやり直す。
    pub fn deferred(now: Instant, window: String, multiple: bool, busy_since: Instant, retry_at: Instant) -> Self {
        let mut pending = Pending::new(now, window);
        pending.carried_multiple = multiple;
        pending.retry_at = Some(retry_at);
        pending.busy_since = Some(busy_since);
        pending
    }

    /// イベントが届いた。`burst_gap` より離れていれば別の塊として数える。
    pub fn touch(&mut self, now: Instant, burst_gap: Duration, structural: bool, write: bool) {
        if now.saturating_duration_since(self.last_event) > burst_gap {
            self.bursts += 1;
        }
        self.last_event = now;
        self.writes += u32::from(write);
        self.structural |= structural;
        self.retry_at = None;
    }

    /// 待っている間に新しいイベントが来ていたパスへ、待ち直しの判断を引き継ぐ。
    pub fn carry_over(&mut self, multiple: bool, busy_since: Instant) {
        self.carried_multiple |= multiple;
        self.busy_since = self.busy_since.or(Some(busy_since));
    }

    /// 複数回の書き込みが 1 つにまとまった可能性が高い (`coalesced`): 塊が 2 つ以上、`Modify` が 3 件以上、
    /// または待ち直す前に既にそう判断していた。
    pub fn is_multiple(&self) -> bool {
        self.carried_multiple || self.bursts > 1 || self.writes > 2
    }

    pub fn due(&self, now: Instant, cfg: &TrackerConfig) -> bool {
        if let Some(retry) = self.retry_at {
            return now >= retry;
        }
        now.duration_since(self.last_event) >= cfg.debounce || now.duration_since(self.first) >= cfg.max_hold
    }

    /// 書き手を待ち始めてから `settle_give_up` 以上たった (その時点の内容を `coalesced` で読む)。
    pub fn waited_too_long(&self, now: Instant, settle_give_up: Duration) -> bool {
        self.busy_since.is_some_and(|since| now.duration_since(since) >= settle_give_up)
    }
}

/// 前のイベントから `debounce` を超えて空いたら、観測の窓を新しくする。
pub fn window_expired(now: Instant, last_event: Instant, debounce: Duration) -> bool {
    now.saturating_duration_since(last_event) > debounce
}

/// リネームの相手の作成を待つために削除を預かり続けるか。窓が開いていて、預かってから `max_hold` 未満の間だけ真。
pub fn keep_holding(window_open: bool, since: Instant, now: Instant, max_hold: Duration) -> bool {
    window_open && now.duration_since(since) < max_hold
}

/// 検証の走査を行う時か。デバウンス中のパスが無く、最後のイベントから `verify_quiet` 以上静かで、
/// 要望・イベントの件数・前回からの間隔のどれかが満たされたとき。
pub fn verify_due(
    wanted: bool,
    events_since_verify: usize,
    pending_is_empty: bool,
    quiet_for: Duration,
    since_last_verify: Duration,
    cfg: &TrackerConfig,
) -> bool {
    if !pending_is_empty || quiet_for < cfg.verify_quiet {
        return false;
    }
    wanted || events_since_verify >= cfg.verify_burst_events || since_last_verify >= cfg.verify_interval
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: Duration = Duration::from_millis(1);

    fn cfg() -> TrackerConfig {
        TrackerConfig::default()
    }

    fn ms(n: u64) -> Duration {
        MS * n as u32
    }

    /// 1 回の保存 (切り詰めと書き込みの 2 件) を `at` に起こす。
    fn save(pending: &mut Pending, at: Instant) {
        pending.touch(at, cfg().burst_gap, false, true);
        pending.touch(at, cfg().burst_gap, false, true);
    }

    #[test]
    fn one_save_with_two_events_at_the_same_instant_is_not_multiple() {
        let t0 = Instant::now();
        let mut p = Pending::new(t0, "w".into());
        save(&mut p, t0);
        assert_eq!((p.bursts, p.writes), (1, 2));
        assert!(!p.is_multiple());
    }

    #[test]
    fn burst_gap_boundary_exactly_the_gap_is_one_burst_and_one_nanosecond_more_is_two() {
        let gap = cfg().burst_gap;
        let t0 = Instant::now();
        let mut exact = Pending::new(t0, "w".into());
        exact.touch(t0, gap, false, false);
        exact.touch(t0 + gap, gap, false, false);
        assert_eq!(exact.bursts, 1);
        assert!(!exact.is_multiple());

        let mut over = Pending::new(t0, "w".into());
        over.touch(t0, gap, false, false);
        over.touch(t0 + gap + Duration::from_nanos(1), gap, false, false);
        assert_eq!(over.bursts, 2);
        assert!(over.is_multiple());
    }

    #[test]
    fn three_or_more_modifies_are_multiple_even_in_one_burst() {
        let t0 = Instant::now();
        let mut p = Pending::new(t0, "w".into());
        save(&mut p, t0);
        assert!(!p.is_multiple());
        p.touch(t0, cfg().burst_gap, false, true);
        assert_eq!(p.writes, 3);
        assert!(p.is_multiple());
        // 構造のイベント (作成・削除) は件数に数えない
        let mut created = Pending::new(t0, "w".into());
        for _ in 0..5 {
            created.touch(t0, cfg().burst_gap, true, false);
        }
        assert!(created.structural && !created.is_multiple());
    }

    #[test]
    fn debounce_fires_300ms_after_the_last_event_and_max_hold_cuts_at_2s() {
        let c = cfg();
        let t0 = Instant::now();
        let mut p = Pending::new(t0, "w".into());
        p.touch(t0, c.burst_gap, false, true);
        assert!(!p.due(t0 + ms(299), &c));
        assert!(p.due(t0 + ms(300), &c));
        // イベントが続くかぎりデバウンスは延びる (100 ms おきに 1 秒まで)
        for i in 1..=10 {
            p.touch(t0 + ms(100 * i), c.burst_gap, false, true);
        }
        assert!(!p.due(t0 + ms(1299), &c));
        assert!(p.due(t0 + ms(1300), &c));
        // 最初のイベントから max_hold (2 秒) に達したら、続いていても期限
        let mut busy = Pending::new(t0, "w".into());
        for i in 0..=19 {
            busy.touch(t0 + ms(100 * i), c.burst_gap, false, true);
        }
        assert!(!busy.due(t0 + ms(1999), &c));
        assert!(busy.due(t0 + ms(2000), &c));
    }

    #[test]
    fn a_deferred_path_is_due_only_at_its_retry_time_and_a_new_event_cancels_the_retry() {
        let c = cfg();
        let t0 = Instant::now();
        let mut p = Pending::deferred(t0, "w".into(), false, t0, t0 + ms(100));
        assert!(!p.due(t0 + ms(99), &c));
        assert!(p.due(t0 + ms(100), &c));
        p.touch(t0 + ms(50), c.burst_gap, false, true);
        assert_eq!(p.retry_at, None);
        assert!(!p.due(t0 + ms(100), &c), "新しいイベントの後はデバウンスに戻る");
    }

    #[test]
    fn waiting_for_a_writer_gives_up_exactly_at_settle_give_up() {
        let give_up = ms(500);
        let t0 = Instant::now();
        let p = Pending::deferred(t0 + ms(10), "w".into(), false, t0, t0 + ms(110));
        assert!(!p.waited_too_long(t0 + ms(499), give_up));
        assert!(p.waited_too_long(t0 + ms(500), give_up));
        // 待っていないパスは打ち切らない
        assert!(!Pending::new(t0, "w".into()).waited_too_long(t0 + ms(100_000), give_up));
    }

    #[test]
    fn carry_over_keeps_the_earliest_wait_start_and_the_multiple_decision() {
        let t0 = Instant::now();
        let mut newer = Pending::new(t0 + ms(40), "w".into());
        newer.carry_over(true, t0);
        assert!(newer.is_multiple());
        assert_eq!(newer.busy_since, Some(t0));
        // 既に待ちの始まりがあれば、それを残す
        newer.carry_over(false, t0 + ms(30));
        assert_eq!(newer.busy_since, Some(t0));
        assert!(newer.is_multiple());
    }

    #[test]
    fn window_changes_only_after_more_than_the_debounce() {
        let d = cfg().debounce;
        let t0 = Instant::now();
        assert!(!window_expired(t0 + d, t0, d));
        assert!(window_expired(t0 + d + Duration::from_nanos(1), t0, d));
    }

    #[test]
    fn held_deletes_are_released_when_the_window_closes_or_max_hold_passes() {
        let max_hold = cfg().max_hold;
        let t0 = Instant::now();
        assert!(keep_holding(true, t0, t0 + max_hold - Duration::from_nanos(1), max_hold));
        assert!(!keep_holding(true, t0, t0 + max_hold, max_hold), "預かりの期限");
        assert!(!keep_holding(false, t0, t0, max_hold), "窓が閉じた");
    }

    #[test]
    fn verify_runs_only_when_quiet_with_nothing_pending_and_one_trigger_holds() {
        let c = cfg();
        let quiet = c.verify_quiet;
        let long = c.verify_interval;
        let none = Duration::ZERO;
        // 要望があっても、静かでなければ・待ちのパスがあれば走らせない
        assert!(!verify_due(true, 0, true, quiet - Duration::from_nanos(1), none, &c));
        assert!(!verify_due(true, 0, false, quiet, none, &c));
        assert!(verify_due(true, 0, true, quiet, none, &c));
        // 件数・間隔の引き金
        assert!(!verify_due(false, c.verify_burst_events - 1, true, quiet, long - Duration::from_nanos(1), &c));
        assert!(verify_due(false, c.verify_burst_events, true, quiet, none, &c));
        assert!(verify_due(false, 0, true, quiet, long, &c));
    }
}
