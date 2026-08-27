---
name: tab5-caster-hw-quirks
description: github.com/yasunorioi/tab5-caster を別個体の mosaic に載せ替えるときに効くハードコード箇所とバグ
metadata:
  type: project
---

tab5-caster（ESP32-P4 / M5Stack Tab5 の NTRIP キャスター、ESP-IDF 5.4.4、default branch は `master`）を別の mosaic 個体で動かすとき直す場所:

- `main/usb_cdc_source.c` と `main/nmea_source.c` に **VID/PID が重複定義**（`0x152A` / `0x85C0`）。共通ヘッダに括り出すべき。
- `MOSAIC_COM_ITFS[] = {0, 2, 4}`（usb_cdc_source.c）と `NMEA_ITF 4`（nmea_source.c）は開発時のベンチ機（CDC 3本＋MSC）前提。2 COM 機では成立しない。
- **バグ**: `usb_cdc_source.c` の sweep ループで `cdc_acm_host_open` 失敗時に `sweep` を進めず `continue` するため、存在しない itf に当たると無限リトライして他の itf に戻れない。
- README の「itf0 は両方向 silent」も個体依存（[[mosaic-g5-p3h-rover-only]] の個体では itf0 が応答する）。

**Why:** README は1個体での実測を一般則のように書いているので、そのまま信じると詰まる。
**How to apply:** 別個体を繋いだらまず `usb` コンソールコマンドで VID/PID と itf トポロジを取り、上記を実測に合わせる。
