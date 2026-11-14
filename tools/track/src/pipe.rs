//! 名前付きパイプの Win32 呼び出し (ipc-contract.md §1)。サーバ側とクライアント側の両方。
//!
//! サーバ: 現在のユーザーだけの DACL、`PIPE_REJECT_REMOTE_CLIENTS`、最初の 1 本だけ `FILE_FLAG_FIRST_PIPE_INSTANCE`。
//! ハンドルは `File` に包んで持つ (Drop で `CloseHandle`)。読み書きは同期。

use std::fs::{File, OpenOptions};
use std::io;
use std::os::windows::io::{AsRawHandle, FromRawHandle};
use std::ptr::null_mut;
use std::time::{Duration, Instant};

use windows_sys::Win32::Foundation::{
    GetLastError, LocalFree, ERROR_ACCESS_DENIED, ERROR_PIPE_BUSY, ERROR_PIPE_CONNECTED, HANDLE, INVALID_HANDLE_VALUE,
};
use windows_sys::Win32::Security::Authorization::{
    ConvertSecurityDescriptorToStringSecurityDescriptorW, ConvertSidToStringSidW,
    ConvertStringSecurityDescriptorToSecurityDescriptorW, GetSecurityInfo, SDDL_REVISION_1,
};
use windows_sys::Win32::Security::{
    GetTokenInformation, TokenUser, DACL_SECURITY_INFORMATION, SECURITY_ATTRIBUTES, TOKEN_QUERY, TOKEN_USER,
};
use windows_sys::Win32::Storage::FileSystem::{FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_ACCESS_DUPLEX};
use windows_sys::Win32::System::Pipes::{
    ConnectNamedPipe, CreateNamedPipeW, GetNamedPipeClientProcessId, PeekNamedPipe, WaitNamedPipeW, PIPE_READMODE_BYTE,
    PIPE_REJECT_REMOTE_CLIENTS, PIPE_TYPE_BYTE, PIPE_UNLIMITED_INSTANCES, PIPE_WAIT,
};
use windows_sys::Win32::System::Threading::{GetCurrentProcess, OpenProcessToken};

/// 入出力バッファ (ipc-contract.md §1)。
const BUFFER_BYTES: u32 = 64 * 1024;

/// クライアントが `ERROR_PIPE_BUSY` のとき待つ上限 (ipc-contract.md §1)。
const BUSY_WAIT: Duration = Duration::from_secs(2);

/// pipe の名前 (`\\.\pipe\mye-track-<projectId>`)。
pub fn pipe_name(project_id: &str) -> String {
    format!(r"\\.\pipe\mye-track-{project_id}")
}

fn wide(text: &str) -> Vec<u16> {
    text.encode_utf16().chain(std::iter::once(0)).collect()
}

/// 終端付きの UTF-16 を読む。
///
/// # Safety
/// `ptr` は NUL 終端の有効な UTF-16 列を指す。
unsafe fn read_wide(ptr: *const u16) -> String {
    let mut len = 0;
    while *ptr.add(len) != 0 {
        len += 1;
    }
    String::from_utf16_lossy(std::slice::from_raw_parts(ptr, len))
}

/// `LocalAlloc` で確保された領域の持ち主。Drop で `LocalFree`。
struct LocalMem(*mut core::ffi::c_void);

// SAFETY: LocalAlloc の領域はスレッドに結び付かない。読むだけで、解放は Drop の 1 回
unsafe impl Send for LocalMem {}

impl Drop for LocalMem {
    fn drop(&mut self) {
        if !self.0.is_null() {
            // SAFETY: LocalAlloc 由来の領域を 1 回だけ解放する
            unsafe {
                LocalFree(self.0);
            }
        }
    }
}

