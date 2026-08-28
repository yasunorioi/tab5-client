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

`CLAUDE.md` → `docs/todo.md` → `docs/design.md`（モジュール地図・コマンド一覧）の順。
`docs/hardware-findings.md` は実機実測の一次情報。

## 3. 実機が無くても進められる（純ロジックはホスト検証）

`tests/fixtures/` に実機採取のバイト列。パーサ/土量ロジックは受信機も ESP-IDF も無しに
ホストの gcc/python で全数・解析検証できる:

```
cc -Imain tools/sbf_selftest.c      main/sbf_parser.c -lm && ./a.out tests/fixtures/mosaic-g5-p3h-sbf.bin
cc -Imain tools/cutfill_selftest.c  main/cutfill.c    -lm && ./a.out
cc -Imain tools/fieldmap_selftest.c main/fieldmap.c   -lm && ./a.out
python3 tools/parse_sbf.py tests/fixtures/mosaic-g5-p3h-sbf.bin --seconds 12.01
```

SBF の期待出力（CRC 失敗ゼロ・パディング無し）:

```
bytes: 20856   CRC-valid blocks: 354   CRC-failed: 0   frame bytes: 20856 / 20856
  4001 DOP x114 / 4007 PVTGeodetic x114 / 4014 ReceiverStatus x12 / 5938 AttEuler x114
```

（Windows で `pwsh .\tools\parse-sbf.ps1` / `parse-rtcm3.ps1` も同結果。RTCM3 は
`eniwa-bd982-rtcm3.bin` を parse-rtcm3 で 27 frames/CRC0）

## 4. ビルド & 焼き

ESP-IDF **5.4.4**。Zig 依存は無い（caster 除去済み）。**この開発機では `~/esp/esp-idf`
に導入済み**（別マシンなら https://dl.espressif.com/dl/esp-idf/ から 5.4.4）。

```
. ~/esp/esp-idf/export.sh
idf.py set-target esp32p4                 # 初回のみ
idf.py -p /dev/ttyACM0 build flash monitor
```

Tab5 は USB-Serial-JTAG（`303a:1001`）で `/dev/ttyACM0`。フラッシュ後の起動ログに
`Mosaic SBF output provisioned` / `RTK fixed` が出れば通し動作。

## 5. 実機での確認手順（コンソール = `tab5>`）

```
stats            # SBF ブロック数 + fix (RTK fixed が出るか)
sbf              # PVT/Att/DOP/RxStatus のデコード値
ntrip            # NTRIP client の接続 + 受信バイト
demofield        # 合成圃場をロード（走行せず地図/土量を確認）
screen map       # 平面図 MAP へ / screen work で戻る
vol              # 切土/盛土 m3
nmeaout          # COM1 NMEA 設定
```

実圃場: `record perim`→外周走行→`record field`→内部走行→`survey fit`→`vol`。

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
