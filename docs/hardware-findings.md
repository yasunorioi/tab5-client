# 実機実測メモ（2026-08-27）

Windows PC に実機を接続して取得した一次情報。推測ではなく実際の応答。

## 1. Septentrio mosaic-go G5 P3H

`# RECEIVER: mosaic-G5 P3H-0100012323 (SEPT) - firmware: 1.0.0`

### USB 列挙（tab5-caster の想定と異なる）

| | 実測値 | tab5-caster のハードコード |
|---|---|---|
| VID | `0x152A` | `0x152A` ✔ |
| PID | **`0x8231`** | `0x85C0` ✘ |
| CDC COM | **2本**: `MI_00`=USB1, `MI_02`=USB2 | `{0, 2, 4}` の3本 ✘ |
| MSC (itf6) | **無し** | 有る前提 |
| itf0 の応答 | **コマンドに応答する** | 「両方向 silent」と記載 ✘ |

Windows 上では `MI_00`→COM4(=USB1)、`MI_02`→COM3(=USB2)。

### この個体は基準局にできない（rover 専用）

```
getReceiverCapabilities
  ReceiverCapabilities, Main+Aux1,
    GPSL1CA+GPSL2PY+GPSL2C+GPSL5+GPSL1C+GLOL1CA+GLOL2P+GLOL2CA+GLOL3
    +GALE1BC+GALE6BC+GALE5a+GALE5b+GEOL1+GEOL5
    +BDSB1I+BDSB2I+BDSB3I+BDSB1C+BDSB2a+BDSB2b
    +QZSL1CA+QZSL2C+QZSL5+QZSL6+QZSL1C+QZSL1S+QZSL1CB+NAVICL5,
    DSK1+COM1+COM2+USB1+USB2,
    DGNSSRover+RTKRover+RTCMv3x+xPPSOutput+TimedEvent
    +InternalLogging+APME+RAIM+IM+DSK1+GalOSNMA, 50, 50

getPVTMode
  PVTMode, Rover, StandAlone+DGNSS+RTKFixed
```

- permission に **`RTKBase` / `DGNSSBase` が無い**
- `help` のコマンドツリーに RTCM3 **出力**が存在しない（`ioOutput` は SBF / NMEA / echo のみ）。
  RTCM3 は `ioInput → tabDiffCorrInRTCM3 → setRTCMv3Usage` = **入力専用**
- `setRTCMv3Output, USB1, ...` → `$R? ... : Invalid command!`
- 基準局に要る `setStaticPosition` 相当も無い（`tabStationInfo` は marker/observer のみ）
- access level は無関係（`DefaultAccessLevel, User, User`、未ログイン状態で確認）

→ **tab5-caster の給餌元にはできない。rover 専用。**

### rover として使える機能

- `GNSSAttitude, MultiAntenna, Fixed` — デュアルアンテナ姿勢が有効
- `DataInOut` 全ポート `auto, SBF+NMEA, (on)` → **RTCM3 を流し込むだけで自動認識。入力側の設定コマンド不要**
- `getRTCMv3Usage` のデフォルト受入リストに、後述の基準局が出す
  `1074 / 1094 / 1124 / 1006 / 1008 / 1013 / 1033` が**全部含まれている**
- capability に `QZSL6` はあるが firmware 1.0.0 では CLAS 非対応（今後の SW 更新待ち）
- 未確認: ステータス行に `ERROR: SW,` が出ていた。アンテナ未接続の屋内放置状態なので、
  測位させた状態で消えるか要確認

### SBF 出力の実測（パーサはこれに対して書く）

```
setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP, msec100
```

USB1 を 3 秒キャプチャして CRC 全数検証した結果:

```
captured: 4336 B / 3.0s  (≒1.4 kB/s)
CRC-valid blocks: 74   CRC-failed: 0
  4007 PVTGeodetic     rev2  len=96   x24
  5938 AttEuler        rev0  len=44   x24
  4001 DOP             rev0  len=32   x24
  4014 ReceiverStatus  rev1  len=104  x2   (これだけ約1Hz)
```

- フレーミング: sync `$@` (0x24 0x40), CRC(u16), ID(u16), Length(u16)。
  ID の下位 13bit がブロック番号、上位 3bit がリビジョン。Length は 4 の倍数で
  ヘッダを含む全長
- CRC は **CRC-16-CCITT (poly 0x1021, init 0)** を **ID 以降**に掛ける。実測で失敗ゼロ
- 1.4 kB/s なので ESP32-P4 には無負荷

### 適用済みの設定（すべて RAM のみ・電源再投入で消える）

```
setAttitudeOffset, 90, 0     # 左右アンテナ配置の補正
setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP, msec100
```

`exeCopyConfigFile` していないので受信機の NVM は無傷。
tab5-caster と同じ「箱が設定の source of truth」方式で、Tab5 側から毎起動プロビジョニングする。

## 2. 基準局 / NTRIP キャスター

`http://rtk.toiso.fit:2101/eniwa-bd982` (Trimble BD982)

