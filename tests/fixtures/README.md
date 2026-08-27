# テストデータ

**実機から採取した本物のバイト列。** 受信機も基準局も無い環境で、パーサを全数検証できる。

| ファイル | 中身 |
|---|---|
| `mosaic-g5-p3h-sbf.bin` | mosaic-G5 P3H の USB1 から 12.01 秒。**20,856 B / 354 ブロック、CRC 失敗ゼロ、パディング無し**（フレームバイトが全長と一致） |
| `eniwa-bd982-rtcm3.bin` | `rtk.toiso.fit:2101/eniwa-bd982` から約 8 秒。**3,328 B / 27 フレーム、CRC 失敗ゼロ** |

どちらも**アンテナ未接続の屋内**で採取しているので、測位解は無効（`PVTGeodetic` の
Mode は No-PVT、`AttEuler` は Do-Not-Use）。**フレーミングと CRC の検証には使えるが、
値の妥当性検証には使えない。** 屋外で測位させた状態のキャプチャは別途必要。

## mosaic-g5-p3h-sbf.bin

採取時の設定:
```
setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP, msec100
```

```
1737 B/s
  4001  DOP             rev0  len=32   x114  (10 Hz)
  4007  PVTGeodetic     rev2  len=96   x114  (10 Hz)
  4014  ReceiverStatus  rev1  len=104  x12   ( 1 Hz)
  5938  AttEuler        rev0  len=44   x114  (10 Hz)
```

フレーミング: sync `$@` (0x24 0x40), CRC(u16 LE), ID(u16 LE), Length(u16 LE)。
ID の下位 13bit がブロック番号、上位 3bit がリビジョン。Length は 4 の倍数でヘッダ込みの全長。
CRC は **CRC-16-CCITT (poly 0x1021, init 0)** を **ID 以降**（`off+4` から `len-4` バイト）に適用。

## eniwa-bd982-rtcm3.bin

**curl が de-chunk した後**のバイト列。キャスターは `Transfer-Encoding: chunked` で返すので、
生ソケットで読む実装はこの形になるまで自前で剥がす必要がある。

```
416 B/s
  1006  ARP + antenna height          x1
  1008  Antenna descriptor + serial   x1
  1033  Receiver/antenna descriptor   x1
  1074  GPS MSM4                      x8   (1 Hz)
  1094  Galileo MSM4                  x8   (1 Hz)
  1124  BeiDou MSM4                   x8   (1 Hz)
```

フレーミング: `0xD3`, 6bit 予約 + 10bit 長, ペイロード, CRC-24Q(3B)。
CRC-24Q は poly `0x1864CFB`, init 0 を**先頭の 0xD3 から**適用。
メッセージ番号はペイロード先頭 12bit。