/// 自分 (このプロセスのトークン) の SID の文字列 (`S-1-5-21-...`)。
pub fn current_user_sid() -> io::Result<String> {
    // SAFETY: トークンのハンドルは必ず閉じる。GetTokenInformation は 2 回呼ぶ (必要な大きさ → 取得)。
    //         TOKEN_USER の領域は buffer の生存中だけ読む
    unsafe {
        let mut token: HANDLE = null_mut();
        if OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) == 0 {
            return Err(io::Error::last_os_error());
        }
        let token = File::from_raw_handle(token as _);
        let handle = token.as_raw_handle() as HANDLE;
        let mut needed = 0u32;
        GetTokenInformation(handle, TokenUser, null_mut(), 0, &mut needed);
        if needed == 0 {
            return Err(io::Error::last_os_error());
        }
        // TOKEN_USER はポインタを含むので、8 バイト境界の領域に取る
        let mut buffer = vec![0u64; (needed as usize).div_ceil(8)];
        if GetTokenInformation(handle, TokenUser, buffer.as_mut_ptr().cast(), needed, &mut needed) == 0 {
            return Err(io::Error::last_os_error());
        }
        let user = &*(buffer.as_ptr().cast::<TOKEN_USER>());
        let mut text: *mut u16 = null_mut();
        if ConvertSidToStringSidW(user.User.Sid, &mut text) == 0 {
            return Err(io::Error::last_os_error());
        }
        let _free = LocalMem(text.cast());
        Ok(read_wide(text))
    }
}

/// 現在のユーザーだけを許可する DACL の SDDL (継承なし)。
pub fn owner_only_sddl(sid: &str) -> String {
    format!("D:P(A;;GA;;;{sid})")
}

/// SDDL から作ったセキュリティ記述子。Drop で解放する。
struct SecurityDescriptor {
    memory: LocalMem,
}

impl SecurityDescriptor {
    fn from_sddl(sddl: &str) -> io::Result<SecurityDescriptor> {
        let text = wide(sddl);
        let mut descriptor = null_mut();
        // SAFETY: text は NUL 終端。確保された領域は LocalMem が解放する
        let ok = unsafe {
            ConvertStringSecurityDescriptorToSecurityDescriptorW(text.as_ptr(), SDDL_REVISION_1, &mut descriptor, null_mut())
        };
        if ok == 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(SecurityDescriptor { memory: LocalMem(descriptor) })
    }
}

/// サーバ側の pipe の 1 本 (1 接続分)。
pub struct PipeInstance {
    file: File,
}

impl PipeInstance {
    /// クライアントが来るまで待つ。先に繋がっていた (`ERROR_PIPE_CONNECTED`) のも成功。
    pub fn connect(&self) -> io::Result<()> {
        // SAFETY: file が生きている間のハンドル。同期 I/O (OVERLAPPED なし)
        let ok = unsafe { ConnectNamedPipe(self.file.as_raw_handle() as HANDLE, null_mut()) };
        if ok != 0 {
            return Ok(());
        }
        // SAFETY: 直前の API のエラーを読むだけ
        match unsafe { GetLastError() } {
            ERROR_PIPE_CONNECTED => Ok(()),
            _ => Err(io::Error::last_os_error()),
        }
    }

    /// 繋がったクライアントのプロセス ID。
    pub fn client_pid(&self) -> Option<u32> {
        let mut pid = 0u32;
        // SAFETY: pid は出力先として初期化済み
        let ok = unsafe { GetNamedPipeClientProcessId(self.file.as_raw_handle() as HANDLE, &mut pid) };
        (ok != 0).then_some(pid)
    }

    pub fn file(&self) -> &File {
        &self.file
    }
}

/// 同じ名前の pipe を作り続ける。最初の 1 本だけ `FILE_FLAG_FIRST_PIPE_INSTANCE`。
pub struct PipeListener {
    name: Vec<u16>,
    descriptor: SecurityDescriptor,
    first_created: bool,
}

impl PipeListener {
    /// 名前と DACL を決める (まだ pipe は作らない)。
    pub fn new(name: &str) -> io::Result<PipeListener> {
        let descriptor = SecurityDescriptor::from_sddl(&owner_only_sddl(&current_user_sid()?))?;
        Ok(PipeListener { name: wide(name), descriptor, first_created: false })
    }

