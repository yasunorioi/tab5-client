# 別のマシンで作業を始める手順

## 1. リポジトリを取る

**private リポジトリ**なので認証が要る。

```
gh auth login          # GitHub.com -> HTTPS -> Yes -> Login with a web browser
gh repo clone yasunorioi/tab5-client
cd tab5-client
```

`upstream`（tab5-caster）が付いていない場合は足す:

```
git remote add upstream https://github.com/yasunorioi/tab5-caster.git
git remote -v
```

> GitHub の fork ボタンは使っていない。**public repo の fork は private にできない**ため、
> clone を新しい private repo に push する形にしてある。

## 2. まず読む

`CLAUDE.md` → `docs/todo.md` の順。`docs/hardware-findings.md` は実機実測の一次情報で、
推測が混じっていないので信用してよい。

## 3. 実機が無くても進められる

`tests/fixtures/` に実機から採取した本物のバイト列がある。
**SBF パーサは受信機なしで全数検証できる**ので、そこから始めるのが効率的。

```
pwsh .\tools\parse-sbf.ps1   -Path .\tests\fixtures\mosaic-g5-p3h-sbf.bin   -Seconds 12.01
pwsh .\tools\parse-rtcm3.ps1 -Path .\tests\fixtures\eniwa-bd982-rtcm3.bin   -Seconds 8
```

期待される出力（どちらも CRC 失敗ゼロ・パディング無し）:

```
bytes: 20856   CRC-valid blocks: 354   CRC-failed: 0   frame bytes: 20856 / 20856
  4001  DOP              rev0 len=32   x114  (10 Hz)
  4007  PVTGeodetic      rev2 len=96   x114  (10 Hz)
  4014  ReceiverStatus   rev1 len=104  x12   ( 1 Hz)
  5938  AttEuler         rev0 len=44   x114  (10 Hz)

bytes: 3328    CRC-valid frames: 27   CRC-failed: 0   frame bytes: 3328 / 3328
  1006/1008/1033 x1 ずつ、1074/1094/1124 x8 ずつ (1 Hz)
```

## 4. ビルド環境（実機に焼く段階になったら）

ESP-IDF **5.4.4 以降**が必要。

- Windows: https://dl.espressif.com/dl/esp-idf/ のインストーラ
- `idf.py set-target esp32p4` → `idf.py build flash monitor`

現状はまだ caster のコードなので Zig 製 `ntripcaster` を `~/ntripcaster` に clone する
必要がある。`docs/todo.md` の 2 番（caster 半分の削除）を先にやれば不要になる。

## 5. 実機を繋ぐ場合

### 受信機（mosaic-go G5 P3H）

USB で繋ぐと CDC-ACM が2本生える。COM 番号を確認する:

```
Get-PnpDevice -Class Ports | ? InstanceId -like '*VID_152A*'
```

`MI_00` → **USB1**（データ用）、`MI_02` → **USB2**（コマンド用に使うと応答が読みやすい）。

```
pwsh .\tools\mosaic-cmd.ps1 -Port COM3 -Command getReceiverCapabilities
pwsh .\tools\mosaic-cmd.ps1 -Port COM3 -Command lstAsciiDisplay -Wait 3500
```

SBF を出させて採取する:

```
pwsh .\tools\mosaic-cmd.ps1 -Port COM3 -Command 'setAttitudeOffset, 90, 0'
pwsh .\tools\mosaic-cmd.ps1 -Port COM3 -Command 'setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP, msec100'
pwsh .\tools\capture-sbf.ps1 -Port COM4 -Seconds 12 -Out .\tests\fixtures\outdoor.bin
```

> 設定はすべて RAM のみ（`exeCopyConfigFile` していない）。電源再投入で消える。
> 受信機の NVM は意図的に触っていない。

### キャスター

認証不要・GGA 不要。

```
curl.exe -s -m 8 -H "Ntrip-Version: Ntrip/2.0" -H "User-Agent: NTRIP probe/0.1" `
  http://rtk.toiso.fit:2101/eniwa-bd982 -o cap.bin
pwsh .\tools\parse-rtcm3.ps1 -Path cap.bin -Seconds 8
```

ソーステーブルは `http://rtk.toiso.fit:2101/` を同じヘッダで GET すると出る。

## 6. 屋外で測位させたら確認すべき残件

`tests/fixtures/` のキャプチャは**アンテナ未接続の屋内**で採取したもの。
屋外で Fixed を得た状態で、以下を確認すること。

- [ ] **pitch → roll の切り替わり。** `setAttitudeOffset, 90, 0`（左右アンテナ配置の補正）を
      入れてあるが、ASCII 画面のラベルは静的で判定できなかった。
      **`AttEuler` ブロックの Pitch / Roll のどちらが Do-Not-Use でないか**を見る
- [ ] `lstAsciiDisplay` のステータス行に出ていた `ERROR: SW,` が消えるか
- [ ] 実際の垂直精度（案件の要求と突き合わせる数字）
