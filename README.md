# tab5-client

**M5Stack Tab5 (ESP32-P4) + Septentrio mosaic-G5 P3H による、トラクター装着型
均平作業機向けの RTK 切土/盛土表示機。**

[tab5-caster](https://github.com/yasunorioi/tab5-caster)（基準局箱）の対になる車載側。

> ## 現在の状態（2026-08）
>
> **実機（Tab5 + P3H）で通し動作。** ESP-IDF 5.4.4/esp32p4 でクリーンビルド。
> USB→SBF パース→cut/fill、NTRIP client→受信機へ補正→**RTK fixed**（hAcc 2cm/vAcc 4cm）。
> **3画面タッチ UI**（作業 ⇄ 平面図MAP ⇄ NMEA設定）、外周走行→バランス平面→**切土/盛土 m³**、
> COM1 から RS232 で外部機器へ NMEA 出力（液晶＋NVS 設定）まで実装・実機確認済み。
>
> **残**: microSD ロガーはコード完成だが SD マウントが HW blocker で未解決。実機は
> 2アンテナ屋外での pitch/roll・実圃場での土量測定が残（[`docs/todo.md`](docs/todo.md)）。
>
> 純ロジック層（SBF パーサ / cut-fill / 土量）は受信機も ESP-IDF も無しに
> `tools/*_selftest.c` をホストの gcc でビルドして全数/解析検証できる。
>
> このリポジトリは tab5-caster の **clone**（fork ボタンではない。GitHub の fork は
> public repo を private にできないため）。upstream 追跡は残してある:
>
> ```
> git remote -v
>   upstream  https://github.com/yasunorioi/tab5-caster.git
> ```

## ドキュメント

| | |
|---|---|
| [`CLAUDE.md`](CLAUDE.md) | **作業コンテキスト。** 踏んではいけない罠がまとまっている。最初に読む |
| [`docs/todo.md`](docs/todo.md) | 次にやること |
| [`docs/handoff.md`](docs/handoff.md) | **別のマシンで作業を始める手順。** clone からフィクスチャ検証、実機の繋ぎ方まで |
| [`docs/hardware-findings.md`](docs/hardware-findings.md) | **実機実測メモ。** P3H の USB 列挙・permission・SBF 構成、キャスターの実接続検証。すべて一次情報 |
| [`docs/design.md`](docs/design.md) | 設計方針、決定事項、tab5-caster からの組み替え、リスク |
| [`tests/fixtures/`](tests/fixtures/) | **実機から採取した本物のバイト列。** 受信機もキャスターも ESP-IDF も無い環境でパーサを全数検証できる |
| [`tools/`](tools/) | 受信機との対話・採取・CRC 検証スクリプト（PowerShell） |
| [`notes/memory/`](notes/memory/) | 作業メモの退避 |

## 実機が無くても始められる

```
pwsh .	ools\parse-sbf.ps1   -Path .	estsixtures\mosaic-g5-p3h-sbf.bin -Seconds 12.01
pwsh .	ools\parse-rtcm3.ps1 -Path .	estsixtures\eniwa-bd982-rtcm3.bin -Seconds 8
```

どちらも CRC 失敗ゼロ・パディング無しで通る。SBF パーサはこれで完全に検証できる。

## 押さえておくべき3点

1. **この P3H は rover 専用で RTCM3 を出力できない。** permission に `RTKBase` が無く、
   コマンドツリーに RTCM3 出力が存在しない。基準局には使えない
   → 補正は自前の BD982（`rtk.toiso.fit:2101/eniwa-bd982`）から取る

2. **USB PID は `0x8231`、CDC COM は2本（USB1=itf0 / USB2=itf2）。**
   tab5-caster のハードコード（`0x85C0`、itf `{0,2,4}`）とは違う。
   加えて sweep ループに、存在しない itf で無限リトライするバグがある

3. **NTRIP キャスターは `Transfer-Encoding: chunked` で返す。**
   素通しすると RTCM3 に chunk ヘッダが混入して CRC がランダムに落ちる

## 精度についての注意

RTK の垂直誤差は水平の約2倍で **実効 ±2cm 程度**。
**レーザーレベラー（±5mm）とは別物。** 案件の要求仕様と要突き合わせ。
