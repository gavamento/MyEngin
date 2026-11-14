//! OS の乱数源 (`BCryptGenRandom`) から作る識別子。シミュレーションの決定性とは無関係 (`rand` crate は足さない)。

use std::io;

use windows_sys::Win32::Security::Cryptography::{BCryptGenRandom, BCRYPT_USE_SYSTEM_PREFERRED_RNG};

/// OS の乱数源から 16 バイトを取る。
pub fn random_bytes16() -> io::Result<[u8; 16]> {
    let mut bytes = [0u8; 16];
    // SAFETY: bytes は 16 バイトの出力先
    let status = unsafe {
        BCryptGenRandom(std::ptr::null_mut(), bytes.as_mut_ptr(), bytes.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
    };
    if status != 0 {
        return Err(io::Error::other(format!("BCryptGenRandom が失敗: 0x{status:08x}")));
    }
    Ok(bytes)
}

/// 16 バイトを 16 進にしたトークン。
pub fn random_token() -> io::Result<String> {
    Ok(random_bytes16()?.iter().map(|b| format!("{b:02x}")).collect())
}

/// UUID v4 の形 (`xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`)。`sessionId` / `operationId` に使う。
pub fn uuid_v4() -> io::Result<String> {
    let mut b = random_bytes16()?;
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    let hex: String = b.iter().map(|x| format!("{x:02x}")).collect();
    Ok(format!("{}-{}-{}-{}-{}", &hex[0..8], &hex[8..12], &hex[12..16], &hex[16..20], &hex[20..32]))
}
