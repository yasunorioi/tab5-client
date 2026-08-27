# 次にやること

## 済み（2026-08、ESP-IDF 5.4.4/esp32p4 でクリーンビルド）

- [x] **SBF パーサ** — Python (`tools/parse_sbf.py`) + C (`main/sbf_parser.c`)。
      `tools/sbf_selftest.c` が fixture を全数検証（354 blk / CRC-fail 0）
- [x] **土量バランス平面 + cut/fill** — `main/cutfill.c`（最小二乗＝balance 平面）。
      `tools/cutfill_selftest.c` が合成圃場で解析検証
- [x] **caster 半分の削除** — rtcm_sink/rtcm_monitor/upstream/components/ntripcaster を
      除去し client 化。Zig 依存も消滅
- [x] **USB→SBF→cut/fill 配線** — `gnss_state`（SBF 状態ホルダ）+ `leveler`（survey/
      plane/delta）。パネルに cut/fill 大表示、console に survey/flat/cutfill
- [x] **毎起動プロビジョニング** — `setAttitudeOffset,90,0` + `setSBFOutput,…msec100`
- [x] **PID 0x8231 / itf {0,2} 化**、NMEA を USB2(itf2) に
- [x] **NTRIP client** — `main/ntrip_client.c`。基準局から RTCM3 を de-chunk して
      受信機へ供給（console: ntrip/ntripset/ntripreset）
- [x] upstream(tab5-caster) の共通ボード層バグ（VID/PID 重複・sweep 無限ループ）

## 0. 先に発注元と詰める（合意済み）

- [x] 垂直精度 ±2cm で要求を満たすか → **±2cm で十分と合意**

## 1. 実機ブリングアップ（最重要・未着手）

**まだ実機に焼いていない。** ビルドは通るがランタイム未検証（TODO(hw) マーカー参照）。

```
. ~/esp/esp-idf/export.sh
idf.py -p <PORT> flash monitor
```

- [ ] P3H が enumerate し、sweep が SBF を latch するか（`usb` / `stats` / `sbf`）
- [ ] プロビジョニング（setAttitudeOffset + setSBFOutput）が `$R:` で通るか
- [ ] 屋外で Fixed を得た状態で **pitch / roll の切り替わり**を確認
      （`setAttitudeOffset,90,0` 済み。`AttEuler` の Pitch/Roll どちらが Do-Not-Use
      でないか。docs/handoff.md #6）
- [ ] `ReceiverStatus` の `rx_error`（屋内で 0x8/0x48 = `ERROR: SW,`）が屋外で消えるか
- [ ] NTRIP client が接続し、fix が RTKFixed に上がるか（`ntrip` / `sbf`）
- [ ] cut/fill: `survey add`×N → `survey fit`（or `flat`）→ パネルの CUT/FILL 表示

## 2. UX / 機能の追加

- [ ] **オンパネル touch ボタン**で survey add / fit / flat（今は console のみ）
- [ ] microSD ロガー（PVT/Att + cut/fill を記録）
- [ ] web ダッシュボードに NTRIP client 状態を出す（今は console のみ）

## 3. 基準局側（余裕のあるとき）

- [ ] BD982 が QZSS を出せるか確認。出せるなら `1114` を追加すれば
      rover 側は無改造でみちびきの恩恵を受ける
