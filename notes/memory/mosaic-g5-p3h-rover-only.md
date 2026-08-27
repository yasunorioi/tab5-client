---
name: mosaic-g5-p3h-rover-only
description: 手持ちの Septentrio mosaic-go G5 P3H は rover 専用で RTCM3 を出力できない。USB PID と CDC 構成も tab5-caster の想定と異なる
metadata:
  type: project
---

2026-08-27 に実機（`mosaic-G5 P3H-0100012323`, firmware 1.0.0）を Windows に繋いで確認した結果:

- **基準局にできない。** `getReceiverCapabilities` の permission は `DGNSSRover+RTKRover+RTCMv3x+…` で `RTKBase`/`DGNSSBase` が無い。`getPVTMode` は `Rover` 固定。`help` のコマンドツリーに RTCM3 **出力**系が一切存在せず（`ioOutput` は SBF/NMEA/echo のみ）、RTCM3 は `setRTCMv3Usage`＝**入力**のみ。`setRTCMv3Output` は `$R? Invalid command!`。access level の問題ではない（DefaultAccessLevel=User、未ログインで確認）。
- **USB PID = 0x8231**（VID 0x152A）。[[tab5-caster-hw-quirks]] の `0x85C0` は別個体の値。
- **CDC-ACM COM は2つだけ**: `MI_00` = **USB1**、`MI_02` = **USB2**。itf4 も MSC(itf6) も無い。itf0 はコマンドに応答する。
- 使えるもの: デュアルアンテナ姿勢（`GNSSAttitude, MultiAntenna, Fixed`）、RTCM3 入力フルセット、`DataInOut` 全ポート auto（補正を流すだけで自動認識）、SBF 出力。
- capability に `QZSL6` はあるが firmware 1.0.0 では CLAS 非対応（今後の SW 更新待ち）。

**Why:** この1点で用途が決まる。基準局用途で買い足す前に必ず permission を確認すること。
**How to apply:** 基準局が要る場面ではこの個体を当てにしない。rover 用途（[[tab5-leveling-rover]]）では制約なし。
