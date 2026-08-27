# tab5-client 設計方針

## 案件

トラクター装着型の**均平作業機**で RTK 測位し、M5Stack Tab5 の液晶に
**切土 / 盛土（cut / fill）** を表示する。オペレータが表示を見てブレードを操作する。

**ブレードの油圧自動制御はスコープ外**（表示のみ）。ただし後から足せる構造にしておく。

## 決定事項

| 項目 | 決定 | 理由 |
|---|---|---|
| アンテナ | **作業機のマストに左右2本** | heading + roll が取れる。均平で効く「ブレードの左右傾き」が直接分かる。刃先の真上に載るのでレバーアーム補正がほぼ不要 |
| 姿勢設定 | `setAttitudeOffset, 90, 0` | 左右（横）基線の補正 |
| 設計面 | **測量走行 → 水平面・土量バランス** | 圃場を一周して点群を集め、切土量と盛土量が釣り合う高さを自動算出。水田の均平では定番。Tab5 内で完結 |
| 高さの取得 | **SBF `PVTGeodetic` の楕円体高** | 圃場内の相対比較なのでジオイド不要。NMEA GGA は標高（ジオイド補正後）かつ桁数もモード情報も足りない |
| 補正源 | `http://rtk.toiso.fit:2101/eniwa-bd982`（自前 BD982） | 認証不要・GGA 不要・1Hz MSM4。実測で CRC ロスゼロ |
| 圃場での通信 | **スマホのテザリング**（Tab5 を STA で接続） | 圃場に WiFi は無い。3.3 kbps しか使わないので十分。LTE モジュール内蔵は USB-A が受信機で埋まるためハブが必要になり割に合わない |
| リポジトリ | tab5-caster の clone を **private** で運用 | GitHub の fork ボタンは public repo の fork を private にできない。clone を新 private repo に push する形にして `upstream` 追跡を残す |

## アーキテクチャ（tab5-caster からの組み替え）

```
 NTRIP caster ──(WiFi/テザリング)── C6 ─ SDIO ─┐
                                                ▼
                                    ntrip_client (de-chunk 必須)
                                                │ RTCM3
                                                ▼
 mosaic-G5 P3H ◄── USB-A (P4 = HS-OTG Host, CDC-ACM, USB1 へ write)
        │
        │ SBF (PVTGeodetic + AttEuler + DOP + ReceiverStatus)
        ▼
   sbf_source ──► 測位・姿勢
                     │
                     ├─► survey  : 点群収集 → 土量バランス平面の算出
                     ├─► guidance: 設計高 - 実測高 = cut/fill
                     ├─► LVGL 画面（大きい数値 + 色 + 平面図 + RTK ステータス）
                     └─► microSD ロガー
```

### 流用する（ほぼそのまま）

- `board_power.c` — **USB-A VBUS ゲート（PI4IOE5V6408 #2, I2C 0x44, P3）。必須**。
  これを叩かないと受信機が enumerate しない
- `display.c` / `esp_lcd_st7123.c` / `tab5_*_init.c` / LVGL / `touch.c`
- `wifi_sta.c` + C6 ESP-Hosted（接続先が変わるだけ）
- `backlight.c` — ブラウンアウト対策。トラクター電源なら余裕はあるが残す価値あり
- `web_server.c` — 圃場設定・設計面パラメータの UI に転用

### 書き換える

- `usb_cdc_source.c` — PID `0x8231`、itf `{0, 2}`、**双方向化**（RTCM3 write / SBF read）
- `nmea_source.c` — **SBF パーサに置換**
- `upstream.c` — NTRIP **server/push** → NTRIP **client**（GET）。骨格は流用可

### 落とす

- `rtcm_sink.c` / `rtcm_monitor.c` / `components/ntripcaster`（Zig caster）

### 新規

- SBF パーサ（PVTGeodetic / AttEuler / DOP / ReceiverStatus）
- 測量点群の収集と土量バランス平面の算出
- cut/fill 表示画面
- microSD ロガー

## 先に潰しておくべきリスク

### 1. 垂直精度 ★最重要

RTK の垂直誤差は水平の約2倍で、**実効 ±2cm 程度**。
**レーザーレベラー（±5mm）には及ばない。**
水田の均平なら実用範囲と思われるが、案件の要求仕様が「レーザー同等」なら成立しない。
**発注元と先に数字を突き合わせること。**

### 2. NTRIP の chunked 転送

キャスターは `Transfer-Encoding: chunked` で返す。
**素通しすると chunk サイズのヘッダ文字列が RTCM3 に混入し、CRC がランダムに落ちる**
（原因が掴みにくい壊れ方）。`esp_http_client` のストリーミングモードを使えば剥がしてくれる。
生ソケットで書くなら自前で de-chunk が必須。

### 3. 電源

- Tab5 の **HVIN は 6〜24V** なのでトラクターの 12V から直結できる。
  ただし**始動時サージ / ロードダンプ対策（DC-DC + TVS）は必須**
- USB-A の 5V はパネルバックライトと共有。受信機＋アンテナ分の余裕を持った電源にすること
  （tab5-caster の README 参照。実際にブラウンアウトで受信機がバスから落ちた実績あり）

### 4. 昼間の視認性

トラクター上で使うので、大きい数値・高コントラスト配色が要る。
Tab5 は 720x1280 の縦長パネル。

## 実機で確認が必要な残件

- **pitch → roll の切り替わり**。`setAttitudeOffset, 90, 0` を入れたが、ASCII 画面のラベルは
  静的なので判定できなかった。アンテナを繋いで測位させ、**AttEuler ブロックの
  Pitch / Roll のどちらが Do-Not-Use でないか**を見る
- ステータス行の `ERROR: SW,` が測位状態で消えるか
- SBF の実効レート（3秒窓で 24 ブロック = 約 8Hz だった。`msec100` 設定なので
  10Hz のはずで、キャプチャの端の影響と思われるが、測位状態で再確認）
