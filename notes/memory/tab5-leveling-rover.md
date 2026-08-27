---
name: tab5-leveling-rover
description: 進行中案件 — M5Stack Tab5 + mosaic-G5 P3H でトラクター装着型均平作業機の RTK 高さ表示機を作る
metadata:
  type: project
---

2026-08-27 開始。トラクター装着型の均平作業機で RTK 測位し、Tab5 の液晶に切土/盛土を表示する案件。
[[tab5-caster-hw-quirks]] のキャスター版から rover 版へ方向転換（理由は [[mosaic-g5-p3h-rover-only]]）。

決定事項:
- **アンテナは作業機のマストに左右2本** → heading + roll。均平で効くブレードの左右傾きが直接取れる。`setAttitudeOffset, 90, 0` を適用（左右基線の補正）。pitch→roll の切替は実際に測位させて AttEuler で要確認。
- **設計面は測量走行 → 水平面・土量バランス**（切土量と盛土量が釣り合う高さを自動算出）。
- **補正は自前基準局**。ただし P3H は base 不可なので基準局用受信機の手当てが別途必要。
- ブレードの油圧自動制御はスコープ外（表示のみ）。

技術的な確定事項:
- 高さは NMEA GGA ではなく **SBF `PVTGeodetic` の楕円体高**を使う（圃場内の相対比較なのでジオイド不要、GGA は桁数もモード情報も足りない）。
- 実測した SBF 構成: `setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP, msec100` で **約1.4 kB/s**。ブロックは PVTGeodetic(4007,rev2,96B) / AttEuler(5938,rev0,44B) / DOP(4001,rev0,32B) / ReceiverStatus(4014,rev1,104B)。CRC は CRC-16-CCITT(poly 0x1021, init 0) を ID 以降に掛ける形で検証済み。
- 圃場に WiFi が無いのでスマホのテザリング経由で NTRIP client を繋ぐ方針。
- Tab5 の HVIN は 6-24V でトラクター 12V 直結可。ただし始動サージ / ロードダンプ対策が要る。
- **垂直精度は実効 ±2cm 程度**でレーザーレベラー(±5mm)には及ばない。発注元の要求仕様と要突き合わせ。

**Why:** 精度要求と基準局の手当てが、着手前に潰しておくべき2大リスク。
**How to apply:** 実装は tab5-caster の board_power / display / wifi_sta / touch を流用し、rtcm_sink と Zig caster は落とす。
