// MyeCollab.dll の C ABI (spec §4.0 の 6 関数)。**ここ以外に export を増やさない** —
// 増やすなら PROTO_VERSION を bump し、C++ の kCollabProtoVersion と同時に上げること
// (check_rules.ps1 の規則 9 が両者を機械照合している)。
//
// 境界の規則:
//   * 文字列は必ず UTF-8 + NUL 終端。Rust 側は自前のアロケータで確保するので、
//     **poll が返したポインタは mye_collab_free 以外で解放しないこと** (C++ の
//     free/delete で返すとヒープが違うので即クラッシュする)。
//   * ハンドルは Box<Service> の生ポインタ。destroy は worker を join してから返る。
//     C++ は destroy → FreeLibrary の順を守ること (逆にすると走行中のコードごと
//     アンロードされる)。
//   * 全 export を catch_unwind で囲む。Rust の panic が FFI 境界を越えると
//     未定義動作 (= エディタごと落ちる)。in-process 化でメモリ隔離を失った代わりに、
//     panic だけはここで止める。

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

pub mod git;
pub mod ops;
pub mod porcelain;
pub mod protocol;
pub mod worker;

use worker::Service;

/// UTF-8 の C 文字列を Rust の String へ。NULL / 不正 UTF-8 は None
///
/// # Safety
/// `p` は NUL 終端の有効なポインタか NULL であること
unsafe fn cstr_to_string(p: *const c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    CStr::from_ptr(p).to_str().ok().map(|s| s.to_owned())
}

/// # Safety
/// `h` は mye_collab_create が返したハンドルか NULL であること
unsafe fn as_service<'a>(h: *mut c_void) -> Option<&'a Service> {
    if h.is_null() {
        None
    } else {
        Some(&*(h as *const Service))
    }
}

#[no_mangle]
pub extern "C" fn mye_collab_proto_version() -> u32 {
    protocol::PROTO_VERSION
}

/// 失敗 (NULL 引数 / 不正 UTF-8 / メモリ) は NULL を返す
#[no_mangle]
pub extern "C" fn mye_collab_create(root_utf8: *const c_char) -> *mut c_void {
    catch_unwind(AssertUnwindSafe(|| {
        let root = match unsafe { cstr_to_string(root_utf8) } {
            Some(r) => r,
            None => return std::ptr::null_mut(),
        };
        Box::into_raw(Box::new(Service::new(root))) as *mut c_void
    }))
    .unwrap_or(std::ptr::null_mut())
}

/// 非同期。応答は poll で届く (要求 JSON はここでコピーされるので呼び出し側は解放してよい)
#[no_mangle]
pub extern "C" fn mye_collab_request(h: *mut c_void, json_utf8: *const c_char) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        let svc = match unsafe { as_service(h) } {
            Some(s) => s,
            None => return,
        };
        if let Some(line) = unsafe { cstr_to_string(json_utf8) } {
            svc.request(line);
        }
    }));
}

/// 応答 or 通知を 1 件。無ければ NULL。戻り値は mye_collab_free で解放する
#[no_mangle]
pub extern "C" fn mye_collab_poll(h: *mut c_void) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let svc = match unsafe { as_service(h) } {
            Some(s) => s,
            None => return std::ptr::null_mut(),
        };
        match svc.poll() {
            // JSON に生の NUL は入らない (serde_json がエスケープする) ので
            // CString::new が失敗する経路は事実上無い。失敗したら黙って捨てるのではなく
            // NULL を返す = その 1 件だけ落ちる
            Some(line) => CString::new(line).map(|c| c.into_raw()).unwrap_or(std::ptr::null_mut()),
            None => std::ptr::null_mut(),
        }
    }))
    .unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub extern "C" fn mye_collab_free(s: *mut c_char) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if !s.is_null() {
            unsafe { drop(CString::from_raw(s)) };
        }
    }));
}

/// worker を join してからハンドルを破棄する。以後 h は使えない
#[no_mangle]
pub extern "C" fn mye_collab_destroy(h: *mut c_void) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if h.is_null() {
            return;
        }
        unsafe { drop(Box::from_raw(h as *mut Service)) };
    }));
}
