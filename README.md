# tab5-client

**M5Stack Tab5 (ESP32-P4) + Septentrio mosaic-G5 P3H による、トラクター装着型
均平作業機向けの RTK 切土/盛土表示機。**

[tab5-caster](https://github.com/yasunorioi/tab5-caster)（基準局箱）の対になる車載側。

> ## ⚠ 現在の状態（2026-08-27）
>
> **コードはまだ tab5-caster のまま。** 設計と実機実測だけが `docs/` に入っている段階。
> 次にやることは [`docs/todo.md`](docs/todo.md) を見ること。
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
| [`docs/hardware-findings.md`](docs/hardware-findings.md) | **実機実測メモ。** P3H の USB 列挙・permission・SBF 構成、キャスターの実接続検証。すべて一次情報 |
| [`docs/design.md`](docs/design.md) | 設計方針、決定事項、tab5-caster からの組み替え、リスク |
| [`docs/todo.md`](docs/todo.md) | 次にやること |
| [`notes/memory/`](notes/memory/) | 作業メモの退避 |

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
