<#
.SYNOPSIS
  RTCM3 キャプチャを CRC-24Q 全数検証してメッセージ内訳を出す。

.EXAMPLE
  # キャスターから取ってきて検証する
  curl.exe -s -m 8 -H "Ntrip-Version: Ntrip/2.0" -H "User-Agent: NTRIP probe/0.1" `
    http://rtk.toiso.fit:2101/eniwa-bd982 -o cap.bin
  .\tools\parse-rtcm3.ps1 -Path cap.bin -Seconds 8

.NOTES
  curl は chunked を自動で剥がす。生ソケットで読んだデータを食わせる場合は
  先に de-chunk しておくこと（さもないと CRC がランダムに落ちる）。
  ★ [byte] -shl のバイト幅切り捨てに注意（parse-sbf.ps1 の NOTES 参照）。
#>
param(
  [Parameter(Mandatory=$true)][string]$Path,
  [double]$Seconds = 0
)
$d = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
function Crc24q([byte[]]$b,[int]$off,[int]$len) {
  [int]$crc = 0
  for ($i=$off; $i -lt $off+$len; $i++) {
    $crc = $crc -bxor ([int]$b[$i] -shl 16)
    for ($k=0;$k -lt 8;$k++) { $crc = $crc -shl 1; if ($crc -band 0x1000000) { $crc = $crc -bxor 0x1864CFB } }
    $crc = $crc -band 0xFFFFFF
  }
  return $crc
}
$names = @{1005='ARP';1006='ARP + antenna height';1007='Antenna descriptor';1008='Antenna descriptor + serial';
           1013='System parameters';1019='GPS eph';1020='GLONASS eph';1033='Receiver/antenna descriptor';
           1042='BeiDou eph';1044='QZSS eph';1045='Galileo F/NAV eph';1046='Galileo I/NAV eph';1230='GLONASS bias';
           1074='GPS MSM4';1077='GPS MSM7';1084='GLONASS MSM4';1087='GLONASS MSM7';1094='Galileo MSM4';1097='Galileo MSM7';
           1104='SBAS MSM4';1107='SBAS MSM7';1114='QZSS MSM4';1117='QZSS MSM7';1124='BeiDou MSM4';1127='BeiDou MSM7';
           1134='NavIC MSM4';1137='NavIC MSM7'}
$tally=@{}; $bad=0; $i=0; $used=0
while ($i -lt $d.Length-6) {
  if ($d[$i] -eq 0xD3) {
    $len = (((([int]$d[$i+1]) -band 0x03) -shl 8) -bor [int]$d[$i+2])
    if ($len -gt 0 -and ($i+3+$len+3) -le $d.Length) {
      $rx = (([int]$d[$i+3+$len]) -shl 16) -bor (([int]$d[$i+4+$len]) -shl 8) -bor ([int]$d[$i+5+$len])
      if ((Crc24q $d $i (3+$len)) -eq $rx) {
        $t = ((([int]$d[$i+3]) -shl 4) -bor (([int]$d[$i+4]) -shr 4))
        $k = "{0,-5} {1}" -f $t, $names[[int]$t]
        $tally[$k]=[int]$tally[$k]+1; $used += 6+$len; $i += 3+$len+3; continue
      } else { $bad++ }
    }
  }
  $i++
}
$tot=($tally.Values|Measure-Object -Sum).Sum
"bytes: $($d.Length)   CRC-valid frames: $tot   CRC-failed: $bad   frame bytes: $used / $($d.Length)"
$tally.GetEnumerator() | Sort-Object {[int](($_.Key -split '\s+')[0])} | ForEach-Object {
  if ($Seconds -gt 0) { "  {0}  x{1}  ({2} Hz)" -f $_.Key, $_.Value, [math]::Round($_.Value/$Seconds,1) }
  else { "  {0}  x{1}" -f $_.Key, $_.Value }
}
