# 次にやること

## 0. 先に発注元と詰める（技術判断ではなく案件判断）

- [ ] **垂直精度 ±2cm で要求を満たすか。** レーザーレベラー（±5mm）とは別物である点の合意

## 1. upstream（tab5-caster）側で先に直す

fork 後に直すと二重メンテになるので、**共通のボード層バグは upstream で先に修正**する。

- [ ] `main/usb_cdc_source.c` と `main/nmea_source.c` に重複定義されている VID/PID を
      共通ヘッダに括り出す
- [ ] **sweep の無限ループバグ**: `usb_cdc_source.c` の sweep ループで
      `cdc_acm_host_open` 失敗時に `sweep` を進めず `continue` するため、
      存在しない itf に当たると無限リトライして他の itf に戻れない
- [ ] README の「itf0 は両方向 silent」「CDC 3本 + MSC」は開発機1個体の実測値であって
      一般則ではない旨を追記（[[docs/hardware-findings.md]] 参照）

## 2. tab5-client のコード整理

- [ ] caster 半分を削除: `main/rtcm_sink.*` / `main/rtcm_monitor.*` /
      `components/ntripcaster` / `main/upstream.*`（client 版で書き直すので一旦削除）
- [ ] `CMakeLists.txt` / `main/CMakeLists.txt` から Zig caster のビルド規則を除去
- [ ] README を tab5-client のものに差し替え

## 3. 実装（この順序が効率的）

**SBF パーサから始める。** 実バイト列が取得済みなので **ESP32 に焼く前に PC 上で
全数検証できる**。ブロック定義と CRC は確定済み（docs/hardware-findings.md）。

- [ ] SBF パーサ（PVTGeodetic 4007 rev2 / AttEuler 5938 rev0 / DOP 4001 rev0 /
      ReceiverStatus 4014 rev1）。CRC-16-CCITT poly 0x1021 init 0、ID 以降に適用
- [ ] 土量バランス平面の算出（純粋なロジックなのでオフラインで詰められる）
- [ ] `usb_cdc_source.c` の双方向化（PID 0x8231 / itf {0,2}）
- [ ] NTRIP client（**de-chunk 必須**。`esp_http_client` のストリーミングモード推奨）
- [ ] 毎起動プロビジョニング（`setAttitudeOffset, 90, 0` + `setSBFOutput, ...`）
- [ ] cut/fill 表示画面
- [ ] microSD ロガー

## 4. 環境

- [ ] **ESP-IDF 5.4.4 以降のインストール**（この PC には未導入）

## 5. 基準局側（余裕のあるとき）

- [ ] BD982 が QZSS を出せるか確認。出せるなら `1114` を追加すれば
      rover 側は無改造でみちびきの恩恵を受ける
