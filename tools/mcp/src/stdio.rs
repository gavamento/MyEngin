//====================================================================================
//                          stdio.rs
//  MyEngin/ 秋田蓮音                                                       10/15/2026
//                                          stdin/stdout の行の読み書き (transport)
//====================================================================================

use std::io::{BufRead, Write};

// 次の 1 行を読む。EOF で None。行末の \r\n と \n を落とし、空行は読み飛ばす。
// UTF-8 として不正な行は Err(バイト列の損失変換) で返す (呼び出し側が -32700 にする)。
pub fn read_line<R: BufRead>(input: &mut R) -> std::io::Result<Option<Result<String, String>>> {
    loop {
        let mut buf = Vec::new();
        if input.read_until(b'\n', &mut buf)? == 0 {
            return Ok(None);
        }
        while matches!(buf.last(), Some(b'\n') | Some(b'\r')) {
            buf.pop();
        }
        if buf.iter().all(|b| b.is_ascii_whitespace()) {
            continue;
        }
        return Ok(Some(match String::from_utf8(buf) {
            Ok(s) => Ok(s),
            Err(e) => Err(String::from_utf8_lossy(e.as_bytes()).into_owned()),
        }));
    }
}

// 応答を 1 行で書き、すぐ flush する (パイプ越しのクライアントは flush されるまで待つ)。
pub fn write_line<W: Write>(out: &mut W, line: &str) -> std::io::Result<()> {
    out.write_all(line.as_bytes())?;
    out.write_all(b"\n")?;
    out.flush()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crlf_と空行を許す() {
        let mut input: &[u8] = b"a\r\n\r\n  \nb\n";
        assert_eq!(read_line(&mut input).unwrap(), Some(Ok("a".to_string())));
        assert_eq!(read_line(&mut input).unwrap(), Some(Ok("b".to_string())));
        assert_eq!(read_line(&mut input).unwrap(), None);
    }

    #[test]
    fn 不正な_utf8_は_err() {
        let mut input: &[u8] = b"\xff\xfe\n";
        assert!(read_line(&mut input).unwrap().unwrap().is_err());
    }
}
