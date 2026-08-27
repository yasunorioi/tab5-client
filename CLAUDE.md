# tab5-client — 作業コンテキスト

## このプロジェクトは何か

トラクター装着型の**均平作業機**で RTK 測位し、M5Stack Tab5 (ESP32-P4) の液晶に
**切土 / 盛土（cut / fill）**を表示する機材。受信機は Septentrio **mosaic-go G5 P3H**、
補正は自前の Trimble BD982 基準局（`rtk.toiso.fit:2101/eniwa-bd982`）から NTRIP で取る。
ブレードの油圧自動制御はスコープ外（表示のみ）。

[tab5-caster](https://github.com/yasunorioi/tab5-caster)（基準局箱）の **clone** で、
コードはまだ caster のまま。`upstream` remote で追跡している。

## 応答は日本語で

ユーザーは日本語で作業している。コミットメッセージとドキュメントも日本語。
コード内のコメントは既存コード（英語）に合わせる。

## 最初に読むもの

1. **`docs/todo.md`** — 次にやること。ここから始める
2. `docs/hardware-findings.md` — 実機実測の一次情報。推測ではなく実際の応答
3. `docs/design.md` — 決定事項と組み替え方針
4. `docs/handoff.md` — 新しいマシンでの立ち上げ手順

## 絶対に踏んではいけない罠

### 1. この P3H は rover 専用で RTCM3 を出力できない

permission に `RTKBase` / `DGNSSBase` が無く、コマンドツリーに RTCM3 **出力**が存在しない
（`setRTCMv3Output` は `$R? Invalid command!`）。**基準局には使えない。**
「mosaic だから tab5-caster がそのまま動くはず」は誤り。RTCM3 は `setRTCMv3Usage` = 入力専用。

### 2. USB の値が tab5-caster のハードコードと違う

| | この個体 | tab5-caster のコード |
|---|---|---|
| PID | **`0x8231`** | `0x85C0` |
| CDC COM | **2本**（USB1=itf0 / USB2=itf2） | `{0, 2, 4}` の3本 |
| MSC (itf6) | **無し** | 有る前提 |
| itf0 | **コマンドに応答する** | 「両方向 silent」と README に記載 |

加えて `main/usb_cdc_source.c` の sweep ループに、`cdc_acm_host_open` 失敗時に `sweep` を
進めず `continue` するため**存在しない itf で無限リトライして他へ戻れないバグ**がある。
VID/PID は `usb_cdc_source.c` と `nmea_source.c` に**重複定義**されている（両方直すこと）。

### 3. NTRIP キャスターは `Transfer-Encoding: chunked` で返す

素通しすると chunk サイズのヘッダ文字列が RTCM3 に混入し、**CRC がランダムに落ちる**
（原因が掴みにくい壊れ方）。`esp_http_client` のストリーミングモードを使えば剥がしてくれる。
生ソケットで書くなら自前で de-chunk が必須。

### 4. 高さは NMEA GGA ではなく SBF `PVTGeodetic` の楕円体高を使う

圃場内の相対比較なのでジオイド不要。GGA は標高（ジオイド補正後）かつ桁数もモード情報も足りない。

### 5. USB-A の VBUS は I/O エキスパンダでゲートされている

`board_power.c`（PI4IOE5V6408 #2, I2C `0x44`, P3 = USB5V_EN, active-high）を叩かないと
受信機が enumerate しない。**この処理は消さないこと。**

## 実機が無くてもできること

`tests/fixtures/` に**実機から採取した本物のバイト列**がある（CRC 全数検証済み）。
SBF パーサと RTCM3 パーサはこれで完全に検証できるので、**受信機もキャスターも
ESP-IDF も無い状態で書き始められる**。詳細は `tests/fixtures/README.md`。

`tools/*.ps1` に採取・検証用スクリプトがある。実機がある場合はそれで再採取できる。

> ⚠ フィクスチャは**アンテナ未接続の屋内**で採取したもの。測位解は無効なので、
> フレーミングと CRC の検証には使えるが、**値の妥当性検証には使えない。**

## ビルド

ESP-IDF **5.4.4 以降**。

```
idf.py set-target esp32p4
idf.py build flash monitor
```

現状はまだ caster のコードなので、Zig 製 `ntripcaster` を `~/ntripcaster` に
別途 clone する必要がある（`components/ntripcaster` は vendoring していない）。
**tab5-client では caster を落とすので、この依存は消える予定**（`docs/todo.md` の 2 番）。

## 精度についての注意

RTK の垂直誤差は水平の約2倍で **実効 ±2cm 程度**。**レーザーレベラー（±5mm）とは別物。**
案件の要求仕様と突き合わせが済んでいない。ここが崩れると設計面の話が全部無駄になるので、
実装を進める前にユーザーに確認状況を聞くこと。
