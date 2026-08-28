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

### Phase A — 平面図（plan-view MAP）画面
docs/design.md の画面仕様「大きい数値 + 色 + **平面図** + RTK ステータス」の平面図部分。
- [ ] LVGL キャンバスに **圃場境界ポリゴン**を描画（fieldmap の boundary）
- [ ] **cut/fill ヒートマップ**（グリッド各セルを CUT=橙/FILL=青/ON GRADE=緑で塗る）
- [ ] **現在位置**マーカー（PVT を E/N に投影して重畳）
- [ ] **土量合計**の表示（cut/fill/net m³ + 面積）
- [ ] E/N → 画面座標のスケーリング（境界 bbox にフィット、縦横比維持）

### Phase B — on-panel ボタン + 画面遷移
今 record/vol はコンソールのみ。作業画面 ⇄ MAP 画面をボタンで切替。
- [ ] `Rec 外周` / `Rec 内部` / `停止` ボタン（leveler_record_set）
- [ ] `土量計算` ボタン（leveler_compute_volumes）
- [ ] 作業画面 ⇄ MAP ⇄ 設定 のナビゲーション（LVGL screen 切替 or tabview）

### Phase C — NMEA 出力設定画面（NVS 永続化）
液晶で message(GGA/GSV/GSA/RMC/VTG/ZDA)×port(COM1/USB2/OFF)×rate(OFF/1/2/5/10Hz)。
- [ ] 設定 UI（行ごとに port + rate）
- [ ] NVS 保存 → `mosaic_provision` が起動時に再送（今の COM1 GGA ハードコードを置換）

### Phase D — microSD ロガー
- [ ] PVT/Att + cut/fill + 土量を microSD に記録（作業ログ・証跡）

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
