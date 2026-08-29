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
- [x] **ポート毎 baud**（4800/9600/19200/38400/57600/115200、既定38400）を設定画面に追加。
      機材に合わせ port 毎に選択→NVS(`baud`/`baud1`)保存→`setCOMSettings,COMx,baudN` で反映。
      設定画面が縦に伸びたので scroll(縦)可に。`nmeaout` に baud 表示追加
- [x] `nmeaout` コンソールは両ポート表示に更新

### Phase C-3 — ★live コマンド不適用の根本原因 + provision 堅牢化 ✅（実機検証済み）
発覚: パネルで COM2 を ON→Apply しても受信機に反映されず、COM2 から NMEA が出なかった。
- [x] **原因特定**: Mosaic は SBF ストリーム開始後は USB1 のコマンドを無視する（$R: 無し）。
      現状の provision は setSBFOutput を先に送っていたため、後続の COM1/COM2/USB2 NMEA は
      毎回「効かない窓」で送られていた（RTCM3 は無関係と切り分け済み）
- [x] **provision 順序修正**: attitude→USB2/COM1/COM2 NMEA→setSBFOutput(最後)。全設定を
      静かな窓で適用（`mosaic_config.c`）
- [x] **per-command リトライ**(`send_acked`/PROVISION_TRIES): 電源投入直後の USB NAK
      (~10-15s settling) を最初のコマンドで吸収。実機で USB2/COM1/COM2/SBF 全て ack→latch 確認
- [x] **TX 直列化**(`s_tx_lock`): RTCM3 と コマンドの同一 EP 競合を防止
- [x] パネル Apply は NVS 保存のみ＋「power-cycle box to apply」。設定反映は**box 電源再投入**で。
      ⚠ソフト適用は不可と判明: `esp_restart`=USB 再列挙不完全、VBUS カット=mosaic wedge（両撤去）
- [x] **実機確認 済**: COM2(38400) の端子に `$…GGA` が出ることを確認（配線 OK）。
      注意: COM2 baud を 38400 に設定するため、受信側ターミナルも 38400 に合わせること

### Phase D — microSD CSV ロガー ✅（実機動作確認済み）
- [x] CSV ロガー実装（`main/logger.c`）: 1Hz で PVT/Att/cut-fill を
      `/sdcard/lvl_NNN.csv` に記録。`log [start|stop]` コマンド。カード有れば自動開始
- [x] Tab5 SD 配線確定（M5 BSP: SDMMC slot0 4bit、CLK43/CMD44/D0-3=39-42、LDO ch4）
- [x] **旧「SD マウント不可」は カード不良**だった。良品カードで `SD mounted USDU1 19073MB`。
      （LDO "voltage 0" 警告と SCR=0 ログは無害/別物＝C6 WiFi SDIO であり microSD 無関係）
- [x] **8.3 ファイル名修正**: `CONFIG_FATFS_LFN_NONE` のため `leveler_000`(11字)は fopen 失敗。
      `lvl_%03d.csv`(8.3準拠)に変更 → マウント後のファイル作成も成功
- [x] 実機: `sd=mounted logging=on rows=…増加 file=/sdcard/lvl_000.csv`、RTK fixed で記録継続を確認
- [ ] （任意）カードを PC で開いて CSV 内容の目視確認

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
