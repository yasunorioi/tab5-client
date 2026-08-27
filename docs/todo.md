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

## 1. 実機ブリングアップ

**2026-08 に実機で確認済み**（Tab5 + P3H、`/dev/ttyACM0`、`idf.py flash`）:

- [x] P3H が enumerate（`152A:8231` / itf `[0,1,2,3]` = CDC×2）し itf0 に SBF latch
- [x] プロビジョニング成功（SBF が流れている＝setSBFOutput 適用済み）
- [x] SBF パース **crc_fails=0**（実機で 2000+ blocks / 失敗ゼロ）
- [x] NTRIP client 接続 → **RTK fixed** 到達（hAcc=0.020 / vAcc=0.040 m）
- [x] cut/fill 実データ動作（`flat` → `cutfill` = ON GRADE、静止で ±数cm）

残り（デュアルアンテナ + 屋外が要る）:

- [ ] **pitch / roll の切り替わり**。ベンチでは `AttEuler` が Do-Not-Use（姿勢は
      2アンテナ収束が要る）。屋外で `AttEuler` の Pitch/Roll どちらが有効か確認
      （`setAttitudeOffset,90,0` 済み。docs/handoff.md #6）
- [ ] `ReceiverStatus` の `rx_error`（ベンチで 0x8 = `ERROR: SW,`）が
      2アンテナ屋外で消えるか
- [ ] 実圃場で `survey add`×N（走行して複数点）→ `survey fit`（balance 平面）→
      パネルの CUT/FILL 表示

## 2. UX / 機能の追加

- [ ] **オンパネル touch ボタン**で survey add / fit / flat（今は console のみ）
- [ ] microSD ロガー（PVT/Att + cut/fill を記録）
- [ ] web ダッシュボードに NTRIP client 状態を出す（今は console のみ）

## 3. 基準局側（余裕のあるとき）

- [ ] BD982 が QZSS を出せるか確認。出せるなら `1114` を追加すれば
      rover 側は無改造でみちびきの恩恵を受ける
