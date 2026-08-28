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

## アーキテクチャ（実装済み）

```
 NTRIP caster ──(WiFi/テザリング)── C6 ─ SDIO ─ ntrip_client.c (de-chunk)
                                                     │ RTCM3
                                                     ▼
 mosaic-G5 P3H ◄── USB-A (P4 HS-OTG Host, CDC-ACM) ── usb_cdc_write() [itf0]
        │
        │ SBF (PVTGeodetic + AttEuler + DOP + ReceiverStatus) [itf0]
        ▼
 usb_cdc_source.c ──► gnss_state.c (SBF パーサ状態) ──┐
                                                       ├─► leveler.c ─┬─ cutfill.c  (バランス平面/delta)
   (COM1 → RS232 外部機器へ GGA/NMEA)                  │              └─ fieldmap.c (外周/点群/土量 m³)
                                                       └─► 画面 (下記)
```

### モジュール地図

**データ経路**
- `usb_cdc_source.c` — USB Host CDC。**SBF は itf0 のみ**読む（sweep に itf2 を入れない）。
  `usb_cdc_write()` で NTRIP の RTCM3 を受信機へ流す
- `gnss_state.c` — SBF パーサ状態の thread-safe ホルダ（sbf_parser.c を内包）
- `sbf_parser.c` — ESP-IDF 非依存の SBF ストリーミングパーサ（ホスト検証可）
- `cutfill.c` — ローカル ENU 投影 + 最小二乗バランス平面 + delta（純ロジック）
- `fieldmap.c` — 境界ポリゴン + 点群 + **MLS 補間で切土/盛土 m³**（MLS は float）
- `leveler.c` — 上記を実 PVT から駆動（survey/record/plane/delta/volume の統合）
- `ntrip_client.c` — 基準局から RTCM3 を de-chunk して受信機へ
- `mosaic_config.c` / `mosaic_usb.h` — 毎起動プロビジョニング（SBF/NMEA/COM1）と VID/PID
- `logger.c` — microSD CSV ロガー（⚠SD マウント未解決）

**UI（LVGL, 3画面）**
- `status_screen.c` — 作業画面 + タッチ indev登録 + 画面遷移の親
- `map_view.c` — 平面図 MAP（キャンバス直描画）
- `settings_view.c` — NMEA 出力設定（NVS）
- `touch.c` — ST7123 タッチ（**報告テーブル全点読み**）+ LVGL 用キャッシュ
- 流用: `display.c`/`esp_lcd_st7123.c`/`tab5_*_init.c`/`backlight.c`/`gnss_view.c`

**ネット/その他（caster から流用）**
- `wifi_sta.c`（C6 ESP-Hosted, GOT_IP で ntrip/web 起動）/ `net_mdns.c` / `web_server.c`（status UI）
- `board_power.c`（USB-A VBUS ゲート・必須）/ `debug_console.c`

**削除済み**: `rtcm_sink.c` / `rtcm_monitor.c` / `upstream.c` / `components/ntripcaster`（Zig）

### コンソール / 画面リファレンス

コンソール（`tab5>`、USB-Serial-JTAG）:
| 分類 | コマンド |
|---|---|
| 測位/SBF | `stats` `sbf` `usb` `nmea` `touch` |
| 受信機 | `mosaic <cmd>` `nmeaout`（COM1設定表示）`ntrip`/`ntripset`/`ntripreset` |
| 均平 | `survey [add\|clear\|fit]` `record <perim\|field\|stop>` `vol` `flat` `cutfill` |
| 画面/検証 | `screen <work\|map>` `demofield`（合成圃場）`log [start\|stop]` |
| 設定 | `wifiset` `wifireset` `webadmin` |

画面遷移: **作業**（cut/fill 大数字＋縦ライトバー＋Perim/Field/Stop・Flat/Fit/Clear・Map>）
⇄ **MAP**（境界＋cut/fill ヒートマップ＋現在位置＋土量、< Work / Vol / Cfg>）
→ **設定**（COM1 NMEA の message×rate、< Work / Apply）

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

## 実機で確認済み（2026-08）

- 0x8231 enumerate → itf0 SBF latch、crc_fails=0、NTRIP→**RTK fixed**（hAcc 2cm/vAcc 4cm）
- 3画面タッチ UI 全操作、record→vol の配線、COM1 NMEA 設定の保存・受信機反映
- SBF 実効レート ~10Hz（`msec100`）で安定

## 実機で確認が必要な残件（2アンテナ + 屋外 or HW）

- **pitch → roll の切り替わり**。ベンチでは AttEuler が Do-Not-Use（2アンテナ収束が要る）。
  屋外で測位させ、AttEuler の Pitch/Roll どちらが有効か（`setAttitudeOffset,90,0` 済み）
- `ReceiverStatus.rx_error`（ベンチで 0x8 = `ERROR: SW,`）が 2アンテナ屋外で消えるか
- **microSD マウント**（HW blocker）: カードは CMD 応答するがデータ線ゼロ（`logger.c` 参照）
- 実圃場で `record perim`→走行→`record field`→`survey fit`→`vol` の実測
