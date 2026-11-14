//! Writer スレッド (architecture.md §3.1)。DB・ジャーナル・Blob への書き込みを 1 本に直列化する。
//!
//! 後続のサブ (監視・pipe・diff) はこのキューへクロージャを積むだけにする。
//! 実行順はキューへ入った順で、同時に 2 つの書き込みは走らない。

use std::fmt;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::mpsc::{self, Receiver, Sender};
use std::thread::JoinHandle;

use crate::store::Store;

type Job = Box<dyn FnOnce(&mut Store) + Send>;

/// Writer が既に止まっている、またはジョブが panic して結果が返らなかった。
#[derive(Debug, PartialEq, Eq)]
pub enum WriterError {
    Stopped,
    JobPanicked,
}

impl fmt::Display for WriterError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            WriterError::Stopped => write!(f, "Writer は停止している"),
            WriterError::JobPanicked => write!(f, "Writer のジョブが panic した"),
        }
    }
}

impl std::error::Error for WriterError {}

pub struct Writer {
    sender: Option<Sender<Job>>,
    handle: Option<JoinHandle<()>>,
}

impl Writer {
    /// Store を Writer スレッドへ移して起動する。以後 Store に触れるのはこのスレッドだけ。
    pub fn spawn(mut store: Store) -> Writer {
        let (sender, receiver) = mpsc::channel::<Job>();
        let handle = std::thread::Builder::new()
            .name("mye-track-writer".into())
            .spawn(move || {
                for job in receiver {
                    // 1 件の panic でキュー全体を止めない (結果の送り手が落ちて、呼び出し側へは JobPanicked が返る)
                    let _ = catch_unwind(AssertUnwindSafe(|| job(&mut store)));
                }
            })
            .expect("writer thread");
        Writer { sender: Some(sender), handle: Some(handle) }
    }

    /// ジョブを積む。結果は返された Receiver で受ける (待たない)。
    pub fn submit<R: Send + 'static>(
        &self,
        job: impl FnOnce(&mut Store) -> R + Send + 'static,
    ) -> Result<Receiver<R>, WriterError> {
        let (tx, rx) = mpsc::channel();
        let boxed: Job = Box::new(move |store| {
            let _ = tx.send(job(store));
        });
        self.sender.as_ref().ok_or(WriterError::Stopped)?.send(boxed).map_err(|_| WriterError::Stopped)?;
        Ok(rx)
    }

    /// ジョブを積み、終わるまで待って結果を返す。
    pub fn call<R: Send + 'static>(
        &self,
        job: impl FnOnce(&mut Store) -> R + Send + 'static,
    ) -> Result<R, WriterError> {
        self.submit(job)?.recv().map_err(|_| WriterError::JobPanicked)
    }

    /// キューの残りを全部実行してから止める。
    pub fn shutdown(mut self) {
        self.stop();
    }

    fn stop(&mut self) {
        self.sender = None;
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

impl Drop for Writer {
    fn drop(&mut self) {
        self.stop();
    }
}
