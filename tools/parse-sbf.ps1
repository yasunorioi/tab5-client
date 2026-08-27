<#
.SYNOPSIS
  SBF キャプチャを CRC 全数検証してブロック内訳を出す。

.EXAMPLE
  .\tools\parse-sbf.ps1 -Path .\tests\fixtures\mosaic-g5-p3h-sbf.bin -Seconds 12.01

.NOTES
  ★ PowerShell の落とし穴: [byte] を左辺にした -shl はバイト幅で切られる。
     ([int]$b[$i] -shl 16) のように必ず [int] へキャストすること。
     これを忘れると CRC が全滅して原因が掴みにくい。
#>
param(
  [Parameter(Mandatory=$true)][string]$Path,
  [double]$Seconds = 0
)
$d = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
function Crc16Ccitt([byte[]]$b,[int]$off,[int]$len) {
  [int]$crc = 0
  for ($i=$off; $i -lt $off+$len; $i++) {
    $crc = $crc -bxor ([int]$b[$i] -shl 8)
    for ($k=0;$k -lt 8;$k++) {
      if ($crc -band 0x8000) { $crc = (($crc -shl 1) -bxor 0x1021) -band 0xFFFF } else { $crc = ($crc -shl 1) -band 0xFFFF }
    }
  }
  return $crc
}
$names = @{4007='PVTGeodetic';5938='AttEuler';4014='ReceiverStatus';4001='DOP';4027='MeasEpoch';5914='ReceiverTime'}
$tally=@{}; $bad=0; $i=0; $used=0
while ($i -lt $d.Length-8) {
  if ($d[$i] -eq 0x24 -and $d[$i+1] -eq 0x40) {
    $crc=[BitConverter]::ToUInt16($d,$i+2); $id=[BitConverter]::ToUInt16($d,$i+4); $len=[BitConverter]::ToUInt16($d,$i+6)
    if ($len -ge 8 -and ($len % 4) -eq 0 -and ($i+$len) -le $d.Length) {
      if ((Crc16Ccitt $d ($i+4) ($len-4)) -eq $crc) {
        $blk=$id -band 0x1FFF; $rev=$id -shr 13
        $k = "{0,-5} {1,-16} rev{2} len={3}" -f $blk, $names[[int]$blk], $rev, $len
        $tally[$k]=[int]$tally[$k]+1; $used+=$len; $i+=$len; continue
      } else { $bad++ }
    }
  }
  $i++
}
$tot=($tally.Values|Measure-Object -Sum).Sum
"bytes: $($d.Length)   CRC-valid blocks: $tot   CRC-failed: $bad   frame bytes: $used / $($d.Length)"
$tally.GetEnumerator() | Sort-Object Name | ForEach-Object {
  if ($Seconds -gt 0) { "  {0}  x{1}  ({2} Hz)" -f $_.Key, $_.Value, [math]::Round($_.Value/$Seconds,1) }
  else { "  {0}  x{1}" -f $_.Key, $_.Value }
}
