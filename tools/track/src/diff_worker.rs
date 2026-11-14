//! Diff ワーカー (architecture.md §3.1)。観測ごとの行 diff を、監視スレッドと Writer の外で計算する。
//!
//! 結果は観測と一緒に返し、呼び出し側 (Collector) が Writer へ渡す。ワーカーは DB に触れず、
//! Blob を読むだけ。計算が panic しても、その観測の Err として返し、ワーカー自体は続ける。

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::mpsc::{channel, Receiver, Sender};
use std::thread::JoinHandle;

use crate::blob::BlobStore;
use crate::diff::DiffLimits;
use crate::observe::{observation_payload, Observation};
use crate::revision::{plan_diff, DiffPlan, RevisionInput};

type Planned = (Vec<Observation>, Vec<Result<DiffPlan, String>>);

struct Job {
    observations: Vec<Observation>,
    reply: Sender<Planned>,
}

pub struct DiffWorker {
    sender: Option<Sender<Job>>,
    handle: Option<JoinHandle<()>>,
}

impl DiffWorker {
    /// `blobs` は読み取り用 (`BlobStore::reader`)。
    pub fn spawn(blobs: BlobStore, limits: DiffLimits) -> DiffWorker {
        let (sender, receiver) = channel::<Job>();
        let handle = std::thread::Builder::new()
            .name("mye-track-diff".into())
            .spawn(move || run(blobs, limits, receiver))
            .expect("diff worker thread");
        DiffWorker { sender: Some(sender), handle: Some(handle) }
    }

    /// 観測ごとの差分を計算して、観測と一緒に返す (長さと順序は入力と同じ)。
    /// ワーカーが止まっているときは、全件 Err の計画を返す (観測は失わない)。
    pub fn plan(&self, observations: Vec<Observation>) -> Planned {
        let stopped = |observations: Vec<Observation>| {
            let plans = observations.iter().map(|_| Err("Diff ワーカーが止まっている".to_string())).collect();
            (observations, plans)
        };
        let Some(sender) = &self.sender else { return stopped(observations) };
        let (reply, answer) = channel();
        if let Err(returned) = sender.send(Job { observations, reply }) {
            return stopped(returned.0.observations);
        }
        // 受け取る前にワーカーが落ちると、観測を取り戻せない。ワーカーは panic を捕まえて必ず返すので起きない想定
        answer.recv().unwrap_or_default()
    }
}

impl Drop for DiffWorker {
    fn drop(&mut self) {
        self.sender = None;
        if let Some(handle) = self.handle.take() {
            let _ = handle.join();
        }
    }
}

fn run(blobs: BlobStore, limits: DiffLimits, receiver: Receiver<Job>) {
    for job in receiver {
        let plans = job
            .observations
            .iter()
            .map(|o| {
                catch_unwind(AssertUnwindSafe(|| plan_one(&blobs, &limits, o)))
                    .unwrap_or_else(|_| Err("差分の計算が panic した".to_string()))
            })
            .collect();
        let _ = job.reply.send((job.observations, plans));
    }
}

fn plan_one(blobs: &BlobStore, limits: &DiffLimits, o: &Observation) -> Result<DiffPlan, String> {
    let excluded = o.after.is_some() && blobs.is_excluded(&o.path);
    let payload = observation_payload(o, excluded);
    let input = RevisionInput::from_payload(blobs, &payload).ok_or_else(|| "観測の payload を読めない".to_string())?;
    plan_diff(blobs, &input, o.after.as_ref().map(|a| a.bytes.as_slice()), limits)
}