    /// 次の接続を待つ pipe を 1 本作る。1 本目は同名の pipe が既にあれば `PermissionDenied` (ERROR_ACCESS_DENIED)。
    pub fn create_instance(&mut self) -> io::Result<PipeInstance> {
        let first_flag = if self.first_created { 0 } else { FILE_FLAG_FIRST_PIPE_INSTANCE };
        let attributes = SECURITY_ATTRIBUTES {
            nLength: std::mem::size_of::<SECURITY_ATTRIBUTES>() as u32,
            lpSecurityDescriptor: self.descriptor.memory.0,
            bInheritHandle: 0,
        };
        // SAFETY: name は NUL 終端、attributes と記述子は呼び出しの間だけ使う
        let handle = unsafe {
            CreateNamedPipeW(
                self.name.as_ptr(),
                PIPE_ACCESS_DUPLEX | first_flag,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES,
                BUFFER_BYTES,
                BUFFER_BYTES,
                0,
                &attributes,
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        self.first_created = true;
        // SAFETY: CreateNamedPipeW が返した、所有権のあるハンドル
        Ok(PipeInstance { file: unsafe { File::from_raw_handle(handle as _) } })
    }
}

/// `ERROR_ACCESS_DENIED` か (1 本目の作成で、同名の pipe が既にあるときの失敗)。
pub fn is_access_denied(e: &io::Error) -> bool {
    e.raw_os_error() == Some(ERROR_ACCESS_DENIED as i32)
}

/// クライアント側で pipe を開く。`ERROR_PIPE_BUSY` は `WaitNamedPipeW` で待って開き直す (最大 2 秒)。
/// pipe が無ければ `NotFound` をそのまま返す (呼び出し側が起動の待ちを決める)。
pub fn open_client(name: &str) -> io::Result<File> {
    let deadline = Instant::now() + BUSY_WAIT;
    let wide_name = wide(name);
    loop {
        match OpenOptions::new().read(true).write(true).open(name) {
            Ok(file) => return Ok(file),
            Err(e) if e.raw_os_error() == Some(ERROR_PIPE_BUSY as i32) => {
                let left = deadline.saturating_duration_since(Instant::now());
                if left.is_zero() {
                    return Err(e);
                }
                // SAFETY: wide_name は NUL 終端
                unsafe {
                    WaitNamedPipeW(wide_name.as_ptr(), left.as_millis().max(1) as u32);
                }
            }
            Err(e) => return Err(e),
        }
    }
}

/// pipe の読み取り側にある、いま読めるバイト数 (待たない)。相手が閉じていれば `Err` (`ERROR_BROKEN_PIPE`)。
/// 同期ハンドルの `ReadFile` は同じハンドルへの `WriteFile` を止めるので、通知を送る接続は読む前にこれで見る。
pub fn bytes_available(file: &File) -> io::Result<u32> {
    let mut available = 0u32;
    // SAFETY: available は出力先として初期化済み。バッファは読まない (長さ 0)
    let ok = unsafe {
        PeekNamedPipe(file.as_raw_handle() as HANDLE, null_mut(), 0, null_mut(), &mut available, null_mut())
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(available)
}

/// 開いた pipe の DACL を SDDL の文字列で読み戻す (試験用の確認と診断)。
pub fn read_dacl_sddl(file: &File) -> io::Result<String> {
    let mut descriptor = null_mut();
    // SAFETY: 出力の記述子は LocalMem が解放する。DACL へのポインタは記述子の中を指すので、記述子より長く使わない
    unsafe {
        let status = GetSecurityInfo(
            file.as_raw_handle() as HANDLE,
            windows_sys::Win32::Security::Authorization::SE_KERNEL_OBJECT,
            DACL_SECURITY_INFORMATION,
            null_mut(),
            null_mut(),
            null_mut(),
            null_mut(),
            &mut descriptor,
        );
        if status != 0 {
            return Err(io::Error::from_raw_os_error(status as i32));
        }
        let _descriptor = LocalMem(descriptor);
        let mut text: *mut u16 = null_mut();
        let ok = ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor,
            SDDL_REVISION_1,
            DACL_SECURITY_INFORMATION,
            &mut text,
            null_mut(),
        );
        if ok == 0 {
            return Err(io::Error::last_os_error());
        }
        let _text = LocalMem(text.cast());
        Ok(read_wide(text))
    }
}
