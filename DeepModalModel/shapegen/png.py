#====================================================================================
#                          png.py
#  MyEngine/ 秋田蓮音                                                      10/11/2026
#                                          最小の PNG ライタ (8bit グレースケール、zlib + struct のみ)
#====================================================================================
"""QA 用コンタクトシートのために PIL 等の依存を増やさない。"""
import struct
import zlib

import numpy as np


def _chunk(tag: bytes, data: bytes) -> bytes:
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def write_png_gray(path, img: np.ndarray):
    """img: uint8 (H,W)。"""
    img = np.asarray(img, dtype=np.uint8)
    if img.ndim != 2:
        raise ValueError("gray image must be 2D")
    h, w = img.shape
    raw = b"".join(b"\x00" + img[y].tobytes() for y in range(h))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 0, 0, 0, 0)  # 8bit, color type 0 (gray)
    data = b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", zlib.compress(raw, 6)) \
        + _chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(data)


def read_png_gray(path) -> np.ndarray:
    """テスト用の最小リーダ (このライタが書いた形式だけ)。"""
    with open(path, "rb") as f:
        data = f.read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos = 8
    width = height = 0
    idat = b""
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        crc = struct.unpack(">I", data[pos + 8 + length:pos + 12 + length])[0]
        assert crc == (zlib.crc32(tag + body) & 0xFFFFFFFF)
        if tag == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length
    raw = zlib.decompress(idat)
    rows = [np.frombuffer(raw[y * (width + 1) + 1:(y + 1) * (width + 1)], dtype=np.uint8) for y in range(height)]
    return np.vstack(rows)
