# 次にやること

## 済み（2026-08、ESP-IDF 5.4.4/esp32p4 でビルド、実機検証済み）

- [x] **SBF パーサ** — Python (`tools/parse_sbf.py`) + C (`main/sbf_parser.c`)。
      `tools/sbf_selftest.c` が全数検証（354 blk / CRC-fail 0）。実機で crc_fails=0
- [x] **土量バランス平面 + cut/fill delta** — `main/cutfill.c`（最小二乗）+
      `main/leveler.c`。`tools/cutfill_selftest.c` で解析検証
- [x] **切土/盛土 VOLUME (m³) + 圃場MAP データ** — `main/fieldmap.c`（外周ポリゴン・
      面積・MLS 補間でグリッド積分）。`tools/fieldmap_selftest.c` で解析解と一致。
      連続記録（距離ゲート）で外周＋点群を収集（`record`/`vol` コマンド）
- [x] **caster 半分の削除**（rtcm_sink/monitor/upstream/ntripcaster）→ client 化
- [x] **NTRIP client** — `main/ntrip_client.c`（de-chunk、受信機へ RTCM3 供給）→
      実機で **RTK fixed** 到達（hAcc 2cm/vAcc 4cm）
- [x] **毎起動プロビジョニング** — setAttitudeOffset + setSBFOutput(USB1@10Hz) +
      NMEA(USB2) + **COM1 GGA@10Hz 38400(RS232 外部機器向け)**
- [x] **オンパネル タッチ UI 土台** — ST7123 座標バグ修正（報告テーブル全点読みで
      INT clear）、四隅較正、LVGL indev。レベラー作業画面（cut/fill 大数字 +
      縦ライトバー + Flat/Survey+/Fit/Clear ボタン）実機動作
- [x] **USB sweep バグ修正** — SBF を itf0 限定に（itf2 で nmea_source と衝突し
      USB ホストが wedge する問題）。これで SBF/RTK 安定
- [x] 垂直精度 ±2cm で要求充足を発注元と合意

## これから（phase 区切り・この順で実装）

### Phase A — 平面図（plan-view MAP）画面 ✅（実機確認済み）
- [x] 圃場境界ポリゴン + cut/fill ヒートマップ + 現在位置 + 土量合計 + bbox フィット
      （`main/map_view.c`。MLS は float 化で watchdog 回避）

### Phase B — on-panel ボタン + 画面遷移 ✅（実機確認済み）
- [x] Perim/Field/Stop（録画・緑ハイライト）/ Flat/Fit/Clear/Map> / MAP の Work・Vol
- [x] 作業 ⇄ MAP ⇄ 設定 のナビゲーション

### Phase C — NMEA 出力設定画面（NVS 永続化）✅（実機確認済み）
- [x] COM1 NMEA を液晶設定（message×rate）→ NVS 保存 → `mosaic_provision` が起動時再送
      （`main/settings_view.c` + `mosaic_config.c`。パネル操作→保存→受信機反映を確認）

### Phase C-2 — COM2 追加 + ポート毎 ON/OFF ✅（コード完成・ビルド済み／実機反映は未確認）
- [x] `mosaic_config` を単一COM1→**COMポート配列(COM1/COM2)**に一般化。各ポートに
      `{enabled(master on/off), msgs, rate}`。COM2=Stream3（COM1=Stream2/パネル=Stream1 と非衝突）。
      boot で両ポートの baud 設定＋NMEA 送信（無効は `none`）。NVS は port0 が旧キー継承・
      port1 は `msgs1/rate1/en1`（COM1 既定 ON / COM2 既定 OFF）
- [x] 設定画面に **Port 切替(COM1/COM2)** + **専用 OUTPUT ON/OFF トグル** を追加。
      rate 行は実レート4つ（OFF ボタン廃止＝トグルが on/off を担う）。Apply で両ポート保存＋反映
- [x] `nmeaout` コンソールは両ポート表示に更新
- [ ] **実機確認**: パネルで COM2 を ON→Apply→受信機に `setNMEAOutput,Stream3,COM2,...` が
      通るか、COM2 の RS232 から NMEA が出るか（COM2 が RS232 トランシーバに配線されている前提）

### Phase D — microSD CSV ロガー ⚠（コード完成・SD マウントは HW blocker）
- [x] CSV ロガー実装（`main/logger.c`）: 1Hz で PVT/Att/cut-fill を
      `/sdcard/leveler_NNN.csv` に記録。`log [start|stop]` コマンド。カード有れば自動開始
- [x] Tab5 SD 配線確定（M5 BSP: SDMMC slot0 4bit、CLK43/CMD44/D0-3=39-42、LDO ch4）
- [ ] **⚠ SD マウントが通らない**（次セッションで HW 調査）。カードは CMD 応答するが
      データ線が全ゼロ(SCR=0)→ FR_NO_FILESYSTEM(13)。FAT32・4bit/1bit・LDO 有無すべて失敗。
      内蔵 LDO "voltage 0 out of [500,2700]" 警告。`logger.c` の mount_sd() コメントに
      調査手順（良品カード再確認 / SD VDD の給電経路 / 動作 M5 demo との差分）を記載
- [ ] マウント解決後: 実機で `log` の rows 増加 + CSV 内容確認

## 実機ブリングアップ 残（2アンテナ + 屋外が要る）
- [ ] **pitch / roll**。ベンチでは AttEuler が Do-Not-Use（2アンテナ収束が必要）。
      屋外で Pitch/Roll どちらが有効か（`setAttitudeOffset,90,0` 済み）
- [ ] `ReceiverStatus.rx_error`（ベンチ 0x8 = `ERROR: SW,`）が 2アンテナ屋外で消えるか
- [ ] **実圃場で `record perim`→走行→`record field`→`survey fit`→`vol`** の実測確認
- [ ] SBF 実効レート（msec100=10Hz 想定）を測位状態で再確認

## 基準局側（余裕のあるとき）
- [ ] BD982 が QZSS を出せるか。出せれば `1114` 追加で rover 側は無改造で恩恵

## メモ（ハマりどころ）
- タッチ ST7123: 報告テーブルを**最後の点まで**読まないと INT が clear されず
  座標が固定値になる（`main/touch.c` READ_LEN=全点）
- USB: SBF は **itf0 のみ**。itf2 は nmea_source。sweep に itf2 を入れると衝突して wedge
- NMEA stream 番号は SBF と別系。COM1 GGA=Stream2 でパネル NMEA(Stream1)と非衝突
- ビルド: `. ~/esp/esp-idf/export.sh && idf.py -p /dev/ttyACM0 build flash`
