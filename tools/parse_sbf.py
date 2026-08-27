#!/usr/bin/env python3
"""Septentrio SBF パーサ（mosaic-go G5 P3H 用）。

parse-sbf.ps1 の Python 移植 + フィールドデコード。
CRC 全数検証してブロック内訳を出すのは ps1 と同じだが、こちらは
PVTGeodetic / AttEuler / DOP / ReceiverStatus の中身までデコードする。
将来 Tab5(ESP32-P4) 側の C パーサに移植するときの参照実装。

  python3 tools/parse_sbf.py tests/fixtures/mosaic-g5-p3h-sbf.bin --seconds 12.01
  python3 tools/parse_sbf.py tests/fixtures/mosaic-g5-p3h-sbf.bin --dump 2

フレーミング: sync $@ (0x24 0x40), CRC(u16 LE), ID(u16 LE), Length(u16 LE)。
ID の下位 13bit がブロック番号、上位 3bit がリビジョン。Length は 4 の倍数で
ヘッダ込みの全長。CRC は CRC-16-CCITT (poly 0x1021, init 0) を ID 以降
(off+4 から len-4 バイト) に適用する。

⚠ 採取フィクスチャはアンテナ未接続の屋内なので測位解は無効。
   Mode は No-PVT、AttEuler は Do-Not-Use。フレーミングと CRC、デコードの
   バイト配置の検証には使えるが、値の妥当性検証には使えない。
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field

SYNC1 = 0x24  # '$'
SYNC2 = 0x40  # '@'

# SBF の Do-Not-Use センチネル。浮動小数は -2e10、整数は全ビット 1。
DNU_F = -2.0e10
DNU_U1 = 0xFF
DNU_U2 = 0xFFFF
DNU_U4 = 0xFFFFFFFF

BLOCK_NAMES = {
    4001: "DOP",
    4007: "PVTGeodetic",
    4014: "ReceiverStatus",
    4027: "MeasEpoch",
    5914: "ReceiverTime",
    5938: "AttEuler",
}

# PVTGeodetic Mode 下位 4bit → 測位種別
PVT_MODE = {
    0: "No PVT",
    1: "Stand-Alone",
    2: "Differential",
    3: "Fixed location",
    4: "RTK fixed",
    5: "RTK float",
    6: "SBAS",
    7: "moving-base RTK fixed",
    8: "moving-base RTK float",
    10: "PPP",
}

PVT_ERROR = {
    0: "No Error",
    1: "Not enough measurements",
    2: "Not enough ephemerides",
    3: "DOP too large",
    4: "Residuals too large",
    5: "No convergence",
    6: "Not enough measurements after outlier rejection",
    7: "Output prohibited (export laws)",
    8: "Not enough differential corrections",
    9: "Base coordinates unavailable",
    10: "Ambiguities not fixed (RTK-fixed only requested)",
}


def crc16_ccitt(buf: bytes, off: int, length: int) -> int:
    """CRC-16-CCITT (poly 0x1021, init 0)。ID 以降に適用する。"""
    crc = 0
    for i in range(off, off + length):
        crc ^= buf[i] << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def _f(v: float):
    """Do-Not-Use の浮動小数を None に潰す。"""
    return None if v <= DNU_F else v


@dataclass
class Block:
    block_num: int
    rev: int
    length: int
    offset: int  # ストリーム先頭からのバイト位置
    name: str
    tow_ms: int | None = None  # ms of week (Do-Not-Use なら None)
    wnc: int | None = None     # week number
    fields: dict = field(default_factory=dict)

    @property
    def label(self) -> str:
        return f"{self.block_num:<5} {self.name:<16} rev{self.rev} len={self.length}"


def _decode_time(body: bytes) -> tuple[int | None, int | None]:
    """全 SBF ブロック共通の先頭 TOW(u32)+WNc(u16)。body はヘッダ後の 8 バイト目以降。"""
    tow, wnc = struct.unpack_from("<IH", body, 0)
    return (None if tow == DNU_U4 else tow, None if wnc == DNU_U2 else wnc)


def _decode_pvtgeodetic(body: bytes) -> dict:
    # rev2 / len=96。body はヘッダ(8B)を除いた本体。オフセットはヘッダ込みの値-8。
    mode, err = struct.unpack_from("<BB", body, 6)  # @14,@15
    lat, lon, height = struct.unpack_from("<ddd", body, 8)  # @16,@24,@32 (rad, rad, m)
    undulation, vn, ve, vu, cog = struct.unpack_from("<fffff", body, 32)  # @40..
    nr_sv = body[66]  # @74
    h_acc, v_acc = struct.unpack_from("<HH", body, 82)  # @90,@92 (cm)
    mode_type = mode & 0x0F
    import math
    return {
        "mode": PVT_MODE.get(mode_type, f"?{mode_type}"),
        "mode_raw": mode,
        "error": PVT_ERROR.get(err, f"?{err}"),
        "lat_deg": None if lat <= DNU_F else math.degrees(lat),
        "lon_deg": None if lon <= DNU_F else math.degrees(lon),
        "height_m": _f(height),  # 楕円体高。cut/fill はこれを使う（ジオイド不要）
        "nr_sv": None if nr_sv == DNU_U1 else nr_sv,
        # 精度は cm 単位の u16 (0.01m 刻み)。Do-Not-Use = 0xFFFF。
        "h_accuracy_m": None if h_acc == DNU_U2 else h_acc / 100.0,
        "v_accuracy_m": None if v_acc == DNU_U2 else v_acc / 100.0,
    }


def _decode_atteuler(body: bytes) -> dict:
    # rev0 / len=44
    nr_sv, err = struct.unpack_from("<BB", body, 6)  # @14,@15
    mode = struct.unpack_from("<H", body, 8)[0]      # @16
    heading, pitch, roll = struct.unpack_from("<fff", body, 12)  # @20,@24,@28 (deg)
    pitch_dot, roll_dot, heading_dot = struct.unpack_from("<fff", body, 24)  # @32..
    return {
        "nr_sv": None if nr_sv == DNU_U1 else nr_sv,
        "error": err,
        "mode": mode,
        # setAttitudeOffset,90,0 済み。屋外 Fixed で heading/pitch/roll の
        # どれが Do-Not-Use でないかを見て pitch↔roll を確定する（docs/todo.md #6）。
        "heading_deg": _f(heading),
        "pitch_deg": _f(pitch),
        "roll_deg": _f(roll),
    }


def _decode_dop(body: bytes) -> dict:
    # rev0 / len=32。xDOP は u16 で 100 倍スケール、Do-Not-Use = 0。
    nr_sv = body[6]  # @14
    pdop, tdop, hdop, vdop = struct.unpack_from("<HHHH", body, 8)  # @16..
    hpl, vpl = struct.unpack_from("<ff", body, 16)  # @24,@28
    scale = lambda v: None if v == 0 else v / 100.0
    return {
        "nr_sv": None if nr_sv == DNU_U1 else nr_sv,
        "pdop": scale(pdop),
        "tdop": scale(tdop),
        "hdop": scale(hdop),
        "vdop": scale(vdop),
        "hpl_m": _f(hpl),
        "vpl_m": _f(vpl),
    }


def _decode_receiverstatus(body: bytes) -> dict:
    # rev1 / len=104
    cpu_load, ext_error = struct.unpack_from("<BB", body, 6)  # @14,@15
    uptime, rx_state, rx_error = struct.unpack_from("<III", body, 8)  # @16,@20,@24
    n, sb_len, cmd_count, temperature = struct.unpack_from("<BBBB", body, 20)  # @28..
    return {
        "cpu_load_pct": None if cpu_load == DNU_U1 else cpu_load,
        "ext_error": ext_error,
        "uptime_s": uptime,
        "rx_state": rx_state,
        "rx_error": rx_error,  # 0 以外は要注意（docs: ステータス行の ERROR: SW,）
        "agc_subblocks": n,
        # Temperature: offset 100 の摂氏。0 は Do-Not-Use。
        "temperature_c": None if temperature == 0 else temperature - 100,
    }


DECODERS = {
    4007: _decode_pvtgeodetic,
    5938: _decode_atteuler,
    4001: _decode_dop,
    4014: _decode_receiverstatus,
}


@dataclass
class ParseResult:
    total_bytes: int
    valid_blocks: int
    crc_failed: int
    frame_bytes: int  # CRC 有効フレームが占めたバイト数
    blocks: list[Block]


def parse(data: bytes, decode: bool = True) -> ParseResult:
    """SBF ストリームを走査して CRC 有効ブロックを返す。"""
    blocks: list[Block] = []
    bad = 0
    used = 0
    i = 0
    n = len(data)
    while i < n - 8:
        if data[i] == SYNC1 and data[i + 1] == SYNC2:
            crc, ident, length = struct.unpack_from("<HHH", data, i + 2)
            if length >= 8 and length % 4 == 0 and i + length <= n:
                if crc16_ccitt(data, i + 4, length - 4) == crc:
                    block_num = ident & 0x1FFF
                    rev = ident >> 13
                    name = BLOCK_NAMES.get(block_num, f"Block{block_num}")
                    blk = Block(block_num, rev, length, i, name)
                    if decode and block_num in DECODERS:
                        body = data[i + 8:i + length]  # ヘッダを除いた本体
                        blk.tow_ms, blk.wnc = _decode_time(body)
                        try:
                            blk.fields = DECODERS[block_num](body)
                        except struct.error:
                            blk.fields = {"_decode_error": True}
                    blocks.append(blk)
                    used += length
                    i += length
                    continue
                bad += 1
        i += 1
    return ParseResult(n, len(blocks), bad, used, blocks)


def _fmt_tow(tow_ms: int | None) -> str:
    if tow_ms is None:
        return "TOW=--"
    return f"TOW={tow_ms / 1000.0:.3f}s"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="SBF キャプチャを CRC 全数検証してデコードする")
    ap.add_argument("path", help="SBF バイナリキャプチャ")
    ap.add_argument("--seconds", type=float, default=0, help="レート算出用の採取秒数")
    ap.add_argument("--dump", type=int, default=0, metavar="N",
                    help="各ブロック種別の先頭 N 個をデコードして表示")
    args = ap.parse_args(argv)

    with open(args.path, "rb") as fh:
        data = fh.read()

    res = parse(data, decode=args.dump > 0)

    print(f"bytes: {res.total_bytes}   CRC-valid blocks: {res.valid_blocks}   "
          f"CRC-failed: {res.crc_failed}   frame bytes: {res.frame_bytes} / {res.total_bytes}")

    # ブロック種別ごとに集計（parse-sbf.ps1 と同じ並び）
    tally: dict[str, int] = {}
    for b in res.blocks:
        tally[b.label] = tally.get(b.label, 0) + 1
    for label in sorted(tally):
        count = tally[label]
        if args.seconds > 0:
            print(f"  {label}  x{count}  ({round(count / args.seconds, 1)} Hz)")
        else:
            print(f"  {label}  x{count}")

    if args.dump > 0:
        print()
        shown: dict[int, int] = {}
        for b in res.blocks:
            if b.block_num not in DECODERS:
                continue
            if shown.get(b.block_num, 0) >= args.dump:
                continue
            shown[b.block_num] = shown.get(b.block_num, 0) + 1
            print(f"@{b.offset:<6} {b.name:<15} {_fmt_tow(b.tow_ms)} WNc={b.wnc}")
            for k, v in b.fields.items():
                if isinstance(v, float):
                    v = f"{v:.6g}"
                print(f"    {k:<16} {v}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
