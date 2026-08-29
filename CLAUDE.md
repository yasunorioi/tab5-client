# tab5-client — 作業コンテキスト

## このプロジェクトは何か

トラクター装着型の**均平作業機**で RTK 測位し、M5Stack Tab5 (ESP32-P4) の液晶に
**切土 / 盛土（cut / fill）**と圃場の**平面図・土量**を表示する機材。受信機は Septentrio
**mosaic-go G5 P3H**、補正は自前の Trimble BD982 基準局
（`rtk.toiso.fit:2101/eniwa-bd982`）から NTRIP で取る。ブレードの油圧自動制御はスコープ外
（表示のみ）。

[tab5-caster](https://github.com/yasunorioi/tab5-caster)（基準局箱）の **clone** から派生。
caster 半分は削除済み。`upstream` remote で tab5-caster を追跡している。

## 現在の状態（2026-08）

**実機（Tab5 + P3H, /dev/ttyACM0）で通し動作している。** ESP-IDF 5.4.4/esp32p4 で
クリーンビルド。

- USB→SBF パース→cut/fill / NTRIP client→受信機へ RTCM3 → **RTK fixed**（hAcc 2cm/vAcc 4cm）
- **3画面タッチ UI**: 作業画面（cut/fill 大数字＋ライトバー＋録画/平面ボタン）⇄
  平面図 MAP（境界＋cut/fill ヒートマップ＋現在位置＋土量）⇄ NMEA 設定（COM1 出力、NVS 保存）
- 外周走行→バランス平面→**切土/盛土 m³** まで算出
- COM1/COM2 から RS232 で GGA/NMEA を外部機器へ（ポート毎に ON/OFF＋message×rate＋baud、
  設定は液晶＋NVS。COM1 既定 ON / COM2 既定 OFF）

- microSD CSV ロガー**動作**（旧マウント不可はカード不良。良品カードで mount 成功、
  `/sdcard/lvl_NNN.csv` に記録。8.3 名必須＝`CONFIG_FATFS_LFN_NONE`）

**未解決/残:**
- 実機ブリングアップ残: 2アンテナ屋外での pitch/roll・`rx_error`、実圃場での `record`→`vol`

## 応答は日本語で

ユーザーは日本語で作業している。コミットメッセージとドキュメントも日本語。
コード内のコメントは既存コード（英語）に合わせる。

## 最初に読むもの

1. **`docs/todo.md`** — 次にやること（phase 区切り）。ここから始める
2. `docs/design.md` — 決定事項・アーキテクチャ・モジュール地図・コンソール/画面リファレンス
3. `docs/hardware-findings.md` — 実機実測の一次情報（P3H の USB/permission/SBF、タッチ、SD）
4. `docs/handoff.md` — 別マシンでのビルド/焼き/検証手順

## 絶対に踏んではいけない罠 / 設計上の要点

### 1. この P3H は rover 専用で RTCM3 を出力できない
permission に `RTKBase`/`DGNSSBase` が無く、RTCM3 は `setRTCMv3Usage`＝**入力専用**。
基準局には使えない。RTCM3 は NTRIP client から USB 経由で受信機へ**流し込む**だけ。

### 2. USB: SBF は itf0 のみ。sweep に itf2 を足すな
この個体は **PID 0x8231 / CDC 2本（USB1=itf0=SBF, USB2=itf2=NMEA）/ MSC 無し**。
SBF は itf0 にだけ出る。`usb_cdc_source.c` の sweep に itf2 を入れると **nmea_source が
開いている itf2 と衝突**して "EP already allocated" で USB ホストが wedge し、fix が
二度と戻らなくなる（実際に踏んだ）。VID/PID は `main/mosaic_usb.h` に一本化済み。

### 3. NTRIP キャスターは `Transfer-Encoding: chunked`
素通しすると chunk サイズ文字列が RTCM3 に混入し CRC がランダムに落ちる。
`ntrip_client.c` は `esp_http_client` のストリーミングで de-chunk 済み。

### 4. 高さは NMEA GGA ではなく SBF `PVTGeodetic` の楕円体高
圃場内の相対比較なのでジオイド不要。cut/fill・土量はすべてこの楕円体高基準。

### 5. USB-A の VBUS は I/O エキスパンダでゲート
`board_power.c`（PI4IOE5V6408 #2, I2C `0x44`, P3=USB5V_EN, active-high）。**消すな。**

### 6. ST7123 タッチは報告テーブルを最後の点まで読む
`touch.c` の READ_LEN は全点（74B）。1点だけ読むと INT が clear されず座標が固定値に
なる（较正で発覚）。raw 座標は 720×1280 パネルに 1:1（swap/flip 無し）。

### 7. 地図/土量の MLS 補間は float
esp32p4 は単精度 FPU・倍精度ソフトfloat。`fieldmap.c` の MLS を double で回すと
地図再描画が ~5s かかり **task watchdog** を踏む。float 化済み（ホスト検証は誤差内で一致）。

### 8. NMEA stream 番号は SBF と別系
`setNMEAOutput` のストリームは SBF と独立。パネル NMEA=Stream1、COM1 出力=Stream2、
COM2 出力=Stream3 で非衝突（`mosaic_config.c`）。COM1/COM2 は各々 ON/OFF＋message×rate＋baud
(4800..115200) を設定画面で独立設定し NVS 保存・毎起動再送（COM1 既定 ON / COM2 既定 OFF、
baud 既定 38400。baud は機材に合わせ port 毎）。

### 9. ★Mosaic は「SBF ストリーム開始前の静かな窓」でしかコマンドを適用しない
RTK 稼働中（SBF が USB1 を流れている）に送った `setNMEAOutput`/`setSBFOutput` は **$R: を
返さず無視される**（実機で確認：rate 変更も none 停止も効かない。RTCM3 の有無は無関係）。
効くのは受信機がまだ SBF を出していない起動直後だけ。従って:
- `mosaic_provision()` は **attitude→USB2/COM1/COM2 NMEA→setSBFOutput(最後)** の順。
  ストリーム開始を最後にし、全設定を静かな窓で適用する（順序を変えるな）。
- 各 provision コマンドは **ack するまでリトライ**（`send_acked`, PROVISION_TRIES）。
  電源投入直後 ~10-15s は USB OUT が NAK してコマンドが落ちるため、最初のコマンドで
  settling を吸収する。
- **設定変更は box の電源再投入でのみ反映**（パネル Apply は NVS 保存だけ→「power-cycle
  box to apply」表示）。⚠ソフト再起動での適用は不可: `esp_restart` は USB を綺麗に
  再列挙できず、VBUS カット(`board_usb_5v_en`)は mosaic を wedge させた（両方失敗、撤去済み）。
  唯一確実なのは物理電源再投入（＝mosaic のコールドブート）。
- `usb_cdc_write`(RTCM3) と `usb_cdc_send_command` は同一 OUT EP を別タスクから叩くので
  `s_tx_lock` で直列化（`cdc_acm_host_data_tx_blocking` は非 re-entrant）。

## 実機が無くてもできること

`tests/fixtures/` に実機採取のバイト列（CRC 全数検証済み）。純ロジックはホストの gcc で
全数/解析検証できる:

```
cc -Imain tools/sbf_selftest.c     main/sbf_parser.c  -lm && ./a.out tests/fixtures/mosaic-g5-p3h-sbf.bin
cc -Imain tools/cutfill_selftest.c main/cutfill.c     -lm && ./a.out
cc -Imain tools/fieldmap_selftest.c main/fieldmap.c   -lm && ./a.out
python3 tools/parse_sbf.py tests/fixtures/mosaic-g5-p3h-sbf.bin --dump 1
```

> ⚠ フィクスチャは**屋内・アンテナ未接続**で採取。測位解は無効なので framing/CRC/
> デコード配置の検証には使えるが、値の妥当性検証には使えない。

## ビルド / 焼き

ESP-IDF **5.4.4**（この開発機では `~/esp/esp-idf` に導入済み）。Zig 依存は無い
（caster 除去済み）。

```
. ~/esp/esp-idf/export.sh
idf.py set-target esp32p4      # 初回のみ
idf.py -p /dev/ttyACM0 build flash monitor
```

コンソール（USB-Serial-JTAG, `tab5>`）主要コマンド: `stats`/`sbf`/`usb`/`nmea`/`mosaic`/
`survey`/`record`/`vol`/`flat`/`fit`/`cutfill`/`screen`/`demofield`/`ntrip`/`nmeaout`/`log`/
`wifiset`/`touch`。詳細は `docs/design.md`。

## 精度についての注意

RTK の垂直誤差は水平の約2倍で **実効 ±2cm 程度**（実機で vAcc 4cm 実測）。
レーザーレベラー（±5mm）とは別物。**発注元と ±2cm で十分と合意済み。**