### ソーステーブル

```
Server: NTRIP NtripCaster/0.5.0    Ntrip-Version: Ntrip/2.0

STR;/TAB5;/TAB5;RTCM 3.2;1033(101),1230(101),1097(561),1044(302),1087(1042),
    1117(1064),1077(831),1127(914),1046(301),1107(1083),1042(302),1137(1087),
    1006(101),1019(300),1020(255);;;;;;;;;N;N;0;;
STR;/eniwa-bd982;/eniwa-bd982;RTCM 3.2;1074(53332),1124(53332),1033(5333),
    1006(5333),1008(5334),1013(5333),1094(53332);;;;;;;;;N;N;0;;
```

`/TAB5` は tab5-caster 箱が push しているマウントポイント（provisioning 文字列と完全一致）。

### 実接続の検証

| 項目 | 結果 |
|---|---|
| 認証 | **不要**。クレデンシャルなしで `200 OK` |
| GGA 送信 | **不要**。送らずにデータが流れた（single-base） |
| 転送 | **`Transfer-Encoding: chunked`** ← 実装上の要注意点 |
| 帯域 | **416 B/s ≒ 3.3 kbps** → 1時間で約 1.5 MB |
| 品質 | 3328 バイト**全部**が CRC 有効フレーム。ロス・ゴミゼロ |

```
CRC-valid frames: 27   CRC-failed: 0   (frame bytes 3328 / 3328)
  1006  ARP + antenna height          x1
  1008  Antenna descriptor + serial   x1
  1033  Receiver/antenna descriptor   x1
  1074  GPS MSM4                      x8   ← 1 Hz
  1094  Galileo MSM4                  x8   ← 1 Hz
  1124  BeiDou MSM4                   x8   ← 1 Hz
```

### 基準局側の改善余地

**GLONASS と QZSS を出していない。** GPS+GAL+BDS で衛星数自体は足りるが、
みちびきは高仰角で入るので樹木・建物際のある圃場では効く。
rover 側は QZSS 対応済み（capability に `QZSL1CA/L2C/L5/L6/L1C/L1S/L1CB`、
受入リストに `1111`〜`1117`）なので、**BD982 が QZSS を出せるなら `1114` を足すだけで
rover 側は無改造で恩恵を受ける**。BD982 の世代次第なので要確認。

## 3. 開発環境（この PC）

- git / Python — あり
- **ESP-IDF 5.4.4 — `~/esp/esp-idf` に導入済み**（toolchain riscv32-esp-elf、cmake/ninja は
  python_env の pip 経由で PATH に乗る）。ビルド: `. ~/esp/esp-idf/export.sh && idf.py ...`
- Zig — 不要（caster 除去済み）

## 4. 実機ブリングアップで判明（2026-08、Tab5 + P3H, /dev/ttyACM0）

### 測位
- 0x8231 enumerate → itf0 で SBF latch、crc_fails=0、NTRIP→**RTK fixed** 到達
- **hAcc 0.020 m / vAcc 0.040 m**（＝水平±2cm・垂直その倍、設計想定通り）
- SBF 実効 ~10Hz（`msec100`）で安定

### USB sweep（重要）
- **SBF は USB1=itf0 のみ**。sweep に itf2 を入れると、itf2 を開いている `nmea_source` と
  衝突して "EP with 4 address already allocated" → USB ホストが wedge し fix が戻らない。
  → `usb_cdc_source.c` の `MOSAIC_COM_ITFS` は `{0}` 固定

### タッチ（ST7123 @ 0x55）
- Tab5(新版)のタッチは **ST7123**（GT911 ではない）。
- **報告テーブルを最後の点まで読まないと INT が clear されず座標が固定値**になる
  （`touch.c` READ_LEN=全点=74B）。指の有無ビットは正しく取れるので、活動検出だけなら
  1点読みでも動くが、座標は死ぬ
- raw 座標は **0..~720（左→右）/ 0..~1280（上→下）**、720×1280 パネルに **1:1（swap/flip 無し）**

### microSD（⚠未解決）
- 配線は M5 公式 BSP: SDMMC slot0 **4bit / CLK=43 CMD=44 D0-3=39-42 / 電源 on-chip LDO ch4**
- 現象: カードは **CMD 応答（CID: MANF 0x92 読取）するがデータ線読み取りが全ゼロ**
  （`SCR: sd_spec=0 bus_width=0`）→ `FR_NO_FILESYSTEM(13)`。FAT32 化後も 4bit/1bit・
  LDO 有無すべて失敗。内蔵 LDO は "voltage 0 out of [500,2700]" 警告
- = 給電されコマンドは通るがデータ転送が失敗。**Tab5 SD の電源/信号統合**の問題（要 HW 調査）

### その他
- P4 は**単精度 FPU・倍精度ソフトfloat**。地図/土量の MLS 補間を double で回すと
  再描画が ~5s → task watchdog。→ float 化（`fieldmap.c`）
