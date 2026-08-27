<#
.SYNOPSIS
  受信機の SBF ストリームを生バイトのままファイルへ採取する。

.EXAMPLE
  # 先に出力を有効化してから
  .\tools\mosaic-cmd.ps1 -Port COM3 -Command 'setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP, msec100'
  .\tools\capture-sbf.ps1 -Port COM4 -Seconds 12 -Out .\tests\fixtures\new-capture.bin
#>
param(
  [string]$Port = 'COM4',
  [int]$Seconds = 12,
  [Parameter(Mandatory=$true)][string]$Out
)
$sp = New-Object System.IO.Ports.SerialPort $Port,115200,'None',8,'One'
$sp.ReadTimeout = 3000; $sp.DtrEnable = $true; $sp.RtsEnable = $true
$sp.Open(); Start-Sleep -Milliseconds 400; $sp.DiscardInBuffer()
$ms = New-Object System.IO.MemoryStream
$sw = [Diagnostics.Stopwatch]::StartNew()
while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
  if ($sp.BytesToRead -gt 0) { $b = New-Object byte[] $sp.BytesToRead; $n = $sp.Read($b,0,$b.Length); $ms.Write($b,0,$n) }
  else { Start-Sleep -Milliseconds 10 }
}
$sw.Stop(); $sp.Close()
$d = $ms.ToArray()
[IO.File]::WriteAllBytes((Resolve-Path -LiteralPath (Split-Path -Parent $Out)).Path + '\' + (Split-Path -Leaf $Out), $d)
"captured {0} B in {1:N2}s => {2} B/s -> {3}" -f $d.Length, $sw.Elapsed.TotalSeconds, [math]::Round($d.Length/$sw.Elapsed.TotalSeconds), $Out
