<#
.SYNOPSIS
  mosaic-go G5 P3H に Septentrio ASCII コマンドを送って応答を表示する。

.EXAMPLE
  .\tools\mosaic-cmd.ps1 -Port COM3 -Command getReceiverCapabilities
  .\tools\mosaic-cmd.ps1 -Port COM3 -Command 'help, setSBFOutput'
  .\tools\mosaic-cmd.ps1 -Port COM3 -Command lstAsciiDisplay -Wait 3500

.NOTES
  Windows 上では MI_00 -> USB1、MI_02 -> USB2 として列挙される。COM 番号は環境依存なので
  Get-PnpDevice -Class Ports | ? InstanceId -like '*VID_152A*' で確認すること。
  データを流していないポートをコマンド用に使うと応答が読みやすい（既定は USB2）。
  USB CDC なのでボーレートは無視される。
#>
param(
  [string]$Port = 'COM3',
  [Parameter(Mandatory=$true)][string]$Command,
  [int]$Wait = 2000
)
$sp = New-Object System.IO.Ports.SerialPort $Port,115200,'None',8,'One'
$sp.ReadTimeout = 5000; $sp.DtrEnable = $true; $sp.RtsEnable = $true
$sp.Open(); Start-Sleep -Milliseconds 300
$sp.DiscardInBuffer()
$sp.Write("$Command`r`n")
Start-Sleep -Milliseconds $Wait
$out = ''
while ($sp.BytesToRead -gt 0) { $out += $sp.ReadExisting(); Start-Sleep -Milliseconds 250 }
$sp.Close()
$out
