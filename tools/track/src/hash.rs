//! SHA-256 と 16 進化。sha2 0.11 の出力は `{:x}` で整形できないので自前で持つ。

use sha2::{Digest, Sha256};

/// バイト列を小文字の 16 進文字列にする。
pub fn to_hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for &b in bytes {
        out.push(DIGITS[(b >> 4) as usize] as char);
        out.push(DIGITS[(b & 0x0f) as usize] as char);
    }
    out
}

/// 内容の SHA-256 を小文字 16 進 (64 桁) で返す。Blob の名前と projectId の元になる。
pub fn sha256_hex(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    to_hex(digest.as_slice())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn known_vectors() {
        assert_eq!(
            sha256_hex(b""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
        );
        assert_eq!(
            sha256_hex(b"abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn hex_covers_all_nibbles() {
        assert_eq!(to_hex(&[0x00, 0x0f, 0xf0, 0xab, 0xff]), "000ff0abff");
    }
}
